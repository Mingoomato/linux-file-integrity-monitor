# Engineering Study Guide — Linux File Integrity Monitor

## Learning objective and project intent

This repository is an educational Linux systems project. It was built to make the difference between a persistent file-state baseline and a best-effort live notification stream concrete. The code demonstrates safe-ish descriptor-relative traversal, content hashing, SQLite persistence, inotify event handling, bounded coalescing, current-state reconciliation, and explicit coverage states. It does not prevent changes, attribute an event to a process, restore files, or promise a complete audit history.

Selected GCC Release, Clang Release, and Clang ASan/UBSan builds and CTest runs passed on the current Linux host. Clang static analysis also passed without diagnostics. These results cover selected cases; the full SRS still has open verification gates. Owner study and independent reproduction have not been observed, and an automated green build is not evidence of personal mastery.

## Why each subsystem exists

| Subsystem | Why it exists | Main code |
|---|---|---|
| Root validation | Limits work to an explicit, canonical lab directory. | `canonical_root`, `cmd_init`, `cmd_watch` |
| Scanner and SHA-256 | Captures the current state of ordinary files and detects many mutations during a read. | `scan_root`, `scan_dir`, `hash_fd`, `same_file_state` |
| SQLite baseline | Persists one accepted snapshot transactionally for later comparison; the watcher validates and reads it without mutation. | `db_open`, `store_rows`, `db_open_readonly`, `load_rows` |
| Full comparator | Reports current created, deleted, modified, and metadata-changed paths. | `compare_rows`, `cmd_verify` |
| inotify watcher | Supplies low-cost local signals about path changes; it is not a durable audit log. | `watch_add`, `add_tree_watches`, `cmd_watch` |
| Rename correlator | Preserves kernel rename-cookie evidence and avoids guessing when a pair is incomplete. | `remember_rename`, `take_rename`, `expire_renames` |
| Event coalescer and hash debounce | Groups a burst, then hashes the settled file and compares it with the current in-memory snapshot. | `pending_event_add`, `flush_pending_event`, `scan_relative_file` |
| Coverage recovery | Rebuilds recursive watches and compares a full current scan after a coverage gap. | `rebuild_and_reconcile`, `emit_reconciliation_diff` |
| Periodic verification | Detects current-state differences even if an event was lost, using a configurable full scan. | `verify_current_snapshot`, `parse_verify_interval` |
| NDJSON journal and shutdown | Makes observations inspectable and provides controlled SIGTERM cleanup. | `emit_event_full`, `cmd_stop`, signal handler |

## How data and control flow

### Baseline creation and full verification

1. `main` parses a command and validates the root where required.
2. `scan_root` anchors traversal at an open root directory FD and records the root device.
3. `scan_dir` enumerates with `fdopendir`, checks entries with `fstatat(..., AT_SYMLINK_NOFOLLOW)`, and opens children using `openat` with `O_NOFOLLOW`.
4. `hash_fd` reads file bytes through OpenSSL EVP SHA-256. `same_file_state` compares device, inode, size, mtime, ctime, mode, UID, and GID before and after hashing.
5. A complete row set is sorted and stored in SQLite inside one transaction. `verify` instead compares a fresh scan with the stored rows using a sorted merge.

### Watch startup and normal events

`watch` requires a DB whose canonical root matches `--root`. It opens this DB read-only, validates ordered row fields and relative paths, and loads them into an in-memory snapshot. It registers recursive watches and scans the tree. The startup scan is compared with the baseline so changes since the last baseline refresh are recorded with before/after digests.

For each inotify batch, the loop maps `wd` plus optional child name to a path. Rename events are correlated by cookie. Other events enter a bounded 256-entry path table. The watcher coalesces masks, increments signal count, records first/last wall-clock time, and waits for a 100 ms quiet window measured with `CLOCK_MONOTONIC`.

After the quiet window, `flush_pending_event` derives a relative path and calls `scan_relative_file`. That helper walks components relative to the already-open root FD, refuses symlinks, uses `O_NONBLOCK` on the final open, hashes a same-device regular file, and rechecks metadata. A changing file is retried up to two times after short delays. The result is compared with the in-memory snapshot and classified. Successful results update that snapshot; the SQLite baseline remains unchanged. Failures become `file.unknown` with an error reason and errno.

### Coverage recovery and periodic verification

On `IN_Q_OVERFLOW`, invalidated watches, directory-tree changes, registration/read/poll errors, or rename-table exhaustion, the watcher emits `coverage.recovering`, rebuilds the inotify instance and recursive watch table, scans current state, and emits differences against the last successful snapshot. `coverage.healthy` means the latest complete watch installation and scan succeeded; it says nothing about reconstructing past events.

`verify_current_snapshot` performs the same current-state comparison at a configured interval. The default is 3,600 seconds; the accepted range is 1 through 86,400. It does not rewrite SQLite. A failed scan emits a degraded event and preserves the previous successful snapshot. The test flag `--inject-overflow-once` exercises the recovery state machine without making the kernel generate `IN_Q_OVERFLOW`.

## Linux interfaces and resource lifetimes

- `open`, `openat`, `O_DIRECTORY`, `O_NOFOLLOW`, `O_NONBLOCK`: open the root, directories, and final files without following symlinks; directory-relative opens reduce path substitution risk.
- `fstat`, `fstatat`, `AT_SYMLINK_NOFOLLOW`: inspect opened objects and directory entries.
- `read`, `poll`, `inotify_init1`, `inotify_add_watch`: consume variable-length filesystem event records without a busy loop.
- `clock_gettime(CLOCK_MONOTONIC)`: measures quiet/timeout intervals. `CLOCK_REALTIME` timestamps are retained for log correlation.
- `fdopendir` owns the duplicated FD; the traversal retains and closes the original directory FD. Each open child/file FD is closed on its local path.
- `EVP_MD_CTX` is allocated per hash and freed before returning. SQLite statements are finalized; SQLite handles are closed after each CLI operation.
- Row vectors own their path strings after `push_row`; `free_rows` releases both. Watch path strings and snapshots are process-owned. The kernel owns watch registrations until the inotify FD closes.
- The signal handler only changes a `volatile sig_atomic_t` flag. Normal loop cleanup flushes pending events, removes the PID file, frees allocations, and closes FDs.

## Memory, concurrency, and security assumptions

The scan and event loop are single-threaded. Hashing after debounce happens synchronously; there is no worker pool or job queue. This avoids shared-memory races but allows slow storage, large files, or journal writes to delay inotify reads. The coalescer and rename table are bounded, and rows have a configured maximum; the full resource-exhaustion matrix has not been tested.

Assumptions: the owner chooses a disposable root; the DB and journal are kept in a private directory; the same user and root are trusted not to rewrite them; and the filesystem behaves according to expected Linux semantics. `O_NOFOLLOW`, root-device checks, and pre/post metadata checks reduce specific risks but do not give an immutable point-in-time snapshot. Inotify can coalesce or lose signals, may not provide actor PID, and cannot recover intermediate changes from a later full scan. Logged paths may contain personal information.

Important failure modes include permission denial, vanished paths, `EAGAIN` during hash stability checks, `EMFILE`, filesystem I/O failure, a full event journal, inotify watch exhaustion, queue overflow, and abrupt termination. The genuine kernel overflow path, persistent watch-resource failure, disk-full behavior, synchronized mutation tests, mount-boundary behavior, large-tree performance, and a second-distribution run remain unverified.

## Alternative approaches

- Path-based traversal is simpler, but directory FDs plus `*at` calls reduce repeated global path resolution. Linux `openat2` with `RESOLVE_BENEATH` is a stronger option where available.
- `fanotify` can observe broader filesystem activity and permission events, but changes privilege and coverage assumptions; it is not a drop-in inotify replacement.
- Audit or eBPF instrumentation can add actor context but brings privilege, kernel-version, and event-loss concerns.
- A worker queue could keep hashing away from the event loop. It would require explicit backpressure, path ownership, cancellation, shutdown, and consistency rules; it is not implemented here.
- Filesystem snapshots can improve point-in-time consistency but are filesystem-specific and cannot reconstruct all process activity.
- Remote or append-only journaling can improve tamper resistance, but introduces transport, key, and availability requirements outside this lab.

## Study route

1. Read `docs/ARCHITECTURE.md` and `docs/THREAT_MODEL.md`.
2. Trace `cmd_init` through scan, hash, and SQLite commit.
3. Trace `cmd_watch` startup from read-only DB rows to the first reconciliation event.
4. Follow one write burst through coalescing, `scan_relative_file`, snapshot update, and NDJSON evidence.
5. Compare synthetic overflow recovery with periodic verification.
6. Attempt one unsolved exercise in `docs/DEBUGGING_EXERCISES.md` and record your own test evidence.

## Official documentation

- [`inotify(7)`](https://man7.org/linux/man-pages/man7/inotify.7.html), [`inotify_add_watch(2)`](https://man7.org/linux/man-pages/man2/inotify_add_watch.2.html), [`inotify_init1(2)`](https://man7.org/linux/man-pages/man2/inotify_init1.2.html)
- [`openat(2)`](https://man7.org/linux/man-pages/man2/openat.2.html), [`openat2(2)`](https://man7.org/linux/man-pages/man2/openat2.2.html), [`stat(2)`](https://man7.org/linux/man-pages/man2/stat.2.html), [`poll(2)`](https://man7.org/linux/man-pages/man2/poll.2.html)
- [`read(2)`](https://man7.org/linux/man-pages/man2/read.2.html), [`fdopendir(3)`](https://man7.org/linux/man-pages/man3/fdopendir.3.html), [`signal(7)`](https://man7.org/linux/man-pages/man7/signal.7.html)
- [SQLite Write-Ahead Logging](https://www.sqlite.org/wal.html), [SQLite Atomic Commit](https://www.sqlite.org/atomiccommit.html)
- [OpenSSL EVP digest API](https://docs.openssl.org/3.0/man3/EVP_DigestInit/)
