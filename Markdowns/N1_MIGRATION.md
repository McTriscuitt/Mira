# N1_MIGRATION.md — Event-Driven Reactive Core

Migration plan for **N1** in `FEATURES_AND_REWORKS.md`: replace the monolithic
30-second tick with an event queue + per-state dispatch, with SSE drained on a
dedicated FreeRTOS task. Subsumes **G22** (instant lockout re-arm), **G23**
(drop `lastFloorOn` accumulator), and awkward-structure item **(a)** (async SSE
task). **N5** (ticks aligned to :00/:30) rides along in Stage 2 for free.

This doc is the **cross-session handoff**. At the start of each session: read
this file, then read only the `main.cpp` regions the current stage touches.
Before ending a session: update the Status table and the per-stage checklist.

---

## Status

| Stage | Description | Status |
|---|---|---|
| 1 | SSE → pinned Core-0 task + event queue + override via dispatcher | **COMPLETE — soak verified (June 12)** — boot, override, mid-batch abort, clobber repair confirmed live June 11. 2-hour telemetry soak June 12 (`Bug Records/June12MorningHeapSoak.md`): heap flat (8,583,419–8,585,031, no trend), SSE stack high-water bottoms at 7,432/12,288 free (floor set by the reconnect TLS handshake, stable across 4 reconnects), echo histogram 18 echo-match / 0 noop / 0 stale-revert / 1 genuine Override, full WAKE ramp + NORMAL handoff clean. Planned overnight soak was cut short by a 1:40 AM Windows Update laptop restart that power-cycled the USB-powered ESP (event log: TrustedInstaller "Operating System: Upgrade (Planned)") — not a firmware fault. Longer-horizon heap watch rolls into Stage 5's 48 h soak item. |
| 2 | `loop()` → consumer/dispatcher; LuxTick; N5 alignment; buttons commented out | **COMPLETE — soak verified (July 14–15, user-accepted)** — overnight soak (`logs/device-monitor-260714-180314.log`): one uninterrupted boot session ~6 h+ (SSE stack watermark continuity 10,040→7,432→7,388→7,340 proves no reboot), heap flat at ~8,584,600 ± 400 B, every tick on :00/:30 with clean skip-ahead events, dashboard commands applied (SOFT_PAUSE + 4× `SET_SOFT_PAUSE_REMAINING`), natural SOFT_PAUSE→resume→wind-down→LOCKED_OUT progression while unobserved. The 10-min `SSE: stale` reconnect cadence on an idle evening is designed behavior (no Hue v2 keepalive). Both capture gaps were host-side: laptop Modern Standby 8:43 PM–12:20 AM, then a Windows Update restart at 1:38 AM killed the logger (device kept running; COM5 re-enumerated). Override regression not explicitly re-run — accepted; Stage 1 path unchanged by Stage 2 and covered by the cross-stage checklist after Stages 4/5. |
| 3 | G22 + G23 — floor edge & lockout re-arm via SSE events; delete `checkFloorState()` | **COMPLETE — overnight soak user-accepted (July 15–16)** — soak (`logs/device-monitor-260715-210725.log`): dashboard-forced wind-down completed 21:10 → LOCKED_OUT via normal completion handoff; user's floor-lamp off-flip 21:26:08 exercised the re-arm falling-edge path as a harmless no-op re-clear (already LOCKED_OUT — floor stays on as the night-light at wind-down completion, so the all-off condition first holds when the user kills the floor lamp); heap flat ~8,584,900 B; SSE stack floor 6,880/12,288 across multiple reconnects; zero false overrides. Capture gaps host-side again (Modern Standby 21:26→01:59; port dead from 03:22). Wake edge verified live July 15 20:43:54 (Stage 3 flash day). *Not strictly exercised:* re-arm from a non-LOCKED_OUT state, Ethernet-pull synthetic-edge wake — both folded into the cross-stage checklist. Same-day follow-up (`56d2841`/`6b2646d`): `WIND_DOWN_GATE_HOUR 19` + `LOCKOUT_RESET_MIN_OF_DAY 1230` (schedule shift; re-arm gate now minute-granular). Side quest: R3's `setCACert()` approach failed on hardware (CN mismatch — see Decisions Log) and was revised to manual pin comparison; the `ensureBridgeCert()` boot probe had been silently failing + re-fetching every boot for the same reason, now fixed. |
| 4 | Ramps & soft-pause expiry → soft timers | **COMPLETE — live verified (July 16, user-accepted)** — reconciler design (see Decisions Log); `ECHO_TRACE` off. All four verification items passed live: wind-down/wake cadence off-boundary from ticks, slider seeks pin+apply, pause expiry on-time (2-min drag fired at ~2:00 not ~2:30 — one-shot beats the old per-tick poll), extend/past-60 re-arm, resume ramp interpolates from actual dimmed state. Slider-to-0 next-loop-pass behavior deferred to post-Stage-5 re-check (command delivery is tick-bound until then). |
| 5 | Dashboard networking → dedicated task (unblocks N2 long-poll) | **CODE COMPLETE (July 16) — not yet flashed** — `netTask` (Core 0, prio 1, 12288 stack) owns all Railway HTTP; `sendLog()` → drop-oldest `logQueue` (12× `LogMsg[120]`); status POST split into main-task `queueDashboardStatus()` (POD snapshot → depth-1 `statusQueue`, `xQueueOverwrite`) + netTask `postDashboardStatus()`; command poll every `NET_CMD_POLL_MS` (5 s, config.h) → `DashboardCmd` events, applied by `dispatchDashboardCmd()` on the main task, ack-after-enqueue; heap/SSE-stack/net-stack telemetry rides every `/api/status` POST with matching columns + migration and an owner-only `/api/telemetry/history` endpoint (48 h default) in `app.py`. Deviation: a fresh snapshot is queued after every applied command (see Decisions Log). **FLASHED + boot-verified July 16 21:11** (after Stage 4 user-acceptance): ticks on :00/:30, telemetry line shows net stack floor 6,624/12,288, heap ~8.5 MB baseline, bridge PUTs 200, no dashboard POST errors (netTask POSTs returning 200). A queued wind-down command was consumed ~5 s after boot and ran to LOCKED_OUT cleanly — first live exercise of the netTask→DashboardCmd→dispatch path. Remaining verification below (command latency, 48 h heap via `/api/telemetry/history`, busy-tick log completeness) + slider-to-0 next-loop-pass re-check + cross-stage regression checklist. |

Each stage is **independently shippable**: compiles, flashes, runs a full day.
One git commit per stage minimum. Verify before moving on.

---

## Decisions Log

- **June 11, 2026 — physical buttons removed** (user note in
  `FEATURES_AND_REWORKS.md`). Per standing preference, button code is
  **commented out, not deleted** (hardware-driven removal — keep ready for
  re-activation). `EvType::ButtonPress` stays in the enum for the future.
- **Core assignment correction.** The N1 sketch says "SSE task pinned to
  Core 1", but Arduino-ESP32 pins `loopTask` to Core 1 already; WiFi/lwIP run
  on Core 0. The SSE task therefore goes to **Core 0** — the genuinely idle
  core from the app's perspective. Priority 1 (same as loop) is fine; it
  spends its life blocked in `vTaskDelay`/TLS reads.
- **`state` is mutated only by the dispatcher** (main task). The SSE task
  never writes `state`; it enqueues events and sets `overridePending`.
- **Step counters stay** (`wakeStep`, `windDownStep`, `pauseResumeStep`) —
  the dashboard sliders seek on them. Timers drive *advancement*, counters
  remain the state representation.
- **N2 (long-poll / dashboard SSE) is out of scope.** Stage 5 only creates
  the network task that makes N2 possible later.
- **June 11, 2026 — Stage 3 re-arm scope (user decision).** "If after 9 PM all
  the lights turn off *at all*, in any state, then lock out." Re-arm therefore
  fires from **any state except HARD_OFF** (including SOFT_PAUSE — matches the
  G22 deep-dive's "re-arm should win over SOFT_PAUSE"; all-off after 21:00 is
  a sleep signal). HARD_OFF stays excluded: it's the explicit kill switch.
- **July 15, 2026 — R3 revised to manual pin comparison (hardware finding).**
  `setCACert()`-based pinning can never handshake on this stack: core 2.0.0's
  `ssl_client.cpp:257` always calls `mbedtls_ssl_set_hostname()` with the
  connect host (verification REQUIRED when a CA is set), we dial by IP, and
  the Hue bridge cert's CN is the bridge ID → CN mismatch, confirmed live as
  an `SSE: connect failed` loop on the first Stage 3 flash. Same flaw meant
  `ensureBridgeCert()`'s NVS-verify probe had been failing — and silently
  re-fetching the cert — on **every boot** since it was written. Fix:
  `peerMatchesPinned()` byte-compares the live peer cert against the NVS PEM
  after the handshake, on the paths where nothing (incl. the application key)
  is sent before the check passes: the boot probe and the SSE stream. The
  HTTPClient paths (`setLight*`/bootstrap GET) transmit on connect, so they
  stay insecure-mode TLS and inherit the boot-time identity check.
- **July 16, 2026 — Stage 4 uses a reconciler, not per-site arm/disarm.** The
  plan's "forceState() becomes the single place transitions arm/disarm timers"
  doesn't survive contact with the code: six transitions bypass `forceState()`
  entirely (wake completion, wind-down completion, `tickNormal`→WIND_DOWN,
  `applyOverridePause`, SSE re-arm, pause expiry). Instead `syncSoftTimers()`
  runs every `loop()` pass and derives the active-timer set purely from
  `(state, pauseResumeActive)` — zero arm/disarm sites, no transition can leak
  a timer. The pause-expiry one-shot's deadline is recomputed from
  `softPauseStart + softPauseDurationMs` each pass, so `SET_SOFT_PAUSE_REMAINING`
  re-aims it with no special handling. Ramp timers arm due-now: a dashboard
  WIND_DOWN/WAKE command or SSE wake edge now PUTs on the next loop pass
  (~50 ms) instead of waiting for the next 30 s boundary — a small deliberate
  improvement over the old same-tick/next-tick behavior. `expireSoftPause()`
  re-checks elapsed-vs-duration on fire to reject a queued-but-stale expiry
  racing an extend command (FIFO: a LuxTick carrying the extend can sit ahead
  of the TimerFire in the queue).
- **July 16, 2026 — Stage 5 pushes a status snapshot after every applied
  command** (small deliberate addition to the plan). `dispatchDashboardCmd()`
  ends with `queueDashboardStatus(lastLux)`, so the snapshot reflecting a
  commanded state chases the ack by ~one netTask pass (~100 ms) instead of
  waiting out the rest of the 30 s tick. This shrinks the June 11
  ack-before-status display window (the frontend's pending-preview hold now
  clears almost immediately) at the cost of a few extra `status_snapshots`
  rows per command — the rows carry `lastLux` (≤ 30 s old, same seed the ramps
  use), so the lux timeline is unaffected. Also locked in: ack-after-enqueue
  (an evQueue-full or dropped ack → command re-fetched next poll — the same
  at-least-once semantics the synchronous poll had, so a rare duplicate apply
  is possible and harmless, as before); unknown commands are still acked so
  they can't wedge the dashboard's pending queue; `Serial` command echo moved
  to netTask fetch time (the wire string doesn't cross the queue).

---

## Target Architecture

```cpp
enum class EvType : uint8_t {
    LuxTick,        // 30 s timer — the only thing the "tick" remains
    SseLight,       // classified light event from the SSE task
    SseAccessory,   // future: button / relative_rotary / zigbee_connectivity
    ButtonPress,    // kept for future use (physical buttons commented out)
    DashboardCmd,   // command pulled by the network task (Stage 5)
    TimerFire       // soft-timer expiry (ramp step, pause expiry, …)
};

struct Event {
    EvType   type;
    uint8_t  a;      // idx / timer id / command enum
    uint8_t  flags;  // SseLight: bit0=prevOn, bit1=nowOn, bits2-5=EchoOutcome
    int32_t  i;      // value (cmdValue, …)
    float    f;      // lux / bri
    uint32_t tMs;    // millis() at enqueue
};

QueueHandle_t evQueue;            // xQueueCreate(32, sizeof(Event)) — POD only, no String
volatile bool overridePending;    // set by SSE task on Override; consumed by dispatcher
portMUX_TYPE  dataMux = portMUX_INITIALIZER_UNLOCKED; // guards lightCache + recentPuts
```

`loop()` end state (after Stage 2):

```cpp
void loop() {
    Event ev;
    if (xQueueReceive(evQueue, &ev, pdMS_TO_TICKS(50)) == pdTRUE) dispatch(ev);
    softTimersTick();   // enqueues TimerFire for due deadlines (Stage 4)
}
```

### Concurrency rules (the whole discipline, three lines)

1. **`lightCache[]` + `recentPuts[]`** are shared between the SSE task and the
   main task. Every read-modify or multi-field copy goes inside
   `portENTER_CRITICAL(&dataMux)` / `portEXIT_CRITICAL(&dataMux)` sections —
   these are tiny field copies, never prints or HTTP.
2. **`state`** is written only by the dispatcher on the main task. The SSE
   task communicates via `evQueue` + `overridePending`.
3. **No `String` crosses tasks.** Events are POD. Log text shipped between
   tasks (Stage 5) uses fixed `char[120]` slots.

### Mid-batch abort equivalence (the one bug we must not reintroduce)

Today, `setLight()` tail-calls `sseTick()`, so an Override flips `state`
*between* PUTs in a `tickNormal` chain and the top-of-function guard
short-circuits the rest. Under the queue model the Override event would sit in
the queue until the chain finishes. Fix (from the N1 spec): the SSE task sets
`overridePending = true` the instant it classifies an Override; the
`setLight()`/`setLightColor()` guard becomes

```cpp
if (overridePending || state == State::SOFT_PAUSE || state == State::HARD_OFF) return;
```

The dispatcher clears `overridePending` when it processes the Override event
(performs the SOFT_PAUSE transition, diagnostic dump, `sendLog`).

---

## Stage 1 — SSE task on Core 0 + event queue + override via dispatcher

**Goal:** SSE events are processed in true real time, regardless of what the
main loop is doing (Railway HTTP, bridge PUTs). Override response latency drops
from "whenever the wait loop next runs" to ~milliseconds. The old `loop()`
structure is otherwise untouched.

**Changes (`src/main.cpp`):**

1. Add `EvType` / `Event` / `evQueue` / `overridePending` / `dataMux` globals.
2. `sseTask(void*)`: `for(;;) { sseTick(); vTaskDelay(pdMS_TO_TICKS(20)); }`.
   Created in `setup()` *after* `sseConnect()`:
   `xTaskCreatePinnedToCore(sseTask, "sse", 12288, nullptr, 1, &sseTaskHandle, 0);`
   Stack 12 KB to start (JSON parse + TLS reads happen in-task); check
   `uxTaskGetStackHighWaterMark()` via Serial during verification and trim.
3. `handleLightUpdate()` — runs on the SSE task now:
   - Cache writes and `recentPuts` access wrapped in `dataMux` sections.
   - On `Override`: **no longer** sets `state`/`softPauseStart` directly and
     no longer calls `sendLog()`/`dumpOverrideDiagnostic()` (no HTTP from the
     SSE task). Instead: `overridePending = true;` + enqueue
     `Event{SseLight, idx, flags(outcome=Override), …}`.
   - All other outcomes: classification + ring recording as today (ring +
     histogram are written on the SSE task; the dispatcher only reads them
     during the Override dump — racing a diagnostic read is acceptable).
4. Dispatcher shim in the existing wait loop: replace the `sseTick()` call
   with a queue drain — `while (xQueueReceive(evQueue, &ev, 0)) dispatchSseLight(ev);`
   `dispatchSseLight()` performs today's Override block: state → SOFT_PAUSE,
   seed `softPauseStart`/`softPauseDurationMs`, clear `pauseResumeActive`,
   `dumpOverrideDiagnostic()`, `sendLog()`, and clears `overridePending`.
5. `setLight()` / `setLightColor()`:
   - Guard extended with `overridePending` (see above).
   - **Remove the tail `sseTick()` call** (the task drains continuously).
   - Keep `noteRecentPut()` before the PUT and the `postedAtMs` refresh after
     (now under `dataMux`). Note: with async drain, the echo can be processed
     *during* the blocking PUT — that's fine, the slot exists before the
     request goes out (that ordering was always the design).
6. `sseConnect()` is only ever called from the SSE task after boot, so the
   reconnect path needs no extra locking. `bootstrapLightStates()` runs in
   `setup()` before the task exists — also safe.

**Verification:**
- [x] `pio run` clean; flash; full boot (cert, bootstrap, SSE connect, purple flourish). *(June 11.)*
- [x] Manual change in NORMAL → SOFT_PAUSE, concise log line on dashboard, full dump on Serial. *(June 11. Classification is real-time; the SOFT_PAUSE application lands when the dispatcher next runs — up to ~2 s if a tick body is mid-flight. Stage 1 semantics; fine.)*
- [x] Mid-batch abort: trigger an override during a multi-bulb transition tick; confirm remaining PUTs in the chain are skipped (Serial shows no setLight lines after the override). *(Verified June 11 — Dresser PUT correctly skipped after mid-chain floor override.)*
- [x] Clobber repair: flip a lamp off mid-transition-tick again; confirm "Override repair" Serial line and the lamp *stays off* through the pause. *(June 11 — worked first try: lamp came back on from the in-flight PUT, repair switched it off ~2 s later, state → SOFT_PAUSE.)*
- [ ] Normal evening: echoes still classify as `EchoMatch`/`NoOpEcho` (histogram via override dump or `ECHO_TRACE`), no false SOFT_PAUSE.
- [ ] `uxTaskGetStackHighWaterMark(sseTaskHandle)` healthy after a day.

**Risk notes:** Serial prints now interleave from two cores — cosmetic. Heap:
one extra task + queue ≈ ~13 KB; fine on the S3.

**Implementation notes (June 11, 2026 — code complete):**
- `recordSseEvent()` now returns its ring slot; on Override the slot index
  travels in `Event.i` so `applyOverridePause()` feeds the triggering event's
  full record to `dumpOverrideDiagnostic()` from the main task.
- `applyOverridePause()` guards on `state == NORMAL` (state may move on in the
  ≤50 ms between classification and dispatch) and is also the belt-and-braces
  target: `drainEventQueue()` calls it flag-only if `overridePending` is set
  with an empty queue (covers a failed `xQueueSend` on a full queue).
- `state` and `pauseResumeActive` made `volatile` (cross-core reads by the
  SSE task). `Event` has default member initializers (clang-tidy).
- Build: RAM 18.3 %, flash 31.1 %. Stage-1 commit contains main.cpp + this doc.

**Hardware finding (June 11, 2026) — in-flight PUT clobber, fixed:**
First live test: user flipped the floor lamp off *during* a transition tick's
floor PUT (lux had just crashed 1669→56). The abort correctly skipped the rest
of the chain (no Dresser PUT), but the in-flight floor PUT couldn't be
recalled — it landed ~1 s after the user's change and turned the lamp back on,
then SOFT_PAUSE froze it that way. Same hole existed in the old synchronous
design; the timing is *correlated* (users react to light changes right when
transition ticks PUT), so it had to be fixed. Fix: `restoreUserOverride()` —
on Override dispatch, if a live `recentPuts` slot's target contradicts the
triggering event's fields, re-PUT the user's own values before flipping to
SOFT_PAUSE (slot lifetime = exactly the clobber-risk window; a false-positive
repair just re-asserts the bulb's current state, harmless).

---

## Stage 2 — `loop()` becomes the consumer; LuxTick; N5; buttons out

**Goal:** Collapse `loop()` to consume → dispatch. The 30 s tick becomes just
another event. Ticks align to wall-clock :00/:30 (N5).

**Changes:**

1. Extract today's tick body (lines ~1573–1615: `timeClient.update()`,
   `printStatus()`, lux read, `luxToTarget`, `pollDashboardCommand()`, the
   state switch, `sendDashboardStatus()`) into `dispatchLuxTick()`. Behavior
   identical — `pollDashboardCommand` stays synchronous until Stage 5.
2. New `loop()`: `xQueueReceive(evQueue, &ev, pdMS_TO_TICKS(50))` → `dispatch(ev)`;
   plus a lightweight scheduler check that enqueues `LuxTick` when due.
3. `dispatch(ev)` switch: `LuxTick` → `dispatchLuxTick()`; `SseLight` →
   `dispatchSseLight()` (from Stage 1).
4. **N5 alignment** (per the spec in `FEATURES_AND_REWORKS.md`):
   ```cpp
   #define ALIGNED_TICKS 1   // comment out for testing (flash-relative 30 s)
   ```
   With `ALIGNED_TICKS`, next LuxTick due = top of the next epoch-:00/:30
   boundary (`timeClient.getEpochTime() % 30`); without, `lastTick + 30 000 ms`.
   Log-and-ignore the single short/long tick after an NTP resync.
5. **Buttons commented out** (decision log): `pinMode` lines, `pollButton()`
   calls, `handleButtonEvents()`, `handleCycleButton()`, and the `ButtonState`
   instances — commented with a `// [buttons removed 2026-06 — see N1_MIGRATION.md]`
   marker. `forceState()` stays fully live (dashboard commands use it).

**Verification:**
- [x] Ticks land on :00/:30 (Serial timestamps; dashboard timeline points on clean boundaries). *(July 14 — 12/12 ticks on-boundary ±1 s over 6 min post-fix, incl. one clean skip-ahead. Reconfirmed across the July 14–15 overnight soak: every logged tick on-boundary, multiple clean skip-aheads.)*
- [x] One full state-machine day: wake (floor-lamp flip), NORMAL curve, wind-down, lockout re-arm. *(July 14–15 soak, user-accepted: NORMAL → SOFT_PAUSE (dashboard) → auto-resume → wind-down → LOCKED_OUT confirmed by the 12:20 AM capture; wake leg + re-arm not directly observed (laptop slept 8:43 PM–12:20 AM) — they get re-verified under Stage 3 anyway, where both move to SSE dispatch.)*
- [x] Dashboard commands still apply (still ≤30 s latency at this stage — expected). *(July 14 — `SET_SOFT_PAUSE_REMAINING 118` polled + applied live during first capture; soak added SOFT_PAUSE + 4 more remaining-time commands.)*
- [x] Override path from Stage 1 still works end-to-end. *(Accepted without an explicit re-run July 15 — Stage 2 didn't touch the classification/dispatch path; covered again by the cross-stage regression checklist.)*

**Implementation notes (July 14, 2026 — code complete):**
- Tick body extracted verbatim into `dispatchLuxTick()`; dispatched via a real
  `LuxTick` event (`serviceLuxTickSchedule()` enqueues when the deadline
  passes; if `xQueueSend` fails on a full queue the deadline stays armed and
  retries next pass — a tick is never silently dropped).
- `scheduleNextLuxTick()` runs at *enqueue* time, so tick processing cost is
  absorbed into the wait exactly like the old `tickStart`-anchored loop.
  `ALIGNED_TICKS` lives in `config.h` (comment out → flash-relative 30 s).
  First boot tick fires immediately (`nextLuxTickDueMs = millis()` at the end
  of `setup()`); alignment starts from tick 2. Short slots (<5 s) log a
  "Tick align" line and **skip to the following boundary** (see hardware
  finding below — the original "schedule the remainder" behavior double-fired).
- New `loop()`: blocks ≤50 ms on `xQueueReceive` → `dispatchEvent()`; on empty
  queue checks `overridePending` (the old `drainEventQueue()` belt-and-braces —
  that helper is deleted, its job now lives in `loop()`).
- Buttons commented out (struct, instances, `pollButton`, `handleButtonEvents`,
  `handleCycleButton`, both `pinMode`s) with `[buttons removed 2026-06]`
  markers. `forceState()` untouched — dashboard commands drive it.
- Deadline check uses `(int32_t)(millis() - nextLuxTickDueMs) >= 0` —
  rollover-safe signed-difference form.

**Hardware finding (July 14, 2026) — duplicate boundary tick, fixed:**
First flash showed tick pairs 3–4 s apart every few minutes (:42:59 + :43:03,
:45:29 + :45:32). Truncated-clock phase error: the deadline is armed in
`millis()` (ms precision) but the next slot is computed from
`timeClient.getEpochTime()` (whole seconds). When the deadline fires a hair
before the second rolls over to the boundary, `secsIntoSlot` reads 29, so the
scheduler armed a 1 s "remainder" slot and fired the *same* boundary twice —
each dupe costing an extra lux read, Railway POST, and potentially bridge PUTs.
Fix in `scheduleNextLuxTick()`: a computed slot under 5 s means the just-fired
tick *was* the boundary tick, so `waitMs += 30000` skips to the following
boundary. Verified live: the skip-ahead event at 17:53:00 produced exactly one
tick at :52:59 and the next at :53:31 — no dupe, alignment self-stabilizes just
after the boundary.

---

## Stage 3 — G22 + G23: floor edge & re-arm move to SSE dispatch

**Goal:** Wake trigger and lockout re-arm land in ~100 ms instead of ≤30 s.
Delete the `lastFloorOn` accumulator and its seven sync sites, plus
`checkFloorState()`.

**Changes:**

1. SSE task: `handleLightUpdate()` captures `prevOn` (pre-write cache) and
   `nowOn`; whenever they differ, enqueue `SseLight` with the edge encoded in
   `flags` — for *every* outcome class, not just Override (an off-flip during
   LOCKED_OUT is `SkipState` today but is exactly the edge we need).
2. `dispatchSseLight()` grows two blocks (state decisions stay on the main task):
   - **Wake (G23):** `state == LOCKED_OUT && idx == LIGHT_FLOOR && rising edge`
     → `triggerWake(lastLux)`.
   - **Re-arm (G22):** falling edge on any light, `hour >= LOCKOUT_RESET_HOUR`,
     and cache shows floor+chest+dresser+ceil1 all off → `LOCKED_OUT`, reset
     `stableLuxCount`/`windDownStep`, clear `excludedLight[]`, cancel any
     active soft pause / resume ramp. Fires from **any state except HARD_OFF**
     (user decision, June 11 — see Decisions Log). Broader than today's
     `checkFloorState()`, which only ran in NORMAL/WIND_DOWN/LOCKED_OUT.
3. Delete `checkFloorState()`, `lastFloorOn`, and all seven
   `lastFloorOn = cachedLight(...)` sync sites (pure-software refactor —
   actual deletion, not comment-out).
4. **Reconnect gap mitigation** (from the G23 deep-dive): on SSE reconnect,
   re-run the bootstrap GET, diff fresh state against the pre-reconnect cache,
   and enqueue synthetic `SseLight` edge events for any on/off differences.
   Note: the bootstrap GET now happens on the SSE task — cache writes under
   `dataMux` like any other SSE write.
5. Boot safety: bootstrap runs before the SSE task starts and enqueues
   nothing, so a floor lamp already on at boot does **not** fire wake —
   same behavior as today.

**Verification:**
- [x] Flip floor lamp on in LOCKED_OUT → wake starts ~instantly. *(July 15, 20:43:54 — user forced wind-down→LOCKED_OUT, flipped the floor lamp off then on; "Wake triggered" landed in the same log-second as the rising-edge event, mid-slot, ~10 s before the next tick. Seeded from the floor's true cached bri (21.34 %). The off-flip also exercised the re-arm falling-edge path, correctly rejected by the hour gate (20 < 21).)*
- [x] After 21:00, kill lights in order → dashboard shows LOCKED_OUT within ~1 s of the last off. *(July 15–16 soak: path exercised as a no-op re-clear only — wind-down completion had already locked out before the user's 21:26:08 floor-lamp off-flip completed the all-off condition. Accepted; a from-NORMAL re-arm rides in the cross-stage checklist.)*
- [x] `grep lastFloorOn src/main.cpp` returns nothing. *(July 15 — accumulator, seven sync sites, and the poll function all deleted; tombstone comments reworded to keep the grep clean.)*
- [ ] Pull bridge Ethernet for 30 s during LOCKED_OUT, flip floor lamp on, reconnect → synthetic edge fires wake. *(Reconnect + on-task resync itself verified live July 15 via the 10-min stale cycle — clean reconnect, resync GET on the SSE task, no missed-flip enqueues, stack floor 7,020/12,288 free.)*

**Implementation notes (July 15, 2026 — code complete, flashed `6b478b6`):**
- Flags packing formalized to the Stage-1 comment's layout: bit0=prevOn,
  bit1=nowOn, bits2–5=EchoOutcome (`packSseFlags()` / `sseFlags*()` helpers).
  `handleLightUpdate()` enqueues on Override, StaleRevert, or any on/off edge;
  edge bits ride on every enqueued event so a burst of queued edges replays in
  order even though the cache has already moved on. `ev.i` carries ringSlot
  (Override) / revertSlot (StaleRevert) / -1 (edge-only).
- `dispatchSseLight()` (main task): Stage-1 Override/StaleRevert actions
  first, then the wake block (LOCKED_OUT + floor rising edge →
  `triggerWake(lastLux)`) and the re-arm block (falling edge, time-of-day ≥
  `LOCKOUT_RESET_MIN_OF_DAY` (minute-granular since the July 15 schedule
  shift; was hour-granular `LOCKOUT_RESET_HOUR`), floor+chest+dresser+ceil1
  all off in cache → LOCKED_OUT; resets counters, clears exclusions, cancels
  resume ramp; logs only on an actual state change). If an Override rode in on the same event,
  its pause applies first and re-arm supersedes it — intended precedence.
- `bootstrapLightStates(bool enqueueEdges=false)`: cache writes now under
  `dataMux`; boot call (setup, enqueueEdges=false) never enqueues (floor
  already on at boot must not fire wake); `sseTick()` calls it with `true`
  after every successful reconnect. Missed on/off flips are recorded as
  `EchoOutcome::SyntheticEdge` (ECHO_OUTCOME_COUNT now 10) and enqueued as
  ordinary SseLight edge events.
- `ECHO_TRACE` enabled for the Stage 3 soak (Serial-only) — turn off at the
  Stage 4 flash.
- Build: RAM 18.3 %, flash 31.1 %. Boot state: `state` initializes to NORMAL —
  user confirmed July 15 that NORMAL is the intended boot default (CLAUDE.md's
  old "LOCKED_OUT at boot" wording was the drift, now fixed).

---

## Stage 4 — Ramps and pause expiry on soft timers

**Goal:** Decouple ramp advancement and soft-pause expiry from the LuxTick so
the 30 s tick's only job is the curve.

**Changes:**

1. Minimal soft-timer module (no FreeRTOS timers needed — checked from `loop()`):
   ```cpp
   struct SoftTimer { bool active; uint8_t id; uint32_t dueMs; uint32_t periodMs; }; // periodMs=0 → one-shot
   ```
   `softTimersTick()` in `loop()` enqueues `TimerFire{a=id}` for due timers
   and re-arms periodic ones. IDs: `TIMER_WAKE_STEP`, `TIMER_WINDDOWN_STEP`,
   `TIMER_RESUME_STEP` (30 s periodic), `TIMER_PAUSE_EXPIRY` (one-shot).
2. `TimerFire` dispatch: wake/wind-down/resume handlers are today's tick
   bodies, reading a fresh `lastLux` (updated every LuxTick) instead of a
   passed-in `lux`. Step counters unchanged — dashboard seeks still work.
3. Timer lifecycle moves into the transition points: `triggerWake()` starts
   `TIMER_WAKE_STEP`; wind-down entry starts `TIMER_WINDDOWN_STEP`; soft-pause
   entry computes `TIMER_PAUSE_EXPIRY` from `softPauseDurationMs`
   (`SET_SOFT_PAUSE_REMAINING` re-arms it); completion/exit stops them.
   `forceState()` becomes the single place transitions arm/disarm timers —
   audit every entry path (dashboard cmd, override, auto-resume, re-arm).
4. `tickSoftPause()` (the per-tick expiry poll) is deleted; `tickNormal` stops
   advancing `pauseResumeStep`. The resume-ramp suppression of override
   detection (`pauseResumeActive`) is unchanged.

**Verification (all passed live July 16, user-verified):**
- [x] Wake and wind-down run at 30 s cadence independent of LuxTick alignment; milestones log correctly.
- [x] Dashboard slider seeks (wake step, wind-down step) still pin and apply.
- [x] Soft pause expires on time; `+10 min` extend and remaining-time slider re-arm correctly (including past 60 min). Discriminating test: a 2-min slider drag expired ~2:00 after the applying tick (the old per-tick poll would have waited to ~2:30 — deadline lands a fraction past the boundary tick). Note: slider-to-0 "ends on next loop pass" is NOT observable until Stage 5 — command delivery itself rides the 30 s tick until then; re-check post-Stage-5.
- [x] Pause → manual dim → expiry → resume ramp still interpolates over 10 min.

**Implementation notes (July 16, 2026 — code complete, flashed):**
- Deviation from the sketch above: no per-site timer lifecycle. `syncSoftTimers()`
  reconciles the active-timer set with `(state, pauseResumeActive)` every
  `loop()` pass (≤50 ms); `softTimersTick()` then enqueues `TimerFire{a=id}` for
  due timers. See the July 16 Decisions Log entry for the rationale and the
  stale-expiry guard.
- IDs per the sketch: `TIMER_WAKE_STEP` / `TIMER_WINDDOWN_STEP` /
  `TIMER_RESUME_STEP` (30 s periodic, `RAMP_STEP_MS`) + `TIMER_PAUSE_EXPIRY`
  (one-shot at `softPauseStart + softPauseDurationMs`, re-aimed by the
  reconciler whenever the deadline moves).
- `dispatchTimerFire()` re-checks state (and `pauseResumeActive` for resume) on
  every fire — a fire queued just before a transition is a no-op, since the
  reconciler only disarms on the next pass.
- Ramp bodies unchanged: `tickWakeRamp(lastLux)` / `tickWindDown()` fire from
  timers; the resume block moved out of `tickNormal()` verbatim into
  `advanceResumeRamp()` (target from `luxToTarget(lastLux)`); `tickSoftPause()`
  became `expireSoftPause()` (elapsed re-check kept as the stale-fire guard).
  `tickNormal()` still early-returns while `pauseResumeActive` (edge detection
  runs, stable-lux counting and curve PUTs suppressed — same as before).
- `dispatchLuxTick()`'s WAKE/WIND_DOWN/SOFT_PAUSE cases are now empty; the tick
  still refreshes `lastLux`, polls commands, and POSTs status in every state.
- Periodic re-arm is beat-anchored (`dueMs += periodMs`) with missed-beat skip
  (no catch-up bursts after a long blocking HTTP call); queue-full leaves the
  deadline armed and retries next pass — same policies as the LuxTick scheduler.
- `ECHO_TRACE` off (Stage 3 soak done). Build: RAM 18.3 %, flash 31.2 %.
  Boot verified live 19:22: ticks on :00/:30, curve driving in NORMAL, heap
  ~8,583,800, SSE stack watermark 10,216.

---

## Stage 5 — Dashboard networking task

**Goal:** Railway HTTP (status POST, log POSTs, command polling) stops
blocking the main task entirely. After this stage the LuxTick handler's only
network I/O is bridge PUTs. This is the prerequisite for N2's long-poll.

**Changes:**

1. `netTask` pinned to Core 0, priority 1. Owns all `DASHBOARD_BASE_URL` HTTP.
2. Outbound: a FreeRTOS queue of fixed-size messages
   (`struct LogMsg { char text[120]; }` + a `StatusSnapshot` POD struct built
   by the main task each tick). `sendLog()` keeps its signature but becomes
   "truncate-copy into a LogMsg and enqueue" — non-blocking, no `String`
   across tasks. Queue-full policy: drop oldest log (never block the main task).
3. Inbound: `netTask` polls `/api/command` (every ~5 s now — it's free) and
   enqueues `DashboardCmd` events. The body of `pollDashboardCommand()`
   becomes the `DashboardCmd` dispatch on the main task — command *application*
   (forceState, step seeks, exclusions) stays on the main task because it
   mutates state. Ack POST stays in `netTask` (acked after enqueue; same
   at-least-once semantics as today).
4. `sendDashboardStatus()` splits: main task fills `StatusSnapshot` (reads of
   `state`/steps/exclusions — all main-task-owned, no locking needed);
   `netTask` serializes + POSTs.
5. **Soak telemetry → dashboard:** add `heap_free` (`esp_get_free_heap_size()`)
   and `sse_stack_free` (`uxTaskGetStackHighWaterMark(sseTaskHandle)`) — plus
   `net_stack_free` for the new `netTask` itself — to `StatusSnapshot` and the
   `/api/status` payload, with matching `status_snapshots` columns + migration
   in `app.py`. Decouples long soaks from the laptop/serial connection (the
   June 11→12 overnight soak was lost to a Windows Update reboot of the
   logging laptop); the per-tick Serial print from Stage 1 stays as-is.

**Verification:**
- [ ] Dashboard command latency ≈ poll interval (~5 s) — visible improvement.
- [ ] Tick processing time (Serial `millis()` delta) drops to bridge-PUT cost only.
- [ ] Log lines and status snapshots arrive complete under a busy tick (no drops in normal operation).
- [ ] Heap stable over 48 h — read from the dashboard's `heap_free` history, no serial connection required.
- [ ] `sse_stack_free` / `net_stack_free` floors hold steady across SSE reconnects (Stage 1 baseline: 7,432/12,288 free).

---

## Cross-Stage Regression Checklist (run after Stages 2, 4, 5)

- [ ] Boot: WiFi → NTP → cert → bootstrap → SSE → purple flourish → Online log.
- [ ] Override: Hue-app change in NORMAL → SOFT_PAUSE, no echo false-positives overnight.
- [ ] Wake: floor-lamp flip in LOCKED_OUT → 20-min staircase → NORMAL handoff, no seam.
- [ ] Wind-down: counter → 60-min dim → chest off @60 → dresser off @120 → LOCKED_OUT.
- [ ] Cycle exclusion: exclude/include from dashboard during NORMAL and during a ramp.
- [ ] Soft pause: slider, extend, expiry, resume ramp.
- [ ] Dashboard: all state buttons, sliders, stable-lux widget, lights-in-cycle chips.

## Documentation updates on completion (or per stage)

- `CLAUDE.md`: "Recently completed" summary; rewrite the Core Behavior loop
  description (wait-loop → consumer/dispatcher); remove button bullets
  (note commented-out, not deleted); N5 note resolved.
- `FEATURES_AND_REWORKS.md`: mark N1, N5, G22, G23 **SHIPPED**.
- `SSE.md`: update "drained from the wait loop" references → Core-0 task;
  mid-batch abort section → `overridePending` mechanism.