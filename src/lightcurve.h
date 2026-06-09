#pragma once
#include <math.h>
#include <Arduino.h>

// ── Curve constants ────────────────────────────────────────────────────────
// To retune: adjust constexpr values only. Do not touch the math functions.
// Brightness values are in percent (0.0–100.0) — native Hue API v2 units.

// The curve has TWO intentional discontinuities, one per conditionally-driven bulb:
//   • S3_LUX_HI (250 lux) — chest joins.    As lux drops through 250, chest cuts
//                                           and floor+dresser step UP to compensate.
//   • S2_LUX_HI (500 lux) — overheads join. As lux drops through 500, overheads cut
//                                           and the three lamps step UP to compensate.
// Active bulbs by lux band:
//   0–250   : floor + dresser
//   250–500 : floor + dresser + chest
//   500+    : floor + dresser + chest + overheads
// All bri jumps are tuning knobs — retune the *_BRI_* values in the lux-curve tuner;
// keep the JS duplicates in dashboard/templates/index.html and lux_curve.html in sync.

// Segment 4: night log rise (0 → 10 lux) — floor + dresser only
constexpr float S4_LUX_HI    = 10.0f;
constexpr float S4_LOG_BASE  = 20.0f;
constexpr float S4_FLOOR_BRI = 19.7f;   // night floor
constexpr float S4_BRI_HI    = 59.1f;   // locked to seg-3 bottom

// Segment 3: plateau crawl (10 → 250 lux) — floor + dresser only
// S3_LUX_HI is the CHEST turn-on breakpoint (also used by tickNormal's chest edge).
constexpr float S3_LUX_HI      = 250.0f;
constexpr float S3_CRAWL_POWER = 0.50f;
constexpr float S3_BRI_HI      = 78.7f;   // bri at 250⁻ — floor+dresser carrying alone

// Segment 2: parabolic rise (250 → 500 lux) — floor + dresser + chest
// Discontinuity at 250: S2_BRI_LO < S3_BRI_HI. Chest joins, so the shared bri drops
// ~8.7%; going the other way (lux < 250) chest cuts and the two lamps step up to match.
// S2_LUX_HI is the OVERHEAD turn-on breakpoint (also used by tickNormal's overhead edge).
constexpr float S2_LUX_HI     = 500.0f;
constexpr float S2_PARA_POWER = 5.0f;
constexpr float S2_BRI_LO     = 70.0f;    // 250⁺ — three lamps at this level
constexpr float S2_BRI_HI     = 85.0f;    // 500⁻ — three lamps, just before overheads

// Segment 1: daytime log rise (500 → 3000 lux) — all bulbs
// Discontinuity at 500: S1_BRI_LO < S2_BRI_HI. Overheads join, so bri drops ~16%
// (the original overhead-compensation feel, just moved from 200 lux to 500 lux).
constexpr float S1_LUX_HI   = 3000.0f;
constexpr float S1_LOG_BASE = 4.5f;
constexpr float S1_BRI_LO   = 69.0f;    // 500⁺ — all bulbs, bottom of the day rise
constexpr float S1_BRI_HI   = 100.0f;   // 100% — max brightness

// Color temp endpoints (mirek)
constexpr float CT_COOL = 280.0f;   // ~3500K — high brightness
constexpr float CT_WARM = 400.0f;   // ~2200K — night/floor

// ── Output struct ──────────────────────────────────────────────────────────
struct LightTarget {
    float    bri;   // brightness percent, 0.0–100.0 (Hue API v2 native)
    uint16_t ct;    // Hue ct mirek, 153–447
};

// ── Math helpers ───────────────────────────────────────────────────────────

// Concave-down log curve: fast rise early, flattens toward t=1.
// logMap(0) = 0, logMap(1) = 1
static inline float _logMap(float t, float base) {
    return logf(1.0f + t * (base - 1.0f)) / logf(base);
}

// Hyperbolic plateau crawl: nearly flat at low power, more noticeable rise at higher power.
// hyperCrawl(0) = 0, hyperCrawl(1) = 1
// Formula: k = 8^p,  val = 1 - 1/(1 + t*k),  norm = 1 - 1/(1 + k),  return val/norm
static inline float _hyperCrawl(float t, float p) {
    float k = powf(8.0f, p);
    float val = 1.0f - 1.0f/(1.0f + t*k);
    float norm = 1.0f - 1.0f/(1.0f + k);
    return val/norm;
}

// ── Entry point ────────────────────────────────────────────────────────────
// Maps ambient lux to Hue bri + ct. Pure math — no side effects.
// Input is clamped to [0, S1_LUX_HI] before processing.
LightTarget luxToTarget(float lux) {
    lux = constrain(lux, 0.0f, S1_LUX_HI);

    float bri;

    if (lux <= S4_LUX_HI) {
        // Segment 4: night log rise (bri ~19.7% → ~59.1%)
        float t = lux / S4_LUX_HI;
        bri = S4_FLOOR_BRI + (S4_BRI_HI - S4_FLOOR_BRI) * _logMap(t, S4_LOG_BASE);

    } else if (lux <= S3_LUX_HI) {
        // Segment 3: plateau crawl (bri ~59.1% → ~78.7%) — floor + dresser only
        float t = (lux - S4_LUX_HI) / (S3_LUX_HI - S4_LUX_HI);
        bri = S4_BRI_HI + (S3_BRI_HI - S4_BRI_HI) * _hyperCrawl(t, S3_CRAWL_POWER);

    } else if (lux <= S2_LUX_HI) {
        // Segment 2: parabolic rise (bri ~70.0% → ~85.0%) — floor + dresser + chest
        // Starts at S2_BRI_LO (~70%), not S3_BRI_HI (~79%): chest-join discontinuity at 250
        float t = (lux - S3_LUX_HI) / (S2_LUX_HI - S3_LUX_HI);
        bri = S2_BRI_LO + (S2_BRI_HI - S2_BRI_LO) * powf(t, S2_PARA_POWER);

    } else {
        // Segment 1: daytime log rise (bri ~69.0% → 100.0%) — all bulbs
        // Starts at S1_BRI_LO (~69%), not S2_BRI_HI (~85%): overhead-join discontinuity at 500
        float t = (lux - S2_LUX_HI) / (S1_LUX_HI - S2_LUX_HI);
        bri = S1_BRI_LO + (S1_BRI_HI - S1_BRI_LO) * _logMap(t, S1_LOG_BASE);
    }

    // Color temp derived from normalized bri — not from lux directly
    float briNorm = (bri - S4_FLOOR_BRI) / (S1_BRI_HI - S4_FLOOR_BRI);
    float ct = CT_WARM - (CT_WARM - CT_COOL) * briNorm;

    return { bri, (uint16_t)ct };
}