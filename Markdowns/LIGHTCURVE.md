# Mira — Lighting Curve Context

This file documents the lux-to-brightness/color-temp mapping curve for the Mira system.
Read this alongside `FIRMWARE.md` before writing or modifying any curve-related code.

---

## Overview

Each lux reading from the VEML7700 maps to two output values sent to the Hue Bridge:
- `bri` — brightness as a `float` percent, 0.0–100.0 (Hue API v2 native units — `dimming.brightness`)
- `ct` — color temperature in mirek, 153–447 (153 = ~6500K cool, 447 = ~2200K warm) — `color_temperature.mirek`

The mapping uses a four-segment piecewise curve. Segments 4/3 and 2/1 are each internally
seamless, but there is a **deliberate discontinuity at 200 lux** — see below.

Color temp is derived globally from normalized brightness: high bri → cool ct, low bri → warm ct.

---

## Bulb Configuration Per Segment

The curve maps to different active bulb sets depending on which side of 200 lux the system is on:

- **Above 200 lux (seg 1 + seg 2):** all four bulbs active — bedside, desk, overhead 1, overhead 2
- **Below 200 lux (seg 3 + seg 4):** overheads off — bedside and desk only

The overhead cutoff at 200 lux (`S3_LUX_HI`) is part of the evening wind-down sequence. As lux drops through 200, the two overhead bulbs turn off and the bedside/desk lamps bump up in brightness to partially compensate. This produces the intentional upward jump in bri at the seg 2/3 boundary (~63.0 % → ~78.7 %).

The overhead on/off logic is handled by the system state machine in `tickNormal()`, not by `luxToTarget()`. `luxToTarget()` only returns the correct bri/ct for whichever bulbs are currently active.

---

## Curve Shape

```
bri (%)
100 |                                                        *
    |                                                  *
 87 |                                       *
    |                                 .  *
 79 |_ _ _ _ _ _ _ _ _ _ _ _ _ _ _ _ _*    <- overheads OFF below here (lux descending)
    |                                       <- seg3 ends at 78.7 %
 63 |                                   *   <- seg2 starts at 63.0 % (discontinuity: 79 → 63)
    |                             .   *
 59 |        * . . . . . *
    |
 20 |    *
    |*
    +--+-------------+--+-----------+-------------------> lux
    0 10            200 200         500                 3000
      [ seg4 ][ seg3 ]|[   seg2   ][     seg1        ]
                                   ^
                              discontinuity
                           overheads turn off,
                           lamps jump from ~63 % → ~79 %
```

- Segment 4 (0–10 lux): log rise from floor (~19.7 % → ~59.1 %) — deep night, bedside + desk only
- Segment 3 (10–200 lux): hyperbolic plateau crawl (~59.1 % → ~78.7 %) — evening wind-down, bedside + desk only
- Discontinuity at 200 lux: as lux drops through 200, overheads off, bri jumps ~63 % → ~79 %
- Segment 2 (200–500 lux): parabolic rise (~63.0 % → ~86.6 %) — transition zone, all four bulbs
- Segment 1 (500–3000 lux): log rise to peak (~86.6 % → 100.0 %) — full daytime, all four bulbs

---

## Curve Constants

All brightness constants are in percent (0.0–100.0) — Hue API v2 native units.

```cpp
// Segment 4: night log rise (0 → 10 lux) — bedside + desk only
constexpr float S4_LUX_HI    = 10.0f;
constexpr float S4_LOG_BASE  = 20.0f;
constexpr float S4_FLOOR_BRI = 19.7f;   // ~50/254 in v1 units — night floor
constexpr float S4_BRI_HI    = 59.1f;   // ~150/254 — locked to S3_BRI_LO

// Segment 3: plateau crawl (10 → 200 lux) — bedside + desk only
constexpr float S3_LUX_HI      = 200.0f;
constexpr float S3_CRAWL_POWER = 0.50f;
constexpr float S3_BRI_HI      = 78.7f;  // ~200/254 — bri just after overheads turn off

// Segment 2: parabolic rise (200 → 500 lux) — all four bulbs
// NOTE: S2_BRI_LO != S3_BRI_HI — intentional discontinuity at 200 lux.
// As lux drops through 200: overheads cut, lamps jump from ~63 % → ~79 % to compensate.
constexpr float S2_LUX_HI     = 500.0f;
constexpr float S2_PARA_POWER = 5.0f;
constexpr float S2_BRI_LO     = 63.0f;   // ~160/254 — seg2 bottom — all four bulbs at this level
constexpr float S2_BRI_HI     = 86.6f;   // ~220/254 — locked to S1_BRI_LO

// Segment 1: daytime log rise (500 → 3000 lux) — all four bulbs
constexpr float S1_LUX_HI   = 3000.0f;
constexpr float S1_LOG_BASE = 4.5f;
constexpr float S1_BRI_HI   = 100.0f;   // 100 % — max brightness

// Color temp endpoints (mirek)
constexpr float CT_COOL = 280.0f;   // ~3500K — high brightness
constexpr float CT_WARM = 400.0f;   // ~2200K — night/floor
```

---

## Math Per Segment

### Segment 4 — log rise (~19.7 % → ~59.1 %)
```
t   = lux / S4_LUX_HI
bri = S4_FLOOR_BRI + (S4_BRI_HI - S4_FLOOR_BRI) * logMap(t, S4_LOG_BASE)
```

### Segment 3 — hyperbolic plateau crawl (~59.1 % → ~78.7 %)
```
t   = (lux - S4_LUX_HI) / (S3_LUX_HI - S4_LUX_HI)
bri = S4_BRI_HI + (S3_BRI_HI - S4_BRI_HI) * hyperCrawl(t, S3_CRAWL_POWER)

hyperCrawl(t, p):
    k    = 8^p
    val  = 1 - 1/(1 + t*k)
    norm = 1 - 1/(1 + k)
    return val / norm
```
Low power (~0.2) = nearly flat line. Higher power (~1.5) = more noticeable rise.

### Segment 2 — parabolic rise (~63.0 % → ~86.6 %)
```
t   = (lux - S3_LUX_HI) / (S2_LUX_HI - S3_LUX_HI)
bri = S2_BRI_LO + (S2_BRI_HI - S2_BRI_LO) * t^S2_PARA_POWER
```
Starts at S2_BRI_LO (~63 %), NOT S3_BRI_HI (~79 %) — this is the intentional discontinuity.
High power = stays low longer, then rises steeply near the segment 1 boundary.

### Segment 1 — log rise (~86.6 % → 100.0 %)
```
t   = (lux - S2_LUX_HI) / (S1_LUX_HI - S2_LUX_HI)
bri = S2_BRI_HI + (S1_BRI_HI - S2_BRI_HI) * logMap(t, S1_LOG_BASE)
```

### logMap (shared helper)
```
logMap(t, base) = log(1 + t*(base-1)) / log(base)
```
Produces a concave-down curve: fast rise early, flattens toward t=1.

### Color temp (global, all segments)
```
briNorm = (bri - S4_FLOOR_BRI) / (S1_BRI_HI - S4_FLOOR_BRI)   // normalize to [0,1]
ct      = CT_WARM - (CT_WARM - CT_COOL) * briNorm               // invert: high bri = cool
```

---

## Implementation File

The curve lives in `lightcurve.h` in the project src directory.

### LightTarget struct
```cpp
struct LightTarget {
    float    bri;   // brightness percent, 0.0–100.0 (Hue API v2 native)
    uint16_t ct;    // Hue ct mirek, 153–447
};
```

### Entry point
```cpp
LightTarget luxToTarget(float lux);
```
Call once per lux poll. Returns clamped, ready-to-send bri and ct values.

### Usage in polling loop
```cpp
#include "lightcurve.h"

float lux = veml.readLux();
LightTarget target = luxToTarget(lux);
// target.bri, target.ct → send to Hue Bridge via HTTP PUT
```

---

## Implementation Notes

- All helpers (`_logMap`, `_hyperCrawl`, `_parabolaMap`) are `static inline` — no heap
  allocation, no overhead.
- Constants are `constexpr float` — live in flash, not RAM.
- Seg 4/3 boundary is locked: S3_BRI_LO is derived from S4_BRI_HI inside luxToTarget().
- Seg 2/1 boundary is locked: S1_BRI_LO is derived from S2_BRI_HI inside luxToTarget().
- Seg 2/3 boundary is intentionally NOT locked: S2_BRI_LO is an independent constant.
- Input lux is clamped to [0, S1_LUX_HI] before processing.
- To retune: adjust the constexpr constants only. Do not touch the math functions.

---

## Relationship to System State

`luxToTarget()` is purely a math function — no side effects, no state. The caller (`tickNormal`, `tickWakeRamp`, `tickWindDown`, `forceState`) is responsible for:
- Suppressing PUTs when the new target drifts by less than `STATE_TOLERANCE_BRI` (1.2 %) or `STATE_TOLERANCE_CT` (3 mirek) from `sentTarget` — the drift check in `tickNormal`
- Not calling during SOFT_PAUSE or HARD_OFF
- Passing the appropriate `dynamics.duration` (in ms) to each `setLight()` call — `30000` to match the 30 s poll interval, `1000` for state-machine snaps
- Handling the overhead on/off logic at the 200 lux threshold (`S3_LUX_HI`) separately — `luxToTarget()` does not manage bulb selection, only bri/ct values
