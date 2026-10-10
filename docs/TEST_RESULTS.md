# Test results

Status: selected FIM verification gates passed on the current Linux host. Full SRS-01 acceptance remains incomplete.

Environment: Kali GNU/Linux Rolling 2026.3, x86_64, Linux `7.1.5+kali-amd64`; GCC 16.2.0, Clang 21.1.8, CMake 4.4.3, OpenSSL 3.6.3, SQLite 3.53.4.

- GCC Release in `build-gcc-review`: configure/build passed; CTest passed 3 consecutive runs (1/1 each).
- Clang Release in `build-clang-review`: configure/build passed; CTest passed 3 consecutive runs (1/1 each).
- Clang Debug ASan/UBSan in `build-asan-review`: configure/build passed; CTest passed 3 consecutive runs (1/1 each).
- `tests/smoke.sh` against the GCC review binary: passed.
- `clang --analyze -std=gnu17 -Wall -Wextra -Wpedantic -Werror -Xanalyzer -analyzer-output=text src/fimctl.c`: passed with no diagnostics.
- `sh -n tests/smoke.sh`, JSON parsing of `contracts/event-v1.schema.json`, and `git diff --check`: passed.
- Draft 2020-12 schema validation of five actual emitted startup, file-change, and periodic-verification events using Python `jsonschema` 4.26.0: passed.
- `clang --analyze -std=gnu17 -Wall -Wextra -Wpedantic -Werror -Xanalyzer -analyzer-output=text src/fimctl.c`: passed with no diagnostics after preserving the scan error before retry sleep.
- The CTest entry passes the tested target directory through `BIN_DIR`, so each compiler/sanitizer test executes its own built binary.
- Smoke coverage: baseline creation and clean verification; modified-file detection; JSON path export; symlink exclusion; invalid/duplicate verification interval rejection; malformed escaping DB-path rejection; startup reconciliation with before/after hashes; coalesced create with raw count and ordered timestamps; deleted and metadata-changed files with hashes; periodic full verification; synthetic degraded→recovering→healthy overflow recovery; paired/unmatched/expired rename observations; new and moved directory watch recovery; unchanged SQLite baseline during watch; event UUID/UTC timestamp format; and SIGTERM shutdown through the PID file.
- `--inject-overflow-once` executes the recovery path deterministically but does not force the kernel to emit `IN_Q_OVERFLOW`.

Not run: `cppcheck`, mount-boundary checks, oversized-file checks, concurrent-mutation/fault injection, genuine kernel queue overflow, watch-limit failure/retry, event-log ENOSPC, 10k-file benchmark, or Ubuntu/RockyOS runtime matrix. A passing smoke test does not establish complete monitoring coverage.
