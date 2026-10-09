# Code Walkthrough — Linux File Integrity Monitor

This walkthrough follows the checked-in code rather than an idealized production design. The single C translation unit is intentionally compact; function names below are the best navigation points.

## 1. CLI and root boundary

Start at `main` in `src/fimctl.c`. It dispatches `init`, `scan`, `verify`, `status`, `export`, `watch`, and `stop`. `canonical_root` converts the user-supplied root to an absolute canonical path, checks that it is an existing directory, and rejects broad system roots. Root-taking operations use that result before touching monitored paths.

## 2. Scanner and file identity

`scan_root` opens the selected directory with `O_DIRECTORY | O_NOFOLLOW`, gets the root device with `fstat`, and calls `scan_dir`.

`scan_dir` uses `dup` + `fdopendir` so each recursive frame can enumerate its own directory stream while retaining the directory FD for relative operations. `fstatat` with `AT_SYMLINK_NOFOLLOW` inspects an entry without following a symlink. Directories are opened by `openat` with `O_DIRECTORY | O_NOFOLLOW`; regular files are opened by `openat` with `O_NOFOLLOW`. The scanner skips a different `st_dev`, symlinks, special files, files above its fixed size limit, and excessive recursion depth.

For a file, the flow is `openat` → `fstat` before → `hash_fd` → `fstat` after → `same_file_state`. A mismatch aborts the scan with an unstable-file error. `hash_fd` initializes an EVP digest context, reads chunks, updates SHA-256, finalizes a 32-byte digest, and renders lowercase hexadecimal. `push_row` transfers the path allocation to the row vector. After traversal, `row_path_compare` lets `scan_root` sort rows.

## 3. Persistence and comparison

`db_open` opens SQLite, enables WAL and `synchronous=FULL`, creates the schema if needed, and accepts only the supported schema version. `store_rows` begins an immediate transaction, writes the root record, replaces file rows, and commits; an intermediate failure requests rollback. Each row records relative path, device, inode, mode, UID, GID, size, nanosecond mtime, and digest.

`compare_rows` walks stored rows ordered by path and the sorted scan vector together. A missing old path becomes deleted; a new path becomes created; a changed digest/size becomes modified; otherwise metadata differences become metadata_changed. This sorted merge uses bounded per-row state rather than a second hash map. The comparison is a current-state comparison; it does not infer an unobserved rename.

## 4. Watcher and event journal

`cmd_watch` creates `inotify_init1(IN_NONBLOCK | IN_CLOEXEC)`, verifies the root device, and calls `add_tree_watches`. `watch_add` subscribes to create/delete/write-close/attribute/move signals and stores the mapping from kernel watch descriptor to path. `add_tree_watches` walks existing directories and avoids symlink and device-crossing paths.

The event loop uses `poll` and `read`. Each read can contain multiple variable-length `inotify_event` records, so the cursor advances by `sizeof(*ev) + ev->len`. The watch descriptor selects a base path; the optional event name appends a child. Event masks are mapped to an event kind and passed to `emit_event`.

`emit_event` escapes control characters and JSON quotes in the path, generates a UUID-like event ID through OpenSSL random bytes, formats a UTC timestamp, and writes one complete line with a retry for `EINTR` and partial writes. The actor is explicitly unknown. The code currently exposes rename cookies without pairing them and reports overflow without automatic reconciliation.

## 5. Shutdown and lifetime audit

The signal handler sets `stopping`; it does not call `printf`, `free`, SQLite, or `close`. Once control returns to the loop, the cleanup path unlinks the PID file, frees watch-path strings and the watch vector, and closes the event and inotify FDs. Review every new `goto`/early return in this function against the opened resource list: `inofd`, `rootfd`, `logfd`, PID file, and `ws` allocations.

## 6. Tests and limits

`tests/smoke.sh` creates a temporary tree and exercises a representative baseline/verify/export/watch/shutdown path. Its pass does not cover queue overflow, rename pairing, concurrent mutation, DB corruption, disk-full behavior, or large-tree resource use. The current code has no multithreaded scanner or hash queue; concurrency risk mainly concerns external filesystem changes while a scan is running.
