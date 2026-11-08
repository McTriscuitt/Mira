# Nov 8, 2026: a Zigbee dropout turned the dresser back on

**Status: fixed and flashed Nov 8, 1:13 AM** (`Online (sw-restart) — Mira 3.1`). Checklist:
"Nov 8 reflash checklist" at the end of `Markdowns/MIRA_3.md`.

## What the user saw

The dresser should have been off (scheduled off at 10:30 PM). It was turned off and then
"flipped back on at around 12am". Nobody touched a wall switch. A storm was going through,
and the home WiFi was struggling.

## The log (Saturday night, Nov 7–8)

```
10:21:52 PM  Chest lost power
10:22:20 PM  Chest has power again
10:30:06 PM  Dresser off                                  ← scheduled
10:44:xx PM  Floor lost power / Floor off — out for today / Floor has power again
11:17:52 PM  Dresser lost power
11:17:57 PM  Dresser off — off on the schedule now
11:18:07 PM  Dresser has power again                      ← Mira turned it ON
11:48:31 PM  Dresser off — dashboard                      ← the user, from the terminal
12:00:40 AM  Dresser lost power
12:00:42 AM  Dresser off — off on the schedule now
12:00:53 AM  Dresser has power again                      ← ON again (what the user saw)
12:36:50 AM  Online (power-on)                            ← the user moved the board to the laptop
 1:05:26 AM  Floor lost power / 1:05:50 Floor has power again   (asleep: left alone)
```

The 12:36 reboot was harmless: a power-on boot sets *want on* from what the bulbs are doing,
so it switches nothing.

## Cause

The Nov 2 wall-switch handling reads `zigbee_connectivity` as the wall switch: lost is your
off, and `connected` again is your on (`applyConnectivity` → `LightPower` →
`Director::onPower` → `onEdge(on, byUser=true)`). Your on sets *want on*, and reconcile
fades the light into line over 15 s.

A powered bulb that drops off the Zigbee mesh for a few seconds produces the same
`connectivity_issue` → `connected` pair. Every pair that night lasted 10–25 s. The dresser
was off and powered the whole time, so "connected" was read as you switching it on, and
Mira turned it on.

## Fix

On `connected`, `applyConnectivity` asks the bridge for that light's on/off
(`bridgeSaysOn`, one GET, one retry) before deciding:

- **Bridge says off:** a dropout. `lostPower` clears, the cache stays off, and a `LightPower`
  goes out with flags bit1 set. `Director::onPower(…, stillOff=true)` only logs
  `Dresser back — still off, a dropout, not a switch`.
- **Bridge says on:** your switch, as before. A bulb restored at the wall comes on by itself,
  or the bridge still has the `on: true` from before the cut.
- **Bridge can't be asked:** counts as your on, as before (Serial: `light check: … unreadable`).

A bulb cut while *off* and restored comes on by itself and may report that a moment after
"connected". With `lostPower` cleared, the classifier takes it as your on.

Tests: `test_dropout_of_an_off_light_never_turns_it_on` (tonight, replayed) and
`test_dropout_of_an_off_light_in_the_day_stays_off`. 76/76 native tests pass.

## Not changed (known limit)

The "lost" half can't be told apart at the moment it happens. A dropout of a light that's
**on** still reads as your off, then your on: out for today for ~10–25 s, then back in
line with a 15 s fade, same as before. The second option discussed (require a minimum
outage before "lost" counts) would fix that, at the cost of real wall-switch offs
registering late. The user didn't choose it.
