---
name: commit-everything-not-gitignored
description: "User wants most everything that isn't gitignored committed and pushed — don't leave docs/logs untracked"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 1c63303a-4316-4a72-8492-23e3d98becc6
---

When committing, include untracked files (Markdowns, bug-record logs, planning docs) rather than scoping commits to only the files just edited. The user said (June 11, 2026): "i would like most everything that isnt gitignored to be commited and pushed."

**Why:** Untracked docs left local create dead references (CLAUDE.md pointed at two Markdowns that weren't in the repo) and risk loss; the .gitignore already fences off what should stay local (secrets.h etc.).

**How to apply:** After making the requested commit, check `git status` for untracked files and fold them into the push — either in the same commit if related or a separate docs commit. Only leave something untracked if it's obviously transient or the user says so. Related: [[no-coauthor-in-commits]].
