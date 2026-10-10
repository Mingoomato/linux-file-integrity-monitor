# Linux Internals Explained — Linux File Integrity Monitor

## Directory file descriptors and `*at` operations

The scanner treats an opened directory FD as the anchor for its children. `fstatat(dirfd, name, ..., AT_SYMLINK_NOFOLLOW)` inspects one entry relative to that anchor. `openat` opens a child relative to the same directory. This narrows races compared with repeatedly resolving an absolute pathname from the process working directory. `O_NOFOLLOW` rejects a symlink at the final component; it does not make every pathname race impossible, so the code also compares file state and restricts traversal.

The device-number comparison (`st_dev`) is a simple mount-crossing boundary. It may skip legitimate submount content and is not a complete mount-namespace policy. The project does not use `openat2(RESOLVE_BENEATH)`; that is a stronger Linux-specific alternative when kernel-enforced resolution constraints are needed.

## Hashing a changing file

`fstat` reports metadata for the opened object, while `read` consumes bytes from the file description. The code compares device, inode, size, mtime, ctime, mode, UID, and GID before and after reading. A difference is treated as an unstable read. For coalesced watch events, a short debounce lets a burst settle before reopening the current path. An `EAGAIN` stability result is retried up to two times with short sleeps; other failures become an explicit unknown observation with errno.

This catches many concurrent writes, but does not create a filesystem snapshot. Same-size rewrites, filesystem timestamp behavior, and changes after the final `fstat` can still limit what one observation proves. A snapshot or write-quiesce protocol is needed for stronger point-in-time consistency. SHA-256 fingerprints content; it does not identify an author or protect a stored digest from an attacker who can rewrite the DB.

## inotify lifecycle and event structure

`inotify_init1` returns an FD for an inotify instance. `inotify_add_watch` attaches a watch and returns a watch descriptor. Reading that FD returns a packed sequence of `struct inotify_event` records, each followed by a variable-length name. `poll` integrates the nonblocking FD with a timeout and shutdown checks.

Important semantics:

- Watches are not recursive; the application installs a watch for each directory and must handle newly created or moved directories.
- Rename notifications can arrive as `IN_MOVED_FROM` and `IN_MOVED_TO` with a shared cookie, but one side may be outside the watched tree or lost.
- Events may be coalesced; inotify is not a durable event log and does not provide a reliable actor PID.
- `IN_Q_OVERFLOW` means events were dropped. The watcher records degraded/recovering coverage, replaces the inotify instance and recursive watch set, then scans current state. That scan can identify the current difference but cannot reconstruct intermediate actions.
- A watch follows an inode, not a promise that the original pathname continues to refer to that object. Pathname-based watch registration still has races.
- A new or moved directory triggers a full watch rebuild and scan. This closes the gap where a child appears before its directory watch is installed, at the cost of rescanning the selected tree.

## Hash debounce and periodic verification

A 100 ms quiet window is measured with `CLOCK_MONOTONIC`; the event record's first/last observation times use `CLOCK_REALTIME` nanoseconds. The bounded coalescer preserves the OR-ed mask and raw signal count. When the quiet window expires, the event loop walks from a persistent root directory FD with `openat`, `O_DIRECTORY`, and `O_NOFOLLOW`, then opens the final file with `O_NOFOLLOW | O_NONBLOCK`. `O_NONBLOCK` prevents a path replaced by a FIFO from hanging the watcher before it checks the file type. The watcher hashes a same-device regular file and compares it with the current in-memory row. A baseline difference is reported with before/after SHA-256 values; a failed hash becomes `file.unknown` with `error_errno`.

The event loop submits copied events to two pthread workers through a mutex/condition-variable queue. At most 1,024 jobs may be queued, active, or awaiting commit. Each job has a monotonic sequence number. Workers hash independently, then insert completed results in sequence order; the main loop alone compares with and updates the in-memory snapshot and writes the journal. An `eventfd` notifies `poll` when work completes. If the bound is full, the event loop waits while committing completed jobs, which bounds memory but can delay reading inotify. A resulting kernel overflow is recovered by a full current-state scan. Worker threads are joined before shared root resources are closed.

The interval option accepts 1 to 86,400 seconds and defaults to 3,600 seconds. Periodic verification scans the complete tree and compares it with the last successful in-memory snapshot. It can notice current-state changes after a missed inotify event. It does not update the SQLite baseline or reconstruct the missed event sequence. A scan failure emits degraded coverage and keeps the previous snapshot.

## SQLite durability boundary

WAL allows readers and a writer to make progress with separate log and database files, but it does not make a filesystem scan atomic. Baseline replacement uses a transaction and `synchronous=FULL` for stronger commit durability. The watcher opens the baseline read-only and keeps its evolving snapshot in memory. A crash between filesystem state and a journal write may leave an incomplete observation; the next scan is a new current-state view, not a reconstruction of the crash interval.

## Resource and error semantics

FDs are process resources and must close on all exits. `fdopendir` takes ownership of its FD; `dup` keeps the directory FD available for `fstatat` and `openat`. `EINTR` may require retry for I/O calls; `EAGAIN` is expected when a nonblocking inotify FD has no queued event and can also signal a file changed during hashing. `ENOSPC` on the journal is evidence loss, not success. The watcher stops on journal write failure; an explicit aggregate lost-event counter is not implemented.

The row snapshot, watch-path strings, coalescer, and job objects are process-owned. The main loop owns the snapshot and journal; worker threads own only the hash operation and its result until it is placed on the completion list. Mutex/condition-variable synchronization protects linked queues, and the sequence number keeps snapshot updates ordered. Recovery drains outstanding jobs before replacing snapshot rows. `eventfd` is polled beside inotify. ThreadSanitizer passed the smoke test, though broader saturation and failure-injection coverage remains open.

## Official references

- [`inotify(7)`](https://man7.org/linux/man-pages/man7/inotify.7.html), [`inotify_add_watch(2)`](https://man7.org/linux/man-pages/man2/inotify_add_watch.2.html), [`inotify_init1(2)`](https://man7.org/linux/man-pages/man2/inotify_init1.2.html)
- [`openat(2)`](https://man7.org/linux/man-pages/man2/openat.2.html), [`openat2(2)`](https://man7.org/linux/man-pages/man2/openat2.2.html), [`stat(2)`](https://man7.org/linux/man-pages/man2/stat.2.html)
- [`poll(2)`](https://man7.org/linux/man-pages/man2/poll.2.html), [`eventfd(2)`](https://man7.org/linux/man-pages/man2/eventfd.2.html), [`read(2)`](https://man7.org/linux/man-pages/man2/read.2.html), [`fdopendir(3)`](https://man7.org/linux/man-pages/man3/fdopendir.3.html)
- [`pthreads(7)`](https://man7.org/linux/man-pages/man7/pthreads.7.html), [`pthread_cond_wait(3)`](https://man7.org/linux/man-pages/man3/pthread_cond_wait.3.html)
- [SQLite WAL](https://www.sqlite.org/wal.html), [SQLite atomic commit](https://www.sqlite.org/atomiccommit.html)
- [OpenSSL EVP digest API](https://docs.openssl.org/3.0/man3/EVP_DigestInit/)
