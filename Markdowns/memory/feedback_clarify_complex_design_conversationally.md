---
name: feedback_clarify_complex_design_conversationally
description: "For open-ended/novel design decisions, discuss conversationally before forcing multiple-choice questions"
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 373cb53f-9717-4e17-87a8-1231fe5040d9
---

For **concrete** decisions with a known option set (e.g. "chest cut at 250 or 300?", "remove which log line?"), the user answers `AskUserQuestion` multiple-choice fine. But for **open-ended or novel design** (architecture, new feature shape), they prefer to **discuss conversationally first** — they rejected a 3-question `AskUserQuestion` batch about per-light control, saying they wanted to clarify, then described the design clearly in prose.

**Why:** the user often has a specific mental model that doesn't match my pre-baked options; multiple-choice can anchor on the wrong framing. They'd rather correct the framing in dialogue.

**How to apply:** when a "discuss design first" signal appears (or the decision is genuinely architectural), restate my understanding in prose and ask focused open questions, instead of immediately offering canned options. Save `AskUserQuestion` for once the option space is actually crisp. Related: [[project_dashboard_design]].
