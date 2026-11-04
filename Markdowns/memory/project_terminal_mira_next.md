---
name: project-terminal-mira-next
description: "Terminal and dashboard rework merged into mira-3 and pushed Nov 3, 2026 (172cb6d); live checks still open in the build log's morning checklist, starting at step 2"
metadata:
  type: project
---

The terminal login, Mira's command tree and the old dashboard move were merged into `mira-3` and pushed on Nov 3, 2026 (merge commit `172cb6d`, previous tip on GitHub `9562c77`). Next: steps 2 onward of the morning checklist at the top of `Markdowns/TERMINAL_MIRA_BUILD_LOG.md`, which also lists every liberty taken.

**Why:** the overnight build's liberties were granted for that build only; the user asked for the merge and push themselves.

**How to apply:** for later design decisions go back to asking (see [[feedback_clarify_complex_design_conversationally]]). Don't assume the deploy succeeded until the user confirms. Still unproven: Railway's client IP header, the origin check, streams through gunicorn, the psycopg2 driver, and `NAP`/`EXTEND` on the real lights.
