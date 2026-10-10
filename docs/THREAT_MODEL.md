# Threat model

## Protected assets

The baseline database and local event journal provide evidence about a chosen lab tree. The tool does not make that evidence tamper-proof.

## Trust boundaries and controls

- The supplied root is untrusted input: it must be absolute, resolve to a real directory, and cannot be a broad system root.
- Entries are inspected without following symlinks. Baseline hashing opens a file without following a final symlink and compares `fstat` state before and after reading.
- Traversal does not cross the root device. Special files and files above the configured 512 MiB cap are omitted.
- A file changed during hashing causes the scan to fail instead of recording a potentially inconsistent digest.
- SQLite uses transactions and WAL. The database and event log should be stored in a user-private directory.
- Queue overflow, watch invalidation, and directory-tree changes trigger a full watch rebuild and current-state scan. The event journal distinguishes reconciliation-derived differences and remains `degraded` if watch registration or scanning fails.

## Residual risks

Inotify can lose or kernel-coalesce events; no actor PID is available; watcher registration has races; and a full scan cannot recover intermediate actions. User-space coalescing is bounded to 256 paths and records raw count/time range. A settled event is hashed synchronously and compared with an in-memory baseline snapshot; this can identify current content and metadata differences but does not reconstruct each write in the burst. The periodic scan is implemented and tested at a short interval, but real queue pressure, watch-limit failures, periodic behavior on a second distribution, and synchronized concurrent-mutation fault cases remain outside verified coverage. The automated overflow case injects the recovery trigger rather than forcing the kernel queue to overflow. A dedicated hash worker queue, database replacement by the same user or root, filesystem snapshot consistency, and path redaction are also outside verified coverage. Coverage indicates the latest completed watch-and-scan state; it is not a guarantee of complete historical monitoring.
