# Technical Defense — Linux File Integrity Monitor

No answers are included. Use the code, SRS, and executed evidence to defend each decision. Questions cover code reading, debugging, security reasoning, Linux interfaces, and design tradeoffs.

## Code reading and data flow

1. Trace `fimctl init` from CLI parsing to the first durable SQLite commit. Which validation happens before traversal?
2. Why does `scan_dir` use both `fstatat(..., AT_SYMLINK_NOFOLLOW)` and `openat(..., O_NOFOLLOW)`?
3. Which fields does `same_file_state` compare, and what can a pre/post `fstat` check still fail to prove?
4. Walk through ownership of a relative path from allocation in `scan_dir` to `free_rows`.
5. How does `compare_rows` distinguish a created path from a deleted path? Why is sorting useful?
6. Which fields are persisted for a baseline row, and which fields decide content versus metadata change?
7. Trace one inotify record from `read` through watch-descriptor lookup and event-journal serialization.
8. Why does `watch` open the SQLite database read-only, and how does it confirm the selected root matches the stored baseline?
9. Trace a coalesced write from the pending table to `scan_relative_file`, hash comparison, snapshot update, and NDJSON evidence.
10. What are the purposes of `before_sha256`, `after_sha256`, `error_errno`, `observation_count`, and the first/last observation times?

## Debugging and failure behavior

11. A file changes repeatedly during debounce. Which errno triggers a retry, how many retries occur, and what happens after they are exhausted?
12. A watched path is replaced with a FIFO. Which open flags prevent the monitor from blocking before it checks the file type?
13. Trace `IN_Q_OVERFLOW` through the coverage events, inotify FD replacement, full scan, and final state. Which part of the smoke evidence is synthetic?
14. A nested file is created immediately after its directory. Why does the monitor rebuild watches and scan the tree?
15. How does the periodic verifier detect current state after an inotify event was missed? What data does it compare and what does it leave unchanged?
16. What happens if the periodic scan fails? Which snapshot remains authoritative for the next attempt?
17. If `IN_MOVED_FROM` arrives without `IN_MOVED_TO`, what can be concluded, and how is the uncertainty represented?
18. How would you test an event-log `ENOSPC` failure without filling the host filesystem?

## Security and Linux semantics

19. What boundary does `canonical_root` establish? Which same-user or privileged attacker remains out of scope?
20. Why are `O_NOFOLLOW` and the root-device check useful, and why are neither a complete mount-namespace policy?
21. Explain why inotify is not a complete audit log and why actor PID attribution is absent.
22. Which event-journal fields could expose personal information? What would path redaction make harder to debug?
23. What does a successful SHA-256 comparison say about bytes, and what does it not authenticate?
24. Why is `CLOCK_MONOTONIC` appropriate for a quiet-window deadline while `CLOCK_REALTIME` is useful for journal timestamps?

## Design tradeoffs

25. Compare inotify, fanotify, and periodic scanning for this read-only lab goal.
26. What would a bounded hash worker queue improve? What backpressure, cancellation, ownership, shutdown, and stale-path problems would it introduce?
27. How could a filesystem snapshot change the consistency model? Which portability costs follow?
28. What evidence is required before claiming `coverage=healthy` after watch loss or queue overflow?
29. The smoke test passes. Which SRS-01 requirements remain incomplete, and what is the next highest-value verification gate?

## Defense checklist

For each answer, cite a function or test, state what was observed, name at least one residual risk, and distinguish implemented behavior from planned behavior. Automated test completion does not establish the owner's personal mastery.
