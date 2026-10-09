# Linux File Integrity Monitor

`fimctl` stores a SHA-256 baseline for ordinary files beneath an explicitly selected lab directory and compares later scans with that baseline. Its `watch` command emits local NDJSON notifications from inotify. It is an educational, read-only observer, not a prevention or recovery product.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Dependencies: a C17 compiler, CMake, OpenSSL libcrypto, and SQLite3 development headers.

## Use

```sh
mkdir -p "$HOME/fim-lab"
./build/fimctl init --root "$HOME/fim-lab" --db ./state.sqlite
./build/fimctl verify --db ./state.sqlite --full
./build/fimctl watch --root "$HOME/fim-lab" --events ./events.ndjson
```

`init` refuses to replace an existing database. `scan` deliberately replaces the current baseline. `verify` returns 2 when it finds differences. Run `watch` in a separate terminal and stop it with Ctrl-C or `fimctl stop --pidfile ./fimd.pid`.

## Scope and limitations

Traversal refuses symlinks and skips filesystems mounted below the selected root during baseline scanning. Inotify does not identify the process that changed a file, can coalesce events, and can overflow. The live watcher is a notification stream; it does not yet reconcile automatically or update the baseline. A degraded event directs the operator to run `verify --full`. Export paths are local data and may contain personal information.

Supported target is a Linux filesystem and an unprivileged lab directory. No Ubuntu/Rocky runtime matrix has been executed. Run `ctest --test-dir build --output-on-failure`; the smoke test covers baseline creation/comparison, JSON path escaping, symlink exclusion, inotify event output, and monitor shutdown. It does not simulate queue overflow, mount boundaries, concurrent mutation, or filesystem errors; see `docs/TEST_RESULTS.md`.

## Safety

The program never modifies monitored files. It is not a complete audit log, does not attribute process actors, does not protect its database from a privileged attacker, and does not provide a point-in-time filesystem snapshot.
