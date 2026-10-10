# Requirements traceability (SRS-01)

Code presence is not full requirement verification. The recorded smoke test covers selected baseline and watcher flows; scenarios outside that test remain open. See `docs/TEST_RESULTS.md`.

| Requirement | Source / artifact | Verification | Status |
|---|---|---|---|
| FIM-FR-001 | `src/fimctl.c:canonical_root` | FIM-T-001 pending | IMPLEMENTED |
| FIM-FR-002 | `src/fimctl.c:scan_dir`, SQLite `files` | tests/smoke.sh (selected cases) | IMPLEMENTED |
| FIM-FR-003 | `scan_dir` | FIM-T-005 pending | IMPLEMENTED |
| FIM-FR-004 | `scan_dir`, `same_file_state` | FIM-T-006 pending | IMPLEMENTED |
| FIM-FR-005 | `watch_add`, `add_tree_watches`, `rebuild_and_reconcile` | `tests/smoke.sh`: new directory plus immediate child; moved-directory watch rebuild | IMPLEMENTED; broader race/failure cases pending |
| FIM-FR-006 | `cmd_watch`, `watch_add` | `tests/smoke.sh`: create/write/delete/move observations and invalidated-watch recovery | IN_PROGRESS; mask/fault matrix pending |
| FIM-FR-007 | `remember_rename`, `take_rename`, `expire_renames` | `tests/smoke.sh`: paired rename, unmatched move-in, move-out timeout | IMPLEMENTED |
| FIM-FR-008 | `cmd_watch`, `rebuild_and_reconcile`, `emit_reconciliation_diff` | `tests/smoke.sh`: `--inject-overflow-once` degraded→recovering→healthy sequence | IMPLEMENTED; genuine kernel queue-overflow gate pending |
| FIM-FR-009 | `pending_event_add`, `flush_pending_event`, `hash_worker_main`, ordered result commit, 256-entry/100ms coalescer | `tests/smoke.sh`: coalesced hashes/count/time range and 1,100-file burst; current worker event passes schema validation | IMPLEMENTED for bounded two-worker hashing (1,024 outstanding jobs); deterministic cap saturation remains unverified |
| FIM-FR-010 | `compare_rows`, `flush_pending_event`, `emit_reconciliation_diff` | `tests/smoke.sh`: modified, created, deleted, metadata-changed and before/after hashes | IN_PROGRESS; full verify rename inference and unknown/error matrix pending |
| FIM-FR-011 | scanner returns explicit errors | FIM-T-008 pending | IN_PROGRESS |
| FIM-FR-012 | NDJSON append in `emit_event` | `tests/smoke.sh`: `/dev/full` journal failure returns status 1 and removes PID file | IN_PROGRESS; real filesystem ENOSPC and partial-write matrix pending |
| FIM-FR-013 | CLI dispatch | tests/smoke.sh (selected commands) | IN_PROGRESS |
| FIM-FR-014 | `schema_meta` | FIM-T-009 pending | IMPLEMENTED |
| FIM-FR-015 | 1,000,000-row scan cap, 256-entry pending tables, two-worker/1,024-job hash pool, signal flag and cleanup | `tests/smoke.sh`: 1,100-file burst and SIGTERM through PID file; GCC ThreadSanitizer smoke repeated 3 times | IN_PROGRESS; deterministic cap-saturation, watch-resource and fault matrix pending |
| FIM-FR-016 | `rebuild_and_reconcile`, `emit_coverage_event` | `tests/smoke.sh`: startup and post-recovery healthy; degraded recovery paths | IN_PROGRESS; persistent failures/real overflow pending |
| FIM-FR-017 | fixed size cap only | FIM-T-005 pending | IN_PROGRESS |
| FIM-FR-018 | `systemd/fimd.service` | unit inspection pending | IMPLEMENTED |
| FIM-FR-019 | `verify --full`, `verify_current_snapshot`, interval parser (1..86,400 seconds; default 3,600) | `tests/smoke.sh`: periodic verification event pair and interval rejection | IMPLEMENTED; longer-duration/runtime matrix pending |
| FIM-FR-020 | — | not applicable to baseline | NOT_APPLICABLE |
| FIM-NFR-01..07 | `docs/TEST_PLAN.md`, `docs/TEST_RESULTS.md` | GCC/Clang Release, ASan/UBSan and ThreadSanitizer smoke; 1,100-file burst; `/dev/full` error exit; actual event schema; Clang analyzer clean | IN_PROGRESS; performance, platform, fault and full accuracy matrix pending |
