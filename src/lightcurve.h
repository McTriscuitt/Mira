#pragma once
#include <math.h>
#include <Arduino.h>

// ── Curve constants ────────────────────────────────────────────────────────
// To retune: adjust constexpr values only. Do not touch the math functions.
// Brightness values are in percent (0.0–100.0) — native Hue API v2 units.

// Segment 4: night log rise (0 → 10 lux) — bedside + desk only
constexpr float S4_LUX_HI    = 10.0f;
constexpr float S4_LOG_BASE  = 20.0f;
constexpr float S4_FLOOR_BRI = 19.7f;   // ~50/254 — night floor
constexpr float S4_BRI_HI    = 59.1f;   // ~150/254 — locked to S3_BRI_LO

// Segment 3: plateau crawl (10 → 200 lux) — bedside + desk only
constexpr float S3_LUX_HI      = 200.0f;
constexpr float S3_CRAWL_POWER = 0.50f;
constexpr float S3_BRI_HI      = 78.7f;   // ~200/254 — bri just after overheads turn off

// Segment 2: parabolic rise (200 → 500 lux) — all four bulbs
// NOTE: S2_BRI_LO != S3_BRI_HI — intentional discontinuity at 200 lux.
// As lux drops through 200: overheads cut, lamps jump from ~63% → ~79% to compensate.
constexpr float S2_LUX_HI     = 500.0f;
constexpr float S2_PARA_POWER = 5.0f;
constexpr float S2_BRI_LO     = 63.0f;    // ~160/254 — seg2 bottom — all four bulbs at this level
constexpr float S2_BRI_HI     = 86.6f;    // ~220/254 — locked to S1_BRI_LO

// Segment 1: daytime log rise (500 → 3000 lux) — all four bulbs
constexpr float S1_LUX_HI   = 3000.0f;
constexpr float S1_LOG_BASE = 4.5f;
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
        // Segment 3: plateau crawl (bri ~59.1% → ~78.7%) — bedside + desk only
        float t = (lux - S4_LUX_HI) / (S3_LUX_HI - S4_LUX_HI);
        bri = S4_BRI_HI + (S3_BRI_HI - S4_BRI_HI) * _hyperCrawl(t, S3_CRAWL_POWER);

    } else if (lux <= S2_LUX_HI) {
        // Segment 2: parabolic rise (bri ~63.0% → ~86.6%) — all four bulbs
        // Starts at S2_BRI_LO (~63%), not S3_BRI_HI (~79%): intentional discontinuity
        float t = (lux - S3_LUX_HI) / (S2_LUX_HI - S3_LUX_HI);
        bri = S2_BRI_LO + (S2_BRI_HI - S2_BRI_LO) * powf(t, S2_PARA_POWER);

    } else {
        // Segment 1: daytime log rise (bri ~86.6% → 100.0%) — all four bulbs
        float t = (lux - S2_LUX_HI) / (S1_LUX_HI - S2_LUX_HI);
        bri = S2_BRI_HI + (S1_BRI_HI - S2_BRI_HI) * _logMap(t, S1_LOG_BASE);
    }

    // Color temp derived from normalized bri — not from lux directly
    float briNorm = (bri - S4_FLOOR_BRI) / (S1_BRI_HI - S4_FLOOR_BRI);
    float ct = CT_WARM - (CT_WARM - CT_COOL) * briNorm;

    return { bri, (uint16_t)ct };
}