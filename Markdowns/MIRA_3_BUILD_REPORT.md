# Mira 3.0 Build Report (Oct 14, 2026)

This is the end-of-build summary you asked for when you left: what got built, the
problems found, the calls I made without you, and what's left. The full spec and
design are in `MIRA_3.md`. This file is the readable recap.

---

## Is 3.0 done?

**The code is done. It hasn't run on the board yet.**

| Step | What | Status |
|---|---|---|
| 0 | Spec + design doc (`MIRA_3.md`) | Done, confirmed by you |
| 1 | Core logic (schedule, decision-making, echo check) | Done |
| 2 | Tests that run the core through simulated days on the PC | Done: 31 tests, all pass |
| 3 | Hardware side (bridge, dashboard, WiFi, clock, lux, crash recovery) | Done |
| 4 | Compiles for the board + code review | Done: 0 warnings, RAM 19.4%, flash 31.0% |
| 5 | **First flash + checking the hardware unknowns** | **Not started.** Waiting for your go-ahead |
| 6 | Docs cleanup + merge `mira-3` into `main` | Docs done. The merge waits for step 5 |

You asked me to build the whole system before it ran on the board, so I didn't
flash. Everything that can be checked without the board has been checked.

**Git state**
- Branch `mira-3`, one local commit: `81765ee`
- The last lux version is tagged `mira-2-final` (local)
- Nothing is pushed. `secrets.h` is untouched and not committed

---

## What got built

### Core logic (no board code, so it can be tested on the PC)
- **`timeline`**: the day's times and levels, the brightness→color-temperature map,
  the fixed events (10:00 ceiling on, 19:00 ceiling off, etc.), and the clock math,
  including midnight and daylight saving.
- **`director`**: decides what each light should be doing and sends a command only
  when that changes. A full normal day is **24 commands total**.
- **`echo.h`**: tells Mira's own commands echoing back from the bridge apart from
  changes you make.

### Hardware side (rewritten from scratch)
- **`bridge`**: sends commands to the Hue bridge, listens to its event stream, keeps
  the security-certificate pin from 2.x.
- **`dashboard`**: logs, status, and command polling. The current dashboard keeps
  working unchanged.
- **`main`**: WiFi + the watchdog (kept as you asked), boot reasons, the clock (real
  Central time with automatic DST), lux reporting, and crash recovery.

### Removed
- The lux curve, the state machine, the old NTP library, and the button code.
- Four lux-era docs: `LIGHTCURVE.md`, `SYSTEM.md`, `FIRMWARE.md`,
  `System Explanation OLD.md`.
- All of it is still in the `mira-2-final` tag.

### Docs
- `MIRA_3.md` is the new main reference.
- `CLAUDE.md` is rewritten for 3.0. Your User Notes are kept as they were, with short
  italic *3.0:* comments where 3.0 changes the answer.
- The other old docs have a banner marking them as 2.x history.

---

## Problems found and fixed (before any flash)

**1. After a reboot, a hold would have dropped the lights to about 1%.**
A hold freezes each light where its fade currently is. After a cold boot, Mira didn't
know how bright the bulbs were, so it worked out "where the fade is" starting from 0%.
*Fix:* at boot, Mira reads each bulb's real brightness from the bridge. Found while
writing the hold test.

**2. Your brightness changes would have gone unnoticed during long fades.**
2.x treated any brightness reading between a command's start and end as Mira's own
echo. That was fine for 30-second ramps. With an hour-long 75→100% fade, *any* change
you made in that range would have been ignored for the whole hour.
*Fix:* Mira now only accepts readings near where the fade **should be right now**.
*Trade-off:* a nudge within about 8% of the fade's current level, or about 5% of its
target, still reads as Mira's. The bulb keeps your level, but no hold starts.
Anything bigger starts a hold.

**3. Mira could have reversed your on/off switches.**
If you turned off the ceiling right after it came on at 10:00, the old logic would have
read that as the bridge glitching and sent "on" again.
*Fix:* Mira never undoes an on/off change you make. Brightness glitches are still
re-sent once.

**4. A bulb switched off at the wall would have been retried every 40 seconds forever.**
The bridge accepts the command even when a bulb can't receive it, so Mira thought it
had worked, then noticed the bulb was still off and tried again, posting a dashboard
message every time.
*Fix:* on/off only counts as done once the bulb confirms it. Retries slow down to once
a minute, and the problem is logged once. A test covers this.

**5. Light changes made during startup went unseen.**
Boot takes a few seconds (the purple flash alone is 3 s). Anything you changed in that
time was missed until the next stream reconnect.
*Fix:* Mira re-reads every light right after it first connects to the bridge.

---

## Decisions I made while you were away

All of these can be changed. Tell me which ones you don't like.

1. **A hold started after 11 PM ends at the 1 AM lights-out.** A hold normally lasts
   until the next fade starts, and after 23:00 there isn't one, so without this a late
   tweak would keep the lights on all night.
2. **Dashboard WAKE while already awake restarts the wake.** I took the explicit
   command to mean you want it. Between 1 and 6 AM it's ignored.
3. **New dashboard command names** (`RESUME`, `HOLD`, `SLEEP`, `NAP_REMAINING`) are
   accepted alongside the old ones, so the dashboard rework won't need a reflash.
4. **The freeze sends each light to its calculated current level** rather than Hue's
   "stop fading" command, which I haven't been able to confirm works.
5. **A missing lux sensor no longer stops the board from booting.** 2.x sat blinking
   red.
6. **If the clock can't sync within 2 minutes, the board restarts** and logs the
   reason as `ntp-timeout`, the same way the WiFi watchdog does.
7. **Hour-long fades can be split into shorter pieces** by changing one line
   (`MAX_FADE_MS` in `config.h`), in case the bridge rejects them. This is tested.
8. **The light list is reordered** (floor, dresser, chest, ceiling). The light IDs and
   `secrets.h` are unchanged. Ceiling 2 is a commented-out row.
9. **The button code was deleted, not commented out.** This breaks your usual
   comment-don't-delete rule. It controlled 2.x states that no longer exist, so
   commented out it would never compile again. The tag keeps it.
10. **The 30-second telemetry tick is always on :00/:30.** The `ALIGNED_TICKS` switch
    is gone because the tick no longer moves any lights.
11. **The boot purple flash** only runs on a real power-on outside sleep hours, so a
    crash at 3 AM doesn't flash the room.

---

## Tests

- **31 tests, all passing.** 30 in `test/test_core`, 1 in `test/test_legs`.
- Most of them run simulated days through the core: a full day, wake (each trigger),
  holds and resumes, naps, sleep, a light going out for the day, reboots, failed
  commands, an unreachable bulb, and splitting long fades.
- The rest check the building blocks directly: the color map, the day's levels, the
  echo check, and the clock math across midnight and DST changes.
- **Check that the tests have teeth:** I disabled the hold's freeze on purpose. The
  tests failed, so they catch real breakage.

**Running them yourself (PowerShell):**
```
$env:PATH = "C:\Program Files\JetBrains\CLion 2024.3.1.1\bin\mingw\bin;" + $env:PATH
pio test -e native -e native_legs
```
They need a PC compiler. CLion's bundled one is the only one on this machine.

---

## Still unknown until it's on the board

The tests can't answer these. They need the real bridge and bulbs.

1. **Does an hour-long fade work as one command?** The Hue limit is believed to be
   about 109 min. If it's rejected, the log shows the bridge's reply and decision #7
   is the fix.
2. **Does the bridge report brightness partway through a fade?** If it does, those
   reports must not be mistaken for your changes. **This is the biggest risk.** Watch
   one full fade.
3. **Does Hue's "stop fading" command work?** Not used, just worth knowing.
4. **Can an off bulb be pre-set** so it turns on at the right level? Off by default
   (`PRESET_OFF_LIGHTS`).
5. **Do bulbs that faded to off come back on at minimum brightness?**
6. **Do fades move evenly?** The freeze assumes they do. A visible jump on a hold
   would mean they don't.

---

## What's next

1. **First flash**, when you say go. Stop the soak logger on COM5 first. The 10-step
   checklist is at the end of `MIRA_3.md`.
2. **Merge `mira-3` into `main`** once it's verified.
3. **Dashboard session:** retire the lux page, add an "away" toggle (HARD_OFF is
   gone), switch to the new command names, and maybe add a nap button.
4. **Daylight saving ends Nov 1.** The board still runs 2.x, which uses a fixed clock
   offset. If 3.0 isn't flashed by then, the 2.x evening times will be an hour off.
   Stopgap on `main`: change `UTC_OFFSET_SEC` from `-18000` to `-21600`.
5. **Push to GitHub** whenever you want. I haven't.
