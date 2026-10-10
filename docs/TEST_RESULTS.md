# Test results

Status: selected Release and ASan/UBSan smoke gates passed on the current Linux host; complete SRS-01 verification remains pending.

Environment: Kali GNU/Linux Rolling 2026.3, x86_64, Linux `7.1.5+kali-amd64`; GCC 16.2.0, Clang 21.1.8, CMake 4.4.3, OpenSSL 3.6.3, SQLite 3.53.4.

- Release configure/build: passed with CMake, C17, OpenSSL Crypto, and SQLite3.
- GCC Release `cmake --build build -j2`: passed.
- `ctest --test-dir build --output-on-failure`: 1/1 passed.
- Clang 21.1.8 Release configure/build and `ctest --test-dir build-clang --output-on-failure`: passed, 1/1.
- `cmake --build build-asan -j2`: passed.
- `ctest --test-dir build-asan --output-on-failure`: 1/1 passed under AddressSanitizer/UndefinedBehaviorSanitizer.
- `ctest --repeat until-fail:3` passed three consecutive times for both Release and ASan/UBSan.
- `clang --analyze -std=gnu17 -Wall -Wextra -Wpedantic -Werror src/fimctl.c`: passed with no diagnostics.
- Smoke coverage: baseline creation and clean verification; modified-file detection and status code; quoted-path JSON export; symlink exclusion; repeated write-burst coalescing with raw count and ordered first/last timestamps; recursive inotify observation; cookie-paired in-root rename; separate unmatched move-in and timed-out move-out records; explicit degraded→recovering→healthy transitions after synthetic overflow; scan-derived create/delete/metadata changes; new-directory and moved-directory watch recovery; UUID event IDs, UTC timestamps, and SIGTERM shutdown through the PID file.
- The test-only `--inject-overflow-once` trigger executes the same recovery path as an observed overflow but does not cause the kernel to emit `IN_Q_OVERFLOW`.

Not run: `cppcheck`, mount-boundary and oversized-file checks, concurrent-mutation/fault injection, genuine kernel queue overflow, watch-limit failure/retry, baseline-aware hash debounce, event-log ENOSPC, 10k-file benchmark, or Ubuntu/Rocky runtime matrix. A passing smoke test does not establish complete monitoring coverage.
