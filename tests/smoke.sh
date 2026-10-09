#!/bin/sh
set -eu

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
bin_dir=${BIN_DIR:-"$root_dir/build"}
bin="$bin_dir/fimctl"
tmp=$(mktemp -d)
watch_pid=
cleanup() {
    if [ -n "$watch_pid" ]; then kill "$watch_pid" 2>/dev/null || true; wait "$watch_pid" 2>/dev/null || true; fi
    rm -rf "$tmp"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$tmp/lab/nested"
printf 'baseline\n' >"$tmp/lab/nested/quoted\"name.txt"
printf 'outside\n' >"$tmp/outside.txt"
ln -s "$tmp/outside.txt" "$tmp/lab/external-link"

"$bin" init --root "$tmp/lab" --db "$tmp/state.sqlite"
"$bin" verify --db "$tmp/state.sqlite" --full
"$bin" export --db "$tmp/state.sqlite" --out "$tmp/export.json"
python3 - "$tmp/export.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as f:
    rows = json.load(f)["files"]
assert [row["path"] for row in rows] == ['nested/quoted"name.txt']
PY

printf 'changed content\n' >"$tmp/lab/nested/quoted\"name.txt"
set +e
"$bin" verify --db "$tmp/state.sqlite" --full --format json >"$tmp/verify.jsonl"
rc=$?
set -e
[ "$rc" -eq 2 ]
python3 - "$tmp/verify.jsonl" <<'PY'
import json, sys
rows = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
assert any(row.get("kind") == "modified" and row.get("path") == 'nested/quoted"name.txt' for row in rows)
PY

"$bin" watch --root "$tmp/lab" --events "$tmp/events.ndjson" --pidfile "$tmp/watch.pid" \
    >"$tmp/watch.log" 2>&1 &
watch_pid=$!
tries=0
while [ ! -s "$tmp/watch.pid" ] && [ "$tries" -lt 100 ]; do sleep 0.05; tries=$((tries + 1)); done
[ -s "$tmp/watch.pid" ]
printf '%s\n' "$$" >"$tmp/not-watch.pid"
chmod 600 "$tmp/not-watch.pid"
set +e
"$bin" stop --pidfile "$tmp/not-watch.pid" >"$tmp/stop-reject.out" 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ]
kill -0 "$$"
ln -s "$tmp/watch.pid" "$tmp/pid-link"
set +e
"$bin" stop --pidfile "$tmp/pid-link" >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ]
printf 'watched\n' >"$tmp/lab/nested/new-file.txt"
tries=0
while ! python3 - "$tmp/events.ndjson" "$tmp/lab/nested/new-file.txt" <<'PY'
import json, os, sys
path = os.path.realpath(sys.argv[2])
try:
    rows = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
except (FileNotFoundError, json.JSONDecodeError):
    raise SystemExit(1)
raise SystemExit(0 if any(row.get("target", {}).get("path") == path for row in rows) else 1)
PY
do
    [ "$tries" -lt 100 ] || { echo "watch event timeout" >&2; exit 1; }
    tries=$((tries + 1)); sleep 0.05
done
"$bin" stop --pidfile "$tmp/watch.pid"
wait "$watch_pid"
watch_pid=
python3 - "$tmp/events.ndjson" <<'PY'
import json, re, sys
rows = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
assert rows
assert all(re.fullmatch(r"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}", row["event_id"]) for row in rows)
assert all(re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ", row["observed_at"]) for row in rows)
PY

echo "FIM smoke: PASS"
