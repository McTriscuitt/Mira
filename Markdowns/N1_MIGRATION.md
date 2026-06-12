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
| 1 | SSE → pinned Core-0 task + event queue + override via dispatcher | **CODE COMPLETE (June 11)** — builds clean; awaiting hardware verification (checklist below) |
| 2 | `loop()` → consumer/dispatcher; LuxTick; N5 alignment; buttons commented out | NOT STARTED |
| 3 | G22 + G23 — floor edge & lockout re-arm via SSE events; delete `checkFloorState()` | NOT STARTED |
| 4 | Ramps & soft-pause expiry → soft timers | NOT STARTED |
| 5 | Dashboard networking → dedicated task (unblocks N2 long-poll) | NOT STARTED |

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
- [ ] `pio run` clean; flash; full boot (cert, bootstrap, SSE connect, purple flourish).
- [ ] Hue-app manual change in NORMAL → SOFT_PAUSE within ~1 s (vs up to 30 s), concise log line on dashboard, full dump on Serial.
- [x] Mid-batch abort: trigger an override during a multi-bulb transition tick; confirm remaining PUTs in the chain are skipped (Serial shows no setLight lines after the override). *(Verified June 11 — Dresser PUT correctly skipped after mid-chain floor override.)*
- [ ] Clobber repair: flip a lamp off mid-transition-tick again; confirm "Override repair" Serial line and the lamp *stays off* through the pause.
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
- [ ] Ticks land on :00/:30 (Serial timestamps; dashboard timeline points on clean boundaries).
- [ ] One full state-machine day: wake (floor-lamp flip), NORMAL curve, wind-down, lockout re-arm.
- [ ] Dashboard commands still apply (still ≤30 s latency at this stage — expected).
- [ ] Override path from Stage 1 still works end-to-end.

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
- [ ] Flip floor lamp on in LOCKED_OUT → wake starts ~instantly.
- [ ] After 21:00, kill lights in order → dashboard shows LOCKED_OUT within ~1 s of the last off.
- [ ] `grep lastFloorOn src/main.cpp` returns nothing.
- [ ] Pull bridge Ethernet for 30 s during LOCKED_OUT, flip floor lamp on, reconnect → synthetic edge fires wake.

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

**Verification:**
- [ ] Wake and wind-down run at 30 s cadence independent of LuxTick alignment; milestones log correctly.
- [ ] Dashboard slider seeks (wake step, wind-down step) still pin and apply.
- [ ] Soft pause expires on time; `+10 min` extend and remaining-time slider re-arm correctly (including past 60 min).
- [ ] Pause → manual dim → expiry → resume ramp still interpolates over 10 min.

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

**Verification:**
- [ ] Dashboard command latency ≈ poll interval (~5 s) — visible improvement.
- [ ] Tick processing time (Serial `millis()` delta) drops to bridge-PUT cost only.
- [ ] Log lines and status snapshots arrive complete under a busy tick (no drops in normal operation).
- [ ] Heap stable over 48 h (`esp_get_free_heap_size()` printed per tick during soak).

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