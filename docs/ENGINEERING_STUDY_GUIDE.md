# Engineering Study Guide — Linux File Integrity Monitor

## Learning objective and implementation status

This repository is an educational, read-only observer. It demonstrates how a baseline scan and a best-effort live event stream can complement each other. The engineering intent is to make the limits visible: a hash comparison can identify a current difference, while inotify cannot promise a complete history. Do not describe this code as prevention, actor attribution, or automatic recovery.

The Release build and CTest smoke case passed on the current Linux host. An earlier separate ASan/UBSan smoke run is recorded as passed. This verifies the selected paths in `docs/TEST_RESULTS.md`, not every SRS-01 MUST. Rename correlation, debounce/coalescing, automatic reconciliation, queue-overflow recovery, fault injection, full benchmark, and distribution matrix remain incomplete or unverified. Treat this guide as instruction for the checked-in baseline, with those gaps kept explicit.

## Why the subsystems exist

| Subsystem | Why it exists | Main implementation |
|---|---|---|
| Root validation | Keeps operations inside an explicit, narrow tree and rejects unsafe roots. | `canonical_root`, `cmd_init`, `cmd_watch` |
| Scanner and SHA-256 | Creates a reproducible snapshot of paths and file state. Directory-relative syscalls reduce path substitution races; before/after `fstat` detects mutation during hashing. | `scan_root`, `scan_dir`, `hash_fd`, `same_file_state` |
| SQLite baseline | Keeps metadata and digest rows in one transactional store with a schema marker. A transaction prevents a failed refresh from leaving a half-written table. | `db_open`, `store_rows`, `get_root` |
| Comparator and CLI | Turns a fresh scan into understandable created/modified/deleted/metadata-changed results. | `compare_rows`, `main` |
| inotify watcher | Exposes kernel filesystem notifications cheaply to an unprivileged process. It provides evidence for follow-up inspection, not a durable audit trail. | `watch_add`, `add_tree_watches`, `cmd_watch` |
| NDJSON journal and stop path | Gives downstream lab tools line-oriented records and a signal-based shutdown mechanism. | `emit_event`, `cmd_stop`, signal handler |

## Data and control flow

### Baseline and verification

1. `main` parses the subcommand; root-taking commands call `canonical_root`.
2. `scan_root` opens the root directory and obtains its device number.
3. `scan_dir` duplicates the directory FD for `fdopendir`, enumerates entries, checks each with `fstatat(..., AT_SYMLINK_NOFOLLOW)`, refuses symlinks and cross-device entries, and opens child directories/files relative to the current FD.
4. `hash_fd` reads bytes and feeds them to OpenSSL EVP SHA-256. `scan_dir` compares metadata before and after reading; an unstable file fails the scan instead of receiving a potentially mixed digest.
5. The rows are sorted by relative path. `store_rows` updates the root and file table in a SQLite transaction. `verify` instead reads the existing rows and `compare_rows` merges two sorted sequences to emit differences.

### Live observation

`cmd_watch` creates a nonblocking inotify FD, installs watches for the existing same-device directory tree, opens an append-only event file, and creates a PID file. It calls `poll` with a one-second timeout, reads a batch of `struct inotify_event` records, maps watch descriptor plus child name to a path, then calls `emit_event`. A newly created directory triggers a best-effort recursive watch registration. SIGINT/SIGTERM sets a flag; the loop then removes the PID file, frees watch paths, and closes the log and inotify FDs.

The current watcher reports `IN_Q_OVERFLOW` as degraded and asks the operator to run a full verify. It does not itself rescan or restore healthy coverage. `IN_MOVED_FROM` and `IN_MOVED_TO` expose the kernel cookie but are currently emitted separately; the code does not correlate them into a rename decision.

## Ownership, lifetimes, and concurrency

- The scanner owns each allocated relative path after `push_row` succeeds. `free_rows` releases the vector and row paths. A failed allocation or unstable read aborts the scan.
- `fdopendir` takes ownership of the duplicated FD; `closedir` closes it. The caller retains and closes the original directory FD. Each opened child/file FD is closed in its local scope on the normal path.
- SQLite statements are finalized after use; SQLite owns bound text because rows use `SQLITE_TRANSIENT`. DB handles are per CLI invocation and closed on completion/error paths.
- The watcher owns the inotify FD, event-log FD, PID-file lifecycle, and heap copies of watched paths. The kernel owns watch state until the inotify instance is closed.
- The scanner is single-threaded. The watcher has one event loop and no worker queue, so it avoids in-process data races but can fall behind if event processing or disk writes are slow.
- Signal state is a `volatile sig_atomic_t` flag. The signal handler avoids doing I/O; cleanup happens in ordinary control flow.

## Security assumptions and failure modes

Assumptions: the caller chooses a disposable root; the DB and journal are stored with user-only permissions; the same user is trusted not to alter them; the monitored filesystem supports the expected Linux semantics. Directory FDs, `O_NOFOLLOW`, and pre/post `fstat` reduce specific path and mutation races but do not create an immutable snapshot.

Important failure modes include permission denial, vanished entries, `EMFILE`, I/O errors, an event-log write failure, inotify watch loss, queue overflow, coalesced events, watch registration races, and process termination. A full scan can establish current state after many event losses, but it cannot reconstruct intermediate actions. File paths can reveal personal data. Inotify does not provide a trustworthy PID for the actor.

## Alternatives and tradeoffs

- **Path-based recursive traversal** is simpler but has more opportunities for symlink/path replacement races; this implementation prefers directory FDs and `*at` operations.
- **fanotify** can observe broader filesystem activity and supports permission events on supported setups, but needs different privileges/coverage assumptions and is not a drop-in inotify replacement.
- **FIM through audit/eBPF** can add actor context but needs privileged kernel integration, version-specific hooks, and additional loss/coverage analysis.
- **Filesystem snapshots** provide a more consistent point-in-time view but require filesystem-specific support and are outside this program's portable scope.
- **A worker queue plus debounce** can bound event-loop work and avoid hashing every intermediate write, but needs backpressure, ownership, shutdown, and lost-event semantics to be designed.

## Study route

1. Read `docs/ARCHITECTURE.md` and `docs/THREAT_MODEL.md`.
2. Trace `cmd_init` and `cmd_verify` through scanner → hash → DB/comparison.
3. Trace one `IN_CLOSE_WRITE` event through `cmd_watch` → `watch_path` → `emit_event`.
4. Use `docs/CODE_WALKTHROUGH.md` and `docs/LINUX_INTERNALS_EXPLAINED.md` while reading the source.
5. Attempt one exercise from `docs/DEBUGGING_EXERCISES.md`; no solution is included in the repository.

## Official references

- [`inotify(7)`](https://man7.org/linux/man-pages/man7/inotify.7.html), [`inotify_add_watch(2)`](https://man7.org/linux/man-pages/man2/inotify_add_watch.2.html), [`inotify_init1(2)`](https://man7.org/linux/man-pages/man2/inotify_init1.2.html)
- [`openat(2)`](https://man7.org/linux/man-pages/man2/openat.2.html), [`stat(2)`](https://man7.org/linux/man-pages/man2/stat.2.html), [`poll(2)`](https://man7.org/linux/man-pages/man2/poll.2.html), [`signal(7)`](https://man7.org/linux/man-pages/man7/signal.7.html)
- [SQLite Write-Ahead Logging](https://www.sqlite.org/wal.html), [SQLite Atomic Commit](https://www.sqlite.org/atomiccommit.html)
- [OpenSSL EVP digest API](https://docs.openssl.org/3.0/man3/EVP_DigestInit/)
