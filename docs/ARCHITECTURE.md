# Architecture

`fimctl` has a small single-process design:

1. The CLI canonicalizes an existing absolute root and rejects broad system roots.
2. The scanner walks from an opened directory descriptor, uses `fstatat(..., AT_SYMLINK_NOFOLLOW)`, rejects symlinks and mount crossings, opens regular files with `O_NOFOLLOW`, hashes through OpenSSL EVP, and compares pre/post `fstat` state.
3. SQLite stores schema version, root identity, file metadata, and SHA-256. Writes use a transaction, WAL, and `synchronous=FULL`.
4. `verify` compares a fresh sorted scan against the stored baseline. `watch` registers inotify watches and appends event.v1-shaped NDJSON.

The scanner uses directory descriptors to reduce path races. The inotify backend currently registers pathnames and is less race-resistant. Rename cookies are correlated in a fixed 256-entry pending table; unmatched move-in and timed-out move-out observations are recorded separately. The monitor does not yet implement event coalescing, bounded hash workers, periodic reconciliation, or reliable coverage recovery. Inotify was selected for unprivileged event observation; fanotify is not a drop-in equivalent and is not implemented.
