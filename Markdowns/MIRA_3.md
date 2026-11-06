# MIRA_3.md — Mira 3.0: Time-of-Day Lighting

Mira 3.0 replaces the lux-reactive curve and its state machine with a **time-of-day
timeline**. At each point in the day where the lights should change, Mira sends a
command with a target brightness, a color temperature, and a duration. The **bulbs**
then do the fading themselves. Lux is read for reporting only.

**Naming:** 2.0 was meant to be the "super reactive" line (the N1 event core plus the
N2 dashboard work). It was never finished. The last lux-reactive build is tagged
**`mira-2-final`**. 3.0 was built on the **`mira-3`** branch and merged into `main` on
Nov 5, 2026 (the user's call, with some checklist items still open). Docs deleted in 3.0 (`LIGHTCURVE.md`, `SYSTEM.md`, `FIRMWARE.md`,
`System Explanation OLD.md`) are still there at that tag.

**Source inputs (verbatim, user-written):** `V3 Migration Docs/`:
- `Initial Migration Thoughts Prompt.txt`: the spec.
- `Clarifications Off of Initial Prompt.txt`: round 1 answers.
- `More Clarification`: round 2 answers.

Where this doc and those files disagree, the files are what the user said and this
doc has a bug.

This doc is the **cross-session handoff**. Start each session by reading this file.
Before ending a session, update the Status table and the Decisions Log.

---

## Status

**Approach (user, Oct 14):** no staged flashing. The whole system is architected
and built before it runs on the board. `main.cpp` was rebuilt from the ground up;
little 2.x code survives beyond what §8 lists as kept.

| Step | Description | Status |
|---|---|---|
| 0 | Spec + this design doc | **CONFIRMED (Oct 14, 2026).** Fade periods (§3) confirmed; hold redefined to end at the next fade period (§6); all small choices accepted. |
| 1 | Pure core: `timeline` (day profile, CT map, events), `director` (behavior engine), `echo` (own-echo test) | **DONE (Oct 14)** |
| 2 | Native tests: simulated days through the core | **DONE (Oct 14).** 31 cases (`test_core` 30, `test_legs` 1), all passing. A mutation check (freeze disabled) was caught. |
| 3 | Hardware shell: clock (SNTP + Central TZ), bridge (commands, SSE classifier, cert pin), dashboard task, lux telemetry, RTC persistence | **DONE (Oct 14)** |
| 4 | Compiles clean for the board + code review | **DONE (Oct 14).** `pio run`: 0 warnings, RAM 19.4%, flash 31.0%. Review found and fixed 5 issues (Decisions Log → "Found in build/review"). |
| 5 | First flash + hardware-unknowns verification | **IN PROGRESS**: flashed Oct 14 23:33 (`4ba1a97`); first-night findings in `MIRA_3_FIRST_NIGHT.md`. Checklist at the end. |
| 6 | Docs: delete lux-era docs, rewrite CLAUDE.md, merge `mira-3` → `main` | **DONE Nov 5.** Docs deleted, CLAUDE.md rewritten for 3.0, and `mira-3` merged into `main` (fast-forward). |
| 7 | Power-loss replay: save the day's facts to NVS, replay missed events at power-on | **FLASHED Nov 2, boot not yet confirmed**: §10. Native tests pass; NVS and a real outage are unverified on the board. |
| 8 | False holds + wall switches: in-line filter, `zigbee_connectivity` | **Wall switches TESTED: the user confirmed them on the real lights, Nov 5** (unknown #7). False holds still unverified: §6 and §8. |
| 10 | Mira 3.1: schedule post (`/api/schedule` at boot and on `SEND_SCHEDULE`), lux sensor removed (code commented out), `MIRA_VERSION` | **FLASHED Nov 5.** Schedule post verified by the user (checklist at the end, all pass); the 3.1 flash only adds the lux removal and the version string. |
| 9 | Dashboard light commands: `LIGHT_ON` / `LIGHT_OFF` (the terminal's `dresser off`, `chest on`) | **FLASHED Nov 5**, twice, both while the board was away from home WiFi, so it's unconfirmed. The second build reworked two things: out for today only while the schedule still wants the light, and a light turned on during a hold joins the hold level (§5, §8 decisions). 74 native tests pass. Checklist at the end. |

**DST note:** the board runs 2.x until step 5. DST ends Nov 1. If 3.0 isn't flashed
by then, 2.x's fixed `UTC_OFFSET_SEC` puts its evening gates an hour off (stopgap on
`main`: `-18000` → `-21600`). The user said ship time isn't a concern.

The dashboard rework is a separate, later effort.

---

## 1. The whole system in one paragraph

Mira keeps two bits per light:
- **want on.** Both you and the schedule write it, and the last writer wins.
- **out for today.** While set, the schedule can't turn that light back on. Only an off
  while the schedule still has that light on, or will turn it on later today, sets it.
  It clears at the light's scheduled off and when Mira goes to sleep (user, Nov 5).

The timeline gives the brightness every light should be at right now: the **in-line
level**. Mira has one job: lights that *want on* are on, at the in-line level.

Three conditions pause that job:
- **asleep:** lights-out until the morning wake.
- **nap:** 60 min after you turn everything off before 9 PM.
- **hold:** after you change brightness, until the next fade period starts (or you
  resume from the dashboard).

While paused, the schedule keeps writing *want on*. When the pause ends, Mira catches
up in one move. There is no state machine beyond that (user direction: "this new
system really shouldn't have any 'states' per se, but rather just puts that change
the lights").

## 2. The day

| Period | Kind | Brightness | Lights (in line) | Events |
|---|---|---|---|---|
| Wake: trigger → +15 min | fade | 25 → 75% | floor; dresser joins at +5 min; chest joins at +10 min | floor fades on at the trigger |
| Wake end → 10:00 | steady | 75% | floor, dresser, chest | — |
| 10:00 → 11:00 | fade | 75 → 100% | + ceiling | ceiling fades on at 10:00 |
| 11:00 → 17:00 | steady | 100% | all | — |
| 17:00 → 18:00 | fade | 100 → 75% | all | — |
| 18:00 → 19:00 | steady | 75% | all | ceiling fades off at 19:00 |
| 19:00 → 20:00 | fade | 75 → 40% | floor, dresser, chest | chest fades off at 20:00 |
| 20:00 → 20:45 | steady | 40% | floor, dresser | — |
| 20:45 → 21:00 | fade | 40 → 25% | floor, dresser | — |
| 21:00 → 22:30 | steady | 25% | floor, dresser | dresser fades off at 22:30 |
| 22:30 → 01:00 | steady | 25% | floor | **01:00 lights-out:** everything still on fades off → asleep |
| asleep → wake | — | — | nothing driven | see §4 |

"The ceiling" is Ceiling_1. Ceiling_2 stays out of the system, as in 2.x; it's a
commented-out row in the roster (`lights.h`) for re-enabling.

All of it is tuned in `src/config.h`: the times, levels, fade lengths, and windows.

**Color temperature** comes from the 2.x brightness→CT map everywhere, evenings included
(the 2.x wind-down's pin at full warm is gone). The formula is
`ct = 400 − 120 × (bri − 19.7) / 80.3`, rounded and clamped to 280–400 mirek:

| Brightness | 100% | 75% | 40% | 25% |
|---|---|---|---|---|
| CT (mirek) | 280 | 317 | 370 | 392 |
| ≈ Kelvin | 3,570 K | 3,150 K | 2,700 K | 2,550 K |

One command moves brightness and CT together, so CT stays approximately on the map
through a whole fade.

A whole day is **24 commands**: one per change, with the bulbs doing the fading. The
native test `test_full_day_auto_wake_one_command_per_change` pins this exactly.

## 3. Fade periods and steady periods

*(Repeated back to the user and confirmed, Oct 14.)*

- Every stretch of the day is a **fade period** (brightness moves between two levels
  across a fixed window) or a **steady period** (brightness holds).
- The fade periods:
  - the wake (15 min from its trigger)
  - 10:00–11:00
  - 17:00–18:00
  - 19:00–20:00
  - 20:45–21:00
- **At the start of a fade period**, Mira sends one command per light: the fade's end
  level, with the fade's full length as the duration. Nothing more goes out until the
  next event.
- **During a steady period**, Mira sends nothing except the on/off events at their
  times.
- **Taking back control** (end of a hold or nap) works like this:
  - Inside a fade period: Mira acts as if it had been fading all along. One command
    sends each light to the fade's end level, using the time left as its duration, so
    the fade still finishes on schedule. Example: 7:15 PM → 40% over 45 min. Lights
    glide from wherever they are.
  - Inside a steady period: a 15 s fade to that period's level. This applies only to
    lights not already there.
- **Lights that are off and coming back on** mid-fade first fade in over 15 s to
  where the fade would be right now, then continue with it. Starting a 45-min
  command from "off" would make them crawl up from zero.
- **A late wake** fades toward the in-line level at its own end. A wake at 9:50 lands
  at 77% at 10:05, then continues the 10:00–11:00 fade. A wake after a cold boot at
  2 PM fades 25 → 100%. The ceiling joins with the chest if the schedule has it on.

## 4. Sleep and wake

- **Asleep** starts when every light is off at 21:00 or later (however it happened),
  or at the 01:00 lights-out. While asleep, Mira drives nothing. A light you turn on
  is left alone. (Bulbs that Mira faded off come back at minimum brightness: a Hue
  quirk that makes a 3 AM light very dim.)
- **Wake** comes from asleep, by the first of:
  1. **06:00 with any light already on** (you're up early).
  2. **Turning a light on after 06:00.**
  3. **09:00 automatically.**
- The wake fades the floor on at 25% (15 s), then rises 25 → 75% over 15 min. The
  dresser joins at +5 min and the chest at +10, each fading in to the in-line level.
- If another light triggered the wake (say the chest), it's already on, so it rides
  the wake level from the start. Its scheduled join becomes a no-op.
- Wake clears every *out for today* bit. Lights you turned off yesterday rejoin
  today.

## 5. What your changes do

| You… | When | Mira… |
|---|---|---|
| turn a light on | asleep (lights-out → wake) | leaves it alone |
| turn a light on | asleep, 06:00 or later | starts the wake |
| turn a light on | in charge | sets *want on* and clears *out*. The light joins: snapped to the in-line level near-instantly (`JOIN_SNAP_MS`, ~0.3 s), then continues any fade |
| turn a light on | nap | ends the nap early. The rest of the nap set comes back too (15 s fade-in) |
| turn a light on | hold | records *want on* and brings that light to the **hold level** (your last brightness in this hold) over 0.3 s. The other lights stay held (user, Nov 5) |
| turn **one** light off | awake | marks it **out for today** (until you turn it back on, its scheduled off, or tomorrow's wake) |
| turn **one** light off | after its scheduled off (ceiling 19:00, chest 20:00, dresser 22:30) | just off: it was off on the schedule anyway, so not out for today |
| cut a light at the wall | awake | same as turning it off |
| restore a light at the wall | hold | fades to the hold level over 15 s; the other lights stay held |
| restore a light at the wall | in charge | it fades into line over 15 s (brightness and CT), then continues any fade |
| turn **every** light off | before 21:00 | **nap** |
| turn **every** light off | 21:00 or later | **asleep** |
| change brightness | in charge (incl. the wake) | **hold**, unless it lands within 5% of the in-line level (§6) |
| change brightness | asleep / nap / hold | nothing |
| change color or CT | anytime | nothing. Color changes aren't tracked; the next command overwrites them |

"One light" versus "every light" is judged over a short **grouping window** (~3 s).
A room toggle in the Hue app arrives as a burst of per-light events, and it should
read as one all-off, not three single-light offs.

**From the dashboard** (user, Nov 5: type `dresser off` or `chest on` "and that will turn
those lights on or off for the day"). A light command does what switching that light in
the app does, except Mira sends the switch, so it takes effect in any mode:

| You type… | When | Mira… |
|---|---|---|
| `<light> off` | awake (incl. wake, hold, nap) | sends the off (15 s fade) and marks it **out for today**, unless its scheduled off has passed (then it's just off). It never starts a nap, even on the last light on; from 21:00 the last light out still means asleep |
| `<light> off` | asleep | sends the off if the light is on |
| `<light> on` | in charge | sets *want on*, clears *out*, and fades it into line over 15 s |
| `<light> on` | hold | the same, right away, but to the **hold level** over 15 s; the other lights stay held |
| `<light> on` | nap | ends the nap. The rest of the nap set comes back too |
| `<light> on` | asleep, 06:00 to 21:00 | starts the wake, with this light riding it from the start |
| `<light> on` | asleep, otherwise | refused: `Wake` starts the day |

The last writer still wins: a light turned on this way still goes off at its scheduled
off (ceiling 19:00, chest 20:00, dresser 22:30).

## 6. Hold, nap, and catching up

*(User asked: "what would cause a hold again? how are we defining hold vs pause?"
Answered Oct 14; the user then redefined when a hold ends. 3.0 retires the word
"pause": the 2.x soft pause splits into these two.)*

**Hold**
- *Cause:* a manual brightness change on any light while Mira is in charge. That
  includes the Hue app slider, a dimmer switch, voice, or a scene.
- *Not a hold:* a report within `IN_LINE_TOL_BRI` (5%) of the in-line level. A hold
  would protect nothing and freeze every other light. Late bridge reports caused most
  of these (`MIRA_3_FIRST_NIGHT.md` → Nov 2). Mira re-sends that one light, so a real
  small nudge can't leave a bulb stopped mid-fade.
- *Effect:* Mira **freezes every light where it is.** Any in-progress fade is stopped
  at its current level. The bulbs run fades themselves, so without a freeze the other
  lights would keep dimming. Then Mira sends nothing.
- *Ends at the start of the next fade period* (10:00, 17:00, 19:00, or 20:45), or at
  22:30 when no fade is left. Mira
  then fades every light from wherever it is to that period's target, in one command,
  arriving on schedule (user, Oct 14: "hold that brightness until the next fade
  period, which it then begins to fade to the next target"). A hold that starts
  *during* a fade lasts until the following fade period.
- *Ends early* if you resume from the dashboard (until the dashboard rework, the
  existing **NORMAL** button is resume), or if you turn every light off (→ nap before
  21:00, asleep after).
- *Ends at 22:30* if it starts after 20:45, when the dresser goes off (user, Nov 2).
- *Ends at 01:00 lights-out* if it starts after 22:30, because nothing else follows
  that night. **(Claude's call.)** Without this, a 23:45 brightness tweak would leave
  the lights on all night, contradicting "floor off at 1 AM".

**Nap**
- *Cause:* every light turned off before 21:00, while awake.
- *Effect:* 60 min of nothing.
- *Ends:*
  - The timer runs out: the lights that were on when the nap started fade back on
    (15 s), plus anything the schedule turned on during the nap, minus anything it
    turned off.
  - Or you turn any light on: the nap ends early and the rest come back with it.
  - **A nap still running at 21:00 becomes sleep** (no fade-in while you're in bed).
- Also clears a hold.

**Catching up** (end of a hold or nap)
- Events that came due during the pause have already updated *want on*. Mira
  reconciles in one move:
  - Lights that want on fade on (15 s).
  - Lights that don't fade off (15 s).
  - Brightness follows the §3 resume rule.
- Lights you switched yourself during a hold keep your choice when that was the later
  action (last writer wins).
- *Out for today* lights stay out.

## 7. Reboots

- **Soft resets** (panic, watchdog, wifi-watchdog, software restart): the remembered
  facts survive in RTC memory (if under 10 min old). Mira resumes without switching
  any lights. Lights that are on get the §3 resume rule. Events that came due while
  it was down still fire (the restored clock reference sees the crossing).
- **Power-on** (RTC memory lost), or RTC state that's stale, from an older layout, or
  corrupt: Mira infers its situation from the clock and light states:

| Situation at power-on | Mira treats it as |
|---|---|
| Any light on, 06:00–01:00 | today's wake already happened; Mira takes control via the resume rule |
| Everything off, 21:00–06:00 | asleep |
| Everything off, 06:00–09:00 | asleep, waiting for wake |
| Everything off, 09:00–21:00 | idle until a light turns on, which starts a late wake (a power blip mid-nap shouldn't switch lights on) |

- **The purple boot flourish** fires only on power-on outside the sleep window. A
  3 AM crash shouldn't flash the chest.

> **Closed by §10 (flashed Nov 2, unverified on the board):** a power-on with facts saved earlier in
> the same Mira-day restores them and replays what it missed. The table above is now
> the fallback for no usable save, or everything off.

---

## 8. Firmware architecture (as built)

A **pure core** (no Arduino; also compiled on the host for the native tests) sits
inside a **hardware shell** (everything that touches WiFi, the bridge, Railway, the
sensor, the clock).

| File | Role | Pure |
|---|---|---|
| `src/config.h` | Every tuning constant. **No longer includes `secrets.h`**, because the core must build where no secrets exist. | ✓ |
| `src/lights.h` | The roster: `LightIdx` (floor 0, dresser 1, chest 2, ceiling 3), `Role`, names, masks | ✓ |
| `src/timeline.h/.cpp` | Day profile (periods), CT map, fixed timed events, crossing detection (midnight + DST) | ✓ |
| `src/director.h/.cpp` | The behavior engine (§1–§7) | ✓ |
| `src/echo.h` | "Is this report one of Mira's own echoes?" (time-aware, see below) | ✓ |
| `src/events.h` | The event queue type both background tasks feed | |
| `src/bridge.h/.cpp` | Hue: cert pin, light cache, commands, the Core-0 SSE task + classifier | |
| `src/dashboard.h/.cpp` | Railway: the Core-0 net task (logs, status, command poll) | |
| `src/main.cpp` | Shell: WiFi + watchdog, boot reason, clock, lux, RTC persistence, wiring | |
| `test/test_core`, `test/test_legs` | Native tests (simulated days) | |

**The loop** (`main.cpp`)
- Drain `evQueue` into the Director (edges, brightness changes, reverts, dashboard
  commands).
- `director.tick()` at 1 Hz.
- Every :00/:30: read lux, print the status line, push status.
- Push status whenever the Director changed something.
- Copy the Director's state to RTC memory on every pass.
- Run the WiFi watchdog.

**The Director** (`director.cpp`)
- `reconcile()` compares, per light, what the period in force wants with the
  last command sent (`Command`: start/target brightness, start time, duration). It
  sends only on a difference, which is how "one command, leave it" is implemented.
  The tick, every input, and the end of every pause all just call it.
- **Join steps:** a light coming on (15 s fade-in) or being snapped in line (0.3 s)
  first lands on the level the period will have when the step ends. The next
  reconcile hands it the rest of the period.
- **Legs:** a fade longer than `MAX_FADE_MS` goes out in legs, each landing before
  the next. With today's schedule (≤ 60 min) this never triggers. It's the one-line
  fallback for hardware unknown #1, covered by `test_legs`.
- **Retries:**
  - An HTTP failure backs off at 5 → 10 → 20 → 40 → 60 s.
  - An on/off only counts as done when the light's echo confirms it, so a bulb
    powered off at the wall gets the same backoff instead of a retry every ~40 s, and
    the failure is logged once.
- **Holds:**
  - The freeze is a ~0.4 s command at each mid-fade light's computed current level.
    Hue's `dimming_delta: stop` would be an alternative but is untested.
  - Clobber repair re-sends your level if one of our commands sent < 3 s earlier
    could land on top of it.
- Time comes in as `Now {epoch ms, local seconds}`. The Director never touches a
  timezone: the shell's `localtime_r` does, so DST is handled once, in one place.

**The classifier** (`bridge.cpp`, Core-0 SSE task). It reads only `on` and `dimming`.

| Event | Outcome |
|---|---|
| on/off flip on a live trajectory | Mira's echo → `LightEdge(byUser=false)` |
| any other on/off flip | yours → `LightEdge(byUser=true)` |
| `zigbee_connectivity` lost or back | your off or on at the wall → `LightPower` |
| brightness within 3 s of a turn-on, or 30 s of a bulb regaining power | part of the turn-on (ignored) |
| brightness on a live trajectory (`echo.h`) | Mira's echo (ignored) |
| brightness within 5% of the cache | no-op (ignored) |
| brightness back at a recent command's start, < 90 s | stale revert → re-send (once; a repeat is ignored) |
| any other brightness change on a light that's on | yours → `UserBrightness` → hold |
| CT / color only | not tracked |

Plus the reconnect resync, which turns each missed on/off flip into an edge.

**Wall switches.** A bulb with no power can't report, so the bridge's light resource
keeps saying `on: true`. `zigbee_connectivity` is the signal, and it belongs to the
device, so bootstrap maps `light.owner.rid` to the roster. While a bulb has no power,
`lostPower` overrides the bridge's on/off everywhere, including the resync that runs
after every 10-minute reconnect, which would otherwise read it as turned back on.

**Echo recognition** (`echo.h`) is time-aware, because 3.0's fades run up to an hour:
- In the first `ECHO_EARLY_MS` (10 s) after a command, the whole start→target range
  counts as Mira's echo. The bridge echoes the target at once.
- After that, only values within `ECHO_PROGRESS_TOL_BRI` (8%) of where the fade should
  be by now count (mid-fade progress reports, if the bridge sends them), plus the
  target once the fade has landed.
- A blanket band would hide your changes for the whole fade.

**Persistence**
- An RTC-noinit blob holds `DirectorState` plus an FNV check seeded with the struct
  size.
- `STATE_MAGIC`'s last byte is the layout version: bump it whenever `DirectorState`
  changes.
- After a restore, commands still running are re-recorded in the echo ring.

**Kept from 2.x (ported, not rewritten in spirit):**
- WiFi watchdog + boot-reason reporting, plus a new `ntp-timeout` marker
- cert pinning (`peerMatchesPinned`)
- bootstrap + reconnect resync, plus one new resync right after the first stream
  connect (it closes the boot-snapshot → stream gap the flourish widened)
- the Core-0 SSE and net tasks
- drop-oldest log queue and depth-1 status queue
- heap/stack telemetry

**Dashboard mapping** (the terminal rework keeps this contract; see `DASHBOARD.md`)
- *Status:* `state` carries the old names so the current dashboard keeps rendering:

  | 3.0 | Reported as |
  |---|---|
  | in charge | `NORMAL` |
  | wake | `WAKE` |
  | hold | `SOFT_PAUSE` |
  | nap | `SOFT_PAUSE` (with remaining seconds) |
  | asleep | `LOCKED_OUT` |

  The status also carries:
  - `mode`: the 3.0 name
  - `lights_on`: what the bridge says is on
  - `bri`/`ct`: the in-line level
  - `excluded`: out-for-today
  - `wake_step`/`wake_total`: 30 s steps of the 15-min wake

  Retired fields are sent as 0, and `lux` is omitted when the sensor is missing.
- *Commands:* old names and new names both work, so the rework needs no reflash.

  | Old | New | 3.0 meaning |
  |---|---|---|
  | `NORMAL` | `RESUME` | end a hold or nap |
  | `SOFT_PAUSE` | `HOLD` | hold now |
  | `WAKE` | `WAKE` | start the wake (06:00–01:00) |
  | `LOCKED_OUT` | `SLEEP` | sleep now (lights untouched) |
  | `SET_SOFT_PAUSE_REMAINING` | `NAP_REMAINING` | nap minutes left |
  | | `NAP` | turn everything off and nap; `value` is minutes, default 60. Refused from 9 PM, while asleep, or already napping. Replaces a hold |
  | | `EXTEND` | add `value` minutes to the hold or nap. A nap still becomes asleep at 9 PM, and lights-out still ends a hold |
  | | `LIGHT_ON` / `LIGHT_OFF` | one light on, or off for today; `value` is the roster index (floor 0, dresser 1, chest 2, ceiling 3). §5 has what each does in each mode |

  `HARD_OFF`, `WIND_DOWN`, the step seeks, stable-lux, and exclude/include are
  acked and logged as retired. (`EXCLUDE_LIGHT`/`INCLUDE_LIGHT` stay retired: their
  values were 2.x roster indices, which `LIGHT_ON`/`LIGHT_OFF` don't share.)

**Tests**
- Run: `pio test -e native -e native_legs`. It needs a host gcc on PATH; CLion's
  MinGW works (`<CLion>\bin\mingw\bin`).
- Covered:
  - CT map, profile, fade-start/hold-end lookup, crossings (midnight, DST
    fall-back/spring-forward)
  - echo windows
  - a full day (24 commands, values and durations checked), manual and 6 AM wakes,
    the sleep window, a late wake after a cold boot
  - snap-then-glide, holds (steady and mid-fade, with freeze and deferred events),
    resume in a fade and in a steady period, the hold after the last fade
  - naps (timeout, early end, 21:00 → sleep), out-for-today and its scheduled-join
    block, all-off after 21:00
  - dashboard wake at night, nap-remaining 0
  - dashboard light commands: off is out for today (and not a nap on the last light;
    asleep after 21:00), off before a scheduled on, on mid-fade, on during a nap and a
    hold, asleep at night and in the day, a bad index
- Without PlatformIO (its registry was blocked in the Nov 5 cloud session), the same two
  envs build by hand: g++ `-std=gnu++11 -Wall -Wextra -I src` on `timeline.cpp`,
  `director.cpp` and the test file, linked with Unity's `unity.c`; add
  `-DMAX_FADE_MS=1800000UL` for `test_legs`.
  - HTTP-failure backoff, an unreachable bulb, stale revert
  - soft-reset restore (no commands re-sent; missed events still fire)
  - legs

## 9. Removed in 3.0

**Firmware:**
- `lightcurve.h` (`luxToTarget`, segments S1–S4; the CT map moved to `timeline.cpp`)
- `tickNormal` / `tickWakeRamp` / `tickWindDown` / `advanceResumeRamp` /
  `expireSoftPause` / `forceState`
- the stable-lux counter
- `sentTarget`, `chestOn` / `overheadsOn`
- exclusions (`cyclePut`, `enforceExclusions`), `saveLastState`
- `enum class State` (HARD_OFF, WIND_DOWN, and the rest)
- 30 s ramp soft timers, `ALIGNED_TICKS` (the telemetry tick is always aligned)
- the lux-gated wake constants, `LOCKOUT_RESET_MIN_OF_DAY`, `WIND_DOWN_GATE_HOUR`,
  `STATE_TOLERANCE_*`, `PAUSE_RESUME_TICKS`, `UTC_OFFSET_SEC`
- the `NTPClient` dependency
- the commented-out button code. It drove 2.x states and can't be revived as-is; the
  tag keeps it.

**Docs (deleted):** `LIGHTCURVE.md`, `SYSTEM.md`, `FIRMWARE.md`,
`System Explanation OLD.md`.

**Deferred to the dashboard session** (done in the terminal rework, merged Nov 3, except the "away" toggle
and the lux page's 2.x curve math, which stays as a legacy page):
- `lux_curve.html`, the `/lux` page, and the curve-JS duplicates in `index.html`
- the dashboard exclusion chips and the HARD_OFF/WIND_DOWN buttons
- with HARD_OFF gone, nothing stops the 09:00 wake while you're away. An "away"
  toggle is a candidate.

---

## 10. Surviving a power loss

*(Designed Oct 15 after the laptop powering the board died; see
`MIRA_3_FIRST_NIGHT.md` → Day 1. Built and flashed Nov 2, unverified on the board.)*

**As built, where it differs from the design below:**
- The everything-off rule (step 4) has no code of its own: with every bulb off, boot
  skips the save and takes the §7 inference, which already idles until a light turns on.
- The log line is `Boot — power loss; caught up from 10:00AM`, without an event count.
- Open question 1 stayed at its recommendation: the no-save fallback isn't tightened.
- NVS's own CRC serves as the check, so `DayFacts` has a magic but no FNV.

### The problem

`DirectorState` lives in RTC-noinit memory. That survives a panic or watchdog reset
but not a power loss. With no memory, the §7 power-on rule takes control of **every
light that's on** (`s_.want = physOn_`, `director.cpp:140`) and sets
`lastLocalSec = now`, so the timeline events missed while the board was dead never
fire. Hue bulbs hold their last command, so they also sat at the 11:00–17:00 level
(100%) the whole time the board was out.

Oct 15: power lost 14:51, back 23:36. Missed: 17:00 fade, 19:00 ceiling off, 20:00
fade + 21:00 chest off, 23:00 fade + 23:30 dresser off. At boot Mira dimmed the
lights that were on to 25% (correct) and left the dresser and chest on (wrong).

The soft-reset path already does the right thing: it restores the state, re-seeds
*want on* from the bulbs, then the first tick fires every event crossed since
`lastLocalSec`, and those events switch lights. **The fix is to give power-on that
same path, from a copy that survives losing power.**

### The design

**1. Save the day's facts to NVS when they change.**
- New POD struct `DayFacts`: `magic`, `asleep`, `wakeFlags`, `want`, `out`,
  `wakeEndBri`, `wakeStartMs`, `napUntilMs`, `holdUntilMs`, plus a stamp `savedMs` /
  `savedLocalSec` (≈ 48 bytes). Leave out `cmd[]` and `lastTickMs`. Those change on
  every command and every tick, and boot rebuilds commands anyway (one takeover
  command per light).
- `main.cpp` compares the Director's current facts with the last saved copy on each
  loop pass. When they differ, it writes them (stamped with now) with one
  `Preferences.putBytes("dayFacts")` in the existing `"mira"` namespace.
- Estimated write rate: wake (≈4), the 19:00/21:00/23:30 offs, asleep, plus each
  hold, nap, and light you switch. Roughly **10–40 writes a day** at ~48 bytes, well
  within NVS wear-leveling. A fade start changes only `cmd[]`, so it doesn't write.
- **Why stamping at save time is enough** (there's no heartbeat, so the board's actual
  time of death is unknown): any event that changed a fact would have written a new
  save. So every event between the last save and the power loss was a no-op, and
  replaying it again is also a no-op. Replaying from `savedLocalSec` is therefore
  exact. (Oct 15: the last save was the 10:00 ceiling-on; replaying 10:00→23:36 means
  the 10:00 event fires again, which does nothing, then the evening offs.)

**2. At power-on, restore if the save is from the same Mira-day.**
- Valid means: magic and check pass, and the save's **Mira-day** (local date, with
  the day starting at 06:00) equals now's. A save from yesterday's Mira-day falls
  through to today's §7 inference, so a power cut overnight still ends in the normal
  wake.
- Restore exactly like a soft reset: copy the facts. If not asleep, `want = physOn`
  and `out &= ~physOn` ("the bulbs are the truth"). Then replay.

**3. Replay in Mira-day seconds, not `crossedLocal()`.**
- `crossedLocal()` assumes ticks are close together. Its 12 h midnight heuristic
  misreads long gaps: 07:00 → 05:00 the next morning (prev − now = 2 h) reads as a
  DST step back and crosses nothing.
- Within one Mira-day, `toMds()` only goes up, so the replay fires each
  `TIMELINE_EVENTS` entry with `savedMds < toMds(ev) ≤ nowMds`, in order, through the
  same `processEvents` switch. Then it sets `lastLocalSec = now`. A new
  `Director::replay(prevMds, nowMds)` (pure core, testable on the host).
- Timers sort themselves out on the first tick: an expired `holdUntilMs` ends the
  hold (fade to the period target), an expired nap ends (or became sleep, if the
  replay crossed 21:00).

**4. One new rule: everything off at restore.**
If the restored state isn't asleep but every bulb is off (you switched everything off
while the board was dead), apply §5's all-off rule *without* starting a nap: at or
after 21:00 → asleep; before → idle, waiting for a light-on (matching §7's "a power
blip mid-nap shouldn't switch lights on").

**5. The no-save fallback** (first boot ever, a save from another Mira-day, a
corrupt save) keeps §7's inference. Open question 1 asks whether to tighten it.

### What Oct 15 would have looked like

Restore at 23:36 → `want = {floor, dresser, chest, ceiling}` (whatever was on) →
replay fires 19:00 ceiling off, 21:00 chest off, 23:30 dresser off → reconcile: three
15 s fade-offs, and the floor gets the 25% steady-period level. Log: `Boot — power
loss; caught up from 10:00AM (3 events)`.

### Known limits

- **A light you turned on while the board was dead can get switched off.** If you'd
  turned the ceiling on at 22:00 with the board out, the 19:00 ceiling-off replay
  clears it, because the bridge doesn't say *when* a light came on. That's acceptable
  for an outage. Once the board is on wall power, outages should be rare.
- **The double boot** (Oct 15: two `power-on`s 15 s apart, likely USB power-cycling
  during laptop startup): the first boot's restore + replay writes a fresh save, so
  the second boot restores that and replays nothing. No special handling needed.
- **DST fall-back** repeats 1–2 AM. A replay across it fires the 01:00 lights-out
  once (one entry in the table), which is right.

### Tests to add (native)

1. `test_power_loss_replays_evening_offs`: Oct 15 exactly. Save at 10:00, power-on
   23:36 with all four at 100% → ceiling, chest, dresser fade off; floor → 25%.
2. `test_power_loss_save_from_yesterday_infers`: save from the previous Mira-day →
   §7 inference, no replay.
3. `test_power_loss_long_gap_same_mira_day`: save 07:00, power-on 05:00 next
   morning → replay crosses the whole day and ends asleep (the case `crossedLocal()`
   gets wrong).
4. `test_power_loss_all_off_after_2100_sleeps`, and the before-21:00 idle case.
5. `test_power_loss_mid_hold_ends_hold`: a hold saved at 15:00, power-on 17:30 →
   the hold ended, fade to the 17:00–18:00 target over the remaining 30 min.
6. `test_save_only_on_fact_change`: a full simulated day writes ≤ ~15 saves, and fade
   starts don't write.

### Open questions for the user

1. **Tighten the no-save fallback too?** With no save, take control of only the
   lights that are both on *and* scheduled on now (`physOn ∩ roleScheduledOn`). It
   would have fixed Oct 15 even without NVS, but after a blip it switches off a light
   you'd deliberately added. Recommendation: **no**; with NVS the fallback only
   covers the first boot of a Mira-day.
2. **Log line wording** for a catch-up boot (proposal above).
3. **Wall power.** A USB wall adapter instead of the laptop removes the cause. The
   fix is still worth having for real power cuts.

---

## Decisions Log

All Oct 14, 2026, from the three `V3 Migration Docs/` inputs unless marked
**(Claude's call)**.

**Scope and structure**
- **Version 3.0**, not 2.0. 2.0 was the unfinished "super reactive" line. Tag
  `mira-2-final`, branch `mira-3` (merged into `main` Nov 5).
- Time-of-day timeline replaces lux control. **Lux is reporting only** and is
  expected to be fully deprecated soon.
- **No state machine.** Behavior is commands; a few remembered facts gate them.
- **One command per fade** (target + CT + duration). The bulbs fade. No 30 s ramps.

**The day and the wake**
- **CT map everywhere** (user declined pinning evenings warm).
- **Wake:**
  - window opens 06:00
  - auto at 09:00
  - any light-on after 06:00 triggers it
  - a light already on at 06:00 triggers it
  - start level **25%** ("will have to be tested")
  - joins at **+5 / +10 min**
- **Asleep:** from lights-out (all off at/after 21:00, or the 01:00 lights-out)
  until the wake window. Light-ons in between are left alone.
- **01:00 lights-out turns off everything** still on, not just the floor.

**Your changes**
- **Brightness change → hold.** Freeze all; held **until the next fade period
  starts**, which then fades everything to that period's target (revised Oct 14, was
  "until resumed from the dashboard"). Dashboard resume and all-off still end it
  early.
- **All off before 21:00 → nap.** 60 min, then a 15 s fade back in.
  - Restore set = what was on at nap start, plus schedule changes during the nap.
  - Turning a light on ends it early, with the rest coming back too.
  - A nap still running at 21:00 becomes sleep.
- **One light off → out for today.** Rejoins when you turn it back on, or at the
  next wake.
- **Out for today only while the schedule still wants the light (user, Nov 5).** The
  user turned the chest on and off from the terminal after its 20:00 off, and it showed
  as out for today: "it is normally out at this time because of the schedule, therefore,
  it should not show as out for the day." The rule (Claude's proposal, accepted): an off
  counts only before that light's scheduled off (ceiling 19:00, chest 20:00, dresser
  22:30, floor at lights-out), so a ceiling turned off at 9:30 still stays off at 10:00.
  The bit also clears at the light's scheduled off and when Mira goes to sleep. Applies
  to the terminal, the Hue app and wall switches alike.
- **A light turned on during a hold joins the hold level (user, Nov 5).** The user
  raised the brightness with the chest off, then turned the chest on, and expected it
  to come up to the hold's level. Accepted from Claude's options: every turn-on (the
  terminal over 15 s, the Hue app over 0.3 s, power back at the wall over 15 s), and the
  hold level is your latest brightness in that hold. A hold from the dashboard's `hold`
  uses the level the lights froze at. The level lives in `DirectorState.holdBri`
  (`STATE_MAGIC` …0302) but not in `DayFacts`: after a power loss mid-hold, a light
  turned on joins in line instead.
- **Color/CT changes are not tracked** (user swapped the earlier "color change =
  remove light" idea for "light off = out for today").
- **Scenes** need no special case. Their brightness change is a hold, and their CT
  change is ignored.

**Commands and catch-up**
- **Events during a hold/nap fire when it ends** (catch-up via *want on*, last
  writer wins).
- **Resume rule:**
  - inside a fade → glide to the fade's end over the time left
  - inside a steady period → 15 s fade
- **15 s fades** for every light Mira turns on or off.

**Kept and removed**
- **HARD_OFF removed.** Dashboard **exclusions removed** (replaced by out for today).
- **WiFi watchdog + telemetry kept.** Override *detection* reworked (§8).
- **R8:** SNTP + POSIX TZ for US Central with automatic DST.
- **Lux code + lux-only docs deleted on the branch** (user: "remove all unnecessary
  lux related code and files"). Dashboard lux files wait for the dashboard session.

**Build approach (user)**
- Architect and build the whole system before it runs on the board. Ground-up
  rewrite; no staged flashing.
- A light you turn on is corrected **near-instantly** (`JOIN_SNAP_MS` = 300 ms, user
  asked for "near instant"). The 15 s fades apply to lights Mira switches itself.

**Claude's calls, accepted by the user Oct 14**
- Grouping window and on-edge grace ≈ 3 s.
- Boot inference rules (§7) and flourish gating.
- Interim dashboard mapping (§8).
- `PRESET_OFF_LIGHTS` defaults **off** until tested (see Hardware unknowns).
- Button code deleted rather than commented. It drove 2.x states; the tag preserves
  it. This bends the comment-vs-delete convention.

**Claude's calls during the build (user away; veto freely)**
- A hold that starts after the last fade period (20:45 since Nov 2) ends at 01:00
  lights-out (§6).
- **Pure core + shell split, with native tests.** `config.h` stops including
  `secrets.h`; the shell files include it directly.
- **Roster reordered** (floor = 0 … ceiling = 3; masks, cache, and persistence all
  follow it). The UUIDs and `secrets.h` are unchanged. Ceiling_2 is a commented row.
- **New dashboard command names** (`RESUME`/`HOLD`/`SLEEP`/`NAP_REMAINING`) are
  accepted alongside the old ones, so the rework can switch without a reflash.
- **The dashboard `WAKE` while already awake** restarts the wake: the explicit
  command wins. It's ignored between 01:00 and 06:00.
- **A missing lux sensor is no longer fatal** (2.x hung at boot blinking red).
- **The telemetry tick is always aligned** to :00/:30, so `ALIGNED_TICKS` is gone.
  It no longer drives lights.
- **The `ntp-timeout` boot reason:** no first time sync within 2 min → restart,
  marked like the wifi-watchdog restart.
- **The freeze uses computed-level commands.** `dimming_delta: stop` stays a
  hardware experiment.

**Evening schedule revised (user, Nov 2)**
- 75 → 40% moved an hour earlier, to 19:00–20:00. The chest still goes off when that
  fade ends, at 40%, which is now 20:00.
- 40 → 25% moved to 20:45–21:00.
- The dresser goes off at 22:30, at 25%, and a hold started after 20:45 ends then. After
  22:30 it's just the floor.
- `SLEEP_CUTOFF` stays 21:00.
- A day is still 24 commands.

**Wall switches and false holds (user, Nov 2)**
- A light cut at the wall is **out for today**. When it's restored it **fades** into line
  (15 s, brightness and CT), rather than the 0.3 s snap of a light you turn on. Every
  light cut at once is left to the normal nap/asleep rule: the floor lamp has no switch,
  so the user considers it very unlikely.
- A report within 5% of the in-line level isn't a hold (Claude's proposal, accepted).

**Dashboard light commands (user, Nov 5)**
- The user asked for admins to type something like `dresser off` or `chest on` in Mira to
  "turn those lights on or off for the day". Off is *out for today*; on clears it.
- **(Claude's calls, change any)**
  - Each command does what the same switch in the Hue app does (§5), so the rules stay the
    ones already agreed. Where the app has nothing to copy, the command acts now: a hold
    protects your change, and this is your change. On during a hold lands at the hold
    level (changed Nov 5, see above; first built at the in-line level), with a 15 s fade-in like Mira's other turn-ons; off fades over 15 s like its
    other offs.
  - Off never starts a nap, even on the last light on: one named light isn't "everything
    off", and a nap would bring it back in an hour. `nap` does that.
  - Asleep, on starts the wake from 06:00 to 21:00, as the app does; outside that it's
    refused, because Mira has no level to give a light at night. Off is sent if the light
    is on.
  - New wire names, `LIGHT_ON`/`LIGHT_OFF`, with the 3.0 roster index.
  - One help row, `<light> on|off`, admin only like the other controls.
  - The site counts a board line `Dashboard command X is unknown`/`retired` as a refusal
    for any command, so until the reflash a light command shows the board refusing it.

**Found in build/review (fixed before any flash)**
1. **The freeze would have dropped lights to ~1% after a cold boot.** The Director
   didn't know the bulbs' brightness at boot, so a freeze computed "where the fade
   is now" from 0%. Boot now seeds each light's level from the bridge snapshot.
   Caught while writing the hold test.
2. **A blanket echo band hid your changes inside hour-long fades.** 2.x matched any
   report between a command's start and end values. With 30 s ramps that was narrow;
   with 60-min fades it would hide a 75→100 fade's whole range for an hour. It's now
   time-aware (`echo.h`, §8).
   - *Known blind spots:* a change within ~8% of where a fade currently is, or within
     ~5% of its target, reads as Mira's. Those are small nudges, and the bulb still
     takes your level. Larger changes hold.
3. **The on/off stale-revert reading was dropped.** Switching off the ceiling that
   just came on at 10:00 looks exactly like the bridge walking back our "on", and 2.x
   would have re-sent it. Your on/off is never fought now. Brightness reverts keep
   the re-send-once behavior.
4. **An unreachable bulb was retried every ~40 s forever, logging each time.** A
   successful HTTP send reset the backoff even though the bulb never came on. On/off
   now counts as done only on its echo. Covered by
   `test_unreachable_bulb_backs_off_and_logs_once`.
5. **The boot-snapshot → stream gap:** changes made during boot (the flourish alone
   takes 3 s) went unseen until the next reconnect. Fixed with a resync right after
   the first stream connect.

---

## Hardware unknowns: verify on the first flashes

*Status from the Railway logs, Oct 14 – Nov 2 (`MIRA_3_FIRST_NIGHT.md` → "Nov 2"):*
*#1 the bridge accepts 60-min fades, landing time unmeasured; #2 mid-fade reports do*
*appear; #3, #4, #5 no data; #6 likely linear; #7 confirmed working by the user, Nov 5.*

1. **60-min single-command fades.** Hue's transition field appears to cap near
   109 min (16 bits of tenths of a second, inherited from v1). The longest stage is
   60 min. Verify one lands on time. If the bridge rejects it (the log shows the HTTP
   code and `bridge said:` body), lower `MAX_FADE_MS`; fades then go in legs.
2. **SSE during long fades.** Does the bridge report intermediate brightness while a
   bulb fades?
   - If yes: those reports should land within `ECHO_PROGRESS_TOL_BRI` of the linear
     path. Watch a full fade for false holds; widen the tolerance if the bulb's curve
     isn't linear.
   - If no: nothing to tune.
3. **`dimming_delta: stop` on v2.** Third-party specs list `up`/`down`/`stop`;
   official semantics are unconfirmed. The freeze doesn't use it.
4. **Pre-setting off bulbs.** The bridge accepts dimming on "soft off" lights with a
   "may not have effect" warning, and community reports conflict. If our bulbs honor
   it, a light turned on lands "just right" with no snap. Test behind
   `PRESET_OFF_LIGHTS`.
5. **Faded-off bulbs return at minimum brightness.** Confirm. It only matters in the
   sleep window.
6. **Linearity.** The freeze and the echo progress check both assume a bulb's fade is
   linear in the reported brightness percent. Watch the freeze for a visible jump.

7. **Wall switches.** Does the bridge send `zigbee_connectivity` when a bulb loses
   power, and how soon? Is `owner.rid` on the stream event as assumed? An off-and-on
   faster than the bridge notices sends no event, so an off-path report still holds.
   **Tested: the user confirmed the wall-switch handling works on the real lights, Nov 5.**
   No timings were reported, and the quick off-and-on (Nov 2 checklist item 2) is still open.

## First-flash checklist (step 5)

1. **Before flashing:** stop the soak logger if it's holding COM5. Then
   `pio run --target upload` and `pio device monitor`.
2. **Boot log:**
   - `Online (<reason>) — Mira 3.0`, `Clock synced.`, the bootstrap lines, then one
     `Boot — …` line.
   - The `[time]` on the status line (every :00/:30) is correct **Central** time.
3. **Taking over:** if lights are on at boot, there should be one 15 s command per
   light, then silence.
4. **A fade start** (17:00 or 19:00 is easiest):
   - one `PUT … over 3600.0s : 200` per light
   - **no `SSE: you set …` line for the next hour** (unknowns #1, #2)
5. **Light on mid-fade (Hue app):** a 0.3 s snap, then one glide command.
6. **Brightness change:**
   - `SSE: you set X to Y% — hold`, then the hold diagnostic block
   - 0.4 s freeze commands to the other mid-fade lights
   - the dashboard shows `SOFT_PAUSE`
   - it ends at the next fade start (or press NORMAL to resume early)
7. **One light off:** `X off — out for today` after ~3 s. Turn it back on and it
   rejoins.
8. **Everything off before 21:00:** a nap; the lights return after 60 min. The legacy
   pause slider (`SET_SOFT_PAUSE_REMAINING`) changes the time left.
9. **Evening and night:**
   - the 19:00 / 20:00 / 22:30 offs
   - all off after 21:00 → `Asleep`
   - 01:00 lights-out
   - next morning: the 06:00 or 09:00 wake
10. **Soft reset:** the next panic or wifi-watchdog restart should log
    `Boot — picked up where it left off`, and no lights should switch.

## Nov 2 reflash checklist

Flashed Nov 2 while the board was away from home WiFi, so run these once it's back. First
confirm the dashboard log shows `Online (…) — Mira 3.0` and a `Boot — …` line.

1. **Done: the user confirmed it on the real lights, Nov 5.** **Wall switch (unknown #7):**
   with serial running, cut the chest at its switch. Time
   `SSE: Chest connectivity connectivity_issue`, then expect `Chest lost power` and
   `Chest off — out for today`. Wait past a 10-min `SSE: stale` reconnect: it must stay
   out. Restore it: `Chest has power again`, then a 15 s PUT and no `you set` hold.
2. **Quick flip:** off and on within a few seconds, and note what the log shows.
3. **Done: the user tested it Nov 5 and believes it worked as expected.** **Power loss:** with lights on, unplug the board across a scheduled event, then plug
   it back in. Expect `Boot — power loss; caught up from …` and the missed offs.
4. **False holds:** watch a morning wake for `reported N% — in line, no hold` where a
   `Hold` used to appear.

## Nov 5 reflash checklist (light commands)

Before flashing, `dresser off` in the terminal should report the board refusing it
(`Dashboard command LIGHT_OFF is unknown`). Then `pio run`, flash, and confirm
`Online (…) — Mira 3.0`.

1. **Off:** `dresser off` → `Dresser off for today — dashboard`, one 15 s off PUT, and
   `status` lists the dresser as out for today. The next scheduled event leaves it off.
2. **On:** `dresser on` → `Dresser on — dashboard`, a 15 s fade into line, then one glide
   command if a fade is running. No `you set` hold.
3. **During a hold:** `chest on` turns on only the chest, fading to the level you set
   (not the schedule's); the held lights don't move. The same from the Hue app (0.3 s).
4. **Last light:** `floor off` with only the floor on before 9 PM stays awake, with no nap.
5. **After a scheduled off (the Nov 5 bug):** after 20:00, `chest on` then `chest off` →
   `Chest off — dashboard (off on the schedule now)`, the terminal says `off.`, and
   `status` doesn't list the chest as out for today. From the Hue app the line is
   `Chest off — off on the schedule now`.
6. **Out clears on its own:** a light that's out for today drops off the `status` list at
   its scheduled off, and nothing is listed overnight.

## Nov 5 reflash checklist (schedule post)

**All pass (user, Nov 5).** Then reflashed as 3.1 with the lux sensor removed: confirm
`Online (…) — Mira 3.1` in the log, and that `status` has no room-light row.

The board posts its schedule (`timeline.cpp` periods and events + the wake, CT and nap
constants from `config.h`) to `POST /api/schedule` once at boot, retrying every
`SCHEDULE_RETRY_MS` (5 min) until the site takes it, and again on the dashboard command
`SEND_SCHEDULE` (`schedule update` in the terminal), which netTask answers itself. The site
shows the board's copy over its own (`dashboard/schedule.py`). Deploy the site first: an
older site answers the boot post with 404, which only means retries.

Before flashing, `schedule update` should report the board refusing it
(`Dashboard command SEND_SCHEDULE is unknown`). Then `pio run`, flash, and confirm
`Online (…) — Mira 3.0`.

1. **Boot post:** Serial shows `Schedule sent to the dashboard.`, and `schedule check`
   says `the board sent this copy` with the boot's time.
2. **On request:** `schedule update` → `Schedule sent — dashboard` in the log, the terminal
   says `the board sent its schedule.`, and `schedule check` shows the new time.
3. **Same schedule:** `schedule` reads the same as before the flash (the site's copy and
   `config.h` already agree; `tests/test_schedule.py` checks that).
