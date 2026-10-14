//
// timeline.cpp — see timeline.h. Pure: builds on the board and on the host.
//
#include "timeline.h"
#include "config.h"
#include <stdio.h>

// Mira-day seconds for a local time-of-day constant (usable in static tables).
#define MDS(sec) ((int32_t)((((int32_t)(sec)) - WAKE_WINDOW_OPEN + 86400L) % 86400L))

namespace {

struct Seg {
    int32_t start;
    int32_t end;
    float   b0;
    float   b1;
};

// The driven day, in order. A segment whose levels differ is a fade period.
const Seg SEGS[] = {
    { 0,                         MDS(MIDDAY_FADE_START),    MORNING_BRI, MORNING_BRI },
    { MDS(MIDDAY_FADE_START),    MDS(MIDDAY_FADE_END),      MORNING_BRI, DAY_BRI     },
    { MDS(MIDDAY_FADE_END),      MDS(AFTERNOON_FADE_START), DAY_BRI,     DAY_BRI     },
    { MDS(AFTERNOON_FADE_START), MDS(AFTERNOON_FADE_END),   DAY_BRI,     EVENING_BRI },
    { MDS(AFTERNOON_FADE_END),   MDS(EVENING_FADE_START),   EVENING_BRI, EVENING_BRI },
    { MDS(EVENING_FADE_START),   MDS(EVENING_FADE_END),     EVENING_BRI, NIGHT_BRI   },
    { MDS(EVENING_FADE_END),     MDS(LATE_FADE_START),      NIGHT_BRI,   NIGHT_BRI   },
    { MDS(LATE_FADE_START),      MDS(LATE_FADE_END),        NIGHT_BRI,   LATE_BRI    },
    { MDS(LATE_FADE_END),        MDS(LIGHTS_OUT_AT),        LATE_BRI,    LATE_BRI    },
};
const int SEG_COUNT = (int)(sizeof(SEGS) / sizeof(SEGS[0]));

inline bool isFade(const Seg& s) { return s.b0 != s.b1; }

}  // namespace

const TimelineEvent TIMELINE_EVENTS[] = {
    { WAKE_WINDOW_OPEN, EventKind::WakeWindow,  Role::Floor   },
    { AUTO_WAKE,        EventKind::AutoWake,    Role::Floor   },
    { CEILING_ON_AT,    EventKind::LightOn,     Role::Ceiling },
    { CEILING_OFF_AT,   EventKind::LightOff,    Role::Ceiling },
    { CHEST_OFF_AT,     EventKind::LightOff,    Role::Chest   },
    { SLEEP_CUTOFF,     EventKind::SleepCutoff, Role::Floor   },
    { DRESSER_OFF_AT,   EventKind::LightOff,    Role::Dresser },
    { LIGHTS_OUT_AT,    EventKind::LightsOut,   Role::Floor   },
};
const int TIMELINE_EVENT_COUNT = (int)(sizeof(TIMELINE_EVENTS) / sizeof(TIMELINE_EVENTS[0]));

int32_t toMds(int32_t localSec) {
    return MDS(localSec);
}

int32_t mdsToLocal(int32_t mds) {
    return (int32_t)((mds + WAKE_WINDOW_OPEN) % 86400L);
}

uint16_t ctForBri(float bri) {
    float t  = (bri - CT_WARM_BRI) / (100.0f - CT_WARM_BRI);
    float ct = (float)CT_WARM - (float)(CT_WARM - CT_COOL) * t;
    if (ct < (float)CT_COOL) ct = (float)CT_COOL;
    if (ct > (float)CT_WARM) ct = (float)CT_WARM;
    return (uint16_t)(ct + 0.5f);
}

Period periodAt(int32_t mds) {
    for (int i = 0; i < SEG_COUNT; i++) {
        const Seg& s = SEGS[i];
        if (mds >= s.start && mds < s.end) {
            Period p = { true, isFade(s), s.start, s.end, s.b0, s.b1 };
            return p;
        }
    }
    Period night = { false, false, MDS(LIGHTS_OUT_AT), 86400, LATE_BRI, LATE_BRI };
    return night;
}

float profileBri(int32_t mds) {
    Period p = periodAt(mds);
    if (!p.fade) return p.b1;
    float t = (float)(mds - p.startMds) / (float)(p.endMds - p.startMds);
    return p.b0 + (p.b1 - p.b0) * t;
}

int32_t nextFadeStartAfter(int32_t mds) {
    for (int i = 0; i < SEG_COUNT; i++) {
        if (isFade(SEGS[i]) && SEGS[i].start > mds) return SEGS[i].start;
    }
    return MDS(LIGHTS_OUT_AT);  // after the last fade, a hold lasts until lights-out
}

bool roleScheduledOn(Role role, int32_t mds) {
    switch (role) {
        case Role::Floor:   return mds < MDS(LIGHTS_OUT_AT);
        case Role::Dresser: return mds < MDS(DRESSER_OFF_AT);
        case Role::Chest:   return mds < MDS(CHEST_OFF_AT);
        case Role::Ceiling: return mds >= MDS(CEILING_ON_AT) && mds < MDS(CEILING_OFF_AT);
    }
    return false;
}

bool crossedLocal(int32_t prevSec, int32_t nowSec, int32_t atSec) {
    if (nowSec == prevSec) return false;
    if (nowSec > prevSec) return atSec > prevSec && atSec <= nowSec;
    if (prevSec - nowSec > 12L * 3600L) return atSec > prevSec || atSec <= nowSec;  // midnight
    return false;  // stepped back (DST fall-back): nothing new crossed
}

void fmtClock(int32_t localSec, char* buf, int len) {
    int h = (int)(localSec / 3600) % 24;
    int m = (int)(localSec % 3600) / 60;
    int h12 = h % 12;
    if (h12 == 0) h12 = 12;
    snprintf(buf, (size_t)len, "%d:%02d%s", h12, m, h < 12 ? "AM" : "PM");
}
