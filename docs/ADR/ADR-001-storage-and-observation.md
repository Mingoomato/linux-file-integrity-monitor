# ADR-001: SQLite baseline and inotify observation

- Status: accepted for the initial implementation.
- Decision: use SQLite3 WAL for baseline metadata and OpenSSL EVP SHA-256 for file contents. Use inotify for unprivileged live notifications.
- Reason: both libraries are established system dependencies; inotify has a clear unprivileged lab workflow.
- Consequences: bounded cookie and per-path coalescing tables preserve rename pairs and event count/time ranges. Overflow/watch invalidation triggers a recursive watch rebuild plus an in-memory full-scan reconciliation. These steps recover current state, not lost history. Baseline-aware hash debounce, periodic scans, actor attribution, and atomic schema migrations are not provided. Kernel queue overflow and resource exhaustion still require real-kernel tests. A notification is not proof of complete audit coverage.
