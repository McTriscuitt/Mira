---
name: n1-migration-in-progress
description: N1 event-driven core migration is underway; plan/handoff lives in Markdowns/N1_MIGRATION.md — read it before any firmware architecture work
metadata: 
  node_type: memory
  type: project
  originSessionId: b650fe17-5ddb-4c71-8411-dd5224fe24e8
---

The N1 migration (event queue + Core-0 SSE task replacing the monolithic 30 s
tick) started June 11, 2026. **Stages 1–3 complete and soak-verified** (Stage 3
overnight soak user-accepted July 16; wake edge verified live July 15
20:43:54; re-arm path exercised only as a no-op re-clear — from-NORMAL re-arm
and the Ethernet-pull synthetic-edge test ride in the cross-stage checklist).
**Stage 4 COMPLETE — live-verified July 16 (user-accepted)**: soft timers via
the `syncSoftTimers()` reconciler, `ECHO_TRACE` off. All four checks passed
(cadence off-boundary, seeks, expiry on-time — 2-min drag fired at ~2:00 not
~2:30 — extend/past-60, resume ramp from actual dimmed state). Slider-to-0
"next loop pass" is only observable post-Stage-5 (command delivery was
tick-bound) — re-check it now that Stage 5 is live.

**Stage 5 FLASHED + boot-verified July 16 21:11** (`app.py` pushed/deployed
first, `19126c6`). `netTask` (Core 0, prio 1, 12288 stack) owns all Railway
HTTP: `sendLog()` → drop-oldest `logQueue` (12× `char[120]`);
`queueDashboardStatus()` (main task, POD snapshot, depth-1 `statusQueue` +
`xQueueOverwrite`) / `postDashboardStatus()` (netTask); command poll every
`NET_CMD_POLL_MS` (5 s, config.h) → `DashboardCmd` events applied by
`dispatchDashboardCmd()` on the main task, ack-after-enqueue. Deviation in
Decisions Log: every applied command pushes a fresh status snapshot (~100 ms),
shrinking the June 11 ack-before-status window. Telemetry
(`heap_free`/`sse_stack_free`/`net_stack_free`) rides every `/api/status`
POST; `app.py` has columns + migration + owner-only `/api/telemetry/history`
(48 h default). Boot capture: ticks on :00/:30, net stack floor 6,624/12,288,
heap ~8.5 MB, netTask POSTs 200; a queued wind-down command applied ~5 s after
boot → clean LOCKED_OUT (first live netTask→DashboardCmd exercise). Stage 5
verification still open: command latency ≈ 5 s, slider-to-0, busy-tick log
completeness, 48 h heap from `/api/telemetry/history`.

**R3 hardware finding (July 15):** `setCACert()` pinning is impossible — core
2.0.0 always enforces hostname verification, Mira dials by IP, bridge cert CN
is the bridge ID → CN mismatch. Shipped instead: `peerMatchesPinned()`
post-handshake byte-compare on the boot probe + SSE stream; HTTPClient paths
stay insecure-mode (they transmit on connect).

Soak-logging practicals: the soak logger detaches on COM5 — **kill python
before flashing** (none running = port free). The laptop's Modern Standby and
Windows Update restarts keep truncating overnight captures (three soaks in a
row) — Stage 5's dashboard heap/stack telemetry removes the laptop from the
loop. This laptop cannot reach the bridge over HTTPS (ARP flaps / AP
isolation), so bridge-API event injection from the PC doesn't work — use the
Hue app or natural cycles for edge tests.

Schedule constants (July 15): WIND_DOWN_GATE_HOUR 19, LOCKOUT_RESET_MIN_OF_DAY
1230. **Deferred:** the `lux <= 10` wind-down gate needs re-tuning — the ESP
sits on the desk, not the windowsill (noted in CLAUDE.md User Notes; pairs
with R8+F2 solar work).

**N2 — near-instant dashboard (July 24): steps 1+2 SHIPPED, step 3 open.**
Step 1 (`3cee62a`): `startStatusBurst()` in `index.html` — 2 s status polls
after any command until `_pendingCommand` clears, 8 s floor (covers
fire-and-forget light chips, which never set `_pendingCommand`) / 20 s cap,
log refetch on burst end. Step 2 (`2679fe9`): `/api/status/stream` SSE in
`app.py` — in-process `_Broadcast` (Condition + status/log version counters)
bumped by status/log ingest + command queue/cancel/ack; shared
`_status_payload()` keeps demo redaction identical to `/api/status/latest`;
25 s pings; `db.session.remove()` between events (pool exhaustion); Procfile
now `gunicorn app:app -w 1 --threads 16` — **`-w 1` is load-bearing** (the
broadcaster is process-local; a second worker would strand its browsers).
Frontend `EventSource` feeds the same `applyStatus()` path; 30 s polls stay
as fallback. Mechanics smoke-tested locally (bump→push/ping/log verified);
**live Railway verify still pending** (user to load dashboard + press a
button). Step 3 remaining: netTask long-poll on `/api/command` (firmware +
Flask, needs reflash). Also open: Stage 5 final checks (48 h heap via
`/api/telemetry/history`, busy-tick log completeness, slider-to-0 re-check)
+ cross-stage regression checklist. After N2: R1+R2 network batch → R8+F2
time/solar → F3 nudge layer → R5–R7 structure cluster. N1 itself is done —
all 5 stages flashed. Local note: `app.py` can't boot on SQLite (Postgres-only
migration SQL at import), so server changes verify live on Railway.

**Why:** the plan doc `Markdowns/N1_MIGRATION.md` is the single cross-session
source of truth — status table, per-stage specs, verification checklists,
decisions log (e.g., Stage 3 re-arm fires from any state except HARD_OFF;
Stage 4 reconciler; Stage 5 command-time snapshot push).

**How to apply:** at session start for firmware work, read N1_MIGRATION.md
first instead of re-deriving from FEATURES_AND_REWORKS.md / SSE.md; update its
status table before ending a session. Budget ≈ one stage per Pro 5-hour
window. Related: [[no-coauthor-in-commits]], [[comment-vs-delete]].
