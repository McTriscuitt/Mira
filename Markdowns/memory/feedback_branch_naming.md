---
name: feedback-branch-naming
description: Git branches get descriptive kebab-case names that say what they're for (e.g. self-hosting-pi), never random session names like claude/happy-goodall-4zwhrm
metadata:
  type: feedback
---

Name every git branch for what it plans to do, so the branch list reads like a table of contents.

**Schema:** `<type>/<what-it-does>`, lowercase kebab-case, 2–4 words after the slash.

| type | for | example |
|---|---|---|
| `feature/` | new behavior in the firmware or site | `feature/nap-button` |
| `fix/` | a bug fix | `fix/zigbee-dropout` |
| `infra/` | hosting, deploys, CI, domains | `infra/self-hosting-pi` |
| `idea/` | notes or plans for something not built yet | `idea/ai-interactive-resume` |
| `docs/` | documentation only | `docs/dashboard-api` |

- One topic per branch. If a second idea comes up, give it its own branch rather than stacking it on an unrelated one.
- Name the branch for what it *will* hold. If its purpose changes, rename it (`git branch -m old new`, then push the new name and delete the old one from the remote).
- Cloud sessions (claude.ai/code) auto-assign names like `claude/happy-goodall-4zwhrm`. Rename to the schema before the first push. If the session can only push to its assigned branch, push there, tell the user the name it should have, and let them rename it.

**Why:** the user couldn't tell which branch held which idea. `claude/happy-goodall-4zwhrm` turned out to hold the AI resume idea, not the Pi self-hosting idea they expected (Nov 20, 2026). Descriptive names make navigation easier.

**How to apply:** whenever you create, push or rename a branch on this project. Before reusing an existing branch for new work, check that its contents match its name.
