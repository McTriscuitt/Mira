#pragma once
//
// echo.h — "is this bridge report one of Mira's own echoes?" for one recorded
// command. Pure, so the native tests cover it; bridge.cpp's classifier calls it
// for every live slot in the recent-command ring.
//
// Time-aware because 3.0's fades run up to an hour. Right after a command
// (ECHO_EARLY_MS) anything between where the light was and where it's going
// counts — the bridge echoes the target at once, the bulb may report its start.
// After that only where the fade should be by now counts (a mid-fade progress
// report), or the target once the fade has landed. A blanket [prior, target]
// band would hide your change for the whole fade.
//
#include <math.h>
#include <stdint.h>
#include "config.h"

inline bool echoOnPath(float priorBri, float targetBri, uint32_t ageMs, uint32_t durMs, float evBri) {
    if (ageMs <= ECHO_EARLY_MS) {
        float lo = fminf(priorBri, targetBri) - TRAJECTORY_TOLERANCE_BRI;
        float hi = fmaxf(priorBri, targetBri) + TRAJECTORY_TOLERANCE_BRI;
        return evBri >= lo && evBri <= hi;
    }
    float t        = durMs ? fminf(1.0f, (float)ageMs / (float)durMs) : 1.0f;
    float expected = priorBri + (targetBri - priorBri) * t;
    return fabsf(evBri - expected) <= ECHO_PROGRESS_TOL_BRI ||
           (ageMs >= durMs && fabsf(evBri - targetBri) <= TRAJECTORY_TOLERANCE_BRI);
}
