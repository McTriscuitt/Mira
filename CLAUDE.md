 # CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project: Mira

Adaptive smart bedroom lighting firmware running on an **Arduino Nano ESP32-S3 (ABX00083)**. It reads ambient lux via a VEML7700 sensor and adjusts 4 Philips Hue bulbs through the local Hue Bridge HTTP REST API.

## User Notes
Behaviors
- all .md's for error parsing and rectifying are in \Mira\Stored Data\
- on /recap, update documentation



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
- **`STATE_TOLERANCE 3`** — min bri or ct delta before a PUT is sent to the bridge; also used for override detection window
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

## Hue API Patterns

Two PUT helpers exist in `main.cpp`:
- `setLight(id, on, bri, ct, transitiontime)` — white/color-temperature mode
- `setLightColor(id, on, bri, hue, sat, transitiontime)` — HSB color mode

`transitiontime` is in units of 100 ms (e.g., `10` = 1 s). GET state via `getLightState(id)` which returns a `LightState {on, bri, ct}`.

## Implemented

- **VEML7700 sensor** — `veml.begin()` in setup, `readLux(VEML_LUX_AUTO)` in loop
- **`src/lightcurve.h`** — 4-segment piecewise lux→bri/ct curve with intentional discontinuity at 250 lux; `LightTarget` struct (`uint8_t bri`, `uint16_t ct`); `luxToTarget(float lux)` entry point. See `LIGHTCURVE.md` for curve math and tuning.
- **State machine** — `enum class State` with 6 states; switch dispatch in `loop()`; dedicated tick functions per state
- **Polling loop** — reads lux every 30 s, sends to active bulbs when `shouldUpdate` (change > `STATE_TOLERANCE`)
- **Dashboard logging** — `sendLog()` posts to Railway dashboard (`/api/log`); logs startup, lux/bri/ct updates, wind-down trigger/milestones/completion, wake start/milestones/completion, soft pause trigger/resume, state transitions
- **`getTimeString()`** — shared time-formatting helper used by `printStatus()` and log messages
- **Overhead off** — edge-detection crossing of `S3_LUX_HI` (250 lux); CEIL_1+2 turn off descending, turn on ascending; bedside+desk update forced on crossing
- **Wind-down** — stable lux counter (lux ≤ 10, hour ≥ 21, 60 readings); 60-min linear dim on bedside+desk to bri=50/ct=400; desk off at end; `transitiontime=300` matches poll interval for seamless gradient; `tickWindDown()` in `main.cpp`
- **Wake sequence** — `triggerWake(lux)` reads actual bedside state via `getLightState()` at trigger time to seed `wakeStartTarget`; `wakeEndTarget = luxToTarget(ambientLux)`; `tickWakeRamp()` linearly interpolates bri and ct over `wakeStep / WAKE_RAMP_TICKS`; bedside and desk turn on every tick, overheads only when `lux >= S3_LUX_HI && rampBri >= S2_BRI_LO`; `transitiontime=300` matches poll interval for seamless gradient; `WAKE_RAMP_TICKS=40` (20 min); logged on start, 25/50/75%, and completion
- **Morning lockout** — `state = LOCKED_OUT` at boot; `checkBedsideState()` polls bedside each tick; rising-edge detection triggers `triggerWake()`; re-arms when all 4 lights confirmed off after `LOCKOUT_RESET_HOUR`; resets `stableLuxCount` and `windDownStep` on re-arm
- **`Preferences` last-state persistence** — `saveLastState(bri, ct)` writes to NVS on every bulb update and at wind-down completion; no longer read by `triggerWake()` (actual bedside state used instead)
- **Override detection** — `checkOverride()` called at top of `tickNormal()`, skipped while `pauseResumeActive` is true; polls all active bulbs via `getLightState()`; `isManualOverride(ls, expectedOn)` returns true if light was turned off manually or bri/ct matches neither `lastTarget` nor `prevTarget` within `STATE_TOLERANCE`; triggers `SOFT_PAUSE` on detection. `prevTarget` holds the value of `lastTarget` before the most recent PUT — prevents false override triggers when a bulb is slow to apply an update (Hue RF lag) while still detecting real overrides even when lux simultaneously changes.
- **Soft pause** — `tickSoftPause()` auto-resumes to NORMAL after `SOFT_PAUSE_MS` (60 min); resume ramp interpolates from pre-pause bri/ct to current ambient over 10 min (`PAUSE_RESUME_TICKS=20`); override detection suppressed for entire resume ramp duration
- **Two buttons** — both active LOW, internal pull-up. `ButtonState` struct tracks debounce and press classification. `pollButton()` called every 50ms inside non-blocking wait loop for each button.
  - **BTN_MODE (D9/GPIO18):** `handleButtonEvents()` dispatches actions. Short press: if not NORMAL → go to NORMAL (with `skipOverrideCheck = true`); if NORMAL → SOFT_PAUSE. Long press (700ms): hard off toggle.
  - **BTN_CYCLE (D10/GPIO21):** `handleCycleButton()` dispatches. Short press: advances `(int)state + 1) % 6` and calls `forceState(next)`. `forceState()` sets all required entry preconditions per state (seeds `windDownStartBri`, calls `triggerWake()`, resets `stableLuxCount` and `skipOverrideCheck`, etc.). Logged for all transitions except WAKE (which `triggerWake()` already logs).
  - `BTN_MODE`, `BTN_CYCLE`, `DEBOUNCE_MS`, `LONG_PRESS_MS` defined in `config.h`.
- **Non-blocking main loop** — `delay(30000)` replaced with a `while (millis() - tickStart < 30000UL)` loop that polls the button every 50ms, keeping the system responsive between lux ticks.
- **Wake progress logging** — `tickWakeRamp()` prints `Wake A/B — bri=X/Y` each tick (current step / total ticks, current ramp bri / end target bri)
- **Wind-down progress logging** — `tickWindDown()` prints `Wind-down X.X/60.0 min — bri=N` each tick (elapsed minutes at 0.5 min/tick)


## Web Dashboard

Flask + HTML/CSS/JS frontend, live on Railway (Hobby plan, PostgreSQL for persistence). `sendLog()` in `main.cpp` posts events to the dashboard only. Remote control of all 6 states via pending command queue polled by firmware on each tick. Demo login (read-only) available via `DEMO_PASSWORD` env var.

See `DASHBOARD.md` for full feature specs, design system, and implementation notes.

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