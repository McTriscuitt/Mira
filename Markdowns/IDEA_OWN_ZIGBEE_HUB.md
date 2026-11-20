# Idea: Mira owns its Zigbee network (a possible Mira 4)

*Saved Nov 20, 2026, for further discussion. Nothing here is decided or built. Every
choice in "Open questions" is yours to make first. Prices and the Zigbee2MQTT claims
below come from memory, not from current listings or a test on these bulbs.*

## The idea in one paragraph

Stop making Mira guess who changed a light. Give Mira its own Zigbee radio, so it is
the only thing that commands the bulbs. Physical controls (a bedside dial, a button at
the door, a phone page) send Mira messages instead of changing the bulbs behind its
back. Move the brain off the dev board onto a small always-on computer (the hub). The
Nano ESP32 becomes the dial. `timeline`, `director`, the native tests and the behavior
rules in `MIRA_3.md` §1–§7 carry over.

## Why

**Mira hears about everything second-hand.** The bridge's event stream reports states
("chest is now 60%"), never who sent the command. Each command Mira sends comes back
looking like your change, and most of `bridge.cpp` exists to tell the two apart: the
echo ring, time-aware echo matching, stale reverts, on-edge grace, the off-grouping
window, the in-line filter, the `holdPending` mid-batch abort.

Most of `Bug Records/` is that one problem:
- the spring false soft pauses and echo misreads,
- the stale-revert and mid-batch patches,
- the Nov 8 Zigbee dropout read as a wall switch,
- Oct 14 – Nov 2: 8 of 14 holds landed within 1% of the level Mira itself was asking
  for (`MIRA_3_FIRST_NIGHT.md`).

Each fix narrowed a guessing window. None can close it, because who sent a change isn't
in the stream.

**The board does a server's job.** With the LCD abandoned, the buttons retired and the
lux sensor removed, it touches nothing physical. It is a headless HTTPS client that has
to be plugged into a laptop to change a schedule time.

**The lights depend on a lot.** WiFi, the bridge's HTTPS API, the cert pin and the
10-minute stream reconnects all have to work for the schedule to run.

## The proposal

```
 bulbs x4 ──────┐
 door button ───┼─ Zigbee ── USB radio ─┐
 motion sensor ─┘                       │
                        ┌───────────────┴──── hub (always on) ─────────────────┐
                        │  Zigbee2MQTT ── broker ── mirad (timeline+director)  │
 bedside dial (ESP32) ──┼─ WiFi ────────────┘                                  │
                        └───────────────────────┬──────────────────────────────┘
                                                │ one outbound connection
                                   cloud broker ── Flask site ── Postgres
                                                │
                                   every visitor's browser, live
```

1. **Mira owns the Zigbee network.** A USB Zigbee radio on the hub, run by Zigbee2MQTT,
   replaces the Hue Bridge for these four bulbs. A bulb belongs to one Zigbee network,
   so the bulbs leave the Hue app. With Mira as the only sender, a report it didn't
   cause can only mean a bulb lost or regained power. The cert pin, HTTPS calls and
   stream reconnects go away.
2. **Your actions arrive as messages.** The dial, the door button and a phone page each
   publish an intent ("dial −3", "all off"). A hold starts because the knob turned.
3. **The brain moves to the hub.** It runs Zigbee2MQTT, an MQTT broker (Mosquitto) and
   `mirad`: the existing core compiled for Linux, with a small new MQTT shell around
   `Director`. Updates are a `git pull` and a service restart; logs are readable remotely.
4. **One message bus from bulb to browser.** The hub forwards public topics over one
   outbound connection to a broker beside the site. That replaces the 5 s command poll,
   the 4-stream cap and the load-bearing `-w 1`. A command's result is a message, not a
   log line to parse. Nothing connects inbound to the house, same as now.
5. **The schedule becomes data.** The times and levels in `config.h` move to a file the
   hub loads, editable from the site. The edit is previewed by the same C++ core
   compiled to WebAssembly, which is the planned `simulate`.

It would also answer several open notes in CLAUDE.md: near-instant dashboard updates,
the serial-like output page (a live log topic), webhooks (one authenticated intent
route, usable from iOS Shortcuts) and the "away" toggle (a motion sensor).

### Why the core moves cheaply

`Director` already reaches the world only through `Env::put()`/`preset()`/`log()` and
the `tick()`/`on…()` inputs, and it already compiles on a PC for the native tests. Moving
the brain means writing a new shell, not a new engine.

The "~109 min" fade cap comes from Zigbee itself: the ZCL transition field is 16 bits
of tenths of a second. Talking Zigbee directly gives hardware unknown #1 a spec answer
(still worth measuring).

## What still has to be inferred

Less than now, but not nothing:

- **Power events.** A bulb that regains power rejoins with a device announce. A bulb
  that briefly drops off the mesh (the Nov 8 storm) might also rejoin and announce.
  The Nov 8 rule should still apply: back on the network but still off means a dropout,
  not your switch. Trial step 1 should test this.
- **Noticing a power cut.** Zigbee2MQTT notices an unpowered bulb through its
  availability check, which can take minutes. That's probably fine, since the bulb is
  off either way, but it should be measured.
- **Other senders.** The Hue Bluetooth app or a remote bound directly to a bulb could
  still change it without Mira knowing. Check that nothing else can reach the bulbs.

## What happens to the code

| Part | Outcome |
|---|---|
| `timeline.*`, `director.*`, `test/`, `MIRA_3.md` §1–§7 | Kept. The Director sheds what exists for guessed inputs: stale reverts, the off-grouping window, the in-line filter, clobber repair |
| `echo.h`, `bridge.cpp` | Removed, replaced by a small MQTT adapter |
| `dashboard.cpp`, WiFi watchdog, RTC blob | Removed. A service restart and a state file replace them; the power-loss replay (§10) is in the core and stays |
| `dashboard/` | Kept, including login/security and the e2e. `broadcast.py` and the board's polling routes go |
| 3.1 firmware | Stays buildable as the rollback |

## Hardware

### Buy (estimates, unverified)

| Item | Roughly | Job |
|---|---|---|
| Raspberry Pi 4/5 or a used mini PC, power supply, SSD or good storage | $60–90 | The hub |
| USB Zigbee coordinator (e.g. Sonoff ZBDongle, SMLIGHT SLZB-07) + short USB extension | $25–35 | Mira's radio |
| Rotary encoder + small 3.3 V display (OLED or round TFT) | $10–20 | The dial, on the Nano already owned. Shortcut: an M5Stack Dial (~$35), a finished ESP32-S3 knob with a round screen |
| A Zigbee button or dimmer | $10–40 | "Everything off" at the door. If the wall switch is a toggle, a Lutron Aurora clamps over it and keeps the bulbs powered |
| Zigbee motion sensor (optional) | $10–25 | Away detection; a wake trigger that doesn't need a lamp touched |
| Two spare ESP32 boards + a short addressable LED strip | ~$25 | A dev board that isn't production, and the room twin below |

**The clock matters.** A Pi 4 has no real-time clock: after a power cut it needs the
internet for the time before the schedule can run, just like the board today. A Pi 5
with its RTC battery, or a mini PC, keeps time, so the lights would run with no WiFi
and no internet.

### Print (3D printer)

| Print | Job |
|---|---|
| Switch covers that hold the door button | Bulbs stay powered and the wall still "works". Make them lift off |
| Dial body, knob and stand | The physical control the LCD attempt was reaching for |
| Hub case | Hub and radio in one box |
| Scale model of the room with 4 addressable LEDs | Mirrors the real lights live, or runs a whole day in a minute on the real core. A test rig that isn't the bedroom, and a demo that travels |

## Hosting

No need to leave Railway. The limits hit so far (4-stream cap, `-w 1`) come from pushing
live data through a synchronous Flask server, not from the host. With a broker doing the
fan-out, Flask goes back to login, command authorization and history. The broker can be
a second small Railway service or a managed one.

Cloudflare (Pages + Durable Objects) would cost about nothing and load faster, but it
means rewriting login, lockouts and logging in TypeScript. Not worth it for this.

## Order, each step reversible

1. **Trial on a spare bulb.** Set up the hub and radio, pair Ceiling 2 (already outside
   the system) and measure the hardware unknowns on it (list below).
2. **Shadow run.** `mirad` drives only Ceiling 2 through real days while 3.1 keeps
   running the room.
3. **Build the daily controls.** Dial, door button and phone page come before cutover,
   because the Hue app goes away.
4. **Cutover.** Move the four bulbs. Rollback: reset them, re-add them to the Hue Bridge,
   plug the 3.1 board back in.
5. **Site.** Broker link, schedule editing, `simulate`, the twin.

### What the trial measures

- #1 a 60-min fade (and one near 109 min) lands on time.
- #2 whether bulbs report mid-fade, now only for display.
- #5 whether a faded-off bulb comes back at minimum.
- #7 power-on behavior after a wall-switch cut, the device announce, how soon a cut is noticed.
- The Nov 8 case: whether a dropout without a power cut also announces.
- The Signe gradient: brightness and color temperature through Zigbee2MQTT.
- Firmware updates through Zigbee2MQTT instead of the Hue app.

## Risks and costs

- **Untested on these bulbs.** Step 1 exists to find out.
- **The Hue app stops working for these four bulbs.**
- **One more computer to keep alive.** Zigbee2MQTT's data folder must be backed up, or a
  dead disk means re-pairing every bulb.
- **The brain is no longer firmware.** The microcontroller work moves to the dial and
  the twin (see open question 2).

## Alternatives considered

- **Own coordinator: ESP32-S3 + ESP32-H2 running Mira's own firmware.** The strongest
  embedded story, but the Zigbee stack is a closed binary and every pairing and device
  quirk becomes Mira's problem. It trades guessing for plumbing. Maybe a Mira 5.
- **Keep the Hue Bridge.** The app stays a back door, so the classifier stays.
- **Home Assistant + Adaptive Lighting.** A weekend's work if only the lights mattered,
  but then Mira's brain isn't yours.

## Open questions (yours to decide)

1. **Can the Hue app go for these four bulbs?** Everything hinges on this. What is it
   used for today that a dial, a door button and a phone page wouldn't cover?
2. **How much of Mira should be firmware?** From least to most embedded work:
   - (a) the dial is the Nano ESP32-S3 over WiFi + MQTT;
   - (b) the dial is an ESP32-C6 or H2 running your own Zigbee end-device firmware, joining
     Mira's network directly (no WiFi needed for it);
   - (c) the coordinator itself is your firmware (see alternatives).
3. **What does the dial turn?** The whole room together (matching the 3.0 rule that all
   lights update together) or one light at a time.
4. **Which hub?** Pi 4, Pi 5 (+ RTC battery), or a used mini PC.
5. **The door:** printed cover + button, or an off-the-shelf dimmer.
6. **The cloud broker:** a second Railway service or a managed one.
7. **Schedule editing on the site:** admin-only? Preview required before saving?
8. **Rollback window:** how long the 3.1 board and Hue Bridge stay ready.
9. **Name:** Mira 4.0, or something else.
