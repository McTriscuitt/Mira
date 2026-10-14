//
// director.cpp — see director.h and Markdowns/MIRA_3.md. Pure: no Arduino, so the
// same file builds for the board and for the host-side tests in test/test_core.
//
#include "director.h"
#include "config.h"
#include "timeline.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const uint32_t Director::STATE_MAGIC;

namespace {

const int64_t DAY_MS       = 86400000LL;
const int64_t WAKE_FADE_MS = (int64_t)WAKE_FADE_S * 1000LL;

inline float   clamp01(float t) { return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); }
inline int64_t abs64(int64_t v) { return v < 0 ? -v : v; }

const char* const MODE_NAMES[] = { "ASLEEP", "WAKE", "ACTIVE", "HOLD", "NAP" };

}  // namespace

Director::Director()
    : env_(0), physOn_(0), pendingOff_(0), pendingOffAtMs_(0),
      lastFadeLoggedMs_(0), dirty_(false) {
    memset(&s_, 0, sizeof(s_));
    s_.magic  = STATE_MAGIC;
    s_.asleep = 1;
    for (int i = 0; i < LIGHT_COUNT; i++) {
        joinDurMs_[i]   = 0;
        knownBri_[i]    = 0.0f;
        retryAtMs_[i]   = 0;
        fails_[i]       = 0;
        presetB1_[i]    = -1.0f;
        presetEndMs_[i] = 0;
    }
}

// ── Time ────────────────────────────────────────────────────────────────────

// Mira-day milliseconds (since 06:00 local) at an absolute instant. Uses now's
// local offset for every instant asked about — valid because nothing driven ever
// spans a DST change (those happen at 02:00, while asleep).
int64_t Director::mdsMsAt(const Now& now, int64_t atMs) const {
    int64_t localMs = (int64_t)now.localSec * 1000LL + (now.ms % 1000LL) + (atMs - now.ms);
    int64_t m = (localMs - (int64_t)WAKE_WINDOW_OPEN * 1000LL) % DAY_MS;
    return m < 0 ? m + DAY_MS : m;
}

int32_t Director::mdsNow(const Now& now) const {
    return (int32_t)(mdsMsAt(now, now.ms) / 1000LL);
}

Director::Active Director::profileAt(const Now& now, int64_t atMs) const {
    int64_t m = mdsMsAt(now, atMs);
    Period  p = periodAt((int32_t)(m / 1000LL));
    Active  a;
    a.driven  = p.driven;
    a.fade    = p.fade;
    a.wake    = false;
    a.startMs = atMs - (m - (int64_t)p.startMds * 1000LL);
    a.endMs   = atMs + ((int64_t)p.endMds * 1000LL - m);
    a.b0      = p.b0;
    a.b1      = p.b1;
    return a;
}

// The period in force at atMs: the wake fade while it runs, else the profile.
Director::Active Director::activeAt(const Now& now, int64_t atMs) const {
    if (s_.wakeStartMs && atMs >= s_.wakeStartMs && atMs < s_.wakeStartMs + WAKE_FADE_MS) {
        Active a;
        a.driven  = true;
        a.fade    = true;
        a.wake    = true;
        a.startMs = s_.wakeStartMs;
        a.endMs   = s_.wakeStartMs + WAKE_FADE_MS;
        a.b0      = WAKE_START_BRI;
        a.b1      = s_.wakeEndBri;
        return a;
    }
    return profileAt(now, atMs);
}

float Director::levelOf(const Active& a, int64_t atMs) {
    if (!a.fade) return a.b1;
    float t = (float)(atMs - a.startMs) / (float)(a.endMs - a.startMs);
    return a.b0 + (a.b1 - a.b0) * clamp01(t);
}

// Where light i is right now, as best we know: along its current command, else
// whatever the bridge last reported.
float Director::currentBri(const Now& now, int i) const {
    const Command& c = s_.cmd[i];
    if (!c.valid) return knownBri_[i];
    if (!c.on) return 0.0f;
    if (c.durMs == 0 || now.ms >= c.t0Ms + (int64_t)c.durMs) return c.b1;
    float t = (float)(now.ms - c.t0Ms) / (float)c.durMs;
    return c.b0 + (c.b1 - c.b0) * clamp01(t);
}

// ── Boot ────────────────────────────────────────────────────────────────────

void Director::boot(const Now& now, LightMask physOn, const float* bri, const DirectorState* r) {
    physOn_     = physOn;
    pendingOff_ = 0;
    for (int i = 0; i < LIGHT_COUNT; i++) {
        joinDurMs_[i] = 0;
        retryAtMs_[i] = 0;
        fails_[i]     = 0;
        // Seeds the start point of the first command to each light — a hold's
        // freeze computes "where the fade is now" from it.
        knownBri_[i]  = bri ? bri[i] : 0.0f;
    }

    int64_t age = r ? now.ms - r->lastTickMs : -1;
    if (r && r->magic == STATE_MAGIC && age >= 0 && age < RESTORE_MAX_AGE_MS) {
        // Soft reset: same facts as before. The restored lastLocalSec makes the
        // first tick fire any event that came due during the reset.
        s_ = *r;
        if (!s_.asleep) {
            // The bulbs are the truth after a reboot — a reset never switches a light.
            s_.want = physOn_;
            s_.out &= (LightMask)~physOn_;
        }
        note("Boot — picked up where it left off (%s)", modeName(mode(now)));
    } else {
        // Power-on (or stale RTC): infer from the clock and the bulbs (MIRA_3.md §7).
        memset(&s_, 0, sizeof(s_));
        s_.magic        = STATE_MAGIC;
        s_.lastTickMs   = now.ms;
        s_.lastLocalSec = now.localSec;
        if (physOn_ == 0 || mdsNow(now) >= toMds(LIGHTS_OUT_AT)) {
            s_.asleep = 1;
            note("Boot — %s; asleep until the wake", physOn_ ? "night" : "everything off");
        } else {
            s_.want = physOn_;
            note("Boot — lights on; taking over");
        }
    }
    dirty_ = true;
}

// ── The tick ────────────────────────────────────────────────────────────────

void Director::tick(const Now& now) {
    if (s_.lastTickMs == 0) return;  // boot() hasn't run — no reference point for crossings yet
    processEvents(now);
    processTimers(now);
    noteFadeStart(now);
    reconcile(now);
    s_.lastTickMs   = now.ms;
    s_.lastLocalSec = now.localSec;
}

void Director::processEvents(const Now& now) {
    for (int e = 0; e < TIMELINE_EVENT_COUNT; e++) {
        const TimelineEvent& ev = TIMELINE_EVENTS[e];
        if (!crossedLocal(s_.lastLocalSec, now.localSec, ev.localSec)) continue;
        char at[12];
        fmtClock(ev.localSec, at, sizeof(at));
        switch (ev.kind) {
            case EventKind::WakeWindow:
                if (s_.asleep && physOn_) startWake(now, -1, "a light was already on");
                break;
            case EventKind::AutoWake:
                if (s_.asleep) startWake(now, -1, at);
                break;
            case EventKind::LightOn:
                if (!s_.asleep) scheduleRole(ev.role, true);
                break;
            case EventKind::LightOff:
                if (!s_.asleep) scheduleRole(ev.role, false);
                break;
            case EventKind::SleepCutoff:
                if (s_.asleep) break;
                if (napActive()) {
                    char why[40];
                    snprintf(why, sizeof(why), "the nap ran into %s", at);
                    goToSleep(why);
                } else if (physOn_ == 0 && pendingOff_ == 0) {
                    goToSleep("everything off");
                }
                break;
            case EventKind::LightsOut:
                if (!s_.asleep) lightsOut(now, at);
                break;
        }
    }
}

void Director::processTimers(const Now& now) {
    if (pendingOff_ && now.ms - pendingOffAtMs_ >= (int64_t)OFF_GROUP_MS) resolveOffGroup(now);
    if (napActive() && now.ms >= s_.napUntilMs) endNap("time's up — lights coming back");
    if (holdActive() && now.ms >= s_.holdUntilMs) endHold("the next fade period");

    if (!s_.asleep && s_.wakeStartMs) {
        if (!(s_.wakeFlags & 1) && now.ms >= s_.wakeStartMs + (int64_t)WAKE_JOIN_DRESSER_S * 1000LL) {
            s_.wakeFlags |= 1;
            wakeJoin(now, Role::Dresser);
        }
        if (!(s_.wakeFlags & 2) && now.ms >= s_.wakeStartMs + (int64_t)WAKE_JOIN_CHEST_S * 1000LL) {
            s_.wakeFlags |= 2;
            wakeJoin(now, Role::Chest);
            wakeJoin(now, Role::Ceiling);  // only on a late wake that runs past CEILING_ON_AT
        }
        if (!(s_.wakeFlags & 4) && now.ms >= s_.wakeStartMs + WAKE_FADE_MS) {
            s_.wakeFlags |= 4;
            note("Wake complete — %.0f%%", s_.wakeEndBri);
        }
    }
}

void Director::noteFadeStart(const Now& now) {
    if (paused()) return;
    Active a = activeAt(now, now.ms);
    if (!a.driven || !a.fade || a.wake) return;
    if (abs64(a.startMs - lastFadeLoggedMs_) < 60000LL) return;
    lastFadeLoggedMs_ = a.startMs;
    int32_t endLocal = (int32_t)((((int64_t)now.localSec + (a.endMs - now.ms) / 1000LL) % 86400 + 86400) % 86400);
    char by[12];
    fmtClock(endLocal, by, sizeof(by));
    note("Fading to %.0f%% by %s", a.b1, by);
}

// ── Reconcile: make the lights match the plan ───────────────────────────────

void Director::reconcile(const Now& now) {
    if (paused()) return;
    Active p = activeAt(now, now.ms);
    if (!p.driven) return;
    for (int i = 0; i < LIGHT_COUNT; i++) reconcileLight(now, i, p);
}

void Director::reconcileLight(const Now& now, int i, const Active& p) {
    const LightMask b = lightBit(i);
    if (pendingOff_ & b) return;           // your off is still being grouped
    if (now.ms < retryAtMs_[i]) return;    // backing off after a failure
    Command&   c    = s_.cmd[i];
    const bool want = (s_.want & b) != 0;
    const bool phys = (physOn_ & b) != 0;

    if (!want) {
        if (!phys) {
#ifdef PRESET_OFF_LIGHTS
            presetLight(now, i, p);
#endif
            return;
        }
        if (c.valid && !c.on) {
            if (now.ms < c.t0Ms + (int64_t)c.durMs + (int64_t)OFF_CONFIRM_MS) return;  // echo pending
            backoff(now, i);  // our off never took
            return;
        }
        send(now, i, false, 0.0f, FADE_MS, false);
        return;
    }

    if (c.valid && c.join && now.ms < c.t0Ms + (int64_t)c.durMs) return;  // join still landing

    if (!phys) {
        if (c.valid && c.on && c.join) {
            if (now.ms < c.t0Ms + (int64_t)c.durMs + (int64_t)OFF_CONFIRM_MS) return;  // echo pending
            backoff(now, i);  // it never came on
            return;
        }
        joinLight(now, i, FADE_MS);  // Mira turns it on: 15 s fade-in to the in-line level
        return;
    }

    if (joinDurMs_[i]) {  // on, but not in line yet (you just switched it on, or the wake began)
        uint32_t d = joinDurMs_[i];
        joinDurMs_[i] = 0;
        joinLight(now, i, d);
        return;
    }

    if (p.fade) {
        // One command carries the light to the fade's end, arriving on schedule.
        if (c.valid && c.on && fabsf(c.b1 - p.b1) < ALIGN_BRI_TOL &&
            abs64(c.t0Ms + (int64_t)c.durMs - p.endMs) <= ALIGN_END_TOL_MS) return;
        int64_t left = p.endMs - now.ms;
        if (left < (int64_t)MIN_FADE_MS) left = MIN_FADE_MS;
        if (left > (int64_t)MAX_FADE_MS) {
            // Longer than one bulb transition may run: go in legs. A leg waits to
            // land like a join step, then this re-aligns with what's left.
            send(now, i, true, levelOf(p, now.ms + (int64_t)MAX_FADE_MS), MAX_FADE_MS, true);
            return;
        }
        send(now, i, true, p.b1, (uint32_t)left, false);
    } else {
        // Steady: be at the level; a light that isn't gets there in 15 s.
        if (c.valid && c.on && fabsf(c.b1 - p.b1) < ALIGN_BRI_TOL) return;
        send(now, i, true, p.b1, FADE_MS, false);
    }
}

void Director::presetLight(const Now& now, int i, const Active& p) {
    int64_t endMs = p.fade ? p.endMs : 0;
    if (presetB1_[i] == p.b1 && presetEndMs_[i] == endMs) return;
    presetB1_[i]    = p.b1;
    presetEndMs_[i] = endMs;
    int64_t dur = p.fade ? p.endMs - now.ms : (int64_t)FADE_MS;
    if (dur < (int64_t)MIN_FADE_MS) dur = MIN_FADE_MS;
    env_->preset((uint8_t)i, p.b1, ctForBri(p.b1), (uint32_t)dur);
}

void Director::send(const Now& now, int i, bool on, float bri, uint32_t durMs, bool join) {
    const bool phys = (physOn_ & lightBit(i)) != 0;
    const float b0  = phys ? currentBri(now, i) : 0.0f;
    const uint16_t ct = ctForBri(on ? bri : b0);
    Env::PutResult r = env_->put((uint8_t)i, on, bri, ct, durMs);
    Command& c = s_.cmd[i];
    if (r == Env::Sent) {
        c.valid = 1;
        c.on    = on ? 1 : 0;
        c.join  = join ? 1 : 0;
        c.b0    = b0;
        c.b1    = on ? bri : 0.0f;
        c.t0Ms  = now.ms;
        c.durMs = durMs;
        // A switch on/off only counts as done when the light's echo confirms it
        // (onEdge) — a bulb powered off at the wall takes every command and never
        // comes on, and the backoff has to keep growing for it.
        if (phys == on) fails_[i] = 0;
    } else if (r == Env::Failed) {
        backoff(now, i);
    }
    // Aborted: nothing went out, so the previous command is still what the bulb is
    // doing — leave it, the hold that caused the abort may need to freeze it.
    dirty_ = true;
}

// Bring light i in line: land on the level the period will have when the join
// ends; reconcile then hands it the rest of the period.
void Director::joinLight(const Now& now, int i, uint32_t durMs) {
    float target = levelOf(activeAt(now, now.ms + durMs), now.ms + durMs);
    send(now, i, true, target, durMs, true);
}

void Director::backoff(const Now& now, int i) {
    s_.cmd[i].valid = 0;
    if (fails_[i] < 8) fails_[i]++;
    uint32_t d = RETRY_BASE_MS << (fails_[i] - 1);
    if (d > RETRY_MAX_MS) d = RETRY_MAX_MS;
    retryAtMs_[i] = now.ms + d;
    if (fails_[i] == 1) note("%s didn't take a command — retrying", lightName(i));
}

// ── The day's transitions ───────────────────────────────────────────────────

void Director::startWake(const Now& now, int triggerIdx, const char* why) {
    s_.asleep      = 0;
    s_.napUntilMs  = 0;
    s_.holdUntilMs = 0;
    s_.out         = 0;  // yesterday's "out for today" lights rejoin
    pendingOff_    = 0;
    s_.wakeStartMs = now.ms;
    s_.wakeFlags   = 0;
    s_.wakeEndBri  = levelOf(profileAt(now, now.ms + WAKE_FADE_MS), now.ms + WAKE_FADE_MS);
    s_.want        = (LightMask)(physOn_ | lightBit(LIGHT_FLOOR));
    for (int i = 0; i < LIGHT_COUNT; i++) {
        retryAtMs_[i] = 0;
        fails_[i]     = 0;
        // Lights already on ride the wake from the start: the one you just switched
        // on snaps in line, any that were on before fade in line over 15 s.
        joinDurMs_[i] = (physOn_ & lightBit(i)) ? (i == triggerIdx ? JOIN_SNAP_MS : FADE_MS) : 0;
    }
    note("Wake — %s (%.0f%% → %.0f%% over %d min)", why, WAKE_START_BRI, s_.wakeEndBri, WAKE_FADE_S / 60);
    dirty_ = true;
}

void Director::wakeJoin(const Now& now, Role role) {
    if (roleScheduledOn(role, mdsNow(now))) scheduleRole(role, true);
}

void Director::scheduleRole(Role role, bool on) {
    for (int i = 0; i < LIGHT_COUNT; i++) {
        if (roleOf(i) != role) continue;
        const LightMask b = lightBit(i);
        if (on) {
            if (s_.out & b) {
                note("%s stays off — out for today", lightName(i));
            } else if (!(s_.want & b)) {
                s_.want |= b;
                note("%s on", lightName(i));
            }
        } else if (s_.want & b) {
            s_.want &= (LightMask)~b;
            note("%s off", lightName(i));
        }
        dirty_ = true;
    }
}

void Director::startHold(const Now& now, int changedIdx, float userBri, uint16_t userCt, const char* why) {
    if (s_.asleep || napActive()) return;
    const bool fresh = !holdActive();
    int32_t endMds = 0;
    if (fresh) {
        int64_t m = mdsMsAt(now, now.ms);
        endMds = nextFadeStartAfter((int32_t)(m / 1000LL));
        s_.holdUntilMs = now.ms + ((int64_t)endMds * 1000LL - m);
        if (s_.holdUntilMs <= now.ms) s_.holdUntilMs = now.ms + 1;
    }

    // The bulbs run their fades themselves — stop every other light where it is,
    // or they'd keep dimming. (Repeated on later changes in the same hold: a
    // freeze aborted by your next change gets another chance.)
    for (int i = 0; i < LIGHT_COUNT; i++) {
        if (i == changedIdx) continue;
        const Command& c = s_.cmd[i];
        if (!(physOn_ & lightBit(i)) || !c.valid || !c.on) continue;
        if (now.ms >= c.t0Ms + (int64_t)c.durMs) continue;  // not mid-fade
        send(now, i, true, currentBri(now, i), FREEZE_MS, false);
    }

    if (changedIdx >= 0 && changedIdx < LIGHT_COUNT) {
        Command& c = s_.cmd[changedIdx];
        knownBri_[changedIdx] = userBri;
        // Clobber repair: a command we sent moments ago may land after your change
        // and fade away from it — put your level back.
        bool clobber = c.valid && c.on && now.ms - c.t0Ms < (int64_t)CLOBBER_WINDOW_MS &&
                       fabsf(c.b1 - userBri) > TRAJECTORY_TOLERANCE_BRI;
        c.valid = 0;
        if (clobber) {
            Env::PutResult r = env_->put((uint8_t)changedIdx, true, userBri,
                                         userCt ? userCt : ctForBri(userBri), 500);
            if (r == Env::Sent) {
                c.valid = 1; c.on = 1; c.join = 0;
                c.b0 = userBri; c.b1 = userBri; c.t0Ms = now.ms; c.durMs = 500;
            }
        }
    }

    if (fresh) {
        char until[12];
        fmtClock(mdsToLocal(endMds), until, sizeof(until));
        note("Hold — %s — until %s", why, until);
    }
    dirty_ = true;
}

void Director::endHold(const char* why) {
    if (!holdActive()) return;
    s_.holdUntilMs = 0;
    note("Hold over — %s", why);
    dirty_ = true;
}

void Director::startNap(const Now& now, LightMask group) {
    s_.holdUntilMs = 0;
    s_.napUntilMs  = now.ms + (int64_t)NAP_MS;
    s_.want       |= group;  // the lights you switched off come back when it ends
    char back[12];
    fmtClock((int32_t)((now.localSec + (int32_t)(NAP_MS / 1000UL)) % 86400L), back, sizeof(back));
    note("Nap — everything off; back at %s", back);
    dirty_ = true;
}

void Director::endNap(const char* why) {
    if (!napActive()) return;
    s_.napUntilMs = 0;
    note("Nap over — %s", why);
    dirty_ = true;
}

void Director::goToSleep(const char* why) {
    s_.asleep      = 1;
    s_.napUntilMs  = 0;
    s_.holdUntilMs = 0;
    s_.want        = 0;
    s_.wakeStartMs = 0;
    s_.wakeFlags   = 0;
    pendingOff_    = 0;
    for (int i = 0; i < LIGHT_COUNT; i++) joinDurMs_[i] = 0;
    note("Asleep — %s", why);
    dirty_ = true;
}

void Director::lightsOut(const Now& now, const char* at) {
    for (int i = 0; i < LIGHT_COUNT; i++) {
        if (physOn_ & lightBit(i)) send(now, i, false, 0.0f, FADE_MS, false);
    }
    char why[32];
    snprintf(why, sizeof(why), "lights out at %s", at);
    goToSleep(why);
}

void Director::resolveOffGroup(const Now& now) {
    const LightMask group = pendingOff_;
    pendingOff_ = 0;
    if (s_.asleep || !group) return;
    if (physOn_ == 0) {
        if (mdsNow(now) >= toMds(SLEEP_CUTOFF)) goToSleep("everything off");
        else startNap(now, group);
        return;
    }
    s_.out |= group;  // want was already cleared at the edge
    for (int i = 0; i < LIGHT_COUNT; i++) {
        if (group & lightBit(i)) note("%s off — out for today", lightName(i));
    }
    dirty_ = true;
}

// ── Inputs ──────────────────────────────────────────────────────────────────

void Director::onEdge(const Now& now, uint8_t idx, bool on, bool byUser, float bri) {
    if (idx >= LIGHT_COUNT) return;
    const LightMask b = lightBit(idx);
    if (on) physOn_ |= b; else physOn_ &= (LightMask)~b;
    if (on && bri > 0.0f) knownBri_[idx] = bri;
    fails_[idx]    = 0;  // the light is reachable and reporting
    retryAtMs_[idx] = 0;
    dirty_ = true;

    if (!byUser) {
        // One of Mira's own (or a stream resync of one). From the sleep cutoff on,
        // the last light going out — however it happened — means asleep.
        if (!on && !s_.asleep && physOn_ == 0 && pendingOff_ == 0 &&
            mdsNow(now) >= toMds(SLEEP_CUTOFF)) {
            goToSleep("everything off");
        }
        reconcile(now);
        return;
    }

    s_.cmd[idx].valid = 0;  // the light is yours now
    char why[40];
    if (on) {
        pendingOff_ &= (LightMask)~b;
        if (s_.asleep) {
            // Inside the wake window your light starts the day; between lights-out
            // and 06:00 it's left alone.
            if (mdsNow(now) < toMds(SLEEP_CUTOFF)) {
                snprintf(why, sizeof(why), "%s turned on", lightName(idx));
                startWake(now, idx, why);
                reconcile(now);
            }
            return;
        }
        s_.want |= b;
        s_.out  &= (LightMask)~b;
        if (napActive()) {
            snprintf(why, sizeof(why), "%s turned on", lightName(idx));
            endNap(why);
        }
        if (!holdActive()) joinDurMs_[idx] = JOIN_SNAP_MS;
    } else {
        if (s_.asleep) return;
        joinDurMs_[idx] = 0;
        s_.want &= (LightMask)~b;  // provisional — the grouping decides out / nap / asleep
        pendingOff_    |= b;
        pendingOffAtMs_ = now.ms;
    }
    reconcile(now);
}

void Director::onUserBrightness(const Now& now, int idx, float bri, uint16_t ct) {
    const bool known = idx >= 0 && idx < LIGHT_COUNT;
    if (known) knownBri_[idx] = bri;
    if (s_.asleep || napActive()) return;
    char why[40];
    if (known) snprintf(why, sizeof(why), "%s set to %.0f%%", lightName(idx), bri);
    else       snprintf(why, sizeof(why), "manual change");
    startHold(now, known ? idx : -1, bri, ct, why);
}

void Director::onStaleRevert(const Now& now, uint8_t idx, bool nowOn) {
    if (idx >= LIGHT_COUNT) return;
    if (nowOn) physOn_ |= lightBit(idx); else physOn_ &= (LightMask)~lightBit(idx);
    s_.cmd[idx].valid = 0;
    retryAtMs_[idx]   = 0;
    note("%s didn't take the last command — re-sending", lightName(idx));
    dirty_ = true;
    reconcile(now);
}

void Director::onCommand(const Now& now, Cmd cmd, int32_t value) {
    char at[12];
    switch (cmd) {
        case Cmd::Resume:
            if (holdActive())      endHold("dashboard");
            else if (napActive())  endNap("dashboard");
            else if (s_.asleep)    note("Resume ignored — asleep (Wake starts the day)");
            break;
        case Cmd::Hold:
            if (s_.asleep || napActive()) note("Hold ignored — %s", s_.asleep ? "asleep" : "napping");
            else startHold(now, -1, 0.0f, 0, "dashboard");
            break;
        case Cmd::Wake:
            if (mdsNow(now) < toMds(LIGHTS_OUT_AT)) {
                startWake(now, -1, "dashboard");
            } else {
                fmtClock(WAKE_WINDOW_OPEN, at, sizeof(at));
                note("Wake ignored — the wake window opens at %s", at);
            }
            break;
        case Cmd::Sleep:
            if (!s_.asleep) goToSleep("dashboard");
            break;
        case Cmd::NapRemaining:
            if (napActive() && value >= 0) {
                s_.napUntilMs = now.ms + (int64_t)value * 60000LL;
                if (s_.napUntilMs <= now.ms) s_.napUntilMs = now.ms + 1;
                note("Nap — %d min left", (int)value);
                dirty_ = true;
            }
            break;
    }
    processTimers(now);  // a nap set to 0 min ends right away
    reconcile(now);
}

// ── Queries ─────────────────────────────────────────────────────────────────

Director::Mode Director::mode(const Now& now) const {
    if (s_.asleep)    return Mode::Asleep;
    if (napActive())  return Mode::Nap;
    if (holdActive()) return Mode::Hold;
    if (s_.wakeStartMs && now.ms < s_.wakeStartMs + WAKE_FADE_MS) return Mode::Wake;
    return Mode::Active;
}

float Director::inLineBri(const Now& now) const {
    if (s_.asleep) return 0.0f;
    Active a = activeAt(now, now.ms);
    return a.driven ? levelOf(a, now.ms) : 0.0f;
}

int32_t Director::pauseRemainingS(const Now& now) const {
    int64_t until = napActive() ? s_.napUntilMs : (holdActive() ? s_.holdUntilMs : 0);
    if (!until || until <= now.ms) return 0;
    return (int32_t)((until - now.ms) / 1000LL);
}

int Director::wakeStep(const Now& now) const {
    if (!s_.wakeStartMs || s_.asleep || now.ms >= s_.wakeStartMs + WAKE_FADE_MS) return 0;
    return (int)((now.ms - s_.wakeStartMs) / 30000LL);
}

bool Director::consumeDirty() {
    bool d = dirty_;
    dirty_ = false;
    return d;
}

const char* Director::modeName(Mode m) {
    return MODE_NAMES[(int)m];
}

void Director::note(const char* fmt, ...) {
    if (!env_) return;
    char buf[100];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    env_->log(buf);
}
