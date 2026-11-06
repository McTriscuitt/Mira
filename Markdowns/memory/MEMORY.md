# Memory Index


- [Terminal rework merged to mira-3](project_terminal_mira_next.md) — pushed Nov 3 (172cb6d); login worked on a phone, other live checks still open in Markdowns/TERMINAL_MIRA_BUILD_LOG.md
- [Mira 3.0 schedule rework](project_mira3_rework.md) — running since Oct 14; Nov 2 changes (new evening, false-hold filter, wall switches, NVS power-loss replay) flashed Nov 2; NAP + EXTEND flashed Nov 3, boot confirmed on the dashboard; wall switches + power loss confirmed by the user Nov 5; LIGHT_ON/LIGHT_OFF (terminal `dresser off`) + out-for-today/hold-level rework flashed Nov 5, untested; mira-3 merged into main Nov 5 (Railway branch switch pending); rest of the checklists in MIRA_3.md still open; board on COM4; native tests need CLion MinGW on PATH; MIRA_3.md is source of truth

- [Crash Investigation Sept 12](project_crash_investigation_sep12.md) — daily morning panics + WiFi wedges; watchdog + reset-reason flashed Sept 12, next crash self-identifies in the Online log (kept in 3.0, + "ntp-timeout"); dashboard SSE staleness caused a false-alarm restart
- [N1 Migration + N2 Progress](project_n1_migration.md) — 2.x history (behavior superseded by 3.0; N2 step 3 long-poll still a dashboard-side candidate). N1 DONE (all 5 stages flashed). N2 steps 1+2 SHIPPED July 24 (`3cee62a` burst polling, `2679fe9` Flask SSE stream; Procfile `-w 1 --threads 16`, `-w 1` load-bearing); live Railway verify pending; step 3 (netTask long-poll, reflash) open. Also open: Stage 5 final checks + cross-stage regression checklist; soak logger holds COM5 (kill before flashing); laptop can't reach bridge over HTTPS; app.py won't boot on SQLite
- [Dashboard Design Preferences](project_dashboard_design.md) — clean, minimalistic, dark mode, low clutter; lux curve tuner as style baseline
- [Wake Sequence Rework (April 24)](project_wake_rework_apr24.md) — 2.x history (3.0 replaced the wake): bri-to-bri interpolation, soft pause override fix, root cause of April 23 failures
- [No co-author in commits](feedback_no_coauthor.md) — never add Co-Authored-By Claude lines; plain commit messages
- [Logical fix over timer bump](feedback_prefer_logical_fix_over_timer_bump.md) — for race bugs, fix the bad check before widening a timeout; preserve SSE/architecture intent
- [Comment vs Delete](feedback_comment_vs_delete.md) — comment out hardware-driven removals, don't delete; keeps code ready for re-activation
- [Clarify complex design conversationally](feedback_clarify_complex_design_conversationally.md) — discuss open-ended/architectural decisions in prose before multiple-choice questions
- [Commit everything not gitignored](feedback_commit_everything_not_gitignored.md) — fold untracked docs/logs into pushes; don't leave them local
