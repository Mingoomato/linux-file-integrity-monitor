# Engineering Study Guide — Linux File Integrity Monitor

## Learning objective and implementation status

This repository is an educational, read-only observer. It demonstrates how a baseline scan and a best-effort live event stream can complement each other. The engineering intent is to make the limits visible: a hash comparison can identify a current difference, while inotify cannot promise a complete history. Do not describe this code as prevention, actor attribution, or automatic recovery.

GCC Release, Clang Release, and separate ASan/UBSan builds and CTest smoke runs passed on the current Linux host; Clang static analysis produced no diagnostics. The watcher keeps bounded pending-rename and event-coalescing tables, emits a matched rename when cookies pair, separately records unmatched moves, and coalesces other per-path signals during a 100 ms quiet window. Coalesced records preserve the OR-ed event mask, signal count, and first/last wall-clock nanosecond timestamps. It enters `recovering` after a queue overflow, invalidated watch, or directory-tree change; it rebuilds recursive watches, scans the current tree, emits reconciliation differences, and restores `healthy` only when both watch installation and scanning succeed. The smoke test exercises recovery through `--inject-overflow-once`; it does not force the kernel inotify queue to overflow. This verifies selected paths, not every SRS-01 MUST. Baseline-aware hash debounce, periodic verification, real overflow and resource fault injection, full benchmarking, and the Ubuntu/Rocky runtime matrix remain incomplete or unverified. Treat this guide as instruction for the checked-in implementation, with those gaps explicit.

## Why the subsystems exist

| Subsystem | Why it exists | Main implementation |
|---|---|---|
| Root validation | Keeps operations inside an explicit, narrow tree and rejects unsafe roots. | `canonical_root`, `cmd_init`, `cmd_watch` |
| Scanner and SHA-256 | Creates a reproducible snapshot of paths and file state. Directory-relative syscalls reduce path substitution races; before/after `fstat` detects mutation during hashing. | `scan_root`, `scan_dir`, `hash_fd`, `same_file_state` |
| SQLite baseline | Keeps metadata and digest rows in one transactional store with a schema marker. A transaction prevents a failed refresh from leaving a half-written table. | `db_open`, `store_rows`, `get_root` |
| Comparator and CLI | Turns a fresh scan into understandable created/modified/deleted/metadata-changed results. | `compare_rows`, `main` |
| inotify watcher | Exposes kernel filesystem notifications cheaply to an unprivileged process. It provides evidence for follow-up inspection, not a durable audit trail. | `watch_add`, `add_tree_watches`, `cmd_watch` |
| Signal coalescer | Bounds redundant per-path work during a short burst while preserving the combined mask, raw count, and time range. Rename records bypass coalescing so cookie correlation remains explicit. | `pending_event_add`, `flush_expired_events`, `pending_event_kind` |
| Coverage recovery | Detects overflow/watch invalidation, rebuilds watches, and compares current state with the last successful scan so a best-effort stream can return to a known current state. | `rebuild_and_reconcile`, `emit_reconciliation_diff`, `emit_coverage_event` |
| NDJSON journal and stop path | Gives downstream lab tools line-oriented records and a signal-based shutdown mechanism. | `emit_event`, `cmd_stop`, signal handler |

## Data and control flow

### Baseline and verification

1. `main` parses the subcommand; root-taking commands call `canonical_root`.
2. `scan_root` opens the root directory and obtains its device number.
3. `scan_dir` duplicates the directory FD for `fdopendir`, enumerates entries, checks each with `fstatat(..., AT_SYMLINK_NOFOLLOW)`, refuses symlinks and cross-device entries, and opens child directories/files relative to the current FD.
4. `hash_fd` reads bytes and feeds them to OpenSSL EVP SHA-256. `scan_dir` compares metadata before and after reading; an unstable file fails the scan instead of receiving a potentially mixed digest.
5. The rows are sorted by relative path. `store_rows` updates the root and file table in a SQLite transaction. `verify` instead reads the existing rows and `compare_rows` merges two sorted sequences to emit differences.

### Live observation

`cmd_watch` opens a local event journal and PID file, then `rebuild_and_reconcile` creates a nonblocking inotify FD, recursively watches same-device directories, and takes the initial scan snapshot. The event loop calls `poll` with a one-second timeout, reads a batch of variable-length `struct inotify_event` records, maps watch descriptor plus child name to a path, and writes an NDJSON observation. A new directory or moved directory triggers watch rebuilding and a full scan, which closes the race where a child file appears before its new directory watch is installed. `IN_MOVED_FROM` observations enter a fixed-capacity table keyed by kernel cookie. A matching `IN_MOVED_TO` emits `path.renamed`; an unmatched move-in emits `file.rename_match_failed`; a move-out still pending after two monotonic-clock seconds emits `file.rename_timeout`.

Non-rename file signals enter a fixed 256-entry coalescing table keyed by full path. Repeated signals within a 100 ms quiet window OR their event masks, increment `observation_count`, and update the last-observed time. An entry flushes when its quiet deadline expires, when the table fills (the oldest entry is flushed early), before recovery, or during shutdown. The record includes `first_observed_unix_ns` and `last_observed_unix_ns`; rename records bypass this table and keep their cookie-based flow.

`IN_Q_OVERFLOW`, `IN_IGNORED`, self-move/unmount signals, unknown watch descriptors, registration/read/poll errors, and rename-table exhaustion initiate recovery. `rebuild_and_reconcile` emits `coverage.recovering`, flushes pending coalesced records, replaces the inotify instance and watch table, scans current files, compares sorted rows to the prior successful snapshot, and emits reconciliation-tagged file differences. It emits `coverage.healthy` only if full watch registration and scan both succeed; otherwise it records `coverage.degraded` and retries after idle intervals. The snapshot is in memory and does not update the SQLite baseline. The explicit `--inject-overflow-once` option exercises the state machine for tests; it is not evidence of a real kernel queue overflow. Rename decisions are limited to cookies observed by this process before timeout; a lost side is never inferred from a pathname alone.

## Ownership, lifetimes, and concurrency

- The scanner owns each allocated relative path after `push_row` succeeds. `free_rows` releases the vector and row paths. A failed allocation or unstable read aborts the scan.
- `fdopendir` takes ownership of the duplicated FD; `closedir` closes it. The caller retains and closes the original directory FD. Each opened child/file FD is closed in its local scope on the normal path.
- SQLite statements are finalized after use; SQLite owns bound text because rows use `SQLITE_TRANSIENT`. DB handles are per CLI invocation and closed on completion/error paths.
- The watcher owns the inotify FD, event-log FD, PID-file lifecycle, heap copies of watched paths, a sorted in-memory file snapshot, a fixed 256-entry pending-rename table on its stack, and a heap-allocated 256-entry coalescing table. Pending entries contain bounded path buffers and monotonic/realtime timestamps; flush/removal and timeout determine their lifetimes. Reconciliation replaces snapshot row allocations only after a complete scan. The kernel owns watch state until the inotify instance is closed.
- The scanner is single-threaded. The watcher has one event loop and no worker queue, so it avoids in-process data races but can fall behind if event processing or disk writes are slow.
- Signal state is a `volatile sig_atomic_t` flag. The signal handler avoids doing I/O; cleanup happens in ordinary control flow.

## Security assumptions and failure modes

Assumptions: the caller chooses a disposable root; the DB and journal are stored with user-only permissions; the same user is trusted not to alter them; the monitored filesystem supports the expected Linux semantics. Directory FDs, `O_NOFOLLOW`, and pre/post `fstat` reduce specific path and mutation races but do not create an immutable snapshot.

Important failure modes include permission denial, vanished entries, `EMFILE`, I/O errors, an event-log write failure, inotify watch loss, queue overflow, kernel-side coalescing, watch registration races, pending-table pressure, and process termination. User-space coalescing reduces redundant records but cannot reconstruct intermediate actions; there is no baseline-aware hash debounce or worker queue yet. The overflow test is synthetic, so host kernel queue-limit behavior remains unverified. File paths can reveal personal data. Inotify does not provide a trustworthy PID for the actor.

## Alternatives and tradeoffs

- **Path-based recursive traversal** is simpler but has more opportunities for symlink/path replacement races; this implementation prefers directory FDs and `*at` operations.
- **fanotify** can observe broader filesystem activity and supports permission events on supported setups, but needs different privileges/coverage assumptions and is not a drop-in inotify replacement.
- **FIM through audit/eBPF** can add actor context but needs privileged kernel integration, version-specific hooks, and additional loss/coverage analysis.
- **Filesystem snapshots** provide a more consistent point-in-time view but require filesystem-specific support and are outside this program's portable scope.
- **Baseline-aware hash debounce and a worker queue** could compare settled file state instead of processing each coalesced observation synchronously. They need explicit backpressure, path ownership, shutdown, and lost-event semantics; this repository coalesces notifications but does not hash event paths against the baseline.

## Study route

1. Read `docs/ARCHITECTURE.md` and `docs/THREAT_MODEL.md`.
2. Trace `cmd_init` and `cmd_verify` through scanner → hash → DB/comparison.
3. Trace a paired move through raw `IN_MOVED_FROM`/`IN_MOVED_TO` records, `remember_rename`, `take_rename`, and the `path.renamed` journal event.
4. Use `docs/CODE_WALKTHROUGH.md` and `docs/LINUX_INTERNALS_EXPLAINED.md` while reading the source.
5. Attempt one exercise from `docs/DEBUGGING_EXERCISES.md`; no solution is included in the repository.

## Official references

- [`inotify(7)`](https://man7.org/linux/man-pages/man7/inotify.7.html), [`inotify_add_watch(2)`](https://man7.org/linux/man-pages/man2/inotify_add_watch.2.html), [`inotify_init1(2)`](https://man7.org/linux/man-pages/man2/inotify_init1.2.html)
- [`openat(2)`](https://man7.org/linux/man-pages/man2/openat.2.html), [`stat(2)`](https://man7.org/linux/man-pages/man2/stat.2.html), [`poll(2)`](https://man7.org/linux/man-pages/man2/poll.2.html), [`signal(7)`](https://man7.org/linux/man-pages/man7/signal.7.html)
- [SQLite Write-Ahead Logging](https://www.sqlite.org/wal.html), [SQLite Atomic Commit](https://www.sqlite.org/atomiccommit.html)
- [OpenSSL EVP digest API](https://docs.openssl.org/3.0/man3/EVP_DigestInit/)
