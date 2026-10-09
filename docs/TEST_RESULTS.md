# Test results

Status: build and smoke test passed on the current Linux host.

- Release configure/build: passed with CMake, C17, OpenSSL Crypto, and SQLite3.
- `ctest --test-dir build --output-on-failure`: 1/1 passed.
- Separate AddressSanitizer/UndefinedBehaviorSanitizer debug build and the same smoke test: passed.
- Smoke coverage: baseline creation and clean verification; modified-file detection and status code; quoted-path JSON export; symlink exclusion; recursive inotify observation; UUID event IDs, UTC timestamps, and SIGTERM shutdown through the PID file.

Not run: GCC/Clang matrix, static analysis, mount-boundary and oversized-file checks, concurrent-mutation/fault injection, queue overflow, 10k-file benchmark, or Ubuntu/Rocky runtime matrix. A passing smoke test does not establish complete monitoring coverage.
