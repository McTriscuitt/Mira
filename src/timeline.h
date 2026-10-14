#pragma once
//
// timeline.h — Mira 3.0's day: the brightness profile, the CT map, and the fixed
// timed events. Pure functions of time, no Arduino dependencies (the native
// tests build this file on the host). Times come from config.h.
//
// Two time coordinates:
//   localSec — local seconds since midnight, 0..86399 (supplied by the shell).
//   mds      — "Mira-day seconds": seconds since the wake window opens (06:00).
//              The evening then runs continuously past midnight (23:30 → 01:00 is
//              mds 63000 → 68400) and the undriven night (01:00 → 06:00) sits at
//              the end, so every period is a plain [start, end) interval.
//
#include <stdint.h>
#include "lights.h"

int32_t  toMds(int32_t localSec);
uint16_t ctForBri(float bri);   // the 2.x brightness→CT map, rounded and clamped

// One stretch of the day. A fade moves b0 → b1 across [startMds, endMds);
// a steady period holds b1. The night (lights-out → 06:00) is not driven.
struct Period {
    bool    driven;
    bool    fade;
    int32_t startMds;
    int32_t endMds;
    float   b0;
    float   b1;
};

Period  periodAt(int32_t mds);              // the fixed profile (the wake overlay lives in the director)
float   profileBri(int32_t mds);            // profile level at mds, interpolated inside fades
int32_t nextFadeStartAfter(int32_t mds);    // when a hold that starts at mds ends (lights-out if none left)
bool    roleScheduledOn(Role role, int32_t mds);  // does the schedule have this role on (after the wake)?

// Fixed timed events, in chronological Mira-day order. Same-time events fire in
// table order (21:00: the chest goes off before the sleep cutoff is checked).
enum class EventKind : uint8_t { WakeWindow, AutoWake, LightOn, LightOff, SleepCutoff, LightsOut };
struct TimelineEvent {
    int32_t   localSec;
    EventKind kind;
    Role      role;   // LightOn / LightOff only
};
extern const TimelineEvent TIMELINE_EVENTS[];
extern const int           TIMELINE_EVENT_COUNT;

// Did the local clock pass atSec going from prevSec to nowSec? Handles the
// midnight wrap; a backward step under 12 h (the DST fall-back hour) crosses
// nothing, so 01:00 lights-out can't fire twice on that night.
bool crossedLocal(int32_t prevSec, int32_t nowSec, int32_t atSec);

void fmtClock(int32_t localSec, char* buf, int len);  // "5:00PM"
int32_t mdsToLocal(int32_t mds);
