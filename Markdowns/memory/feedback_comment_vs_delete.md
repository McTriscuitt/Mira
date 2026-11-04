---
name: feedback-comment-vs-delete
description: Prefer commenting out code over deleting it when the feature may return
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 85b464db-588d-4e75-93ac-c42994360e8d
---

When removing a feature or disabling calls that may be re-enabled later (e.g. a removed hardware component like a ceiling bulb), comment out the lines rather than deleting them.

**Why:** User wants to keep the code available for quick re-activation without having to reconstruct it from history.

**How to apply:** If a user says "remove this" but the context suggests it's a temporary or hardware-driven removal (bulb gone, pin unused, etc.), default to commenting. If they want it truly gone, they'll say delete/remove explicitly.
