# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project: Mira

Smart bedroom lighting firmware running on an **Arduino Nano ESP32-S3 (ABX00083)**. **Mira 3.0** (October 2026) drives 4 Philips Hue bulbs on a **time-of-day timeline** through the local Hue Bridge v2 API: at each point in the day where the lights should change, it sends one command per light with a target brightness, color temperature, and duration, and the bulbs do the fading. A VEML7700 lux sensor is read for reporting only.

**Start every firmware session with `Markdowns/MIRA_3.md`.** It's the 3.0 spec, architecture, decisions log, and first-flash checklist; the user's verbatim inputs are in `V3 Migration Docs/`. The lux-reactive 2.x system is tagged `mira-2-final` (3.0 is on branch `mira-3` until it's verified on the board).

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
  - *3.0: yes — a brightness change in any state is a hold (MIRA_3.md §6).*

- add a nap button on the dashboard
  - turns off the lights and set to soft pause for 90 minutes
  - *3.0: turning every light off before 9 PM is now a 60-min nap; a dashboard button is a dashboard-session item.*

- ~~this may seem silly, but change it so that polls happen on :00 and :30, instead of whenever 
the system reflashes. make it commentable so that it can be ignored for testing.~~ **SHIPPED July 14 (N5/`ALIGNED_TICKS` in config.h — comment out to revert to flash-relative 30 s)**
  - *3.0: the 30 s tick only carries telemetry now and is always aligned; `ALIGNED_TICKS` is gone.*

- maybe add another secret page for dashboard error logs?

- **(July 15) wind-down lux gate needs re-tuning for sensor placement** — the ESP currently
  sits on the desk (laptop's there), not the windowsill, so lux readings don't reflect
  window light the way the curve/thresholds assume. Wind-down should be able to begin
  earlier without lux having to drop all the way to ≤ 10. Revisit the `lux <= 10` gate in
  `tickNormal` (and possibly the placement story generally) — deferred, different day.
  - *3.0: obsolete — there's no lux gate; the evening is time-driven.*

### Priority Bugs


### Resolved Priority Bugs
- **RESOLVED (June 11)** — soft-pause→NORMAL dashboard staleness (ack-before-status window; frontend pending-preview fix). Full write-up: `Bug Records/June11_SoftPause_AckBeforeStatus_DashboardStale.md`. Still-relevant residual: the same ack-before-status window exists for step-seek commands — N2 shrinks it, but only an ack-after-status reorder removes it.


## Recently completed

- **Mira 3.0 built, not yet flashed (Oct 14, 2026)** — ground-up rewrite on branch `mira-3`: pure core (`timeline` + `director` + `echo`) with 31 native tests, new hardware shell, SNTP + Central TZ with automatic DST. Status table and first-flash checklist: `Markdowns/MIRA_3.md`.
- 2.x history lives in `git log` and the Markdowns docs: N1 all 5 stages (flashed July 16), N2 steps 1+2 (July 24), WiFi watchdog + boot reasons (Sept 12). `Markdowns/FEATURES_AND_REWORKS.md` and `N1_MIGRATION.md` describe 2.x.

## Build, Flash & Test (PlatformIO)

- `pio run` / `pio run --target upload` / `pio device monitor` (115200 baud) build and flash the board (`default_envs = arduino_nano_esp32`).
- **Native tests:** `pio test -e native -e native_legs`. They run the pure core through simulated days on the host and need a host gcc/g++ on PATH (CLion's bundled MinGW works: `<CLion>\bin\mingw\bin`). The board env ignores tests.
- PlatformIO is the only build system. The IDE is CLion with the PlatformIO plugin. `.pio/libdeps/` is auto-generated — do not edit files there.
- The board build is `-std=gnu++11` (Arduino-ESP32 2.0.17, GCC 8.4). Keep the core C++11: the native env compiles it with the same standard.

## Hardware

| Component | Detail |
|-----------|--------|
| MCU | Arduino Nano ESP32-S3 (ABX00083) |
| Lux sensor | Adafruit VEML7700 — I2C, SDA=A4/GPIO11, SCL=A5/GPIO12. Reporting only in 3.0; optional (a missing sensor no longer hangs boot) |
| Buttons | D9 / D10 — retired June 2026; the code was deleted in 3.0 (it drove 2.x states; `mira-2-final` keeps it) |
| Bulbs | Floor (Signe gradient), dresser, chest, ceiling 1 — Hue White & Color Ambiance, driven in v2 percent + mirek. Ceiling 2 exists but is out of the system |
| Onboard RGB | Active LOW. Boot: red blink = WiFi, blue blink = waiting for the time, solid blue = time synced, off = boot complete |

**LCD was attempted and abandoned** — HD44780 requires 3.5V logic minimum (0.7 × 5V VDD) but ESP32-S3 outputs 3.3V. Level shifting was not pursued. No display is planned.

## Key Configuration

- **`src/config.h`** — tracked, public: every behavior and tuning constant (the day's times and levels, fades, windows, tolerances, watchdog timing). **3.0: it no longer `#include`s `secrets.h`** — the pure core includes it and must build on a host with no secrets; the shell files (`main.cpp`, `bridge.cpp`, `dashboard.cpp`) include `secrets.h` directly.
- **`src/secrets.h`** — gitignored: WiFi creds, `HUE_API_KEY`/`ESP32_API_KEY`, `HUE_V2_BASE_URL`/`HUE_BRIDGE_HOST`, `LIGHT_UUID_*`, `DASHBOARD_BASE_URL`. Copy `src/secrets.h.example` on a fresh clone.
- **Hue Bridge:** `192.168.1.186`; `HUE_V2_BASE_URL = "https://192.168.1.186/clip/v2"`; the key goes in the `hue-application-key` header.
- **Roster (`src/lights.h`):** `LIGHT_FLOOR=0, LIGHT_DRESSER=1, LIGHT_CHEST=2, LIGHT_CEIL_1=3, LIGHT_COUNT=4`. This order indexes masks, the bridge cache, the echo ring, and persisted state. UUIDs live in `bridge.cpp`'s `LIGHT_UUIDS[]` in the same order. The timeline speaks in `Role`s, so a re-enabled `LIGHT_CEIL_2` (a commented-out row) would follow the ceiling with no other change.
- **Clock:** `MIRA_TZ "CST6CDT,M3.2.0,M11.1.0"` via `configTzTime()` — US Central with automatic DST. `UTC_OFFSET_SEC` and NTPClient are gone.

## Core Behavior (summary — the spec is `Markdowns/MIRA_3.md` §1–§7)

The day: wake (the first light on after 6 AM, a light already on at 6 AM, or 9 AM) → 25→75% over 15 min (floor, dresser +5 min, chest +10 min) → 10:00 ceiling on, →100% by 11:00 → 17:00–18:00 →75% → 19:00 ceiling off → 20:00–21:00 →40%, chest off → 23:00–23:30 →25%, dresser off → 01:00 lights-out. CT follows the 2.x brightness→CT map everywhere (280 mirek at 100% … 400 at 19.7%).

There is **no state machine**. Each light has a *want on* bit (you and the schedule both write it; last writer wins) and an *out for today* bit. `Director::reconcile()` makes the lights match: lights that want on are on, following the period in force. It sends a command only when the plan differs from the last command sent, so a whole day is 24 commands. Three conditions pause it, and while paused the schedule keeps writing *want on*:
- **asleep** — everything off at/after 9 PM, or the 1 AM lights-out. Lights you turn on before 6 AM are left alone.
- **nap** — everything off before 9 PM → 60 min, then back with a 15 s fade-in. A light on ends it early; still napping at 9 PM → asleep.
- **hold** — a manual brightness change → everything freezes until the next fade period starts (10:00/17:00/20:00/23:00), which then fades all lights to that period's target. Dashboard NORMAL resumes early.

One light off → out for today (back when you turn it on, or at the next wake). A light you turn on joins in line near-instantly (0.3 s snap). Color/CT changes aren't tracked.

## Code Map (MIRA_3.md §8 has the detail)

Pure core (no Arduino; also compiled on the host by the tests):
- `timeline.*` — the day profile in "Mira-day seconds" (since 06:00, so the evening runs past midnight), the CT map, the fixed events, `crossedLocal()` (midnight wrap; the DST fall-back hour crosses nothing)
- `director.*` — the behavior engine. Inputs: `tick()` 1 Hz, `onEdge()`, `onUserBrightness()`, `onStaleRevert()`, `onCommand()`. Output: `Env::put()`
- `echo.h` — the own-echo test

Shell:
- `main.cpp` — WiFi + watchdog, boot reason, clock, lux, RTC persistence, the loop
- `bridge.cpp` — cert pin, cache, `bridgePut()`, the Core-0 SSE task + classifier
- `dashboard.cpp` — the Core-0 net task: logs, status, command poll
- `events.h` — the queue type both tasks feed

## Hue API Patterns

- **Commands:** `bridgePut(idx, on, bri, ct, durMs)` sends `on` + `dimming.brightness` + `color_temperature.mirek` + `dynamics.duration` (or just `on:false` + duration). 200 and 207 count as sent. Bodies are built with `snprintf` (no `String` churn). `bridgePreset()` (experimental, `PRESET_OFF_LIGHTS`) sends dimming to a light that's off; `bridgeFlourish()` is the boot purple flash. Every command is recorded in the echo ring (`noteRecentPut()`) **before** the HTTP request — the bridge can echo faster than HTTPClient returns.
- **Mid-batch abort:** `holdPending` (set by the SSE task the instant it sees your brightness change) makes `bridgePut()` return `Aborted` until the main task applies the hold, so the rest of a multi-light batch can't land on top of your change.
- **Bootstrap/resync:** one `GET /clip/v2/resource/light` (ArduinoJson filter) seeds the cache at boot. It runs again right after the first stream connect and after every reconnect, turning each missed on/off flip into an edge event.
- **NVS cert pinning (2.x, unchanged):** `ensureBridgeCert()` loads/fetches the bridge cert (`Preferences` "mira", `hueCert`/`hueCertExpiry`). **CA-mode `setCACert` is impossible here** — Mira dials by IP and the cert's CN is the bridge ID, and the core always verifies the hostname. Instead `peerMatchesPinned()` byte-compares the peer cert post-handshake on the boot probe and the SSE stream (nothing is sent before it passes). HTTPClient paths transmit on connect, so they run insecure-mode TLS and inherit the boot-time check.
- **Transition length:** every scheduled fade is ≤ 60 min, under Hue's believed ~109 min cap. `MAX_FADE_MS` splits longer fades into legs (hardware unknown #1 in MIRA_3.md).

## Implementation Gotchas (3.0)

- **The classifier reads only `on` and `dimming`.** The cache write happens after classification (the bridge splits combined changes across events). On/off flips are Mira's echo only when on a live trajectory, otherwise yours — **there's no on/off stale-revert reading** (it would fight your switch). Brightness is checked in order: on-edge grace (3 s) → time-aware echo (`echo.h`) → no-op vs cache → stale revert (< 90 s, re-sent once) → yours → hold.
- **Echo matching is time-aware.** For 10 s after a command, the full start→target range counts as Mira's echo; after that, only within 8% of where the fade should be by now. A blanket band would hide your changes inside an hour-long fade. Known blind spot: nudges within ~8% of the fade's position, or ~5% of its target, read as Mira's.
- **On/off commands only count as done when echoed.** An unreachable bulb (wall switch off) backs off to 60 s instead of retrying every ~40 s, and the failure is logged once.
- **Holds freeze with computed-level commands** (~0.4 s to each mid-fade light's current level, from the `Command` trajectory). The Director seeds each light's brightness from the bridge at boot — a freeze computed from an unknown start would drop lights to ~1%.
- **Persistence:** `DirectorState` is POD in RTC-noinit memory with an FNV check. Soft resets restore it (if < 10 min old) without switching any lights; power-on infers from the clock and the bulbs. **Bump `Director::STATE_MAGIC`'s last byte whenever `DirectorState`'s layout changes.**
- **Time** reaches the Director as `Now { epoch ms, local seconds }`. Only the shell touches the timezone. Nothing driven spans a DST change (those happen at 2 AM, while asleep).
- **`millis()` rollover:** always compare with subtraction (`millis() - start >= interval`), never `millis() > start + interval`. The core uses epoch ms (int64), which doesn't roll over.
- Diagnostics: the SSE event ring (16) + outcome counts are dumped to Serial on every hold; `#define ECHO_TRACE 1` traces every classified event.

## Web Dashboard

Flask + HTML/CSS/JS on Railway (Hobby plan, PostgreSQL). The firmware's net task posts logs and status and polls `/api/command` every 5 s (`NET_CMD_POLL_MS`). Demo login (read-only) via `DEMO_PASSWORD`. See `Markdowns/DASHBOARD.md`.

**Interim, until the dashboard session (MIRA_3.md §8):**
- The status `state` carries 2.x names so the current UI keeps rendering: active → `NORMAL`, hold/nap → `SOFT_PAUSE`, asleep → `LOCKED_OUT`, wake → `WAKE`. It also carries `mode` (the 3.0 name) and `lights_on`; `excluded` shows out-for-today.
- Commands: `NORMAL`/`RESUME`, `SOFT_PAUSE`/`HOLD`, `WAKE`, `LOCKED_OUT`/`SLEEP`, `SET_SOFT_PAUSE_REMAINING`/`NAP_REMAINING`. Retired 2.x commands are acked and logged.
- The `/lux` page and the curve JS are 2.x leftovers; with HARD_OFF gone, an "away" toggle is a candidate.

**Planned: GitHub Actions selective deploy** — disconnect Railway's built-in git auto-deploy; replace with a GitHub Actions workflow (~20 lines of YAML) that triggers Railway deploys only when dashboard-relevant files change (`app.py`, `requirements.txt`, dashboard HTML/CSS/JS). Firmware changes (`src/`, `platformio.ini`, etc.) must never trigger a Railway deploy. Requires storing a Railway API token as a GitHub Actions secret.

**Open design: Webhooks / external triggers** — inbound webhooks to Flask (`/webhook/external`) so external services can trigger Mira (Google Calendar, Siri Shortcuts, IFTTT). The existing `ESP32_API_KEY` bearer token pattern should extend to any inbound webhook endpoints. No implementation decisions made yet.

## Deprioritized / Maybe Later

- **HD44780 LCD** — attempted 4/20; abandoned. ESP32-S3 outputs 3.3V logic; HD44780 at 5V VDD requires VIH ≥ 3.5V (0.7 × VCC). Signal levels fall below threshold. Level shifting not pursued. No display planned.
- **3-button control scheme** (Mode / Confirm / Cycle) — abandoned alongside LCD.
- **DAY_TYPES / WORK / RELAXED** — per-weekday schedules; 3.0 runs one schedule every day.
