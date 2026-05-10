 # CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project: Mira

Adaptive smart bedroom lighting firmware running on an **Arduino Nano ESP32-S3 (ABX00083)**. It reads ambient lux via a VEML7700 sensor and adjusts 4 Philips Hue bulbs through the local Hue Bridge HTTP REST API.

## User Notes
Behaviors
- all .md's for error parsing and rectifying are in \Mira\Stored Data\
- all .md's for information on the system code/upgrades/etc are in \mira\Markdowns
- on /recap, update documentation

- stableLuxCount on dashboard
  - editable
  - resetable
  - slider
- add a "recruiter" role so that recruiters from companies can see what owner role see, but cannot edit. 
  - in between demo and owner permissions 
  - no hidden owner IP button allowed for recruiter role
  - 
- disable zooming on mobile
- create section/page for serial-like output?
  - would that be too intense and slow the whole system down substantially? 
- PRIORITY
  - rework files to accomodate dashboard being integrated into main "portfolio" page
    - railway will become the landing page, with /mira and /tempproject coming from that (something like that)  

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
| Bulbs | 4× Philips Hue E26 White & Color Ambiance (bri 0–254, ct 153–447 Mired) |
| Onboard RGB | Active LOW (LOW = on, HIGH = off) |

**LCD was attempted and abandoned** — HD44780 requires 3.5V logic minimum (0.7 × 5V VDD) but ESP32-S3 outputs 3.3V. Level shifting was not pursued. No display is planned.

## Key Configuration (`src/config.h`)

`config.h` holds WiFi credentials and Hue API details. It is checked into the repo but **must not be shared publicly**.

- **Hue Bridge:** `192.168.1.186`, API user token in `HUE_BASE_URL`
- **Light IDs:** `LIGHT_BEDSIDE=3`, `LIGHT_DESK=4`, `LIGHT_CEIL_1=1`, `LIGHT_CEIL_2=2`
- **UTC offset:** `UTC_OFFSET_SEC` — currently `-14400` (UTC-4 / Eastern Daylight)
- **`STATE_TOLERANCE_BRI 1.2f`** — min bri delta (percent) before a PUT is sent; also used for override detection window (≈ 3/254 in old v1 units)
- **`STATE_TOLERANCE_CT 3`** — min ct delta (mirek) before a PUT is sent; also used for override detection window
- **`BTN_MODE 9`** — D9 (GPIO18); short press: NORMAL/pause toggle; long press: hard off toggle
- **`BTN_CYCLE 10`** — D10 (GPIO21); short press: cycle through all 6 states in order, forcing entry preconditions
- **`LOCKOUT_RESET_HOUR 21`** — 24h hour after which all-lights-off re-arms the morning lockout (9 PM)
- **`SOFT_PAUSE_MS 3600000UL`** — soft pause auto-resume duration (60 min)
- **`WAKE_RAMP_TICKS 40`** — wake ramp duration (20 min at 30 s/tick)
- **Turn-on order:** Bedside → Desk → Overhead 1+2
- **Turn-off order:** Overhead 1+2 → Desk → Bedside

## Core Behavior & State Machine

State machine: `enum class State { LOCKED_OUT, NORMAL, WAKE, WIND_DOWN, SOFT_PAUSE, HARD_OFF }` in `main.cpp`. The `loop()` dispatches on `state` via a switch statement. Each state has a dedicated tick function.

1. **LOCKED_OUT** — startup default; no auto-on. `checkBedsideState()` watches for manual bedside on-flip to trigger wake. Re-arms from NORMAL/WIND_DOWN when all lights confirmed off and `hour >= LOCKOUT_RESET_HOUR`.
2. **WAKE** — `tickWakeRamp()` linearly interpolates bri and ct from the actual bedside state at trigger → `luxToTarget(ambientLux)` over `WAKE_RAMP_TICKS` (20 min). Overheads turn on mid-ramp when `lux >= S3_LUX_HI && rampBri >= S2_BRI_LO`. Hands off to NORMAL on completion.
3. **NORMAL** — `tickNormal()` runs the lux→bri/ct curve, overhead edge detection, stable-lux counter, and override detection each tick. Transitions to WIND_DOWN when stable counter hits 60.
4. **WIND_DOWN** — `tickWindDown()` linearly dims bedside+desk to floor over 60 min; desk off at step 120; transitions to LOCKED_OUT.
5. **SOFT_PAUSE** — `tickSoftPause()` suspends all commands; auto-resumes to NORMAL after `SOFT_PAUSE_MS`.
6. **HARD_OFF** — loop does nothing; no polling, no commands.

Evening phases are lux-driven (not time-driven) so the system adapts to seasonal sunset variation automatically.

## Hue API Patterns (current — v1 with Phase 1 shim)

Two PUT helpers exist in `main.cpp`:
- `setLight(id, on, bri, ct, transitiontime)` — white/color-temperature mode; `bri` is `float` (0.0–100.0%), converted to v1 int internally via `bri * 2.54f`
- `setLightColor(id, on, bri, hue, sat, transitiontime)` — HSB color mode; same `bri` shim applies

`transitiontime` is in units of 100 ms (e.g., `10` = 1 s). GET state via `getLightState(id)` which returns a `LightState {on, bri, ct}` where `bri` is `float` (0.0–100.0%), scaled from the v1 int response via `bri / 2.54f`.

**A full v1 → v2 migration is in progress.** See the section below. Phase 1 (float bri refactor) is complete — the shims above will be removed in Phase 2 when the API calls switch to HTTPS v2.

## Planned: Hue API v2 Migration

Migration from v1 (local HTTP, integer light IDs) to v2 (local HTTPS, UUID light IDs, SSE events). **Phase 1 complete** — see `Markdowns/HANDOFF_v2_migration.md` for the full 3-phase plan and per-phase scope.

### Why

- v2 supports Server-Sent Events (SSE) from the bridge — instant override detection instead of polling
- Current `checkOverride()` fires up to 4 GET requests per tick, causing ~45 s actual tick intervals instead of 30 s
- SSE eliminates all GET polling; bridge pushes events over a persistent connection

### HTTPS / Cert Management

Do **not** use `setInsecure()` for normal operation. Target architecture — fully automatic via NVS:

1. On startup, load stored cert + expiry timestamp from NVS (`Preferences`)
2. If no cert, or expired, or within 30 days of expiry → fetch from bridge using `setInsecure()` for that one call only, then store new cert + `notAfter` timestamp to NVS
3. All normal Hue connections use NVS cert via `client.setCACert()`
4. Expiry check uses NTP time (already available)

The single insecure fetch is acceptable risk — local LAN only, only when no valid cert is stored.

### Decisions Made

- **`LightTarget.bri` → `float` (0.0–100.0%)** — refactor throughout entire codebase (`luxToTarget()`, ramp math, tolerance comparisons, all of it). No conversion shim at the PUT boundary; all math works in percent end to end
- **`dynamics.duration` replaces `transitiontime`** — units change from 100 ms to ms
- **`ct`/`mirek`** — same unit, no change
- **Light UUIDs** — do a one-time manual discovery (`GET https://192.168.1.186/clip/v2/resource/light`) to retrieve the four UUIDs, hardcode in `config.h` alongside existing integer light defines. UUIDs are stable (Zigbee identity); only change if a bulb is factory-reset and re-paired
- **SSE stream** — `GET https://192.168.1.186/eventstream/clip/v2` with `hue-application-key` header; read inside the existing non-blocking wait loop alongside button polling
- **Double-buffer (`sentTarget`/`prevSentTarget`) removed** once SSE is in place — override detection triggers on event payload, not polled comparison
- **Tick timing fix** — subtract processing time from the 30 s wait so intervals are consistently 30 s:

```cpp
unsigned long tickStart = millis();
// ... all tick processing ...
unsigned long elapsed = millis() - tickStart;
unsigned long waitTime = (elapsed < 30000UL) ? (30000UL - elapsed) : 0;
// wait waitTime instead of flat 30000
```

### Scope of Changes

| Area | Change |
|---|---|
| `config.h` | New base URL, separate `HUE_API_KEY` define, UUID defines for each bulb |
| NVS cert storage | New cert fetch/store/expiry helper |
| `LightTarget` struct | `bri` → `float` |
| `lightcurve.h` | All curve constants and `luxToTarget()` output in 0.0–100.0 range |
| `setLight()` / `setLightColor()` | v2 JSON schema, HTTPS client, `hue-application-key` header |
| `getLightState()` | Removed once SSE is in place |
| `checkOverride()` | Replaced by SSE event handler |
| Non-blocking wait loop | Add SSE stream reader + tick timing correction |
| Full codebase | `millis()` rollover audit (see below) |

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
- **`src/lightcurve.h`** — 4-segment piecewise lux→bri/ct curve with intentional discontinuity at 200 lux; `LightTarget` struct (`float bri` 0.0–100.0%, `uint16_t ct`); `luxToTarget(float lux)` entry point. All bri constants in percent (native Hue API v2 units).
- **State machine** — `enum class State` with 6 states; switch dispatch in `loop()`; dedicated tick functions per state
- **Polling loop** — reads lux every 30 s, sends to active bulbs when `shouldUpdate` (bri change > `STATE_TOLERANCE_BRI` or ct change > `STATE_TOLERANCE_CT`)
- **Dashboard logging** — `sendLog()` posts to Railway dashboard (`/api/log`); logs startup, lux/bri/ct updates, wind-down trigger/milestones/completion, wake start/milestones/completion, soft pause trigger/resume, state transitions
- **`getTimeString()`** — shared time-formatting helper used by `printStatus()` and log messages
- **Overhead off** — edge-detection crossing of `S3_LUX_HI` (250 lux); CEIL_1+2 turn off descending, turn on ascending; bedside+desk update forced on crossing; `overheadsOn` flag is firmware intent (not physical state) — synced from actual bridge state (`getLightState(LIGHT_CEIL_1).on`) on all NORMAL re-entry points to prevent stale state after user manual changes during SOFT_PAUSE, WAKE, or HARD_OFF
- **Wind-down** — stable lux counter (lux ≤ 10, hour ≥ 21, 60 readings); 60-min linear dim on bedside+desk to bri=50/ct=400; desk off at end; `transitiontime=300` matches poll interval for seamless gradient; `tickWindDown()` in `main.cpp`
- **Wake sequence** — `triggerWake(lux)` reads actual bedside state via `getLightState()` at trigger time to seed `wakeStartTarget`; `wakeEndTarget = luxToTarget(ambientLux)`; `tickWakeRamp()` linearly interpolates bri and ct over `wakeStep / WAKE_RAMP_TICKS`; bedside and desk turn on every tick, overheads only when `lux >= S3_LUX_HI && rampBri >= S2_BRI_LO`; `transitiontime=300` matches poll interval for seamless gradient; `WAKE_RAMP_TICKS=40` (20 min); logged on start, 25/50/75%, and completion
- **Morning lockout** — `state = LOCKED_OUT` at boot; `checkBedsideState()` polls bedside each tick; rising-edge detection triggers `triggerWake()`; re-arms when all 4 lights confirmed off after `LOCKOUT_RESET_HOUR`; resets `stableLuxCount` and `windDownStep` on re-arm; `lastBedsideOn` is an edge-detector accumulator (not physical state) — synced from bridge on all NORMAL and LOCKED_OUT re-entry points to prevent false wake triggers or false lockout re-arms after user changes during SOFT_PAUSE, WAKE, or HARD_OFF
- **`Preferences` last-state persistence** — `saveLastState(bri, ct)` writes to NVS on every bulb update and at wind-down completion; no longer read by `triggerWake()` (actual bedside state used instead)
- **Override detection** — `checkOverride()` called at top of `tickNormal()`, skipped while `pauseResumeActive` is true; polls all active bulbs via `getLightState()`; `isManualOverride(ls, expectedOn)` returns true if light was turned off manually or bri/ct matches neither `sentTarget` nor `prevSentTarget` within `STATE_TOLERANCE_BRI`/`STATE_TOLERANCE_CT`; triggers `SOFT_PAUSE` on detection. `prevSentTarget` holds the value of `sentTarget` before the most recent PUT — prevents false override triggers when a bulb is slow to apply an update (Hue RF lag) while still detecting real overrides even when lux simultaneously changes. Will be replaced by SSE event handler in Phase 3.
- **Soft pause** — `tickSoftPause()` auto-resumes to NORMAL after `SOFT_PAUSE_MS` (60 min); resume ramp interpolates from pre-pause bri/ct to current ambient over 10 min (`PAUSE_RESUME_TICKS=20`); override detection suppressed for entire resume ramp duration; `overheadsOn` and `lastBedsideOn` synced from bridge on auto-resume (and on all other NORMAL/LOCKED_OUT re-entry points) to prevent stale edge-detection state after user manual changes during the pause window
- **Two buttons** — both active LOW, internal pull-up. `ButtonState` struct tracks debounce and press classification. `pollButton()` called every 50ms inside non-blocking wait loop for each button.
  - **BTN_MODE (D9/GPIO18):** `handleButtonEvents()` dispatches actions. Short press: if not NORMAL → go to NORMAL (syncs `overheadsOn` and `lastBedsideOn` from bridge, sets `skipOverrideCheck = true`); if NORMAL → SOFT_PAUSE. Long press (700ms): hard off toggle; HARD_OFF → LOCKED_OUT path syncs `lastBedsideOn` from bridge.
  - **BTN_CYCLE (D10/GPIO21):** `handleCycleButton()` dispatches. Short press: advances `(int)state + 1) % 6` and calls `forceState(next)`. `forceState()` sets all required entry preconditions per state (seeds `windDownStartBri`, calls `triggerWake()`, resets `stableLuxCount` and `skipOverrideCheck`, etc.). Logged for all transitions except WAKE (which `triggerWake()` already logs).
  - `BTN_MODE`, `BTN_CYCLE`, `DEBOUNCE_MS`, `LONG_PRESS_MS` defined in `config.h`.
- **Non-blocking main loop** — `delay(30000)` replaced with a `while (millis() - tickStart < 30000UL)` loop that polls the button every 50ms, keeping the system responsive between lux ticks.
- **Wake progress logging** — `tickWakeRamp()` prints `Wake A/B — bri=X/Y` each tick (current step / total ticks, current ramp bri / end target bri)
- **Wind-down progress logging** — `tickWindDown()` prints `Wind-down X.X/60.0 min — bri=N` each tick (elapsed minutes at 0.5 min/tick)


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