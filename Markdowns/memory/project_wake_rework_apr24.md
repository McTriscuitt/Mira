---
name: Wake Sequence Rework (April 24)
description: Wake rework from lux-space to bri-to-bri interpolation, and soft pause override fix
type: project
originSessionId: dba0bd9d-1168-49c3-9458-bf0d76e82d7c
---
Wake sequence reworked on 2026-04-24:
- `triggerWake()` now reads actual bedside state via `getLightState()` instead of loading NVS savedBri and calling `invertLux()`
- `tickWakeRamp()` linearly interpolates bri and ct from `wakeStartTarget` → `wakeEndTarget` over fixed tick count (40 WORK / 120 RELAXED)
- Desk turns on every tick (previously only when wakeLux >= S4_LUX_HI, which caused false override detection)
- `invertLux()` and `wakeStepSize()` removed from codebase; lux-space wake variables (`wakeLux`, `wakeStartLux`, `wakeEndLux`) replaced with `wakeStep`, `wakeStartTarget`, `wakeEndTarget`
- Discord logs on both wake start and wake completion

**Why:** April 23 wake failures — lux-space ramp completed in one tick when ambient lux (0.2) was below wakeStartLux (0.9 from stale NVS); desk never turned on; checkOverride falsely triggered soft pause.

Soft pause fix same session:
- `checkOverride()` now skipped while `pauseResumeActive` is true (was only skipped for first tick via `skipOverrideCheck`)
- Prevented false override during 10-min resume ramp when bulbs hadn't finished transitioning

**How to apply:** If wake or soft-pause resume issues arise, check wakeStartTarget seeding and pauseResumeActive guard first.
