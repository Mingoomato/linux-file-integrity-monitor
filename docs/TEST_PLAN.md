# Test plan

Run from a disposable Linux environment after dependencies are installed.

- Configure/build with the README commands under GCC and Clang; run CTest from each build directory and ensure it uses that build's `fimctl`.
- Exercise empty, nested, Unicode, symlink, special-file, mount-boundary, and oversized-file trees.
- Compare SHA-256 values to `sha256sum`; modify, add, delete, rename, and chmod files between baseline and verify.
- Exercise stable-hash retries, permission denial, corrupt/future-version databases, existing database refusal, event-log ENOSPC, real inotify overflow, watch-registration failure, and SIGTERM.
- Run the 10k-file benchmark only after recording machine, kernel, and filesystem details.

The current smoke test covers baseline creation and comparison, startup reconciliation hashes, symlink exclusion, invalid/duplicate periodic interval rejection, create/delete/metadata event hashing, a repeated write burst with count and timestamp evidence, a periodic full-scan event pair, paired and unmatched rename flows, move timeout, new-directory and moved-directory watch recovery, a synthetic degraded→recovering→healthy overflow transition, database immutability during watch, and shutdown. The `--inject-overflow-once` option does not cause a kernel `IN_Q_OVERFLOW`. Actual queue pressure, watch-limit failure/retry, synchronized concurrent-mutation behavior, mount-boundary behavior, filesystem faults, benchmark, and distribution matrix remain pending. Privileged or host-wide fault tests belong in a disposable VM.
