# Mira Project — Claude Code Context

This file provides full project context for AI-assisted development.
Read this before writing or modifying any code.

---
## User Notes



---

## Project Summary

**Mira** is an adaptive smart lighting system for a bedroom. An Arduino Nano ESP32-S3
reads ambient lux every 30 seconds from a VEML7700 sensor and automatically adjusts
Philips Hue bulbs (brightness + color temperature) via the local Hue Bridge HTTP API.
The system includes 2 buttons for manual control.

---

## Hardware Status

All hardware has arrived as of April 2026. The VEML7700 sensor, Nano ESP32-S3, and single tactile button (D9) are physically connected and confirmed working. The LCD was attempted and abandoned — see Deprioritized section.

---

## Hardware

| Component     | Details                                                                                  |
|---------------|------------------------------------------------------------------------------------------|
| Microcontroller | Arduino Nano ESP32-S3 with headers (ABX00083)                                            |
| Lux Sensor    | Adafruit VEML7700 breakout (#4162, Stemma QT)                                            |
| Smart Bulbs   | Philips Hue Essential Starter Kit — Bridge + 4x E26 White & Color Ambiance (2200K–6500K) |
| Display       | HD44780 16x2 LCD — attempted and abandoned (3.3V logic incompatible with 5V VDD)         |
| Button (Mode)  | tactile push button (D9/GPIO18) — short press: NORMAL/SOFT_PAUSE toggle; long press: HARD_OFF toggle |
| Button (Cycle) | tactile push button (D10/GPIO21) — short press: cycle through all 6 states in order, forcing entry preconditions |

---

## Physical Setup Notes

- Nano ESP32-S3 is seated in a breadboard via its pre-soldered headers
- VEML7700 sensor has no headers soldered (no soldering equipment available)
- Sensor is connected via its Stemma QT JST connector using a Stemma QT to jumper wire cable
  — jumper ends go into the breadboard. No soldering required or used.
- Sensor is physically pinned to the breadboard by running a spare jumper wire through
  a corner mounting hole on the breakout board
- Current setup is breadboard-based for development. Perfboard/permanent mount deferred
  until firmware is complete.

---

## Pin Assignments

### VEML7700 (I2C via Stemma QT cable)

| Signal | ESP32 Pin | Stemma QT wire color |
|---|---|---|
| SDA | A4 (GPIO11) | Blue |
| SCL | A5 (GPIO12) | Yellow |
| VIN | 3.3V | Red |
| GND | GND | Black |

Note: Breakout has onboard I2C pullup resistors — no external pullups needed.
Expected I2C address: 0x10

### HD44780 LCD
Attempted and abandoned 4/20. ESP32-S3 outputs 3.3V; HD44780 at 5V VDD requires VIH ≥ 3.5V. No display planned.

### Buttons

| Signal | Arduino Pin | GPIO | Role |
|---|---|---|---|
| BTN_MODE | D9 | GPIO18 | Short: NORMAL/SOFT_PAUSE toggle; Long (700ms): HARD_OFF toggle |
| BTN_CYCLE | D10 | GPIO21 | Short: cycle through all 6 states in order |

Both active LOW, internal pull-up.

---

## Philips Hue Configuration

| Parameter | Value |
|---|---|
| Bridge IP | 192.168.1.186 |
| API Username | vaEUEcUhwdlUGtwhKiS8pEXSWw8hV0DOd-3Kfzud |
| API Version | Local HTTP API v1 |
| Base URL | `http://192.168.1.186/api/vaEUEcUhwdlUGtwhKiS8pEXSWw8hV0DOd-3Kfzud` |

### Light IDs

| Hue ID | Name | Role |
|---|---|---|
| 3 | Bedside | Bedside lamp |
| 4 | Desk | Desk lamp |
| 1 | Ceiling_1 | Overhead 1 |
| 2 | Ceiling_2 | Overhead 2 |

### Bulb Capabilities
- Color temp range: ct 153–447 mirek (≈2200K–6500K)
- Brightness: bri 0–254 (effective min ~2, night bedside target ~25)
- All changes use `transitiontime` (units of 100ms) for smooth fading
- Use `colormode: ct` for all commands (not xy or hue/sat)

---

## Core Behavior

### Lux Polling
- Polls VEML7700 every 30 s via `readLux(VEML_LUX_AUTO)`
- Each lux reading maps to a `LightTarget {uint8_t bri, uint16_t ct}` via `luxToTarget()` in `src/lightcurve.h`
- Curve is a 4-segment piecewise function — see `LIGHTCURVE.md` for shape, constants, and tuning guide
- Only sends Hue API update if change exceeds `STATE_TOLERANCE` (default: ±3 units)
- All updates use `transitiontime` for smooth fading

### Turn-on / Turn-off Order
- **Turn on:** Bedside (3) → Desk (4) → Overhead 1+2 (1, 2)
- **Turn off:** Overhead 1+2 (1, 2) → Desk (4) → Bedside (3)

### State Machine

`enum class State { LOCKED_OUT, NORMAL, WAKE, WIND_DOWN, SOFT_PAUSE, HARD_OFF }` — global in `main.cpp`.
`loop()` dispatches on `state` via switch; each state has a dedicated tick function.

### Daily Cycle

#### Morning Lockout *(implemented)*
- `state = LOCKED_OUT` at boot — suppresses all auto-on
- `checkBedsideState(lux)` runs every tick in LOCKED_OUT (and NORMAL, WIND_DOWN)
- Rising-edge detection: `!lastBedsideOn && bedside.on` in LOCKED_OUT → `triggerWake(lux)`
- Re-arms when all 4 lights confirmed off AND `hour >= LOCKOUT_RESET_HOUR` (9 PM); resets `stableLuxCount`, `windDownStep`
- Falling-edge detection uses 3 extra GETs (desk, ceil1, ceil2) only on the tick bedside turns off — not every tick

#### Wake Sequence *(implemented)*
- `triggerWake(lux)` calls `getLightState(LIGHT_BEDSIDE)` to read the actual bedside bri/ct at trigger time; seeds `wakeStartTarget` from that reading (or floor values if bulb is off); `wakeEndTarget = luxToTarget(ambientLux)`; sets `state = WAKE`
- `tickWakeRamp(lux)` linearly interpolates bri and ct from `wakeStartTarget` → `wakeEndTarget` per tick; sets all active bulbs each step
- Sequential turn-on: bedside + desk (every tick), overheads only when `lux >= S3_LUX_HI && rampBri >= S2_BRI_LO`
- `transitiontime = 300` (30 s) — matches poll interval exactly for a seamless continuous gradient
- `WAKE_RAMP_TICKS = 40` ticks (20 min); ramp ends at `t >= 1.0`; sets `state = NORMAL`

#### Daytime Auto Cycling *(implemented)*
- Continuous lux → bri + ct updates across all active bulbs in NORMAL state
- ±3 unit tolerance prevents unnecessary API calls

#### Evening Phases (lux-driven, not time-driven)

All phase triggers are based on lux readings, not time of day — adapts to seasonal sunset variation automatically.

1. **Transition** *(implemented)* — as lux falls from daytime, all 4 bulbs dim gradually per `luxToTarget()`
2. **Overhead off** *(implemented)* — lux drops through **300 lux** → CEIL_1+2 turn off; bedside+desk jump from bri≈160 → bri≈200 to compensate (intentional discontinuity at lightcurve seg 2/3 boundary)
3. **Post-dark lockout** *(deprioritized)* — lux drops to ≤10 lux → ~2 hr hold; deemed unnecessary
4. **Stable dark detection** *(implemented)* — lux stable within 2–8 lux for 30 min (60 readings), hour ≥ 21 — weighted counter increments when in range, decrements (floor 0) otherwise
5. **Wind-down** *(implemented)* — 60-min linear dim on bedside+desk from current bri → floor (bri=50, ct=400); `transitiontime=300` matches poll interval for seamless gradient; desk turns off at step 120; bedside holds at floor until manual off; `state = LOCKED_OUT` on completion
6. **Morning lockout** — re-arms when all lights confirmed off after 9 PM

---

## Override System

### Soft Pause *(implemented)*
- **Auto-trigger:** `checkOverride()` called at top of `tickNormal()`; polls all active bulbs; `isManualOverride(ls, expectedOn)` returns true if light turned off manually or bri/ct matches neither `lastTarget` nor `prevTarget` within `STATE_TOLERANCE`; sets `state = SOFT_PAUSE`, records `softPauseStart = millis()`. `prevTarget` stores the value of `lastTarget` before each PUT — guards against false positives when a Hue bulb is slow to apply an update (RF lag), without creating a grace-period window that would let a real override slip through if lux simultaneously changes enough to push a new PUT.
- **Button trigger:** Short press when in NORMAL state
- **Behavior:** System skips all `setLight()` calls while paused
- **Auto-resume:** `tickSoftPause()` resumes to NORMAL after `SOFT_PAUSE_MS` (60 min); resets `lastTarget = {255, 0}` sentinel to force first update after resume

### Hard Off *(implemented)*
- **Trigger:** Button long press (700ms)
- **Behavior:** `loop()` does nothing in `HARD_OFF`; no polling, no commands
- **Re-enable:** Same long press

### Force Wind Down / Force Wake
- Planned as controls in the web dashboard — no firmware trigger implemented yet

---

## Buttons *(implemented)*

Two tactile buttons, both active LOW, internal pull-up. Both polled every 50ms in the non-blocking wait loop between lux ticks.

### BTN_MODE — D9 (GPIO18)

| Press type | Action |
|---|---|
| Short press | If not NORMAL → go to NORMAL (sets `skipOverrideCheck = true`); if already NORMAL → SOFT_PAUSE |
| Long press (700ms) | Hard off toggle (HARD_OFF ↔ LOCKED_OUT) |

### BTN_CYCLE — D10 (GPIO21)

Short press only. Advances `(int)state + 1) % 6` through the state enum order and calls `forceState()`.

`forceState(State next)` sets all required entry preconditions per state:

| Target state | What forceState does |
|---|---|
| LOCKED_OUT | Resets `stableLuxCount = 0`, `windDownStep = 0` |
| NORMAL | Resets `lastTarget` sentinel, sets `skipOverrideCheck = true`, resets `stableLuxCount = 0` |
| WAKE | Calls `triggerWake(lastLux)` (reads actual bedside state via `getLightState`, seeds ramp) |
| WIND_DOWN | Seeds `windDownStartBri` from `lastTarget` (or `luxToTarget(lastLux)` if sentinel), resets `windDownStep = 0` |
| SOFT_PAUSE | Sets `softPauseStart = millis()` |
| HARD_OFF | Sets state only |

Discord is logged for all transitions except WAKE (`triggerWake` already logs it). `lastLux` is a global updated every tick so `forceState` can call `triggerWake` from the wait loop.

---

## Web Dashboard *(live)*

Flask + HTML/CSS/JS frontend hosted on Railway (Hobby plan, PostgreSQL). Live at the URL in `config.h` (`DASHBOARD_BASE_URL`). Firmware posts status and polls commands on every tick via `sendDashboardStatus()` and `pollDashboardCommand()`. Logging posts to the dashboard via `sendLog()`.

See `DASHBOARD.md` for full feature specs, design system, and implementation notes.

---

## Configuration Constants (actual `src/config.h`)

```cpp
#define STATE_TOLERANCE      3          // bri/ct units — update suppression + override detection
#define LOCKOUT_RESET_HOUR   23         // 24h hour after which all-lights-off re-arms lockout
#define SOFT_PAUSE_MS        3600000UL  // soft pause auto-resume (60 min)
#define WAKE_RAMP_TICKS          40     // 20 min ÷ 30 s/tick
```

---

## Hue API Usage

### Set light state
```
PUT http://192.168.1.186/api/vaEUEcUhwdlUGtwhKiS8pEXSWw8hV0DOd-3Kfzud/lights/{id}/state
Content-Type: application/json

{
  "on": true,
  "bri": 200,
  "ct": 300,
  "transitiontime": 10
}
```

### Get light state (for override detection)
```
GET http://192.168.1.186/api/vaEUEcUhwdlUGtwhKiS8pEXSWw8hV0DOd-3Kfzud/lights/{id}
```

### transitiontime units
- Value is in units of 100ms
- `transitiontime: 10` = 1 second fade
- `transitiontime: 200` = 20 second fade

---

## Libraries Used

```ini
[env:arduino_nano_esp32]
platform = espressif32
board = arduino_nano_esp32
framework = arduino
monitor_speed = 115200
lib_deps =
    adafruit/Adafruit VEML7700 Library
    adafruit/Adafruit BusIO
    arduino-libraries/NTPClient
```

### Built-in (no lib_deps entry needed)
- `WiFi.h` — ESP32 Arduino core
- `HTTPClient.h` — ESP32 Arduino core
- `Preferences.h` — NVS flash storage; used for last-state bri/ct only
- `Wire.h` — I2C (Arduino core)

---

## NTP / Time
- WiFi required for NTP sync at startup
- Use `NTPClient` library to get current day of week
- No RTC module — NTP only
- Day of week available via `timeClient.getDay()` for time-based logic

---

## Dev Environment
- **IDE:** CLion + PlatformIO plugin
- **OS:** Windows 11
- **Board flashing:** USB-C via PlatformIO upload
- **Serial monitor:** 115200 baud
- **Known quirk:** CLion caches environment variables at launch — if `claude` or other
  PATH entries aren't recognized in CLion's terminal, a full CLion restart is the fix

---

## ECEN 310 Notes
- Project is both a personal home automation project AND Tristan's ECEN 310 (Embedded Systems) final project
- Prof. Johansen approved the ESP32-S3 in place of the standard Uno R3, with the condition
  that the project must be rigorous enough to meet all assignment requirements
- Assignment requires C/C++ and assembly language; primary gap to address is the assembly component
- Strongest candidate for assembly: HD44780 parallel LCD write routine, implemented
  at register level with a performance comparison against LiquidCrystal library

---

## Remote Logging

`sendLog()` in `main.cpp` posts every event to the Railway dashboard (`/api/log`) via `sendDashboardLog()`.

Currently logged:
- Startup
- Bulb updates (when `shouldUpdate` fires in NORMAL)
- Wake sequence: start, 25/50/75% milestones, completion
- Wind-down: trigger, 15/30/45 min milestones, completion
- Soft pause trigger and auto-resume
- State transitions via buttons or dashboard commands

---

## Future Plans
- Web dashboard — see `DASHBOARD.md`
