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

## Exercise 2 — Coalesce event bursts while preserving evidence

**Goal:** Reduce redundant downstream work during rapid writes without losing the evidence needed to explain the observed burst.

**Starting points:** `cmd_watch`, `emit_event`, the `coverage` object in event.v1, and the `IN_CLOSE_WRITE`/`IN_ATTRIB` masks in `inotify(7)`.

**Tasks:**

1. Generate a burst of write and metadata events for one fixture file.
2. Define a bounded coalescing window and an output record that preserves first/last observation times and raw event count.
3. Add a deterministic test for a burst, a quiet period, and queue pressure.
4. Decide how to flush pending records during shutdown and how to report dropped/coalesced counts.

**Constraints:** Bound memory and event delay. Do not hash or modify monitored files. Keep actor attribution unknown and make dropped evidence visible.

**Acceptance:** Tests show a bounded summary of the burst, retain its time/count range, and distinguish coalescing from lost events.

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
