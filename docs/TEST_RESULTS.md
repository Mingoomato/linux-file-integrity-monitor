# Test results

> Latest / 최신: [2026-10-11 checkpoint](VALIDATION_CHECKPOINT.md). Earlier entries below describe their own historical revisions; they do not override this scoped checkpoint.

Status: selected FIM verification gates passed on the current Linux host. Full SRS-01 acceptance remains incomplete.

Environment recorded for this run: Kali GNU/Linux Rolling 2026.3, x86_64, Linux `7.1.5+kali-amd64`; GCC 16.2.0, Clang 21.1.8, CMake 4.4.3, OpenSSL 3.6.3, SQLite 3.53.4.

- GCC Release build and CTest: passed 3 consecutive runs (1/1 each).
- Clang Release build and CTest: passed 3 consecutive runs (1/1 each).
- Clang Debug ASan/UBSan build and CTest: passed 3 consecutive runs (1/1 each).
- ThreadSanitizer Debug build and CTest: passed 3 consecutive runs (1/1 each).
- Clang static analysis with `-Wall -Wextra -Wpedantic -Werror`: passed with no diagnostics.
- `sh -n tests/smoke.sh`, JSON parsing of `contracts/event-v1.schema.json`, and `git diff --check`: passed.
- Draft 2020-12 validation passed for three actual watcher events, including a worker-generated `file.created` event.
- CTest passes the target directory through `BIN_DIR`, so each build runs its own `fimctl` binary.
- Smoke coverage: baseline creation and clean verification; modified-file detection; JSON path export; symlink exclusion; invalid/duplicate interval rejection; malformed escaping baseline path rejection; startup reconciliation hashes; coalesced create with raw count and ordered timestamps; deleted and metadata-changed files with hashes; periodic full verification; synthetic degraded→recovering→healthy overflow recovery; paired/unmatched/expired rename observations; new and moved directory watch recovery; unchanged SQLite baseline during watch; 1,100-file worker-pool event burst; `/dev/full` journal-write failure with nonzero exit and PID cleanup; and SIGTERM shutdown through the PID file.
- Worker review confirms two hash threads, a cap of 1,024 outstanding jobs, eventfd completion notification, and sequence-ordered main-loop commits. The 1,100-file test verifies delivered observations at that workload; it does not deterministically prove the queue reached its hard cap.

Not run or still open: `cppcheck` (not installed), genuine kernel `IN_Q_OVERFLOW`, deterministic worker-queue cap saturation/backpressure, watch-limit exhaustion and retry, mount-boundary checks, oversized-file checks, synchronized concurrent-mutation/fault injection, real filesystem ENOSPC, 10k-file benchmark, and Ubuntu/RockyOS runtime matrix. The synthetic overflow case injects the recovery trigger rather than forcing kernel overflow. A passing smoke suite is evidence for its tested paths, not proof of complete monitoring coverage or full SRS conformance.
