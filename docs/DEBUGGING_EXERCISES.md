# Debugging and Independent Reproduction Exercises

These exercises are intentionally unsolved. Attempt one before asking for a walkthrough. No complete solution or patch is included here. Use a disposable directory under `/tmp`; do not point experiments at personal or system data.

## Exercise 1 — New nested directory coverage

**Goal:** Reproduce and diagnose the race/coverage behavior when a directory is created and files are written immediately beneath it.

**Starting points:** `cmd_watch`, the `IN_CREATE | IN_ISDIR` branch, `add_tree_watches`, and `tests/smoke.sh`.

**Tasks:**

1. Add a deterministic fixture that creates a nested directory and writes a child immediately.
2. Record which inotify events are emitted and whether a second-level child is observed.
3. Make watch-registration failure visible as degraded coverage without claiming the event stream is complete.
4. Add a regression assertion for the chosen behavior.

**Constraints:** Keep the watcher read-only. Do not follow symlinks or cross the root device. Do not silently convert a watch error into a healthy event.

**Acceptance:** The test is repeatable, and its output distinguishes a failed watch from a quiet directory.

## Exercise 2 — Pair a rename without inventing certainty

**Goal:** Extend event handling to correlate `IN_MOVED_FROM`/`IN_MOVED_TO` cookies within a bounded time.

**Starting points:** `struct inotify_event`, the cookie field in `emit_event`, the `cmd_watch` event loop, and the rename section of `inotify(7)`.

**Tasks:**

1. Design a bounded pending-move table keyed by cookie.
2. Decide how to represent a move out of scope, move into scope, a missing pair, and timeout.
3. Add tests for a paired rename, a missing peer, a cookie timeout, and a duplicate/zero cookie.
4. Ensure timeout means “unpaired observation,” not a fabricated rename.

**Constraints:** Bound memory and event delay. Preserve raw event evidence and keep actor attribution unknown.

**Acceptance:** Every emitted decision can be traced to original kernel masks and cookie; no unmatched event is mislabeled as a confirmed rename.

## Exercise 3 — Stable file hashing under concurrent mutation

**Goal:** Add a focused test for the `fstat` before/hash/after stability contract.

**Starting points:** `scan_dir`, `hash_fd`, `same_file_state`, and the unstable-file error path.

**Tasks:**

1. Create a large temporary file and mutate it while a scan runs.
2. Check whether the scan fails clearly, retries, or records a digest.
3. Add a test seam or deterministic synchronization point rather than relying only on timing luck.
4. Verify that a failed scan does not replace the committed baseline with partial rows.

**Constraints:** Do not add file writes to the monitor. The test harness may mutate only its own temporary fixture. Keep memory bounded.

**Acceptance:** A mutation during hashing cannot be reported as a successful stable digest; the baseline transaction remains all-or-nothing.

## Owner attempt record

Record the chosen exercise, a short design note, test evidence, and unresolved questions in the owner’s own notes. The repository does not infer completion from a build or a submitted patch. The owner’s reproduction status is currently **not yet evidenced**.
