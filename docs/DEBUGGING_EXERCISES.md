# Debugging and Independent Reproduction Exercises

These three exercises are intentionally unsolved. Each is a bounded implementation task that can be built and tested independently from the rest of the repository. Do not look for a complete solution in the docs; none is included. Use temporary fixtures under `/tmp`, never personal or system data.

## Exercise 1 — Stable descriptor hash

**Goal:** Reproduce the scanner's guarantee that a digest is associated with a file state that remained stable during the read.

**Starting points:** `hash_fd`, `same_file_state`, `scan_relative_file`.

**Tasks:**

1. In a small standalone C test helper or isolated exercise file, open one regular file and compute its SHA-256 through a file descriptor.
2. Record the file state before and after reading and return a distinct unstable result when the state changes.
3. Add one ordinary success case and one deterministic or synchronized mutation case using a file your test created itself.

**Constraints:** Do not follow a final symlink. Do not read the whole file into memory. Do not change files outside the test fixture.

**Acceptance:** A stable fixture produces the expected digest; a mutation is not silently reported as a stable digest; every opened FD and digest context is released on success and error paths.

## Exercise 2 — Sorted baseline diff

**Goal:** Independently implement the small merge operation used to compare sorted path rows.

**Starting points:** `struct file_row`, `row_path_compare`, `compare_rows`, `emit_reconciliation_diff`.

**Tasks:**

1. Create two small, sorted arrays of path/hash/metadata rows.
2. Classify paths present on only one side and paths with changed content or metadata.
3. Add fixtures for one create, one delete, one content change, one metadata-only change, and identical arrays.

**Constraints:** Keep the implementation linear in the number of rows after sorting. Do not treat a same-content path difference as proof of a rename.

**Acceptance:** The output is deterministic, every input row is accounted for once, and content changes are distinguishable from metadata-only changes.

## Exercise 3 — Bounded event coalescer

**Goal:** Reproduce the event table's bounded burst aggregation without implementing inotify itself.

**Starting points:** `struct event_batch`, `pending_event_add`, `pending_poll_timeout`, `flush_pending_event`.

**Tasks:**

1. Add signals for the same path and verify the aggregate retains the combined mask, observation count, and first/last timestamps.
2. Add a signal after the quiet window and verify it starts a new aggregate.
3. Fill a small fixed-capacity table and define how an incoming distinct path is handled without silently dropping prior evidence.

**Constraints:** Measure quiet time monotonically. Keep memory bounded and preserve an explicit outcome for capacity pressure.

**Acceptance:** Tests cover repeated path, expired quiet window, capacity pressure, and cleanup; no aggregate exceeds the configured table bound.

## Owner attempt record

After attempting an exercise, record the exercise number, your design, code/test evidence, and unresolved questions in your own notes. Repository implementation status and owner reproduction status are tracked separately. Owner reproduction is currently **not yet evidenced**; no mastery claim is made.
