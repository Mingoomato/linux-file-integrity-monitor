# Test plan

Run from a disposable Linux environment after dependencies are installed.

- Build with the exact CMake commands in `README.md` under GCC and Clang.
- Exercise empty, nested, Unicode, symlink, special-file, mount-boundary, and oversized-file trees.
- Compare SHA-256 values to `sha256sum`; modify, add, delete, rename, and chmod files between baseline and verify.
- Exercise unstable files, permission denial, corrupt/future-version databases, existing database refusal, event-log ENOSPC, inotify overflow, watch registration failure, and SIGTERM.
- Run the 10k-file benchmark only after measuring and recording machine/kernel/filesystem details.

The current smoke test covers an in-root cookie-paired rename, a move into the root with no observed peer, an out-of-root move whose peer times out, a repeated write burst coalesced with count and timestamps, new-directory recursive recovery, directory-move watch rebuilding, and a synthetic overflow trigger that verifies the full-scan diff for create/delete/metadata changes. The `--inject-overflow-once` option exercises the recovery state machine but does not force a kernel `IN_Q_OVERFLOW`. Actual queue pressure, watch-limit failure/retry, baseline-aware hash debounce, unstable-file synchronization, mount-boundary behavior, filesystem faults, benchmark, and distribution matrix remain pending. Privileged or host-wide fault tests require a disposable VM.
