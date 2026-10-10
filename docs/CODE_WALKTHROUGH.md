# Code Walkthrough — Linux File Integrity Monitor

This guide describes the implementation currently in `src/fimctl.c`. The repository was built as a compact Linux systems lab: students can trace a baseline scan, an inotify event, a stable hash, an event-journal record, and a recovery scan in one C translation unit. The single-file layout keeps the learning path short; it is not a claim that a production monitor should put every subsystem in one file.

## 1. CLI, root, and baseline database

Start at `main`. It dispatches `init`, `scan`, `verify`, `status`, `export`, `watch`, and `stop`. Root-taking commands pass through `canonical_root`. The watcher requires `--root`, `--db`, and `--events`; it opens the baseline database read-only and rejects a root that differs from the root stored in the database. `--verify-interval-seconds` accepts one value from 1 through 86,400; it defaults to 3,600 seconds.

`db_open_readonly` checks the schema version. `load_rows` reads sorted baseline rows into an in-memory snapshot and rejects malformed or escaping relative paths before they can be used for reconciliation. Watch operation never updates the database. This is a deliberate separation: `init` and `scan` manage a baseline, while `watch` records observations and current-state differences.

## 2. Scanner and file identity

`scan_root` opens the selected root with `O_DIRECTORY | O_NOFOLLOW`, gets its device number with `fstat`, and calls `scan_dir`.

`scan_dir` duplicates each directory FD for `fdopendir`, so `closedir` owns the duplicate while relative operations retain the original. `fstatat(..., AT_SYMLINK_NOFOLLOW)` inspects entries without following symlinks. Child directories and files are opened relative to the directory FD with `openat` and `O_NOFOLLOW`. The scanner skips symbolic links, special files, other devices, oversized files, and excessive recursion depth.

For each regular file, the sequence is `openat` → `fstat` before → `hash_fd` → `fstat` after → `same_file_state`. The state comparison includes device, inode, size, mtime, ctime, mode, UID, and GID. A changed state makes a full scan fail instead of committing a potentially mixed digest. `hash_fd` reads 64 KiB chunks and feeds OpenSSL EVP SHA-256. `push_row` transfers the allocated relative path to a row vector; `scan_root` sorts rows by path.

## 3. Persistence and full verification

`db_open` enables SQLite WAL and `synchronous=FULL`. `store_rows` uses a transaction to replace file rows and root metadata, rolling back on an intermediate failure. Each row stores the relative path, device, inode, mode, UID, GID, size, nanosecond mtime, and SHA-256.

`verify` creates a fresh sorted scan and `compare_rows` merges it with database rows in path order. It reports created, deleted, modified, and metadata-changed paths. This comparison is a current-state comparison; it does not prove when a change happened. Full verification currently does not infer a rename by matching inode or content across two paths.

## 4. Watch startup and initial reconciliation

`cmd_watch` opens the event journal, creates a protected PID file, opens the root directory FD, loads the database snapshot, and calls `rebuild_and_reconcile`. That function creates a nonblocking inotify FD, recursively installs directory watches with `watch_add` and `add_tree_watches`, and scans the current tree. The first scan is compared to the loaded baseline snapshot, so changes made since `init` or the last `scan` are recorded with before/after hashes and `reconciliation=true`. Only a successful watch installation and scan produce `coverage.healthy`.

The event loop calls `poll`, reads variable-length `struct inotify_event` records, resolves a watch descriptor to its watched path, and appends the event name. `IN_MOVED_FROM` and `IN_MOVED_TO` use a bounded cookie table. A paired cookie emits `path.renamed`; a move-in without its peer and a move-out whose peer times out are represented separately rather than guessed to be a rename.

## 5. Debounce, hashing, and journal evidence

Non-rename signals enter a bounded 256-entry table keyed by path. The watcher ORs event masks, increments the raw observation count, and remembers first/last wall-clock timestamps. A 100 ms quiet window is measured using `CLOCK_MONOTONIC`; the recorded timestamps use `CLOCK_REALTIME` so they can be compared with other logs. The oldest entry flushes early if the table fills. Shutdown and coverage recovery flush pending entries as well.

`flush_pending_event` derives a root-relative path and calls `scan_relative_file`. That helper walks each path component from the already-open root FD using `openat`, `O_DIRECTORY`, and `O_NOFOLLOW`; it opens the final file with `O_NOFOLLOW | O_NONBLOCK`. It hashes only a same-device regular file and compares metadata before and after hashing. An `EAGAIN` stability result is retried up to two times with a short delay. The new file row is compared to the current in-memory snapshot and classified as created, modified, deleted, metadata-changed, unchanged, or unknown. Successful state changes update the in-memory snapshot, not the SQLite database. Event evidence can include `before_sha256`, `after_sha256`, `error_errno`, the combined mask, count, and observation-time range.

The debounce is synchronous in the event loop. There is no worker pool or job queue, so there are no shared worker-owned rows or queue shutdown races; a slow file read or journal write can delay processing the next inotify batch. The pending tables remain bounded, but they do not make the kernel notification stream lossless.

## 6. Overflow recovery and periodic verification

An `IN_Q_OVERFLOW`, invalidated watch, unknown watch descriptor, watch/read/poll failure, rename-table exhaustion, or directory-tree change enters `rebuild_and_reconcile`. The function emits `coverage.recovering`, flushes pending rows, replaces the inotify instance and recursive watch table, scans the tree, and compares that scan with the last successful in-memory snapshot. Scan-derived differences include hashes and `reconciliation=true`. It emits `coverage.healthy` only when both watch registration and the full scan succeed; otherwise it records a degraded reason and retries after idle periods. The database baseline is not rewritten.

`verify_current_snapshot` performs a full current-tree scan at the configured interval, defaulting to one hour. It emits `periodic_verification` and `periodic_verification_complete` coverage events. A successful scan updates the in-memory snapshot after writing differences. A failed scan records a degraded coverage event and keeps the previous successful snapshot. The test option `--inject-overflow-once` exercises the recovery state machine, but it does not make the kernel produce a real `IN_Q_OVERFLOW`.

## 7. Ownership, shutdown, and failure review

- `scan_root` owns a row vector; `push_row` transfers each path allocation and `free_rows` releases it.
- `fdopendir` owns the duplicated directory FD. The caller closes the original directory FD and each `openat` child/file FD.
- SQLite statements are finalized after use. SQLite copies bound strings where `SQLITE_TRANSIENT` is used. Watch DB handles are opened read-only and closed on exit.
- The watcher owns the root, inotify, journal, and PID-file lifecycle; the watch-path strings; the current in-memory snapshot; and the bounded pending tables. The kernel keeps watch registrations until the inotify FD closes.
- The signal handler sets a `volatile sig_atomic_t` flag only. Ordinary control flow flushes pending observations, unlinks the PID file, frees row/path allocations, and closes FDs.

Relevant failures include permission denial, vanished files, symlink replacement, `EAGAIN` from a changing file, `EMFILE`, filesystem I/O failure, journal write failure, watch exhaustion, queue overflow, and process termination. A current-state scan cannot recreate intermediate writes or actor identity. The automated overflow case is synthetic; actual kernel queue pressure is still unverified.

## 8. Tests and remaining limits

`tests/smoke.sh` covers baseline creation and comparison, startup reconciliation hashes, event coalescing, hashed create/delete/metadata observations, interval parsing, periodic full verification, synthetic overflow recovery state transitions, paired/unmatched/expired moves, recursive directory-watch recovery, baseline immutability during watch, and shutdown. GCC Release, Clang Release, and ASan/UBSan CTest runs use each build's own executable.

The smoke test does not prove genuine kernel queue overflow behavior, watch-limit recovery, event-log ENOSPC behavior, mount-boundary handling, stable hashing under synchronized concurrent mutation, large-tree performance, or the Ubuntu/RockyOS matrix. See `docs/TEST_RESULTS.md` for the current evidence and open gates.
