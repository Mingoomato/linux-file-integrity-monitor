# Debugging and Independent Reproduction Exercises

These exercises are intentionally unsolved. Attempt one before asking for a walkthrough. No complete solution or patch is included here. Use a disposable directory under `/tmp`; do not point experiments at personal or system data.

## Exercise 1 — Watch resource failure and retry

**Goal:** Verify that partial recursive watch registration remains visibly degraded and can recover after the resource constraint is removed.

**Starting points:** `watch_add`, `add_tree_watches`, `rebuild_and_reconcile`, and `tests/smoke.sh`.

**Tasks:**

1. Add a test-only, bounded way to force a recursive watch-registration failure without changing host sysctls.
2. Assert that the journal records `coverage.degraded` and does not claim healthy coverage while a directory is unwatched.
3. Remove the injected constraint and demonstrate that a later retry rebuilds the watch tree and returns to healthy coverage.
4. Verify that a child file created during the degraded interval appears in the reconciliation report when observable.

**Constraints:** Keep the watcher read-only. Do not follow symlinks or cross the root device. Do not lower or modify host-wide inotify limits. Do not silently convert a watch error into a healthy event.

**Acceptance:** The test is repeatable, bounded, and distinguishes persistent watch failure from a quiet directory and successful recovery.

## Exercise 2 — Coalescer capacity and no-drop behavior

**Goal:** Verify bounded coalescer behavior when more distinct paths become pending than the table can hold.

**Starting points:** `pending_event_add`, `flush_pending_event`, `pending_poll_timeout`, and `tests/smoke.sh`.

**Tasks:**

1. Generate distinct file events for more than 256 paths before their quiet windows expire.
2. Verify that the oldest record flushes early, every observed path still appears, and its mask/count/time-range evidence remains valid.
3. Add a deterministic assertion that the table stays within its bound and no record is silently dropped.
4. Consider whether an explicit coalesced/early-flush marker would help operators distinguish capacity pressure from an ordinary quiet-window flush.

**Constraints:** Keep memory bounded. Do not fill the host filesystem or hash/modify unrelated files. Keep actor attribution unknown and make any loss visible.

**Acceptance:** Tests prove the 257th distinct pending path does not silently discard an earlier observation and that all evidence fields remain internally consistent.

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
