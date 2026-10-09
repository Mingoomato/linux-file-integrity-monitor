# Linux Internals Explained — Linux File Integrity Monitor

## Directory file descriptors and `*at` operations

The scanner treats an opened directory FD as the anchor for its children. `fstatat(dirfd, name, ..., AT_SYMLINK_NOFOLLOW)` inspects one entry relative to that anchor. `openat` then opens the child relative to the same directory. This narrows races compared with repeatedly resolving a full path from the process working directory. `O_NOFOLLOW` rejects a symlink at the final component; it does not make every pathname race impossible, so the code still compares file state and restricts traversal.

The device-number comparison (`st_dev`) is a simple mount-crossing boundary. It can skip legitimate submount content, and it is not a complete mount-namespace policy. The project does not use `openat2(RESOLVE_BENEATH)`; that is a stronger Linux-specific alternative when an application needs kernel-enforced path-resolution constraints.

## Hashing a changing file

`fstat` reports metadata for the opened object, while `read` consumes bytes from the file description. Comparing metadata before and after hashing catches many concurrent changes, but not every possible same-size/same-timestamp rewrite on filesystems with coarse timestamp resolution. A filesystem snapshot or write-quiesce protocol is needed for a stronger point-in-time guarantee. SHA-256 gives a content fingerprint; it does not authenticate who created the file or protect the stored digest from an attacker who can rewrite the DB.

## inotify lifecycle and event structure

`inotify_init1` returns a file descriptor for an inotify instance. `inotify_add_watch` attaches a watch and returns a watch descriptor. Reading that FD returns a packed sequence of `struct inotify_event` records, each followed by a variable-length name. `poll` allows this nonblocking FD to be integrated with a timeout and shutdown checks.

Important semantics:

- Watches are not recursive; the application must add one for each directory and for newly created directories.
- Rename events can arrive as `IN_MOVED_FROM` and `IN_MOVED_TO` with a shared cookie, but a pair may be outside the watched tree or one side may be lost.
- Events can be coalesced; inotify is not a durable event log and does not report a reliable actor PID.
- `IN_Q_OVERFLOW` means events were dropped. The only safe interpretation is degraded historical coverage followed by a fresh state reconciliation.
- Watching pathnames creates race windows when directories are renamed/replaced. An inotify watch follows the watched inode, not a promise that the pathname remains the same object.

This repository implements event emission and an overflow warning. Automatic full reconciliation and coverage restoration are not yet implemented.

## SQLite durability boundary

WAL allows readers and a writer to make progress with separate log and database files, but it does not make a multi-file filesystem scan atomic. The project uses a DB transaction for replacing baseline rows and `synchronous=FULL` for stronger commit durability. A crash can still occur between the filesystem scan and transaction; the next full scan is a new observation, not a reconstruction of the crash interval.

## Resource and error semantics

FDs are process resources and must be closed on all exits. `fdopendir` takes ownership of its FD; `dup` is used to avoid closing the directory FD needed by `fstatat`/`openat`. `EINTR` may require retry for I/O calls; `EAGAIN` is expected when reading the nonblocking inotify FD with no ready event. `ENOSPC` on the event journal must be treated as evidence loss, not success. The current watcher exits its loop on journal write failure, but its exit status and degraded-state reporting deserve further hardening.

## Official references

- [`inotify(7)`](https://man7.org/linux/man-pages/man7/inotify.7.html) and [`inotify_add_watch(2)`](https://man7.org/linux/man-pages/man2/inotify_add_watch.2.html)
- [`openat(2)`](https://man7.org/linux/man-pages/man2/openat.2.html), [`stat(2)`](https://man7.org/linux/man-pages/man2/stat.2.html), [`poll(2)`](https://man7.org/linux/man-pages/man2/poll.2.html), [`read(2)`](https://man7.org/linux/man-pages/man2/read.2.html)
- [Linux `openat2(2)`](https://man7.org/linux/man-pages/man2/openat2.2.html) for a stronger resolution API
- [SQLite WAL](https://www.sqlite.org/wal.html) and [atomic commit](https://www.sqlite.org/atomiccommit.html)
- [OpenSSL EVP digest API](https://docs.openssl.org/3.0/man3/EVP_DigestInit/)
