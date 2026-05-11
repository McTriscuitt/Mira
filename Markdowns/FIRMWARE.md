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

All hardware has arrived and is wired up: VEML7700 lux sensor, Nano ESP32-S3, and two tactile buttons (BTN_MODE on D9, BTN_CYCLE on D10). The HD44780 LCD was attempted and abandoned (3.3 V logic incompatible with 5 V VDD) — see "Abandoned Hardware" in `PIN_ASSIGNMENTS.md`.

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
| API Version | Local v2 HTTPS (CLIP API) |
| Base URL | `https://192.168.1.186/clip/v2` (`HUE_V2_BASE_URL` in `config.h`) |
| Auth header | `hue-application-key: <HUE_API_KEY>` |
| TLS | `WiFiClientSecure` with bridge cert pinned via NVS — see "TLS / Cert Rotation" |
| Event stream | `https://192.168.1.186/eventstream/clip/v2` — see `SSE.md` |

The bridge application key and bulb UUIDs are kept in `config.h` (not committed publicly) and are referenced by name throughout the firmware.

### Light IDs

Lights are addressed by their v2 UUIDs (stable Zigbee identities). A small `lightCache[4]` indexed by `LIGHT_BEDSIDE / LIGHT_DESK / LIGHT_CEIL_1 / LIGHT_CEIL_2` (defined in `config.h`) mirrors the bridge's current state via SSE.

| Cache index | UUID define (in `config.h`) | Role |
|---|---|---|
| `LIGHT_BEDSIDE = 0` | `LIGHT_UUID_BEDSIDE` | Bedside lamp |
| `LIGHT_DESK    = 1` | `LIGHT_UUID_DESK`    | Desk lamp |
| `LIGHT_CEIL_1  = 2` | `LIGHT_UUID_CEIL_1`  | Overhead 1 |
| `LIGHT_CEIL_2  = 3` | `LIGHT_UUID_CEIL_2`  | Overhead 2 |

UUIDs were discovered once via `GET https://192.168.1.186/clip/v2/resource/light` and hardcoded. They only change if a bulb is factory-reset and re-paired.

### Bulb Capabilities

- Color temp range: `mirek` 153–447 (~2200 K warm — ~6500 K cool)
- Brightness: `dimming.brightness` is a `float` percent 0.0–100.0 (Hue v2 native units)
- All transitions use `dynamics.duration` in **milliseconds** (e.g. `30000` for a 30 s ramp matching the poll interval, `1000` for a snap)
- `setLight()` writes white/ct mode (`color_temperature.mirek`)
- `setLightColor()` writes color mode (`color.xy`), converting v1 HSB to CIE xy internally; used only for the startup purple flourish

---

## Core Behavior

### Lux Polling
- Polls VEML7700 every 30 s via `readLux(VEML_LUX_AUTO)`
- Each lux reading maps to a `LightTarget {float bri, uint16_t ct}` via `luxToTarget()` in `src/lightcurve.h`
- Curve is a 4-segment piecewise function with an intentional discontinuity at 200 lux — see `LIGHTCURVE.md` for shape, constants, and tuning guide
- `tickNormal` only sends a Hue PUT when the new target drifts by more than `STATE_TOLERANCE_BRI` (1.2 %) or `STATE_TOLERANCE_CT` (3 mirek) from the last commanded `sentTarget`
- All updates pass `dynamics.duration` in ms — `30000` to match the 30 s poll interval (seamless gradient), `1000` for snap-style state-machine PUTs

### Turn-on / Turn-off Order
- **Turn on:** Bedside (3) → Desk (4) → Overhead 1+2 (1, 2)
- **Turn off:** Overhead 1+2 (1, 2) → Desk (4) → Bedside (3)

### State Machine

`enum class State { LOCKED_OUT, NORMAL, WAKE, WIND_DOWN, SOFT_PAUSE, HARD_OFF }` — global in `main.cpp`.
`loop()` dispatches on `state` via switch; each state has a dedicated tick function.

### Daily Cycle

#### Morning Lockout *(implemented)*
- `state = LOCKED_OUT` at boot — suppresses all auto-on
- `checkBedsideState(lux)` runs every tick in LOCKED_OUT, NORMAL, and WIND_DOWN
- Rising-edge detection: `!lastBedsideOn && bedside.on` in LOCKED_OUT → `triggerWake(lux)`
- Re-arms when all 4 lights confirmed off (via `cachedLight()`) AND `hour >= LOCKOUT_RESET_HOUR` (9 PM, `LOCKOUT_RESET_HOUR = 21`); resets `stableLuxCount`, `windDownStep`
- Edge detection uses the SSE-synced `lightCache[]` — no per-tick HTTP GETs

#### Wake Sequence *(implemented)*
- `triggerWake(lux)` reads `cachedLight(LIGHT_BEDSIDE)` for the bulb's current bri/ct at trigger time; seeds `wakeStartTarget` from that (or floor values if the bulb is off); `wakeEndTarget = luxToTarget(ambientLux)`; sets `state = WAKE`
- `tickWakeRamp(lux)` linearly interpolates bri and ct from `wakeStartTarget` → `wakeEndTarget` per tick; sets all active bulbs each step
- Sequential turn-on: bedside + desk every tick; overheads added when `lux >= S3_LUX_HI && rampBri >= S2_BRI_LO`
- `dynamics.duration = 30000` (30 s) — matches poll interval for a seamless continuous gradient
- `WAKE_RAMP_TICKS = 40` ticks (20 min); ramp ends at `t >= 1.0`; hands off to `state = NORMAL`

#### Daytime Auto Cycling *(implemented)*
- Continuous lux → bri + ct updates across all active bulbs in NORMAL state
- Drift tolerance prevents unnecessary PUTs: `STATE_TOLERANCE_BRI = 1.2 %`, `STATE_TOLERANCE_CT = 3` mirek
- Override detection runs asynchronously inside the SSE handler — see `SSE.md`

#### Evening Phases (lux-driven, not time-driven)

All phase triggers are based on lux readings, not time of day — adapts to seasonal sunset variation automatically.

1. **Transition** *(implemented)* — as lux falls from daytime, all 4 bulbs dim gradually per `luxToTarget()`
2. **Overhead off** *(implemented)* — lux drops through **200 lux** (`S3_LUX_HI`) → CEIL_1+2 turn off; bedside+desk jump from bri ≈ 78.7 % → bri ≈ 63 % to compensate (intentional discontinuity at lightcurve seg 2/3 boundary — see `LIGHTCURVE.md`)
3. **Stable dark detection** *(implemented)* — lux stable within `[2, 8]` for 30 min (60 readings), hour ≥ 21 — weighted counter increments in range, decrements toward floor 0 otherwise
4. **Wind-down** *(implemented)* — 60-min linear dim on bedside+desk from current bri → floor (`S4_FLOOR_BRI ≈ 19.7 %`, `CT_WARM = 400` mirek); `dynamics.duration = 30000` matches poll interval for seamless gradient; desk turns off at step 120; bedside holds at floor until manual off; `state = LOCKED_OUT` on completion
5. **Morning lockout** — re-arms when all lights confirmed off after 9 PM

---

## Override System

### Soft Pause *(implemented)*
- **Auto-trigger:** SSE-driven, per-light trajectory-matched override detection in `handleLightUpdate()`. See `SSE.md` for the full mechanism. On an off-trajectory event for a light Mira expects to be on, sets `state = SOFT_PAUSE` and `softPauseStart = millis()`.
- **Button trigger:** BTN_MODE short press when in NORMAL.
- **Behavior:** `tickSoftPause()` is a no-op until expiry — system skips all `setLight()` calls while paused.
- **Auto-resume:** Resumes to NORMAL after `SOFT_PAUSE_MS` (60 min). Resets `sentTarget = {-1.0f, 0}` sentinel to force a fresh PUT, syncs `overheadsOn` and `lastBedsideOn` from cache, then runs a 10-min interpolation ramp (`PAUSE_RESUME_TICKS = 20`) from the pre-pause target to current ambient. Override detection is suppressed for the entire resume ramp via the `pauseResumeActive` flag.

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
| Short press | If not NORMAL → go to NORMAL (resets `sentTarget` to sentinel; syncs `overheadsOn` and `lastBedsideOn` from cache); if already NORMAL → SOFT_PAUSE |
| Long press (700ms) | Hard off toggle (HARD_OFF ↔ LOCKED_OUT) |

### BTN_CYCLE — D10 (GPIO21)

Short press only. Advances `(int)state + 1) % 6` through the state enum order and calls `forceState()`.

`forceState(State next)` sets all required entry preconditions per state:

| Target state | What forceState does |
|---|---|
| LOCKED_OUT | Resets `stableLuxCount = 0`, `windDownStep = 0`; syncs `lastBedsideOn` from `cachedLight(LIGHT_BEDSIDE).on` |
| NORMAL | Resets `sentTarget = {-1.0f, 0}` sentinel; resets `stableLuxCount = 0`; syncs `overheadsOn` and `lastBedsideOn` from cache. No transition-time mute is needed — override discrimination is per-PUT via the `recentPuts[]` trajectory ring (see `SSE.md`). |
| WAKE | Calls `triggerWake(lastLux)` (reads bedside via `cachedLight()`, seeds ramp) |
| WIND_DOWN | Seeds `windDownStartBri` from `sentTarget.bri` (or `luxToTarget(lastLux).bri` if sentinel), resets `windDownStep = 0` |
| SOFT_PAUSE | Sets `softPauseStart = millis()` |
| HARD_OFF | Sets state only |

A log entry is sent for all transitions except WAKE (`triggerWake` already logs it). `lastLux` is a global updated every tick so `forceState` can call `triggerWake` from the wait loop.

---

## Web Dashboard *(live)*

Flask + HTML/CSS/JS frontend hosted on Railway (Hobby plan, PostgreSQL). Live at the URL in `config.h` (`DASHBOARD_BASE_URL`). Firmware posts status and polls commands on every tick via `sendDashboardStatus()` and `pollDashboardCommand()`. Logging posts to the dashboard via `sendLog()`.

See `DASHBOARD.md` for full feature specs, design system, and implementation notes.

---

## Configuration Constants (actual `src/config.h`)

```cpp
#define STATE_TOLERANCE_BRI  1.2f       // percent — tickNormal drift threshold (NOT used for override comparison)
#define STATE_TOLERANCE_CT   3          // mirek — tickNormal drift threshold
#define LOCKOUT_RESET_HOUR   21         // 24h hour after which all-lights-off re-arms lockout
#define SOFT_PAUSE_MS        3600000UL  // soft pause auto-resume (60 min)
#define WAKE_RAMP_TICKS      40         // 20 min ÷ 30 s/tick
#define PAUSE_RESUME_TICKS   20         // 10 min — soft-pause resume ramp
#define DEBOUNCE_MS          50UL
#define LONG_PRESS_MS        700UL
```

Echo-discrimination tolerances and SSE timing constants live in `src/main.cpp` near the SSE globals (`TRAJECTORY_TOLERANCE_BRI = 5.0f`, `TRAJECTORY_TOLERANCE_CT = 15`, `RECENT_PUT_GRACE_MS = 5000UL`, `SSE_RECONNECT_DELAY_MS = 5000UL`, `SSE_STALE_TIMEOUT_MS = 600000UL`) — see `SSE.md`.

---

## Hue API Usage (v2 HTTPS)

### Set light state — CT mode (`setLight()`)
```
PUT https://192.168.1.186/clip/v2/resource/light/<uuid>
Content-Type: application/json
hue-application-key: <HUE_API_KEY>

{
  "on":                { "on": true },
  "dimming":           { "brightness": 47.4 },
  "color_temperature": { "mirek": 361 },
  "dynamics":          { "duration": 30000 }
}
```

### Set light state — Color mode (`setLightColor()`)

Used only for the startup purple flourish. Internally converts v1 HSB (hue 0–65535, sat 0–254) to CIE xy via `_hsbToXY()`.

```
PUT https://192.168.1.186/clip/v2/resource/light/<uuid>
Content-Type: application/json
hue-application-key: <HUE_API_KEY>

{
  "on":       { "on": true },
  "dimming":  { "brightness": 78.7 },
  "color":    { "xy": { "x": 0.2845, "y": 0.1234 } },
  "dynamics": { "duration": 1000 }
}
```

### Get light state — bootstrap only
```
GET https://192.168.1.186/clip/v2/resource/light
hue-application-key: <HUE_API_KEY>
```

Called once at startup by `bootstrapLightStates()` to seed `lightCache[]`. After that, the cache is kept fresh via the SSE event stream — no per-tick GETs.

### SSE event stream
```
GET https://192.168.1.186/eventstream/clip/v2
hue-application-key: <HUE_API_KEY>
```

Long-lived HTTPS connection. Drained by `sseTick()` in the wait loop. See `SSE.md` for the full protocol, cache semantics, echo discrimination, and override detection flow.

### dynamics.duration units

- Value is in **milliseconds** (v2 — replaces the v1 `transitiontime` field which was in 100 ms ticks)
- `30000` = 30 s ramp (matches the 30 s poll interval — seamless gradient)
- `1000` = 1 s snap (state-machine PUTs, overhead on/off)

---

## TLS / Cert Rotation

All v2 calls and the SSE stream use `WiFiClientSecure.setCACert(_bridgeCertPem.c_str())` with the bridge's self-signed certificate pinned via NVS. `ensureBridgeCert()` runs at startup (after NTP sync) and handles cert lifecycle automatically:

1. Load PEM + expiry epoch from NVS (`Preferences` namespace `"mira"`, keys `hueCert` / `hueCertExpiry`)
2. If stored cert is present and not within 30 days of expiry, verify it with a quick `setCACert` connection probe. If the probe succeeds, use the stored cert and we're done.
3. Otherwise (no stored cert, expired, near-expiry, or probe failure): fetch a fresh cert using `setInsecure()` for that single call, parse `notAfter` via `mbedtls/x509_crt.h`, and store the new PEM + expiry epoch to NVS.

The single insecure fetch is acceptable risk — local LAN only, only when no valid cert is stored. After that fetch, every Hue connection (PUTs, bootstrap GET, SSE stream) uses the pinned cert.

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
    bblanchon/ArduinoJson
```

### Built-in (no lib_deps entry needed)
- `WiFi.h` — ESP32 Arduino core
- `HTTPClient.h` — ESP32 Arduino core
- `WiFiClientSecure.h` — HTTPS via the bridge cert pinned in NVS
- `mbedtls/x509_crt.h` — used by `ensureBridgeCert()` to parse the bridge certificate's `notAfter` expiry timestamp
- `Preferences.h` — NVS flash storage; stores the bridge TLS cert PEM + expiry epoch (`hueCert`/`hueCertExpiry`) and last-state bri/ct
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
