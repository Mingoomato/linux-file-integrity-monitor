# Requirements traceability (SRS-01)

Code presence is not full requirement verification. The recorded smoke test covers selected baseline and watcher flows; scenarios outside that test remain open. See `docs/TEST_RESULTS.md`.

| Requirement | Source / artifact | Verification | Status |
|---|---|---|---|
| FIM-FR-001 | `src/fimctl.c:canonical_root` | FIM-T-001 pending | IMPLEMENTED |
| FIM-FR-002 | `src/fimctl.c:scan_dir`, SQLite `files` | tests/smoke.sh (selected cases) | IMPLEMENTED |
| FIM-FR-003 | `scan_dir` | FIM-T-005 pending | IMPLEMENTED |
| FIM-FR-004 | `scan_dir`, `same_file_state` | FIM-T-006 pending | IMPLEMENTED |
| FIM-FR-005 | `watch_add`, `add_tree_watches` | FIM-T-004 pending | IN_PROGRESS |
| FIM-FR-006 | `cmd_watch` | FIM-T-002 pending | IN_PROGRESS |
| FIM-FR-007 | `cmd_watch` cookie field | FIM-T-003 pending | IN_PROGRESS |
| FIM-FR-008 | `cmd_watch` overflow event | FIM-T-007 pending | IN_PROGRESS |
| FIM-FR-009 | — | FIM-T-002 pending | NOT_STARTED |
| FIM-FR-010 | `compare_rows` | FIM-T-002 pending | IMPLEMENTED |
| FIM-FR-011 | scanner returns explicit errors | FIM-T-008 pending | IN_PROGRESS |
| FIM-FR-012 | NDJSON append in `emit_event` | FIM-T-008 pending | IN_PROGRESS |
| FIM-FR-013 | CLI dispatch | tests/smoke.sh (selected commands) | IN_PROGRESS |
| FIM-FR-014 | `schema_meta` | FIM-T-009 pending | IMPLEMENTED |
| FIM-FR-015 | bounded scan rows, SIGTERM handler | FIM-T-010 pending | IN_PROGRESS |
| FIM-FR-016 | — | FIM-T-004 pending | NOT_STARTED |
| FIM-FR-017 | fixed size cap only | FIM-T-005 pending | IN_PROGRESS |
| FIM-FR-018 | `systemd/fimd.service` | unit inspection pending | IMPLEMENTED |
| FIM-FR-019 | `verify --full` scan path | FIM-T-010 pending | IN_PROGRESS |
| FIM-FR-020 | — | not applicable to baseline | NOT_APPLICABLE |
| FIM-NFR-01..07 | `docs/TEST_PLAN.md` | not run | NOT_STARTED |
