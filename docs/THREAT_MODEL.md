# Threat model

## Protected assets

The baseline database and local event journal provide evidence about a chosen lab tree. The tool does not make that evidence tamper-proof.

## Trust boundaries and controls

- The supplied root is untrusted input: it must be absolute, resolve to a real directory, and cannot be a broad system root.
- Entries are inspected without following symlinks. Baseline hashing opens a file without following a final symlink and compares `fstat` state before and after reading.
- Traversal does not cross the root device. Special files and files above the configured 512 MiB cap are omitted.
- A file changed during hashing causes the scan to fail instead of recording a potentially inconsistent digest.
- SQLite uses transactions and WAL. The database and event log should be stored in a user-private directory.

## Residual risks

Inotify can lose/coalesce events; no actor PID is available; watcher registration has races; the current watcher does not reconcile automatically; database replacement by the same user or root is not prevented; filesystem snapshot consistency is not provided; JSON path redaction and fully escaped export are not implemented. Coverage must not be interpreted as a guarantee of complete historical monitoring.
