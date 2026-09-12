 # CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project: Mira

Adaptive smart bedroom lighting firmware running on an **Arduino Nano ESP32-S3 (ABX00083)**. It reads ambient lux via a VEML7700 sensor and adjusts 4 Philips Hue bulbs through the local Hue Bridge HTTP REST API.

## User Notes
- all .md's for error parsing and rectifying are in \Mira\Bug Records\
- all .md's for information on the system code/upgrades/etc are in \mira\Markdowns


- add a "recruiter" role so that recruiters from companies can see what owner role see, but cannot edit. 
  - in between demo and owner permissions 
  - no hidden owner IP button allowed for recruiter role

- create section/page for serial-like output?
  - would that be too intense and slow the whole system down substantially? 
- PRIORITY
  - rework files to accomodate dashboard being integrated into main "portfolio" page
    - railway will become the landing page, with /mira and /tempproject coming from that (something like that)  

- is there a way to do sse-like updates (near-instant) with the dashboard? 
- should the wind down and wake ramps catch soft pause-esque changes?

- add a nap button on the dashboard
  - turns off the lights and set to soft pause for 90 minutes

- ~~this may seem silly, but change it so that polls happen on :00 and :30, instead of whenever 
the system reflashes. make it commentable so that it can be ignored for testing.~~ **SHIPPED July 14 (N5/`ALIGNED_TICKS` in config.h — comment out to revert to flash-relative 30 s)**

- maybe add another secret page for dashboard error logs?

- **(July 15) wind-down lux gate needs re-tuning for sensor placement** — the ESP currently
  sits on the desk (laptop's there), not the windowsill, so lux readings don't reflect
  window light the way the curve/thresholds assume. Wind-down should be able to begin
  earlier without lux having to drop all the way to ≤ 10. Revisit the `lux <= 10` gate in
  `tickNormal` (and possibly the placement story generally) — deferred, different day.

### Priority Bugs


### Resolved Priority Bugs
- **RESOLVED (June 11)** — soft-pause→NORMAL dashboard staleness (ack-before-status window; frontend pending-preview fix). Full write-up: `Bug Records/June11_SoftPause_AckBeforeStatus_DashboardStale.md`. Still-relevant residual: the same ack-before-status window exists for step-seek commands — N2 shrinks it, but only an ack-after-status reorder removes it.


## Recently completed

Change history lives in `git log` and the Markdowns docs — `Markdowns/FEATURES_AND_REWORKS.md` (feature/rework index with status banners) and `Markdowns/N1_MIGRATION.md` (N1 status table + decisions log). Shipped through: **N1 all 5 stages** (flashed July 16), **N2 steps 1+2** (dashboard-side burst polling + Flask→browser SSE, July 24; step 3 netTask long-poll still open).

## Build & Flash (PlatformIO)

Standard PlatformIO CLI: `pio run` / `pio run --target upload` / `pio device monitor` (115200 baud). PlatformIO is the only build system. The IDE is CLion with the PlatformIO plugin. `.pio/libdeps/` is auto-generated — do not edit files there.

## Hardware

| Component | Detail |
|-----------|--------|
| MCU | Arduino Nano ESP32-S3 (ABX00083) |
| Lux sensor | Adafruit VEML7700 — I2C, SDA=A4/GPIO11, SCL=A5/GPIO12 |
| Button (Mode) | D9 (GPIO18) — **retired June 2026, code commented out** (was: short = NORMAL/pause toggle, long = hard off) |
| Button (Cycle) | D10 (GPIO21) — **retired June 2026, code commented out** (was: short press cycles through all 6 states) |
| Bulbs | 4× Philips Hue E26 White & Color Ambiance (chest, dresser, ceiling 1+2) + 1× Signe gradient floor lamp; bri 0–254 / ct 153–447 Mired in v1 units, driven in v2 percent + mirek |
| Onboard RGB | Active LOW (LOW = on, HIGH = off) |

**LCD was attempted and abandoned** — HD44780 requires 3.5V logic minimum (0.7 × 5V VDD) but ESP32-S3 outputs 3.3V. Level shifting was not pursued. No display is planned.

## Key Configuration (`src/config.h` + `src/secrets.h`)

Config is split in two:
- **`src/config.h`** — **tracked/public** behavior + tuning constants (curve gates, tolerances, timing, button pins, cache indices). No secrets. It `#include`s `secrets.h` at the top, so `main.cpp` only needs `#include "config.h"`.
- **`src/secrets.h`** — **gitignored**, deployment-specific values: WiFi creds, `HUE_API_KEY`/`ESP32_API_KEY`, `HUE_V2_BASE_URL`/`HUE_BRIDGE_HOST` (bridge IP), `LIGHT_UUID_*`, `DASHBOARD_BASE_URL`. Copy **`src/secrets.h.example`** (tracked template) → `secrets.h` and fill it in on a fresh clone. Because it's gitignored, edits to these values (and to firmware tuning constants you keep there) never reach the repo.

- **Hue Bridge:** `192.168.1.186`; `HUE_V2_BASE_URL = "https://192.168.1.186/clip/v2"` + `HUE_API_KEY` (passed as `hue-application-key` header); `HUE_BRIDGE_HOST = "192.168.1.186"` for the SSE/cert connection probe
- **Cache indices:** `LIGHT_CHEST=0`, `LIGHT_DRESSER=1`, `LIGHT_CEIL_1=2`, `LIGHT_CEIL_2=3`, `LIGHT_FLOOR=4` — index into `lightCache[LIGHT_COUNT]` (`LIGHT_COUNT=5`) in `main.cpp`. `LIGHT_CHEST`/`LIGHT_DRESSER` are the **renamed** bedside/desk bulbs (same physical UUIDs, label-only change); `LIGHT_FLOOR` is the Signe gradient floor lamp added as the wake start-point + wind-down night-light anchor. `LIGHT_COUNT` sizes `lightCache[]` / `recentPuts[]` and bounds-checks `idx`.
- **Light UUIDs (v2):** `LIGHT_UUID_CHEST`, `LIGHT_UUID_DRESSER`, `LIGHT_UUID_CEIL_1`, `LIGHT_UUID_CEIL_2`, `LIGHT_UUID_FLOOR` — hardcoded in `secrets.h` from discovery; stable Zigbee identities, only change on factory reset. Used as the v2 endpoint path component and resolved back to cache indices via `idxByUuid()`. A `lightName(idx)` helper maps the index back to a friendly label (`Chest`/`Dresser`/`Ceiling_1`/`Ceiling_2`/`Floor`) for Serial + bootstrap logging.
- **UTC offset:** `UTC_OFFSET_SEC` — currently `-14400` (UTC-4 / Eastern Daylight)
- **`STATE_TOLERANCE_BRI 1.2f`** — min bri delta (percent) before a PUT is sent by `tickNormal`'s drift check (≈ 3/254 in old v1 units). No longer used for SSE override detection — that uses the wider `TRAJECTORY_TOLERANCE_BRI` instead.
- **`STATE_TOLERANCE_CT 3`** — min ct delta (mirek) before a PUT is sent by `tickNormal`'s drift check. No longer used for SSE override detection — see `TRAJECTORY_TOLERANCE_CT`.
- **`TRAJECTORY_TOLERANCE_BRI 5.0f`** — width of the per-light echo-discrimination window used by both `eventMatchesRecentPut()` (trajectory check) and the no-op event filter (event vs pre-event cache check) in `handleLightUpdate()`. Percent. Wider than `STATE_TOLERANCE_BRI` because it sizes Zigbee/bulb-side step quantization (~1–3% slop), not curve-drift sensitivity. User-driven overrides are double-digit percent, comfortably outside this window.
- **`TRAJECTORY_TOLERANCE_CT 15`** — same purpose for ct (mirek), same dual-use in both filters. White-ambiance bulbs quantize ct to discrete steps; 15 mirek absorbs the slop without masking real CT overrides (typically 50+ mirek).
- **`RECENT_PUT_GRACE_MS 5000UL`** — post-`dynamics.duration` slack window during which an incoming SSE event for a light is still considered a possible echo of a recent PUT. Sized for Zigbee mesh settle latency. After this expires, the no-op filter takes over as the catch for late settling echoes.
- **`RECENT_PUT_RING_SIZE 3`** — slots per light in the trajectory ring. Lets overlapping in-flight PUTs (wake-ramp tick + state-change PUT, or lux-driven PUT after an overhead toggle) keep their trajectories live simultaneously.
- **`SSE_EVENT_RING_SIZE 16`** — depth of the diagnostic `sseEventRing[]` that records every classified SSE light event with pre-event cache + outcome. Dumped on `Override` fire.
- **Wake-ramp turn-on gates** (in `config.h`) — the wake ramp is a brightness staircase that doubles as a sunrise: the floor lamp leads from tick 0, then `WAKE_SECONDARY_BRI 40.0f` + `WAKE_SECONDARY_LUX 250.0f` gate chest+dresser joining, then `WAKE_OVERHEAD_LUX 500.0f` (with the `S2_BRI_LO` 70% bri gate) gates the overheads. Both stages now **match** their NORMAL breakpoints: `WAKE_SECONDARY_LUX` (250) == `S3_LUX_HI` (250) and `WAKE_OVERHEAD_LUX` (500) == `S2_LUX_HI` (500), so neither the chest nor the overheads show a step at the WAKE→NORMAL handoff. Neither curve breakpoint should be repurposed.
- **`BTN_MODE 9` / `BTN_CYCLE 10`** — still defined, but physical buttons are retired (June 2026); all button code is commented out, not deleted (`[buttons removed 2026-06]` markers)
- **`LOCKOUT_RESET_MIN_OF_DAY 1230`** — minutes-since-midnight after which all-lights-off re-arms the morning lockout (1230 = 8:30 PM, aligned with the earliest wind-down completion; minute-granular since July 15, was hour-granular `LOCKOUT_RESET_HOUR 21`)
- **`WIND_DOWN_GATE_HOUR 19`** — 24h hour after which the stable-lux counter may accumulate toward WIND_DOWN; earliest completion = gate + 30 min counter + 60 min ramp (7 PM gate → lights down by 8:30 PM on a dark evening; lux still rules, the hour is only the floor). Lowered from 21 July 15, 2026 (schedule change)
- **`SOFT_PAUSE_MS 3600000UL`** — soft pause auto-resume duration (60 min)
- **`WAKE_RAMP_TICKS 40`** — wake ramp duration (20 min at 30 s/tick)
- **Turn-on order (wake):** Floor (tick 0) → Chest + Dresser (`rampBri ≥ 40% && lux ≥ 250`) → Overhead 1+2 (`rampBri ≥ 70% && lux ≥ 500`)
- **Turn-off order (wind-down):** Overhead 1+2 (start) → Chest (midpoint, step 60) → Dresser (end, step 120); Floor stays on at floor brightness (19.7%) as the night-light anchor

## Core Behavior & State Machine

State machine: `enum class State { LOCKED_OUT, NORMAL, WAKE, WIND_DOWN, SOFT_PAUSE, HARD_OFF }` in `main.cpp`. Since N1 Stage 2, `loop()` is a **consumer/dispatcher**: it blocks ≤50 ms on `evQueue` (`xQueueReceive`), dispatches events (`LuxTick` → `dispatchLuxTick()`, `SseLight` → `dispatchSseLight()`, `TimerFire` → `dispatchTimerFire()`, `DashboardCmd` → `dispatchDashboardCmd()`), arms the next LuxTick (`:00/:30`-aligned under `ALIGNED_TICKS` — N5), and services the Stage 4 soft timers (`syncSoftTimers()` + `softTimersTick()`). `dispatchLuxTick()` holds the tick body: status print, lux read (refreshes `lastLux`), `tickNormal()` when NORMAL, status snapshot. **Since N1 Stage 5 all dashboard HTTP lives on the Core-0 `netTask`** (log POSTs via drop-oldest `logQueue`, status POSTs via depth-1 `statusQueue`, `/api/command` poll every 5 s → `DashboardCmd` events; command *application* stays on the main task) — the tick's only network I/O is bridge PUTs. **Since N1 Stage 4 (July 16), ramp advancement and soft-pause expiry run on soft timers, not the tick**: `TIMER_WAKE_STEP`/`TIMER_WINDDOWN_STEP`/`TIMER_RESUME_STEP` (30 s periodic) and `TIMER_PAUSE_EXPIRY` (one-shot). No transition site arms/disarms anything — `syncSoftTimers()` reconciles the active-timer set with `(state, pauseResumeActive)` every loop pass, and ramp timers arm due-now, so a dashboard WIND_DOWN/WAKE command or SSE wake edge starts its ramp in ~50 ms instead of at the next 30 s boundary. SSE events are produced by the Core-0 `sseTask`.

1. **LOCKED_OUT** — no auto-on. A manual **floor-lamp** on-flip fires wake via SSE edge dispatch (`dispatchSseLight()`, ~100 ms — Stage 3 replaced the old per-tick `checkFloorState()` poll). Lockout re-arm is likewise edge-driven: any falling edge after `LOCKOUT_RESET_MIN_OF_DAY` (8:30 PM) with floor/chest/dresser/ceil1 all off re-arms from **any state except HARD_OFF** (clears exclusions, cancels pause/resume). *(Boot default is **NORMAL**, not LOCKED_OUT — confirmed intended July 15.)*
2. **WAKE** — `tickWakeRamp()` linearly interpolates bri and ct from the actual **floor-lamp** state at trigger → `luxToTarget(ambientLux)` over `WAKE_RAMP_TICKS` (20 min). The floor lamp leads from tick 0; chest+dresser join when `lux ≥ WAKE_SECONDARY_LUX (250) && rampBri ≥ WAKE_SECONDARY_BRI (40%)`; overheads turn on when `lux ≥ WAKE_OVERHEAD_LUX (500) && rampBri ≥ S2_BRI_LO (70%)`. Both thresholds match their NORMAL counterparts (`WAKE_SECONDARY_LUX` == `S3_LUX_HI` == 250; `WAKE_OVERHEAD_LUX` == `S2_LUX_HI` == 500), so there is no step at the WAKE→NORMAL handoff at any lux level. Hands off to NORMAL on completion.
3. **NORMAL** — `tickNormal()` runs the lux→bri/ct curve, edge detection for the two conditionally-driven bulbs (**chest at `S3_LUX_HI` 250 lux**, **overheads at `S2_LUX_HI` 500 lux** — each cuts below its threshold with the remaining lamps stepping up to compensate), cycle-exclusion enforcement, and the stable-lux counter each tick (override detection is real-time in the SSE handler, not the tick). Floor + dresser are the always-driven core (full lux participants); chest joins above 250, overheads above 500. Transitions to WIND_DOWN when stable counter hits 60.
4. **WIND_DOWN** — `tickWindDown()` linearly dims floor+chest+dresser toward floor over 60 min; **chest off at the midpoint (step 60), dresser off at the end (step 120)**, floor lamp holds at floor brightness; transitions to LOCKED_OUT.
5. **SOFT_PAUSE** — all commands suspended; auto-resumes to NORMAL when the one-shot `TIMER_PAUSE_EXPIRY` fires after `softPauseDurationMs` (seeded to `SOFT_PAUSE_MS` on entry, adjustable at runtime via the dashboard remaining-time slider / `+10 min` extend button — `SET_SOFT_PAUSE_REMAINING`; the reconciler re-aims the deadline automatically). `expireSoftPause()` seeds the 10-min resume ramp (`advanceResumeRamp()` on `TIMER_RESUME_STEP`).
6. **HARD_OFF** — loop does nothing; no polling, no commands.

Evening phases are lux-driven (not time-driven) so the system adapts to seasonal sunset variation automatically.

## Hue API Patterns (current — v2 HTTPS + SSE, all 3 phases complete)

Two PUT helpers exist in `main.cpp`:
- `setLight(uuid, on, bri, ct, durationMs)` — white/CT mode; `bri` is `float` (0.0–100.0%, native v2 units); `durationMs` in ms (e.g., `30000` = 30 s poll interval, `1000` = 1 s snap)
- `setLightColor(uuid, on, bri, hueV1, satV1, durationMs)` — HSB color mode; internally converts v1 hue (0–65535) + sat (0–254) to CIE xy via `_hsbToXY()`. Used only for the startup purple flourish.

Both use `WiFiClientSecure` in insecure-mode TLS with the `hue-application-key` header (see the cert-pinning note below for why CA-mode verification is impossible here). Both call `noteRecentPut()` *before* the HTTP request so the SSE handler can recognize echoes of the PUT — see `Markdowns/SSE.md`. Both also **short-circuit at the top** if `overridePending || state == SOFT_PAUSE || state == HARD_OFF`, returning before `noteRecentPut()` and the HTTP request: this is the **mid-batch abort guard** that prevents the remaining bulbs in a chained `tickNormal` / `tickWakeRamp` / `tickWindDown` batch from getting PUT after the Core-0 SSE task classifies an Override mid-chain (the task sets `overridePending` the instant it classifies; the SOFT_PAUSE transition itself waits in `evQueue` for the dispatcher).

GET is used once per boot by `bootstrapLightStates()` (a single `GET /clip/v2/resource/light`) to seed `lightCache[]`, and re-run by the SSE task after every reconnect (`bootstrapLightStates(true)`, Stage 3) to resync the cache and enqueue synthetic edge events for on/off flips missed while the stream was down. Between those, the cache is kept fresh by the persistent SSE event stream — no per-tick GETs.

**NVS cert management + pinning (revised July 15):** `ensureBridgeCert()` runs at startup (after NTP sync). Loads cert + expiry from NVS (`Preferences` namespace `"mira"`, keys `hueCert`/`hueCertExpiry`). Re-fetches using `setInsecure()` if absent, expired, or within 30 days of expiry. **CA-mode verification (`setCACert`) is impossible on this stack** — the core always enforces mbedtls hostname matching, Mira dials by IP, and the bridge cert's CN is the bridge ID, so pinned handshakes die on CN mismatch (found live July 15; the old NVS-verify probe had been silently re-fetching every boot because of it). Instead, `peerMatchesPinned()` byte-compares the peer cert against the stored PEM post-handshake on the boot probe and the SSE stream (nothing, including the app key, is sent before it passes); the HTTPClient paths transmit on connect and therefore run insecure-mode TLS, inheriting the boot-time identity check.

**All three migration phases are complete.** See `Markdowns/HANDOFF_v2_migration.md` for the historical plan and `Markdowns/SSE.md` for the SSE subsystem reference. Post-Phase-3 refinements (recent-PUT trajectory ring replacing time-window mute, two-tier tolerance) are documented in `Markdowns/SSE v1.0 Awkward Structure.md` and `Markdowns/SSE.md`.

## Hue API v2 Migration (historical — complete)

All three phases (v1→v2: float bri percent, v2 HTTPS PUT schema + cert handling, SSE stream + `lightCache[]`) and the four post-Phase-3 echo-discrimination refinements are shipped. History and design rationale: `Markdowns/HANDOFF_v2_migration.md` (original plan + what shipped beyond it), `Markdowns/SSE.md` (subsystem reference), `Markdowns/SSE v1.0 Awkward Structure.md` (design critique), `Markdowns/ECHO_DISCRIMINATION.md` (design-space brainstorm).

## millis() Rollover Safety

`millis()` rolls over to zero after ~49.7 days. Subtraction-based elapsed time is safe across rollover due to unsigned wrap. Always use the safe form:

```cpp
// SAFE
if (millis() - startTime >= interval)

// UNSAFE — do not use
if (millis() > startTime + interval)
if (millis() >= someAbsoluteTimestamp)
```

Audit every `millis()` comparison in `main.cpp` and verify it uses the subtraction form before or during the v2 migration. **Audit complete — all comparisons already use safe subtraction form.**

## Implementation gotchas (full narratives: `Markdowns/`, esp. `SSE.md`; the code is the reference)

- **Lux curve** (`src/lightcurve.h`) — the two discontinuities (chest 250 lux, overheads 500) are **intentional**; the bri-jump magnitudes are tuning knobs. Keep the JS duplicates in `dashboard/templates/index.html` and `lux_curve.html` in sync with any curve change.
- **`overheadsOn` / `chestOn` are firmware intent, not physical state** — synced from the cache on every NORMAL/LOCKED_OUT re-entry point to prevent stale edge detection after manual changes during SOFT_PAUSE/WAKE/HARD_OFF.
- **Cycle exclusion is distinct from soft pause** — excluded lights are skipped by all driving states via `cyclePut()` but stay **override-watched** (a Hue-app change still fires the global soft pause); cleared on the daily lockout re-arm.
- **Wind-down chest cut is cache-guarded** — chest goes off at step 60 even if a dashboard step-seek jumps past it.
- **Override detection** — `handleLightUpdate()` classifies every SSE event into one of 10 `EchoOutcome`s; only `Override` fires SOFT_PAUSE. Four layers: recent-PUT trajectory ring → no-op filter → stale-revert filter (re-assert, don't pause) → mid-batch abort guard in `setLight()`/`setLightColor()`. The cache write happens **after** classification (the bridge splits combined changes across events — a cache-merged comparison would false-positive). Diagnostics always on: `sseEventRing[]` (16) + `echoOutcomeCounts[]`; `#define ECHO_TRACE 1` for per-event Serial tracing. Full reference: `Markdowns/SSE.md`.
- **Soft pause duration is mutable** (`softPauseDurationMs`, rewritten by `SET_SOFT_PAUSE_REMAINING` as absolute minutes-from-now) rather than back-solving `softPauseStart` — extends past 60 min would otherwise wrap the unsigned `millis()` math. Resume ramp seeds from the floor lamp's **actual cached state** (R10) and suppresses override detection for its whole duration.
- **Buttons are commented out, not deleted** (`[buttons removed 2026-06]` markers, comment-vs-delete convention); `EvType::ButtonPress` stays in the enum; `forceState()` remains fully live for dashboard commands and sets all per-state entry preconditions.
- **`saveLastState()` NVS persistence is write-only now** — `triggerWake()` no longer reads it (seeds from the floor lamp's actual cached state).


## Web Dashboard

Flask + HTML/CSS/JS frontend, live on Railway (Hobby plan, PostgreSQL for persistence). `sendLog()` in `main.cpp` posts events to the dashboard only. Remote control of all 6 states via pending command queue polled by the firmware's `netTask` every 5 s (`NET_CMD_POLL_MS` — N1 Stage 5; was once per 30 s tick). Demo login (read-only) available via `DEMO_PASSWORD` env var. `/lux` page (lux curve + timeline) is owner-only — not visible to demo users.

See `DASHBOARD.md` for full feature specs, design system, and implementation notes.

**Planned: GitHub Actions selective deploy** — disconnect Railway's built-in git auto-deploy; replace with a GitHub Actions workflow (~20 lines of YAML) that triggers Railway deploys only when dashboard-relevant files change (`app.py`, `requirements.txt`, dashboard HTML/CSS/JS). Firmware changes (`src/`, `platformio.ini`, etc.) must never trigger a Railway deploy. Requires storing a Railway API token as a GitHub Actions secret.

**Open design: Webhooks / external triggers** — inbound webhooks to Flask (`/webhook/external`) so external services can trigger state changes (Google Calendar sleep event → `WIND_DOWN`, Siri Shortcut → any state, IFTTT → arbitrary triggers). The existing `ESP32_API_KEY` bearer token pattern should extend to any inbound webhook endpoints. Connects to the planned Google Calendar integration in `DASHBOARD.md`. No implementation decisions made yet.

## Deprioritized / Maybe Later

- Post-dark lockout — ~2 hr hold after lux first drops ≤10, suppresses dimming before wind-down; deemed unnecessary
- **HD44780 LCD** — attempted 4/20; abandoned. ESP32-S3 outputs 3.3V logic; HD44780 at 5V VDD requires VIH ≥ 3.5V (0.7 × VCC). Signal levels fall below threshold. Level shifting not pursued. No display planned.
- **3-button control scheme** (Mode / Confirm / Cycle) — abandoned alongside LCD. Config screen and simultaneous-press hard off depended on having 3 buttons. Replaced by two-button spec.
- **DAY_TYPES / WORK / RELAXED** — per-weekday wake ramp duration removed; single `WAKE_RAMP_TICKS` constant used instead.