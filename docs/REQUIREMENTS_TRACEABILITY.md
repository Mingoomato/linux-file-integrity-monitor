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
| FIM-FR-008 | `cmd_watch`, `rebuild_and_reconcile`, `emit_reconciliation_diff` | `tests/smoke.sh`: `--inject-overflow-once`, coverage transitions and create/delete/metadata reconciliation | IMPLEMENTED; genuine kernel queue-overflow gate pending |
| FIM-FR-009 | `pending_event_add`, `flush_expired_events`, 256-entry/100ms coalescer | `tests/smoke.sh`: repeated writes produce one record with count and first/last timestamps | PARTIAL; baseline-aware hash debounce/worker queue pending |
| FIM-FR-010 | `compare_rows`, `emit_reconciliation_diff` | `tests/smoke.sh`: modified, created, deleted, metadata-changed; watcher rename-cookie path | IN_PROGRESS; full verify rename classification and unknown/error cases pending |
| FIM-FR-011 | scanner returns explicit errors | FIM-T-008 pending | IN_PROGRESS |
| FIM-FR-012 | NDJSON append in `emit_event` | FIM-T-008 pending | IN_PROGRESS |
| FIM-FR-013 | CLI dispatch | tests/smoke.sh (selected commands) | IN_PROGRESS |
| FIM-FR-014 | `schema_meta` | FIM-T-009 pending | IMPLEMENTED |
| FIM-FR-015 | 1,000,000-row scan cap, 256-entry rename table, signal flag and cleanup | `tests/smoke.sh`: SIGTERM through PID file; resource cap/fault matrix pending | IN_PROGRESS |
| FIM-FR-016 | `rebuild_and_reconcile`, `emit_coverage_event` | `tests/smoke.sh`: startup and post-recovery healthy; degraded recovery paths | IN_PROGRESS; persistent failures/real overflow pending |
| FIM-FR-017 | fixed size cap only | FIM-T-005 pending | IN_PROGRESS |
| FIM-FR-018 | `systemd/fimd.service` | unit inspection pending | IMPLEMENTED |
| FIM-FR-019 | `verify --full` scan path | FIM-T-010 pending | IN_PROGRESS |
| FIM-FR-020 | — | not applicable to baseline | NOT_APPLICABLE |
| FIM-NFR-01..07 | `docs/TEST_PLAN.md`, `docs/TEST_RESULTS.md` | GCC/Clang Release and ASan/UBSan smoke; Clang analyzer clean | IN_PROGRESS; performance, platform, fault and full accuracy matrix pending |
