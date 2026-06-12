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

- this may seem silly, but change it so that polls happen on :00 and :30, instead of whenever 
the system reflashes. make it commentable so that it can be ignored for testing.

### Priority Bugs


## Resolved Priority Bugs
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


## Recently completed

- **Stale-revert filter (phantom-override fix)** — third echo-discrimination layer in `handleLightUpdate()`: an event matching a recent PUT's *prior* values (`eventMatchesPriorPut()`, 90 s lookback incl. expired slots) is classified `StaleRevert` instead of `Override` — it's the bridge correcting its optimistic echo after the lamp dropped our PUT, not a user change. The dispatcher re-asserts the slot's target (`reassertRecentPut()`, per-light cooldown) instead of soft-pausing. Root-caused from a June 11 misfire on the Signe floor lamp. `RecentPut` gained `priorOn`; `EchoOutcome` count is now 9. Details in `Markdowns/SSE.md`.
- **stableLuxCount dashboard widget** — NORMAL section shows a 0–59 slider and "reset" button; firmware sends count in status payload; `SET_STABLE_LUX_COUNT` command handler added (clamps to 59); DB column + migration added.
- **Disable mobile zoom** — `user-scalable=no, maximum-scale=1.0` added to all four HTML template viewports.
- **Log timestamps → CST** — all `toLocaleTimeString`/`toLocaleDateString` calls pinned to `America/Chicago` in `index.html` and `logs.html`.

- **Wind-down completion pending button** — pressing `finish` (and slider seeks) now show a "pending" indicator under the slider, like the state buttons, and pin the slider to the requested step until the firmware's next poll applies it.
- **Soft pause slider + "extend" button** — slider scrubs minutes remaining (mutable `softPauseDurationMs`, `SET_SOFT_PAUSE_REMAINING` command); the `+10 min` button sends an *absolute* target (displayed + 10) so rapid taps converge instead of cancelling to a single +10.
- **Wind-down finish button mobile highlight** — added `.finish-btn:active` outside the `@media (hover:hover)` block so touch presses give feedback.
- **Override fire message** — `dumpOverrideDiagnostic()` no longer `sendLog()`s the verbose dump to the dashboard (Serial-only); the concise "Manual override — soft pause" line stays.
- **Individual light on/offs (cycle exclusion)** — a dashboard "Lights in cycle" chip row toggles lights out of Mira's update cycle (`excludedLight[]`, `EXCLUDE_LIGHT`/`INCLUDE_LIGHT`). An excluded light is turned off once and skipped by every driving state, but stays override-watched (a Hue-app change still fires the global soft pause). Cleared on the daily lockout re-arm. Separate from soft pause, which is unchanged.
- **Chest/overhead discontinuity rework** — chest now cuts at 250 lux, overheads at 500 (was a single 200-lux step); two discontinuities, remaining lamps step up to compensate when a bulb leaves. New `chestOn` flag mirrors `overheadsOn`.



## Build & Flash (PlatformIO)

```bash
# Build
pio run

# Build + upload to connected Nano ESP32
pio run --target upload

# Open serial monitor (115200 baud)
pio device monitor

# Build + upload + monitor in one step
pio run --target upload && pio device monitor
```

PlatformIO is the only build system. The IDE is CLion with the PlatformIO plugin. `.pio/libdeps/` is auto-generated — do not edit files there.

## Hardware

| Component | Detail |
|-----------|--------|
| MCU | Arduino Nano ESP32-S3 (ABX00083) |
| Lux sensor | Adafruit VEML7700 — I2C, SDA=A4/GPIO11, SCL=A5/GPIO12 |
| Button (Mode) | D9 (GPIO18), active LOW, internal pull-up — short: NORMAL/pause toggle, long: hard off |
| Button (Cycle) | D10 (GPIO21), active LOW, internal pull-up — short press cycles through all 6 states |
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
- **`BTN_MODE 9`** — D9 (GPIO18); short press: NORMAL/pause toggle; long press: hard off toggle
- **`BTN_CYCLE 10`** — D10 (GPIO21); short press: cycle through all 6 states in order, forcing entry preconditions
- **`LOCKOUT_RESET_HOUR 21`** — 24h hour after which all-lights-off re-arms the morning lockout (9 PM)
- **`SOFT_PAUSE_MS 3600000UL`** — soft pause auto-resume duration (60 min)
- **`WAKE_RAMP_TICKS 40`** — wake ramp duration (20 min at 30 s/tick)
- **Turn-on order (wake):** Floor (tick 0) → Chest + Dresser (`rampBri ≥ 40% && lux ≥ 250`) → Overhead 1+2 (`rampBri ≥ 70% && lux ≥ 500`)
- **Turn-off order (wind-down):** Overhead 1+2 (start) → Chest (midpoint, step 60) → Dresser (end, step 120); Floor stays on at floor brightness (19.7%) as the night-light anchor

## Core Behavior & State Machine

State machine: `enum class State { LOCKED_OUT, NORMAL, WAKE, WIND_DOWN, SOFT_PAUSE, HARD_OFF }` in `main.cpp`. The `loop()` dispatches on `state` via a switch statement. Each state has a dedicated tick function.

1. **LOCKED_OUT** — startup default; no auto-on. `checkFloorState()` watches for a manual **floor-lamp** on-flip (rising edge) to trigger wake. Re-arms from NORMAL/WIND_DOWN when the floor lamp goes off and chest/dresser/ceiling are all confirmed off and `hour >= LOCKOUT_RESET_HOUR`.
2. **WAKE** — `tickWakeRamp()` linearly interpolates bri and ct from the actual **floor-lamp** state at trigger → `luxToTarget(ambientLux)` over `WAKE_RAMP_TICKS` (20 min). The floor lamp leads from tick 0; chest+dresser join when `lux ≥ WAKE_SECONDARY_LUX (250) && rampBri ≥ WAKE_SECONDARY_BRI (40%)`; overheads turn on when `lux ≥ WAKE_OVERHEAD_LUX (500) && rampBri ≥ S2_BRI_LO (70%)`. Both thresholds match their NORMAL counterparts (`WAKE_SECONDARY_LUX` == `S3_LUX_HI` == 250; `WAKE_OVERHEAD_LUX` == `S2_LUX_HI` == 500), so there is no step at the WAKE→NORMAL handoff at any lux level. Hands off to NORMAL on completion.
3. **NORMAL** — `tickNormal()` runs the lux→bri/ct curve, edge detection for the two conditionally-driven bulbs (**chest at `S3_LUX_HI` 250 lux**, **overheads at `S2_LUX_HI` 500 lux** — each cuts below its threshold with the remaining lamps stepping up to compensate), cycle-exclusion enforcement, the stable-lux counter, and override detection each tick. Floor + dresser are the always-driven core (full lux participants); chest joins above 250, overheads above 500. Transitions to WIND_DOWN when stable counter hits 60.
4. **WIND_DOWN** — `tickWindDown()` linearly dims floor+chest+dresser toward floor over 60 min; **chest off at the midpoint (step 60), dresser off at the end (step 120)**, floor lamp holds at floor brightness; transitions to LOCKED_OUT.
5. **SOFT_PAUSE** — `tickSoftPause()` suspends all commands; auto-resumes to NORMAL after `softPauseDurationMs` (seeded to `SOFT_PAUSE_MS` on entry, adjustable at runtime via the dashboard remaining-time slider / `+10 min` extend button — `SET_SOFT_PAUSE_REMAINING`).
6. **HARD_OFF** — loop does nothing; no polling, no commands.

Evening phases are lux-driven (not time-driven) so the system adapts to seasonal sunset variation automatically.

## Hue API Patterns (current — v2 HTTPS + SSE, all 3 phases complete)

Two PUT helpers exist in `main.cpp`:
- `setLight(uuid, on, bri, ct, durationMs)` — white/CT mode; `bri` is `float` (0.0–100.0%, native v2 units); `durationMs` in ms (e.g., `30000` = 30 s poll interval, `1000` = 1 s snap)
- `setLightColor(uuid, on, bri, hueV1, satV1, durationMs)` — HSB color mode; internally converts v1 hue (0–65535) + sat (0–254) to CIE xy via `_hsbToXY()`. Used only for the startup purple flourish.

Both use `WiFiClientSecure` with the NVS-stored bridge TLS cert (`_bridgeCertPem`) and the `hue-application-key` header. Both call `noteRecentPut()` *before* the HTTP request so the SSE handler can recognize echoes of the PUT — see `Markdowns/SSE.md`. Both also **short-circuit at the top** if `state == SOFT_PAUSE || state == HARD_OFF`, returning before `noteRecentPut()` and the HTTP request: this is the **mid-batch abort guard** that prevents the remaining bulbs in a chained `tickNormal` / `tickWakeRamp` / `tickWindDown` batch from getting PUT after an SSE override flips state to SOFT_PAUSE in the middle of the chain (each `setLight*()` tail-calls `sseTick()` to drain echoes, which can fire the override transition between PUTs).

GET is used exactly once per boot, by `bootstrapLightStates()` (a single `GET /clip/v2/resource/light`), to seed `lightCache[]`. After that, the cache is kept fresh by the persistent SSE event stream — no per-tick GETs.

**NVS cert management:** `ensureBridgeCert()` runs at startup (after NTP sync). Loads cert + expiry from NVS (`Preferences` namespace `"mira"`, keys `hueCert`/`hueCertExpiry`). Re-fetches using `setInsecure()` if absent, expired, or within 30 days of expiry. All normal v2 calls use the stored PEM via `client.setCACert()`.

**All three migration phases are complete.** See `Markdowns/HANDOFF_v2_migration.md` for the historical plan and `Markdowns/SSE.md` for the SSE subsystem reference. Post-Phase-3 refinements (recent-PUT trajectory ring replacing time-window mute, two-tier tolerance) are documented in `Markdowns/SSE v1.0 Awkward Structure.md` and `Markdowns/SSE.md`.

## Hue API v2 Migration (historical context — all phases complete)

Mira was originally a Hue v1 (local HTTP) integration. Over three phases the firmware migrated to v2 (local HTTPS + SSE):

- **Phase 1** — `LightTarget.bri` refactored from `uint8_t` (0–254) to `float` (0.0–100.0 %) end-to-end; all curve constants in `lightcurve.h` rewritten in percent.
- **Phase 2** — `setLight()` / `setLightColor()` rewritten to v2 HTTPS JSON schema (`on.on`, `dimming.brightness`, `color_temperature.mirek`, `dynamics.duration`, `color.xy`); NVS-pinned bridge TLS cert via `ensureBridgeCert()`; dashboard `status_snapshots.bri` column migrated to `FLOAT`.
- **Phase 3** — persistent SSE event stream; `bootstrapLightStates()` + `lightCache[]` replace per-tick `getLightState()` polling; override detection moved into `handleLightUpdate()`; `checkOverride()` deleted; tick timing fixed via `tickStart`-anchored wait loop.

**Post-Phase-3 refinements** — four layered changes to echo discrimination + override handling, in chronological order: (1) Phase 3's flat time-window mute (`muteOverride`) was replaced with a per-light **`recentPuts[]` trajectory record** + **two-tier tolerance** (`STATE_TOLERANCE_*` for drift detection, `TRAJECTORY_TOLERANCE_*` for echo discrimination). (2) Single-slot `recentPuts[4]` promoted to a **3-slot ring per light** `recentPuts[LIGHT_COUNT][3]` (the per-light dimension was `[4]` at the time and is now `LIGHT_COUNT`=5 with the floor lamp added) so overlapping in-flight PUTs (wake-ramp tick + state-change PUT, or lux-driven PUT after an overhead toggle) keep their trajectories live simultaneously. (3) Added a **no-op event filter** that fires when the trajectory check fails — if the event values are within `TRAJECTORY_TOLERANCE_*` of the pre-event cache, classify as `NoOpEcho` rather than `Override`. Closes the timing gap on "skipping PUT" ticks where the slot expired before the bridge's late settling confirmation arrived. (4) Added a **mid-batch abort guard** at the top of `setLight()` / `setLightColor()`: early-return if `state == SOFT_PAUSE || state == HARD_OFF`. Closes the structural gap where an SSE-driven override fired *between* PUTs in a chained tick (`tickNormal`'s 4-bulb batch, `tickWakeRamp`, `tickWindDown`) — the override flipped state to SOFT_PAUSE on PUT #N's tail `sseTick()`, but PUTs #N+1..#K still went out, driving the un-PUT bulbs to the now-stale curve target the user just overrode. (These batches now drive 3 lights — floor+chest+dresser — plus overheads, rather than the original bedside+desk+overheads.) Full reference in `Markdowns/SSE.md`; design critique in `Markdowns/SSE v1.0 Awkward Structure.md`; the brainstorm doc that explored the design space is `Markdowns/ECHO_DISCRIMINATION.md`.

The original 3-phase planning doc is preserved in `Markdowns/HANDOFF_v2_migration.md` for historical reference; the post-Phase-3 section there summarizes what shipped beyond the original plan.

### millis() Rollover Safety

`millis()` rolls over to zero after ~49.7 days. Subtraction-based elapsed time is safe across rollover due to unsigned wrap. Always use the safe form:

```cpp
// SAFE
if (millis() - startTime >= interval)

// UNSAFE — do not use
if (millis() > startTime + interval)
if (millis() >= someAbsoluteTimestamp)
```

Audit every `millis()` comparison in `main.cpp` and verify it uses the subtraction form before or during the v2 migration. **Audit complete — all comparisons already use safe subtraction form.**

## Implemented

- **VEML7700 sensor** — `veml.begin()` in setup, `readLux(VEML_LUX_AUTO)` in loop
- **`src/lightcurve.h`** — 4-segment piecewise lux→bri/ct curve with **two intentional discontinuities**: chest joins at `S3_LUX_HI` (250 lux, ~78.7→70.0%) and overheads join at `S2_LUX_HI` (500 lux, ~85.0→69.0%). Active bulbs by band: 0–250 floor+dresser, 250–500 +chest, 500+ +overheads. `LightTarget` struct (`float bri` 0.0–100.0%, `uint16_t ct`); `luxToTarget(float lux)` entry point. All bri constants in percent (native Hue API v2 units). The bri-jump magnitudes are tuning knobs — keep the JS duplicates in `dashboard/templates/index.html` and `lux_curve.html` in sync.
- **State machine** — `enum class State` with 6 states; switch dispatch in `loop()`; dedicated tick functions per state
- **Polling loop** — reads lux every 30 s, sends to active bulbs when `shouldUpdate` (bri change > `STATE_TOLERANCE_BRI` or ct change > `STATE_TOLERANCE_CT`)
- **Dashboard logging** — `sendLog()` posts to Railway dashboard (`/api/log`); logs startup, lux/bri/ct updates, wind-down trigger/milestones/completion, wake start/milestones/completion, soft pause trigger/resume, state transitions
- **`getTimeString()`** — shared time-formatting helper used by `printStatus()` and log messages
- **Overhead off** — edge-detection crossing of `S2_LUX_HI` (500 lux) in NORMAL; CEIL_1+2 turn off descending, turn on ascending; remaining lamps update forced on crossing; `overheadsOn` flag is firmware intent (not physical state) — synced from `cachedLight(LIGHT_CEIL_1).on` on all NORMAL re-entry points to prevent stale state after user manual changes during SOFT_PAUSE, WAKE, or HARD_OFF
- **Chest off** — edge-detection crossing of `S3_LUX_HI` (250 lux) in NORMAL, same pattern as overheads; chest cuts below 250 (floor+dresser step up via the curve discontinuity), re-joins above; `chestOn` flag mirrors `overheadsOn` (firmware intent, synced from `cachedLight(LIGHT_CHEST).on` on every NORMAL/LOCKED_OUT re-entry and set true when WAKE lights the chest)
- **Cycle exclusion (individual light on/offs)** — `excludedLight[LIGHT_COUNT]` flag, toggled by the dashboard "Lights in cycle" chips (`EXCLUDE_LIGHT`/`INCLUDE_LIGHT`, value = cache index). An excluded light is turned off once by `enforceExclusions()` (cache-guarded) and skipped by every driving state via the `cyclePut()` wrapper (exclusion-aware `setLight`); INCLUDE resets `sentTarget` so it re-joins next tick. Excluded lights stay **override-watched** (`expectedOn` forced true) so a manual Hue-app change still fires the global soft pause. Cleared on the daily lockout re-arm. Reported to the dashboard in `/api/status` (`excluded.{floor,chest,dresser,ceiling}`). **Distinct from soft pause** — soft pause is unchanged.
- **Wind-down** — stable lux counter (lux ≤ 10, hour ≥ 21, 60 readings); 60-min linear dim on floor+chest+dresser to floor bri/ct=warm; **chest off at the midpoint (step 60, cache-guarded so a dashboard step-seek past 60 still kills it), dresser off at the end (step 120)**; floor lamp holds at floor brightness as the night-light anchor; `dynamics.duration=30000` matches poll interval for seamless gradient; `tickWindDown()` in `main.cpp`
- **Wake sequence** — `triggerWake(lux)` reads actual **floor-lamp** state via `cachedLight(LIGHT_FLOOR)` at trigger time to seed `wakeStartTarget`; `wakeEndTarget = luxToTarget(ambientLux)`; `tickWakeRamp()` linearly interpolates bri and ct over `wakeStep / WAKE_RAMP_TICKS`; the **floor lamp turns on every tick (leads the ramp)**, chest+dresser join when `lux >= WAKE_SECONDARY_LUX (250) && rampBri >= WAKE_SECONDARY_BRI (40%)`, overheads only when `lux >= WAKE_OVERHEAD_LUX (600) && rampBri >= S2_BRI_LO (70%)`; `dynamics.duration=30000` matches poll interval for seamless gradient; `WAKE_RAMP_TICKS=40` (20 min); logged on start, 25/50/75%, and completion
- **Morning lockout** — `state = LOCKED_OUT` at boot; `checkFloorState()` polls the floor lamp each tick; rising-edge detection triggers `triggerWake()`; re-arms when the floor lamp goes off and chest/dresser/ceiling are confirmed off after `LOCKOUT_RESET_HOUR`; resets `stableLuxCount` and `windDownStep` on re-arm; `lastFloorOn` is an edge-detector accumulator (not physical state) — synced from bridge on all NORMAL and LOCKED_OUT re-entry points to prevent false wake triggers or false lockout re-arms after user changes during SOFT_PAUSE, WAKE, or HARD_OFF
- **`Preferences` last-state persistence** — `saveLastState(bri, ct)` writes to NVS on every bulb update and at wind-down completion; no longer read by `triggerWake()` (actual floor-lamp state used instead)
- **Override detection (SSE, three-layer: trajectory ring + no-op filter + stale-revert filter, with mid-batch abort)** — implemented in `handleLightUpdate()` on every incoming SSE light event. Classifies each event into one of 9 `EchoOutcome` values (`UnknownUuid`, `NoFields`, `SkipState`, `SkipPauseResume`, `SkipOffLight`, `EchoMatch`, `NoOpEcho`, `StaleRevert`, `Override`); only `Override` triggers `SOFT_PAUSE`. **Layer 1 — trajectory ring**: each outgoing PUT calls `noteRecentPut(idx, on, bri, ct, durationMs)` before the HTTP request, snapshotting `(priorBri/Ct from cache, targetBri/Ct, postedAtMs, durationMs)` into one of 3 slots in `recentPuts[idx][RECENT_PUT_RING_SIZE]` (prefers inactive/expired; overwrites oldest only when all 3 are live). `eventMatchesRecentPut()` iterates all 3 slots and returns true if any live slot's trajectory `[min(prior, target), max(prior, target)] ± TRAJECTORY_TOLERANCE_*` covers every present field; first match wins. **Layer 2 — no-op event filter**: if the trajectory check fails, before declaring `Override` the handler checks whether the event's values are within `TRAJECTORY_TOLERANCE_*` of the **pre-event cache** (`cacheOnBefore/cacheBriBefore/cacheCtBefore`, snapshotted before the cache write). If so → `NoOpEcho`, not `Override`. Closes the timing gap on "skipping PUT" ticks where the bridge's late settling confirmation event arrives after the slot's `durationMs + RECENT_PUT_GRACE_MS` has expired. **Layer 3 — stale-revert filter**: if the no-op check also fails, `eventMatchesPriorPut()` compares the event against the *prior* (pre-PUT) values (`priorOn/priorBri/priorCt`) of every slot posted within `STALE_REVERT_LOOKBACK_MS` (90 s), **including expired slots** (slot data survives the `active=false` sweep). A match means the lamp never applied one of our PUTs and the bridge is correcting its optimistic echo at its next lamp poll (~35 s out — observed June 11 on the Signe: a dashboard resume PUT was dropped Zigbee-side; the correction landed on exactly the slot's priorBri/priorCt and fired a phantom soft pause). → `StaleRevert`: the dispatcher calls `reassertRecentPut()` to re-PUT the slot's *target* (500 ms snap, fresh trajectory slot covers the retry's echo), with a per-light cooldown (one re-assert per lookback window) so a persistently deaf lamp doesn't ping-pong — no `overridePending`, no pause. Fingerprint rationale: a user override lands on arbitrary values; a revert lands exactly where our own PUT started. Accepted trade-off: a user manually returning a lamp to its pre-PUT value within 90 s is re-asserted once; their next different adjustment fires `Override` normally. **Diagnostics (always on)**: every classified event is appended to a 16-entry global ring `sseEventRing[]` with pre-event cache snapshot and outcome; `echoOutcomeCounts[]` keeps a cumulative histogram since boot; on `Override` fire, `dumpOverrideDiagnostic()` ships a multi-line dump to the dashboard via `sendLog()` including the triggering event, every active recentPuts slot, the last 6 events for this idx, and the outcome histogram. Set `#define ECHO_TRACE 1` near the `EchoOutcome` enum for per-event Serial logging during tuning. Cache write happens *after* classification so `cacheBefore` stays meaningful for both the diagnostic record and the no-op filter; the override check itself never reads the cache (the bridge splits combined state changes across multiple events, so cache-merged comparison would generate false positives). **Layer 4 — mid-batch abort**: both `setLight()` and `setLightColor()` early-return at the top if `state == SOFT_PAUSE || state == HARD_OFF`. Because each PUT helper tail-calls `sseTick()` to drain echoes, an Override that fires after PUT #N in a `tickNormal` 4-bulb chain (or any other batched caller) flips state synchronously before PUT #N+1; the guard short-circuits the remaining PUTs so they don't drive the un-PUT bulbs to the now-stale lux-curve target the user just overrode. Closed-loop, per-light, replaces the prior `muteOverride()` time-window mute and the old `sentTarget`/`prevSentTarget` double-buffer; `STATE_TOLERANCE_*` is no longer used for override comparison. Full reference: `Markdowns/SSE.md`.
- **Soft pause** — `tickSoftPause()` auto-resumes to NORMAL after `softPauseDurationMs` (seeded to `SOFT_PAUSE_MS` 60 min on entry; the dashboard remaining-time slider and `+10 min` extend button both send `SET_SOFT_PAUSE_REMAINING` (absolute minutes-from-now) to rewrite it — kept mutable rather than back-solving `softPauseStart` so extends past 60 min don't wrap the unsigned `millis()` math); resume ramp interpolates from pre-pause bri/ct to current ambient over 10 min (`PAUSE_RESUME_TICKS=20`); override detection suppressed for entire resume ramp duration; `overheadsOn` and `lastFloorOn` synced from bridge on auto-resume (and on all other NORMAL/LOCKED_OUT re-entry points) to prevent stale edge-detection state after user manual changes during the pause window
- **Two buttons** — both active LOW, internal pull-up. `ButtonState` struct tracks debounce and press classification. `pollButton()` called every 50ms inside non-blocking wait loop for each button.
  - **BTN_MODE (D9/GPIO18):** `handleButtonEvents()` dispatches actions. Short press: if not NORMAL → go to NORMAL (resets `sentTarget` to sentinel, syncs `overheadsOn` and `lastFloorOn` from bridge); if NORMAL → SOFT_PAUSE. Long press (700ms): hard off toggle; HARD_OFF → LOCKED_OUT path syncs `lastFloorOn` from bridge.
  - **BTN_CYCLE (D10/GPIO21):** `handleCycleButton()` dispatches. Short press: advances `(int)state + 1) % 6` and calls `forceState(next)`. `forceState()` sets all required entry preconditions per state (seeds `windDownStartBri`, calls `triggerWake()`, resets `stableLuxCount`, resets `sentTarget` sentinel on NORMAL, etc.). Logged for all transitions except WAKE (which `triggerWake()` already logs).
  - `BTN_MODE`, `BTN_CYCLE`, `DEBOUNCE_MS`, `LONG_PRESS_MS` defined in `config.h`.
- **Non-blocking main loop** — `delay(30000)` replaced with a `while (millis() - tickStart < 30000UL)` loop that polls the button every 50ms, keeping the system responsive between lux ticks.
- **Wake progress logging** — `tickWakeRamp()` prints `Wake A/B — bri=X/Y` each tick (current step / total ticks, current ramp bri / end target bri)
- **Wind-down progress logging** — `tickWindDown()` prints `Wind-down X.X/60.0 min — bri=N` each tick (elapsed minutes at 0.5 min/tick)
- **Hue API v2 helpers** — `setLight(uuid, on, bri, ct, durationMs)` and `setLightColor(uuid, on, bri, hueV1, satV1, durationMs)` send v2 HTTPS JSON (`on.on`, `dimming.brightness`, `color_temperature.mirek` or `color.xy`, `dynamics.duration` in ms). Both call `noteRecentPut()` before issuing the PUT so the SSE handler can recognize the resulting echoes — see `Markdowns/SSE.md`. Both short-circuit before `noteRecentPut()` if `state == SOFT_PAUSE || state == HARD_OFF` (mid-batch abort guard — see "Override detection" bullet above). `_bridgeCertPem` global holds the PEM loaded from NVS; `ensureBridgeCert()` in `setup()` auto-fetches and caches the bridge TLS cert. `_hsbToXY()` converts v1 HSB to CIE xy for color mode.
- **SSE event stream** — persistent HTTPS connection (`sseClient` global, `sseTick()` drained from the wait loop) that pushes bridge state changes; updates `lightCache[4]` and runs override detection in `handleLightUpdate()`. `bootstrapLightStates()` seeds the cache once at startup via a single GET, then SSE keeps it fresh. Full reference in `Markdowns/SSE.md`.
- **Dashboard bri migration** — `status_snapshots.bri` column type migrated to `FLOAT`; startup migration SQL converts historical v1 rows (`bri > 100`) to percent on first deploy. Both JS lux curve calculators (`index.html`, `lux_curve.html`) use v2 percent constants; bri displays show `%` suffix.


## Web Dashboard

Flask + HTML/CSS/JS frontend, live on Railway (Hobby plan, PostgreSQL for persistence). `sendLog()` in `main.cpp` posts events to the dashboard only. Remote control of all 6 states via pending command queue polled by firmware on each tick. Demo login (read-only) available via `DEMO_PASSWORD` env var. `/lux` page (lux curve + timeline) is owner-only — not visible to demo users.

See `DASHBOARD.md` for full feature specs, design system, and implementation notes.

**Planned: GitHub Actions selective deploy** — disconnect Railway's built-in git auto-deploy; replace with a GitHub Actions workflow (~20 lines of YAML) that triggers Railway deploys only when dashboard-relevant files change (`app.py`, `requirements.txt`, dashboard HTML/CSS/JS). Firmware changes (`src/`, `platformio.ini`, etc.) must never trigger a Railway deploy. Requires storing a Railway API token as a GitHub Actions secret.

**Open design: Webhooks / external triggers** — inbound webhooks to Flask (`/webhook/external`) so external services can trigger state changes (Google Calendar sleep event → `WIND_DOWN`, Siri Shortcut → any state, IFTTT → arbitrary triggers). The existing `ESP32_API_KEY` bearer token pattern should extend to any inbound webhook endpoints. Connects to the planned Google Calendar integration in `DASHBOARD.md`. No implementation decisions made yet.

## Deprioritized / Maybe Later

- Post-dark lockout — ~2 hr hold after lux first drops ≤10, suppresses dimming before wind-down; deemed unnecessary
- **HD44780 LCD** — attempted 4/20; abandoned. ESP32-S3 outputs 3.3V logic; HD44780 at 5V VDD requires VIH ≥ 3.5V (0.7 × VCC). Signal levels fall below threshold. Level shifting not pursued. No display planned.
- **3-button control scheme** (Mode / Confirm / Cycle) — abandoned alongside LCD. Config screen and simultaneous-press hard off depended on having 3 buttons. Replaced by two-button spec.
- **DAY_TYPES / WORK / RELAXED** — per-weekday wake ramp duration removed; single `WAKE_RAMP_TICKS` constant used instead.

## Libraries

| Library | Purpose |
|---------|---------|
| `Adafruit VEML7700` | Lux sensor over I2C |
| `Adafruit BusIO` | I2C/SPI abstraction (VEML7700 dependency) |
| `NTPClient` | Wall-clock time via UDP |
| `ArduinoJson` | Parse Hue Bridge JSON responses |
| `WiFi`, `HTTPClient` | ESP32 built-ins for Hue REST calls |
| `Preferences` | NVS flash storage — used for last-state bri/ct |
| `LiquidCrystal` | HD44780 LCD (built-in, not yet wired up) |