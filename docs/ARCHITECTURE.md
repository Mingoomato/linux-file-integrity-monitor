# Architecture

`fimctl` has a small single-process design:

1. The CLI canonicalizes an existing absolute root and rejects broad system roots.
2. The scanner walks from an opened directory descriptor, uses `fstatat(..., AT_SYMLINK_NOFOLLOW)`, rejects symlinks and mount crossings, opens regular files with `O_NOFOLLOW`, hashes through OpenSSL EVP, and compares pre/post `fstat` state.
3. SQLite stores schema version, root identity, file metadata, and SHA-256. Writes use a transaction, WAL, and `synchronous=FULL`.
4. `verify` compares a fresh sorted scan against the stored baseline. `watch` registers inotify watches and appends event.v1-shaped NDJSON.

The scanner uses directory descriptors to reduce path races. The inotify backend currently registers pathnames and is less race-resistant. Rename cookies and ordinary per-path event bursts use bounded 256-entry pending tables; unmatched move-in and timed-out move-out observations are recorded separately. Non-rename events coalesce over a 100 ms quiet window and preserve the mask, count, and observation-time range. Queue overflow, invalidated watches, new directories, and directory moves trigger a recovery cycle: the process replaces the inotify instance, recursively rebuilds watches, scans current file state, emits create/delete/content/metadata differences against the last successful scan, and reports `coverage.healthy` only if watch installation and scanning both succeed. A failed attempt remains `degraded` and is retried after idle intervals. The watcher does not update the SQLite baseline during this operation. Baseline-aware hash debounce, periodic verification, and a bounded hash worker queue remain unimplemented. Inotify was selected for unprivileged event observation; fanotify is not a drop-in equivalent and is not implemented.
