# June 11 — Soft pause → NORMAL command: dashboard kept displaying SOFT_PAUSE (RESOLVED)

*(Moved from CLAUDE.md "Resolved Priority Bugs" during the Sept 12 CLAUDE.md trim — content unchanged.)*

- **RESOLVED (June 11)** — soft pause → dashboard NORMAL command showed on serial but the dashboard
  kept displaying SOFT_PAUSE. Not a firmware state bug. Root cause: the firmware acks the command at
  the top of its tick (`pollDashboardCommand()`), several seconds *before* `sendDashboardStatus()`
  posts the snapshot reflecting the new state — so there's a short window where `pending_command_id`
  is cleared but the latest snapshot is stale. If the browser's 30 s status poll landed in that
  window, `fetchStatus()` dropped the pending-state preview and reverted the display to the stale
  state for a full poll cycle. **Frontend fix applied** in `index.html`: after ack, the pending
  preview is held until the snapshot's `state` matches the commanded state, capped at one extra poll
  cycle so a genuine flip-back (e.g. an SSE override re-pausing right after a commanded resume —
  see `Markdowns/Was Normal to Soft Pause Error May 11.md`) still wins. Residual: up to ~60 s
  worst-case display latency from the two unsynchronized 30 s pollers (firmware command poll +
  browser status poll) — addressed by N2 (near-instant dashboard) and N5 (:00/:30 tick alignment)
  in `Markdowns/FEATURES_AND_REWORKS.md`. Note the same ack-before-status window also exists for
  step-seek commands (slider pins); N2 shrinks it but only an ack-after-status reorder removes it.
