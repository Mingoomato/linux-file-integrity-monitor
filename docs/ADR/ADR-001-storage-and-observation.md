# ADR-001: SQLite baseline and inotify observation

- Status: accepted for the initial implementation.
- Decision: use SQLite3 WAL for baseline metadata and OpenSSL EVP SHA-256 for file contents. Use inotify for unprivileged live notifications.
- Reason: both libraries are established system dependencies; inotify has a clear unprivileged lab workflow.
- Consequences: event loss and process attribution limitations remain explicit. This implementation does not yet provide rename correlation, automatic reconciliation, or atomic baseline migrations. A notification is not proof of complete audit coverage.
