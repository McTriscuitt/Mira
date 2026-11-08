---
name: mira-3-schedule-rework
description: "Mira 3.0 (NOT 2.0) time-of-day rewrite — running on the board since Oct 14 2026; wall switches + power loss confirmed Nov 5; LIGHT_ON/LIGHT_OFF + out-for-today/hold-level rework flashed Nov 5, untested; merged into main Nov 5; source of truth Markdowns/MIRA_3.md; read it before any firmware work"
metadata:
  node_type: memory
  type: project
  originSessionId: 0ef52c24-96bc-4dea-8b86-04940bb05db1
  modified: 2026-11-08T00:00:00.000Z
---

Mira 3.0 replaces the lux-reactive system with a time-of-day timeline of single
long-duration PUTs (the bulbs do the fading); no state machine — per-light
"want on"/"out for today" bits plus asleep/nap/hold conditions, reconciled by
`Director::reconcile()` (sends only when plan ≠ last command → 24 commands/day).
"2.0" was the unfinished "super reactive" N1/N2 line — never call this 2.0.

**Status:** flashed Oct 14 2026 23:33 CT from `mira-3`; merged into `main` Nov 5.
Board on **COM4**. Last lux build tagged `mira-2-final` (pushed to GitHub Nov 5).

**Nov 2 session, flashed and pushed on `mira-3`, boot and behaviour not yet confirmed on the board:**
new evening (75→40 at 19:00–20:00, chest off 20:00; 40→25 at 20:45–21:00; dresser off
22:30; holds after 20:45 end 22:30), in-line filter for false holds, `zigbee_connectivity`
wall-switch handling, NVS power-loss replay (§10). The next step is the "Nov 2 reflash
checklist" at the end of MIRA_3.md; wall-switch timing (hardware unknown #7) can only be
measured at home. The dashboard overhaul for the portfolio comes after that.

**Nov 5:** the user confirmed the wall-switch handling works on the real lights (checklist
item 1 / unknown #7; no timings given) and power-loss replay (item 3, "i believe functioned
as expected"); items 2 and 4 are open. **Flashed Nov 5** (board away from home WiFi, so
`Online` is unconfirmed): the dashboard light commands `LIGHT_ON`/`LIGHT_OFF` (terminal
`dresser off`, `chest on`; MIRA_3.md §5), plus two user-requested reworks: out for today
only before that light's scheduled off (it also clears at the scheduled off and at sleep),
and any light turned on during a hold joins the hold level (your last brightness in it,
`DirectorState.holdBri`, `STATE_MAGIC` …0302). Nov 5 reflash checklist in MIRA_3.md is open.

**Nov 8 (storm night):** Zigbee dropouts (`connectivity_issue` → `connected`, 10–25 s, no
one at a switch) were read as wall-switch ons and turned the dresser back on after its
22:30 off. Fixed and flashed Nov 8 1:13 AM: on reconnect the bridge is asked, and still off
= dropout, nothing changes (user chose this; the min-outage option for lights that are on
was not chosen). `Bug Records/Nov8_Zigbee_Dropout_Turned_Dresser_On.md`.

**Branch:** `mira-3` merged into `main` Nov 5 (user's call); work on `main`. Railway deploys
from `main` (switched Nov 5), and the `mira-3` branch is deleted.

**Facts that aren't in the code:** 18 days of logs (Oct 14–Nov 2) showed no panics or
watchdog resets after 3.0, DST fine. The board isn't always online, so a telemetry gap
isn't automatically a fault: ask first. Lights do sometimes get cut at the wall; the
floor lamp has no switch.

Running tests: `pio test -e native -e native_legs` needs a host g++ on PATH —
prepend `C:\Program Files\JetBrains\CLion 2024.3.1.1\bin\mingw\bin` (CLion's
MinGW; nothing else on this machine has a host compiler). Board build is
gnu++11 — keep the core C++11.

**Why:** user disliked the lux-reactive behavior; wants simple + predictable. They
explicitly want questions instead of liberties on unspecified behavior.

**How to apply:** read `Markdowns/MIRA_3.md` first (status table, spec, decisions log,
hardware unknowns, checklists); update it before ending a session. User's verbatim inputs:
`V3 Migration Docs/`. Dashboard log/telemetry readable by logging in with the owner
password ([[reference_credentials]]) when serial capture is off. Supersedes the post-N2
roadmap in [[n1-migration-in-progress]].
