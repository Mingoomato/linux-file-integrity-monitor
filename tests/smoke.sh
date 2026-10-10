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
printf 'metadata fixture\n' >"$tmp/lab/metadata.txt"
printf 'outside\n' >"$tmp/outside.txt"
ln -s "$tmp/outside.txt" "$tmp/lab/external-link"

"$bin" init --root "$tmp/lab" --db "$tmp/state.sqlite"
"$bin" verify --db "$tmp/state.sqlite" --full
for invalid_interval in 0 86401 -1 invalid; do
    set +e
    "$bin" watch --root "$tmp/lab" --db "$tmp/state.sqlite" --events "$tmp/invalid-events.ndjson" \
        --verify-interval-seconds "$invalid_interval" >"$tmp/invalid-interval.out" 2>&1
    rc=$?
    set -e
    [ "$rc" -eq 2 ] || { echo "invalid verification interval was accepted: $invalid_interval" >&2; exit 1; }
done
set +e
"$bin" watch --root "$tmp/lab" --db "$tmp/state.sqlite" --events "$tmp/invalid-events.ndjson" \
    --verify-interval-seconds 1 --verify-interval-seconds 2 >"$tmp/duplicate-interval.out" 2>&1
rc=$?
set -e
[ "$rc" -eq 2 ]
"$bin" export --db "$tmp/state.sqlite" --out "$tmp/export.json"
python3 - "$tmp/export.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as f:
    rows = json.load(f)["files"]
assert [row["path"] for row in rows] == ['metadata.txt', 'nested/quoted"name.txt']
PY
python3 - "$tmp/state.sqlite" "$tmp/malformed.sqlite" <<'PY'
import sqlite3, sys
source = sqlite3.connect(sys.argv[1])
target = sqlite3.connect(sys.argv[2])
source.backup(target)
target.execute("UPDATE files SET relative_path='../outside.txt' WHERE relative_path='metadata.txt'")
target.commit()
source.close()
target.close()
PY
set +e
"$bin" watch --root "$tmp/lab" --db "$tmp/malformed.sqlite" --events "$tmp/malformed-events.ndjson" \
    >"$tmp/malformed-baseline.out" 2>&1
rc=$?
set -e
[ "$rc" -eq 1 ]
[ ! -e "$tmp/malformed-events.ndjson" ]

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

"$bin" watch --root "$tmp/lab" --db "$tmp/state.sqlite" --events "$tmp/events.ndjson" \
    --pidfile "$tmp/watch.pid" \
    --inject-overflow-once \
    >"$tmp/watch.log" 2>&1 &
watch_pid=$!
tries=0
while [ ! -s "$tmp/watch.pid" ] && [ "$tries" -lt 100 ]; do sleep 0.05; tries=$((tries + 1)); done
[ -s "$tmp/watch.pid" ]
tries=0
while ! python3 - "$tmp/events.ndjson" <<'PY'
import json, sys
try:
    rows = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
except (FileNotFoundError, json.JSONDecodeError):
    raise SystemExit(1)
raise SystemExit(0 if any(row.get("kind") == "coverage.healthy" and
                          row.get("coverage", {}).get("reason") == "reconciliation_complete"
                          for row in rows) else 1)
PY
do
    [ "$tries" -lt 100 ] || { echo "initial coverage reconciliation timeout" >&2; exit 1; }
    tries=$((tries + 1)); sleep 0.05
done
python3 - "$tmp/events.ndjson" "$tmp/lab/nested/quoted\"name.txt" <<'PY'
import hashlib, json, os, sys
path = os.path.realpath(sys.argv[2])
rows = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
old_hash = hashlib.sha256(b"baseline\n").hexdigest()
new_hash = hashlib.sha256(b"changed content\n").hexdigest()
assert any(row.get("kind") == "file.modified" and
           row.get("target", {}).get("path") == path and
           row.get("evidence", {}).get("before_sha256") == old_hash and
           row.get("evidence", {}).get("after_sha256") == new_hash and
           row.get("evidence", {}).get("reconciliation") is True
           for row in rows)
PY
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
printf 'burst one\n' >>"$tmp/lab/nested/new-file.txt"
printf 'burst two\n' >>"$tmp/lab/nested/new-file.txt"
tries=0
while ! python3 - "$tmp/events.ndjson" "$tmp/lab/nested/new-file.txt" <<'PY'
import json, os, sys
path = os.path.realpath(sys.argv[2])
try:
    rows = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
except (FileNotFoundError, json.JSONDecodeError):
    raise SystemExit(1)
coalesced = any(row.get("target", {}).get("path") == path and
                row.get("kind") == "file.created" and
                row.get("evidence", {}).get("observation_count", 0) >= 2 and
                row.get("evidence", {}).get("before_sha256") is None and
                row.get("evidence", {}).get("after_sha256") ==
                __import__("hashlib").sha256(b"watched\nburst one\nburst two\n").hexdigest() and
                isinstance(row.get("evidence", {}).get("first_observed_unix_ns"), int) and
                isinstance(row.get("evidence", {}).get("last_observed_unix_ns"), int) and
                row["evidence"]["first_observed_unix_ns"] <=
                row["evidence"]["last_observed_unix_ns"]
                for row in rows)
raise SystemExit(0 if coalesced else 1)
PY
do
    [ "$tries" -lt 100 ] || { echo "watch event timeout" >&2; exit 1; }
    tries=$((tries + 1)); sleep 0.05
done
printf 'reconcile this state\n' >"$tmp/lab/nested/reconcile-marker.txt"
chmod 600 "$tmp/lab/metadata.txt"
rm "$tmp/lab/nested/quoted\"name.txt"
mv "$tmp/lab/nested/new-file.txt" "$tmp/lab/nested/renamed-file.txt"
tries=0
while ! python3 - "$tmp/events.ndjson" "$tmp/lab/nested/renamed-file.txt" <<'PY'
import json, os, sys
path = os.path.realpath(sys.argv[2])
try:
    rows = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
except (FileNotFoundError, json.JSONDecodeError):
    raise SystemExit(1)
from_events = [row for row in rows if row.get("kind") == "file.moved_from"]
to_events = [row for row in rows if row.get("kind") == "file.moved_to" and row.get("target", {}).get("path") == path]
matched = [row for row in rows if row.get("kind") == "path.renamed" and row.get("target", {}).get("path") == path]
raise SystemExit(0 if matched and any(a.get("evidence", {}).get("rename_cookie") == matched[-1].get("evidence", {}).get("rename_cookie") for a in from_events) and to_events else 1)
PY
do
    [ "$tries" -lt 100 ] || { echo "rename correlation timeout" >&2; exit 1; }
    tries=$((tries + 1)); sleep 0.05
done
mv "$tmp/lab/nested/renamed-file.txt" "$tmp/moved-outside.txt"
tries=0
while ! python3 - "$tmp/events.ndjson" "$tmp/lab/nested/renamed-file.txt" <<'PY'
import json, os, sys
path = os.path.realpath(sys.argv[2])
try:
    rows = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
except (FileNotFoundError, json.JSONDecodeError):
    raise SystemExit(1)
raise SystemExit(0 if any(row.get("kind") == "file.rename_timeout" and row.get("target", {}).get("path") == path for row in rows) else 1)
PY
do
    [ "$tries" -lt 120 ] || { echo "rename timeout event missing" >&2; exit 1; }
    tries=$((tries + 1)); sleep 0.05
done
python3 - "$tmp/events.ndjson" "$tmp/lab/nested/reconcile-marker.txt" "$tmp/lab/metadata.txt" <<'PY'
import hashlib, json, os, sys
marker = os.path.realpath(sys.argv[2])
metadata = os.path.realpath(sys.argv[3])
rows = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
assert any(row.get("kind") == "file.created" and
           row.get("target", {}).get("path") == marker and
           row.get("evidence", {}).get("after_sha256") ==
           hashlib.sha256(b"reconcile this state\n").hexdigest()
           for row in rows)
assert any(row.get("kind") == "file.deleted" and
           row.get("target", {}).get("path", "").endswith('/nested/quoted"name.txt') and
           row.get("evidence", {}).get("before_sha256") ==
           hashlib.sha256(b"changed content\n").hexdigest()
           for row in rows)
assert any(row.get("kind") == "file.metadata_changed" and
           row.get("target", {}).get("path") == metadata and
           row.get("evidence", {}).get("before_sha256") ==
           hashlib.sha256(b"metadata fixture\n").hexdigest() and
           row.get("evidence", {}).get("after_sha256") ==
           hashlib.sha256(b"metadata fixture\n").hexdigest()
           for row in rows)
PY
tries=0
while ! python3 - "$tmp/events.ndjson" "$tmp/lab/nested/reconcile-marker.txt" <<'PY'
import json, os, sys
try:
    rows = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
except (FileNotFoundError, json.JSONDecodeError):
    raise SystemExit(1)
degraded_i = next((i for i, row in enumerate(rows)
                   if row.get("kind") == "coverage.degraded" and
                   row.get("coverage", {}).get("reason") == "test_injected_queue_overflow"), -1)
recovering_i = next((i for i, row in enumerate(rows)
                     if row.get("kind") == "coverage.recovering" and
                     row.get("coverage", {}).get("reason") == "test_injected_queue_overflow"), -1)
healthy_i = next((i for i, row in enumerate(rows)
                  if i > recovering_i and row.get("kind") == "coverage.healthy" and
                  row.get("coverage", {}).get("reason") == "reconciliation_complete"), -1)
injected = 0 <= degraded_i < recovering_i < healthy_i
healthy = healthy_i >= 0
raise SystemExit(0 if injected and healthy else 1)
PY
do
    [ "$tries" -lt 100 ] || { cat "$tmp/watch.log" >&2; echo "overflow recovery sequence missing" >&2; exit 1; }
    tries=$((tries + 1)); sleep 0.05
done
printf 'incoming\n' >"$tmp/incoming.txt"
mv "$tmp/incoming.txt" "$tmp/lab/nested/incoming.txt"
tries=0
while ! python3 - "$tmp/events.ndjson" "$tmp/lab/nested/incoming.txt" <<'PY'
import json, os, sys
path = os.path.realpath(sys.argv[2])
try:
    rows = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
except (FileNotFoundError, json.JSONDecodeError):
    raise SystemExit(1)
raise SystemExit(0 if any(row.get("kind") == "file.rename_match_failed" and row.get("target", {}).get("path") == path for row in rows) else 1)
PY
do
    [ "$tries" -lt 100 ] || { echo "unmatched rename event missing" >&2; exit 1; }
    tries=$((tries + 1)); sleep 0.05
done
mkdir "$tmp/lab/new-directory"
printf 'created before the recursive scan\n' >"$tmp/lab/new-directory/child.txt"
tries=0
while ! python3 - "$tmp/events.ndjson" "$tmp/lab/new-directory/child.txt" <<'PY'
import json, os, sys
path = os.path.realpath(sys.argv[2])
try:
    rows = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
except (FileNotFoundError, json.JSONDecodeError):
    raise SystemExit(1)
recovered = any(row.get("kind") == "coverage.healthy" and
                row.get("coverage", {}).get("reason") == "reconciliation_complete" and
                any(prev.get("kind") == "coverage.recovering" and
                    prev.get("coverage", {}).get("reason") == "new_directory_discovered"
                    for prev in rows[:i])
                for i, row in enumerate(rows))
created = any(row.get("kind") == "file.created" and
              row.get("target", {}).get("path") == path
              for row in rows)
raise SystemExit(0 if recovered and created else 1)
PY
do
    [ "$tries" -lt 100 ] || { echo "new directory recursive reconciliation missing" >&2; exit 1; }
    tries=$((tries + 1)); sleep 0.05
done
mv "$tmp/lab/new-directory" "$tmp/lab/moved-directory"
tries=0
while ! python3 - "$tmp/events.ndjson" <<'PY'
import json, sys
try:
    rows = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
except (FileNotFoundError, json.JSONDecodeError):
    raise SystemExit(1)
recovered = any(row.get("kind") == "coverage.healthy" and
                row.get("coverage", {}).get("reason") == "reconciliation_complete" and
                any(prev.get("kind") == "coverage.recovering" and
                    prev.get("coverage", {}).get("reason") in
                    ("directory_tree_moved", "watch_invalidated")
                    for prev in rows[:i])
                for i, row in enumerate(rows))
raise SystemExit(0 if recovered else 1)
PY
do
    [ "$tries" -lt 100 ] || { echo "moved directory watch recovery missing" >&2; exit 1; }
    tries=$((tries + 1)); sleep 0.05
done
printf 'after directory move\n' >"$tmp/lab/moved-directory/after-move.txt"
tries=0
while ! python3 - "$tmp/events.ndjson" "$tmp/lab/moved-directory/after-move.txt" <<'PY'
import json, os, sys
path = os.path.realpath(sys.argv[2])
try:
    rows = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
except (FileNotFoundError, json.JSONDecodeError):
    raise SystemExit(1)
raise SystemExit(0 if any(row.get("target", {}).get("path") == path for row in rows) else 1)
PY
do
    [ "$tries" -lt 100 ] || { echo "moved directory descendant is not watched" >&2; exit 1; }
    tries=$((tries + 1)); sleep 0.05
done
"$bin" stop --pidfile "$tmp/watch.pid"
wait "$watch_pid"
watch_pid=
"$bin" watch --root "$tmp/lab" --db "$tmp/state.sqlite" --events "$tmp/periodic-events.ndjson" \
    --pidfile "$tmp/periodic-watch.pid" --verify-interval-seconds 1 \
    >"$tmp/periodic-watch.log" 2>&1 &
watch_pid=$!
tries=0
while [ ! -s "$tmp/periodic-watch.pid" ] && [ "$tries" -lt 100 ]; do sleep 0.05; tries=$((tries + 1)); done
[ -s "$tmp/periodic-watch.pid" ]
tries=0
while ! python3 - "$tmp/periodic-events.ndjson" <<'PY'
import json, sys
try:
    rows = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
except (FileNotFoundError, json.JSONDecodeError):
    raise SystemExit(1)
periodic = [i for i, row in enumerate(rows)
            if row.get("kind") == "coverage.recovering" and
            row.get("coverage", {}).get("reason") == "periodic_verification"]
complete = [i for i, row in enumerate(rows)
            if row.get("kind") == "coverage.healthy" and
            row.get("coverage", {}).get("reason") == "periodic_verification_complete"]
raise SystemExit(0 if any(done > start for start in periodic for done in complete) else 1)
PY
do
    [ "$tries" -lt 100 ] || { cat "$tmp/periodic-watch.log" >&2; echo "periodic verification did not run" >&2; exit 1; }
    tries=$((tries + 1)); sleep 0.05
done
"$bin" stop --pidfile "$tmp/periodic-watch.pid"
wait "$watch_pid"
watch_pid=
"$bin" export --db "$tmp/state.sqlite" --out "$tmp/final-baseline.json"
python3 - "$tmp/final-baseline.json" <<'PY'
import hashlib, json, sys
rows = json.load(open(sys.argv[1], encoding="utf-8"))["files"]
baseline = next(row for row in rows if row["path"] == 'nested/quoted"name.txt')
assert baseline["sha256"] == hashlib.sha256(b"baseline\n").hexdigest()
assert not any(row["path"] == "nested/new-file.txt" for row in rows)
PY
python3 - "$tmp/events.ndjson" <<'PY'
import json, re, sys
rows = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
assert rows
assert all(re.fullmatch(r"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}", row["event_id"]) for row in rows)
assert all(re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ", row["observed_at"]) for row in rows)
PY

echo "FIM smoke: PASS"
