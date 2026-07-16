#pragma once
//
// config.h — tracked, public behavior/tuning constants. No secrets here.
// Deployment-specific values (WiFi creds, Hue/dashboard keys, bridge IP, light
// UUIDs) live in secrets.h, which is gitignored. Copy secrets.h.example →
// secrets.h and fill it in. main.cpp includes config.h, which pulls in secrets.h.
//
#include "secrets.h"

// UTC offset in seconds — e.g. UTC-4 = -14400, UTC-5 = -18000, UTC-6 = -21600, UTC-7 = -25200
#define UTC_OFFSET_SEC  (-18000)

// Minimum delta before a new PUT is sent to the bridge by tickNormal's drift check.
// bri is in percent (0.0–100.0); ct is in mirek — different units require separate tolerances.
// (Override/echo discrimination uses the wider TRAJECTORY_TOLERANCE_* in main.cpp, not these.)
#define STATE_TOLERANCE_BRI 1.2f  // ≈ 3/254 in v1 bri units
#define STATE_TOLERANCE_CT  3

// Time of day (minutes since midnight, local) after which all-lights-off
// re-arms the morning lockout. 1230 = 20:30 — aligned with the earliest
// wind-down completion (WIND_DOWN_GATE_HOUR 19 + 30 min counter + 60 min
// ramp). Minute-granular since July 15, 2026 (was hour-granular, 21).
#define LOCKOUT_RESET_MIN_OF_DAY 1230

// Hour (24h, local time) after which the stable-lux counter may accumulate
// toward WIND_DOWN. Earliest possible wind-down completion is this hour
// + 30 min (counter: 60 low-lux readings) + 60 min (ramp) — gate 19 → done by
// 20:30 when the room is already dark at 19:00; a brighter evening pushes it
// later (lux stays in charge, the hour only sets the floor). Lowered from 21
// on July 15, 2026 — schedule change, user wants lights down by ~8:30 PM.
#define WIND_DOWN_GATE_HOUR 19

// Cache indices into lightCache[] in main.cpp (Phase 3 — repurposed from old v1 integer IDs).
// These index the LIGHT_UUID_* defines in secrets.h (resolved via idxByUuid/lightUuid).
#define LIGHT_CHEST    0   // was LIGHT_BEDSIDE
#define LIGHT_DRESSER  1   // was LIGHT_DESK
#define LIGHT_CEIL_1   2
#define LIGHT_CEIL_2   3
#define LIGHT_FLOOR    4   // Signe floor lamp — wake start-point + wind-down night-light anchor
#define LIGHT_COUNT    5   // sizes lightCache[] / recentPuts[] / excludedLight[] and bounds-checks idx

// Wake sequence — total poll ticks for the lux ramp (30 s per tick)
#define WAKE_RAMP_TICKS 40   // 20 min

// Wake ramp turn-on gates (a brightness staircase that doubles as a sunrise sequence):
// the floor lamp leads from tick 0; chest+dresser join once the ramp is meaningfully
// lit AND the morning is bright enough; the overheads come last and only on bright
// mornings. The two stages use *different* lux gates so a moderately bright morning
// (250–600 lux) lights floor+chest+dresser but no overheads — the adaptive, seasonal
// behavior the system is built around. Brightness gates are in percent (v2 units).
// WAKE_SECONDARY_LUX (250) is aligned with the lux-curve's chest breakpoint S3_LUX_HI
// (250), so the chest has no WAKE→NORMAL handoff seam. WAKE_OVERHEAD_LUX (500) now
// matches NORMAL's overhead breakpoint S2_LUX_HI (500), closing the 500–600 lux
// morning seam where overheads would snap on at the WAKE→NORMAL handoff.
#define WAKE_SECONDARY_BRI  40.0f   // chest+dresser join once rampBri ≥ this
#define WAKE_SECONDARY_LUX 250.0f   // ...and ambient lux ≥ this (matches S3_LUX_HI)
#define WAKE_OVERHEAD_LUX  500.0f   // overheads join once rampBri ≥ S2_BRI_LO (70%) and lux ≥ this (matches S2_LUX_HI — no WAKE→NORMAL seam)

// Soft pause — auto-resume after this many milliseconds (60 min). Seeds the runtime
// softPauseDurationMs, which the dashboard slider / +10 extend button can rewrite.
#define SOFT_PAUSE_MS 3600000UL

// N5 — align lux ticks to wall-clock :00/:30 instead of flash-relative 30 s.
// Comment out for testing (reverts to "30 s from whenever the tick fired").
// NTPClient is second-granular, so alignment is ±1 s. See N1_MIGRATION.md Stage 2.
#define ALIGNED_TICKS 1

// Button pins — active LOW, internal pull-up
#define BTN_MODE  9   // D9  — short: NORMAL/pause toggle, long: hard off
#define BTN_CYCLE 10  // D10 — short: cycle through all states

// Button timing
#define DEBOUNCE_MS    50UL
#define LONG_PRESS_MS 700UL

// Soft pause resume ramp — ticks to drift from pre-pause bri/ct to current ambient (30s/tick)
#define PAUSE_RESUME_TICKS 20  // 10 min
