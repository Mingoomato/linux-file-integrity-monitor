# Test plan

Run from a disposable Linux environment after dependencies are installed.

- Build with the exact CMake commands in `README.md` under GCC and Clang.
- Exercise empty, nested, Unicode, symlink, special-file, mount-boundary, and oversized-file trees.
- Compare SHA-256 values to `sha256sum`; modify, add, delete, rename, and chmod files between baseline and verify.
- Exercise unstable files, permission denial, corrupt/future-version databases, existing database refusal, event-log ENOSPC, inotify overflow, watch registration failure, and SIGTERM.
- Run the 10k-file benchmark only after measuring and recording machine/kernel/filesystem details.

The smoke test has been run; the listed edge, fault-injection, and benchmark scenarios remain pending. Privileged tests require a disposable VM.
