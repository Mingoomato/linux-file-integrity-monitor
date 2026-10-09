# Technical Defense — Linux File Integrity Monitor

No answers are included. Use the code and executed evidence to defend each decision. Questions span code reading, debugging, security reasoning, Linux behavior, and alternatives.

## Code reading and data flow

1. Trace `fimctl init` from CLI parsing to the first durable DB commit. Which validation happens before traversal?
2. Why does `scan_dir` use both `fstatat(..., AT_SYMLINK_NOFOLLOW)` and `openat(..., O_NOFOLLOW)`?
3. What does the pre/post `fstat` comparison prove, and what can it not prove about a concurrently changing file?
4. Walk through the ownership of a relative path from allocation in `scan_dir` to `free_rows`.
5. How does `compare_rows` distinguish a created file from a deleted file? Why is sorting useful?
6. Which fields are persisted for each baseline row, and which ones determine a content modification?
7. Trace one inotify record from `read` through watch-descriptor lookup to an NDJSON line.

## Debugging and failure behavior

8. A file appears in the tree but `verify --full` aborts with an unstable-file error. What code path could cause it, and what evidence would you collect?
9. The watcher prints a degraded warning after `IN_Q_OVERFLOW`. What recovery does the current code perform automatically, and what must the operator do?
10. A nested file created immediately after its directory is sometimes missed. Which registration race and failure paths would you inspect?
11. How would you test an `ENOSPC` failure while appending events without filling the host filesystem?
12. If `IN_MOVED_FROM` arrives without `IN_MOVED_TO`, what can be concluded? How should the record represent uncertainty?

## Security and Linux semantics

13. What security boundary does the canonical root check establish? Which same-user or privileged attacker remains out of scope?
14. Why are `O_NOFOLLOW` and a root-device check useful, and why are neither a complete mount-namespace policy?
15. Explain why inotify events are not a complete audit log and why actor PID attribution is absent.
16. Which data in the event journal could be personal information, and what would path pseudonymization change for debugging?
17. What does a successful SHA-256 comparison say about file contents, and what does it not authenticate?

## Design tradeoffs

18. Compare inotify, fanotify, and periodic scanning for this repository’s stated read-only goal.
19. What would a bounded worker queue and hash debounce improve? What new failure modes would they introduce?
20. How could a filesystem snapshot change the consistency model? Which portability cost would follow?
21. What tests are required before claiming `coverage=healthy` after a watch loss or queue overflow?
22. The smoke test passes. Which SRS-01 requirements remain incomplete or unverified, and what is the next highest-value gate?

## Defense checklist

For each answer, point to the function or test, state the observed evidence, identify at least one residual risk, and distinguish implemented behavior from planned behavior. Automated test completion does not establish the owner’s personal mastery.
