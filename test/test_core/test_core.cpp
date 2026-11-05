//
// test_core.cpp — Mira 3.0's pure core (timeline + director) driven through
// simulated days on the host. Every command the director sends is recorded by a
// fake bridge, which echoes on/off changes back the way the real SSE stream
// does. Run: pio test -e native   (behavior reference: Markdowns/MIRA_3.md)
//
#include <unity.h>
#include <stdio.h>
#include <string>
#include <utility>
#include <vector>
#include "config.h"
#include "director.h"
#include "echo.h"
#include "timeline.h"

namespace {

struct PutRec {
    int64_t  t;
    uint8_t  idx;
    bool     on;
    float    bri;
    uint16_t ct;
    uint32_t dur;
    bool     preset;
};

// The bridge as the director sees it: records commands; on/off changes come back
// as echoes the test delivers after each step (like SSE events arriving later).
struct FakeEnv : Director::Env {
    std::vector<PutRec>                    puts;
    std::vector<std::string>               logs;
    std::vector<std::pair<uint8_t, bool> > echoes;
    bool      on[LIGHT_COUNT];
    float     bri[LIGHT_COUNT];
    bool      deaf[LIGHT_COUNT];  // takes every command, never changes (powered off at the wall)
    int64_t   nowMs;
    PutResult result;
    bool      verbose;

    FakeEnv() : nowMs(0), result(Sent), verbose(false) {
        for (int i = 0; i < LIGHT_COUNT; i++) { on[i] = false; bri[i] = 0.0f; deaf[i] = false; }
    }
    PutResult put(uint8_t idx, bool o, float b, uint16_t ct, uint32_t dur) {
        PutRec r = { nowMs, idx, o, b, ct, dur, false };
        puts.push_back(r);
        if (verbose) printf("  put %-8s on=%d bri=%6.2f ct=%u dur=%u\n", lightName(idx), o ? 1 : 0, b, ct, dur);
        if (result != Sent) return result;
        if (deaf[idx]) return Sent;
        if (on[idx] != o) { on[idx] = o; echoes.push_back(std::make_pair(idx, o)); }
        if (o) bri[idx] = b;
        return Sent;
    }
    void preset(uint8_t idx, float b, uint16_t ct, uint32_t dur) {
        PutRec r = { nowMs, idx, false, b, ct, dur, true };
        puts.push_back(r);
    }
    void log(const char* m) {
        logs.push_back(m);
        if (verbose) printf("  log: %s\n", m);
    }
};

const int64_t DAY_MS = 86400000LL;
const int64_t DAY0   = 20000LL * DAY_MS;  // an arbitrary local midnight (local == epoch in the sim)

int64_t T(int day, int h, int m, int s = 0) {
    return DAY0 + (int64_t)day * DAY_MS + ((int64_t)h * 3600 + m * 60 + s) * 1000LL;
}

struct Sim {
    Director d;
    FakeEnv  env;
    int64_t  ms;

    Sim() : ms(0) { d.begin(&env); }

    Now now() const {
        Now n;
        n.ms       = ms;
        n.localSec = (int32_t)((ms / 1000LL) % 86400LL);
        return n;
    }
    void set(int64_t t) { ms = t; env.nowMs = t; }
    // Lights that are on at boot sit at the profile's level for that moment.
    void boot(int64_t t, LightMask physOn, const DirectorState* restored = 0, const DayFacts* saved = 0) {
        set(t);
        float level = profileBri(toMds(now().localSec));
        for (int i = 0; i < LIGHT_COUNT; i++) {
            env.on[i]  = (physOn & lightBit(i)) != 0;
            env.bri[i] = env.on[i] ? level : 0.0f;
        }
        d.boot(now(), physOn, env.bri, restored, saved);
    }
    void deliver() {
        while (!env.echoes.empty()) {
            std::pair<uint8_t, bool> e = env.echoes.front();
            env.echoes.erase(env.echoes.begin());
            d.onEdge(now(), e.first, e.second, false, env.bri[e.first]);
        }
    }
    void runTo(int64_t t) {  // 1 s ticks, like the shell's loop
        while (ms < t) {
            set(ms + 1000);
            d.tick(now());
            deliver();
        }
    }
    void userOn(int idx, float b) {
        env.on[idx] = true; env.bri[idx] = b;
        d.onEdge(now(), (uint8_t)idx, true, true, b);
        deliver();
    }
    void userOff(int idx) {
        env.on[idx] = false;
        d.onEdge(now(), (uint8_t)idx, false, true, 0.0f);
        deliver();
    }
    void userBri(int idx, float b) {
        env.bri[idx] = b;
        d.onUserBrightness(now(), idx, b, 0);
        deliver();
    }
    void powerLost(int idx) {  // cut at the wall: the bulb goes dark and hears nothing
        env.on[idx] = false; env.deaf[idx] = true;
        d.onPower(now(), (uint8_t)idx, false, 0.0f);
        deliver();
    }
    void powerBack(int idx, float b) {  // the bulb comes back at its own level
        env.on[idx] = true; env.bri[idx] = b; env.deaf[idx] = false;
        d.onPower(now(), (uint8_t)idx, true, b);
        deliver();
    }
    bool logged(const char* text) const {
        for (size_t k = 0; k < env.logs.size(); k++)
            if (env.logs[k].find(text) != std::string::npos) return true;
        return false;
    }
    void command(Director::Cmd c, int32_t v = 0) {
        d.onCommand(now(), c, v);
        deliver();
    }
    size_t mark() const { return env.puts.size(); }
    int count(size_t from) const { return (int)(env.puts.size() - from); }
    const PutRec* find(size_t from, int idx) const {
        for (size_t k = from; k < env.puts.size(); k++)
            if (env.puts[k].idx == idx && !env.puts[k].preset) return &env.puts[k];
        return 0;
    }
    int countFor(size_t from, int idx) const {
        int n = 0;
        for (size_t k = from; k < env.puts.size(); k++)
            if (env.puts[k].idx == idx && !env.puts[k].preset) n++;
        return n;
    }
    Director::Mode mode() const { return d.mode(now()); }
};

const LightMask FLOOR   = 1 << LIGHT_FLOOR;
const LightMask DRESSER = 1 << LIGHT_DRESSER;
const LightMask CHEST   = 1 << LIGHT_CHEST;
const LightMask CEIL    = 1 << LIGHT_CEIL_1;
const LightMask ALL     = FLOOR | DRESSER | CHEST | CEIL;

void assertPut(const PutRec* p, bool on, float bri, uint32_t durMs, uint32_t durTol = 2000) {
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL(on, p->on);
    if (on) TEST_ASSERT_FLOAT_WITHIN(0.6f, bri, p->bri);
    TEST_ASSERT_UINT32_WITHIN(durTol, durMs, p->dur);
}

}  // namespace

void setUp() {}
void tearDown() {}

// ── timeline ────────────────────────────────────────────────────────────────

void test_ct_map_matches_2x() {
    TEST_ASSERT_EQUAL_UINT16(280, ctForBri(100.0f));
    TEST_ASSERT_EQUAL_UINT16(317, ctForBri(75.0f));
    TEST_ASSERT_EQUAL_UINT16(370, ctForBri(40.0f));
    TEST_ASSERT_EQUAL_UINT16(392, ctForBri(25.0f));
    TEST_ASSERT_EQUAL_UINT16(400, ctForBri(19.7f));
    TEST_ASSERT_EQUAL_UINT16(400, ctForBri(5.0f));   // clamped warm
}

void test_profile_levels() {
    TEST_ASSERT_FLOAT_WITHIN(0.01f,  75.0f, profileBri(toMds(HHMM(9, 30))));
    TEST_ASSERT_FLOAT_WITHIN(0.01f,  87.5f, profileBri(toMds(HHMM(10, 30))));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, profileBri(toMds(HHMM(12, 0))));
    TEST_ASSERT_FLOAT_WITHIN(0.01f,  87.5f, profileBri(toMds(HHMM(17, 30))));
    TEST_ASSERT_FLOAT_WITHIN(0.01f,  75.0f, profileBri(toMds(HHMM(19, 0))));
    TEST_ASSERT_FLOAT_WITHIN(0.01f,  57.5f, profileBri(toMds(HHMM(19, 30))));
    TEST_ASSERT_FLOAT_WITHIN(0.01f,  40.0f, profileBri(toMds(HHMM(20, 15))));
    TEST_ASSERT_FLOAT_WITHIN(0.01f,  32.5f, profileBri(toMds(HHMM(20, 52) + 30)));
    TEST_ASSERT_FLOAT_WITHIN(0.01f,  25.0f, profileBri(toMds(HHMM(0, 30))));
    TEST_ASSERT_FALSE(periodAt(toMds(HHMM(2, 0))).driven);
    TEST_ASSERT_TRUE(periodAt(toMds(HHMM(19, 30))).fade);
    TEST_ASSERT_FALSE(periodAt(toMds(HHMM(12, 0))).fade);
}

void test_next_fade_start_is_the_hold_end() {
    TEST_ASSERT_EQUAL_INT32(toMds(HHMM(10, 0)), holdEndAfter(toMds(HHMM(9, 5))));
    TEST_ASSERT_EQUAL_INT32(toMds(HHMM(17, 0)), holdEndAfter(toMds(HHMM(12, 0))));
    TEST_ASSERT_EQUAL_INT32(toMds(HHMM(20, 45)), holdEndAfter(toMds(HHMM(19, 15))));  // not the fade it's in
    TEST_ASSERT_EQUAL_INT32(toMds(HHMM(22, 30)), holdEndAfter(toMds(HHMM(20, 50))));  // the dresser's off
    TEST_ASSERT_EQUAL_INT32(toMds(HHMM(1, 0)),  holdEndAfter(toMds(HHMM(23, 45))));   // lights-out
}

void test_crossing_midnight_and_dst() {
    TEST_ASSERT_TRUE(crossedLocal(HHMM(8, 59), HHMM(9, 0), HHMM(9, 0)));
    TEST_ASSERT_FALSE(crossedLocal(HHMM(9, 0), HHMM(9, 0) + 1, HHMM(9, 0)));
    TEST_ASSERT_TRUE(crossedLocal(HHMM(23, 59) + 59, 0, 0));              // midnight
    TEST_ASSERT_TRUE(crossedLocal(HHMM(23, 59), HHMM(1, 0), HHMM(1, 0)));  // across midnight, long step
    // DST fall-back: 01:59:59 CDT → 01:00:00 CST. Lights-out (01:00) must not refire.
    TEST_ASSERT_FALSE(crossedLocal(HHMM(1, 59) + 59, HHMM(1, 0), HHMM(1, 0)));
    TEST_ASSERT_FALSE(crossedLocal(HHMM(1, 0), HHMM(1, 0) + 1, HHMM(1, 0)));
    // Spring-forward: 01:59:59 → 03:00:00 skips the 02:00 hour, which has no events.
    TEST_ASSERT_FALSE(crossedLocal(HHMM(1, 59) + 59, HHMM(3, 0), HHMM(1, 0)));
}

// ── echo recognition (echo.h) ───────────────────────────────────────────────

void test_echo_early_window_accepts_the_whole_range() {
    // 10:00 command: 75 → 100 over 60 min. Seconds later the bridge echoes the
    // target, or the bulb reports where it starts — both ours.
    TEST_ASSERT_TRUE(echoOnPath(75.0f, 100.0f, 500, 3600000, 100.0f));
    TEST_ASSERT_TRUE(echoOnPath(75.0f, 100.0f, 500, 3600000, 75.0f));
    TEST_ASSERT_FALSE(echoOnPath(75.0f, 100.0f, 500, 3600000, 50.0f));
}

void test_echo_mid_fade_only_near_the_current_position() {
    const uint32_t HALF = 1800000;  // 10:30 — the fade is at 87.5%
    TEST_ASSERT_TRUE(echoOnPath(75.0f, 100.0f, HALF, 3600000, 87.5f));   // a progress report
    TEST_ASSERT_TRUE(echoOnPath(75.0f, 100.0f, HALF, 3600000, 82.0f));   // within the slop
    TEST_ASSERT_FALSE(echoOnPath(75.0f, 100.0f, HALF, 3600000, 78.0f));  // you, dimming it
    TEST_ASSERT_FALSE(echoOnPath(75.0f, 100.0f, HALF, 3600000, 100.0f)); // the target, too early — not ours
    // Once landed, the target is ours again (late settle inside the grace).
    TEST_ASSERT_TRUE(echoOnPath(75.0f, 100.0f, 3601000, 3600000, 100.0f));
}

void test_echo_off_fade_and_short_commands() {
    TEST_ASSERT_TRUE(echoOnPath(40.0f, 0.0f, 2000, FADE_MS, 20.0f));    // fading out
    TEST_ASSERT_TRUE(echoOnPath(30.0f, 57.5f, 200, JOIN_SNAP_MS, 57.5f)); // a snap's echo
}

// ── a whole day ─────────────────────────────────────────────────────────────

void test_full_day_auto_wake_one_command_per_change() {
    Sim s;
    s.boot(T(0, 5, 0), 0);
    TEST_ASSERT_EQUAL((int)Director::Mode::Asleep, (int)s.mode());
    s.runTo(T(0, 8, 59, 59));
    TEST_ASSERT_EQUAL(0, s.count(0));  // asleep: nothing

    size_t m = s.mark();
    s.runTo(T(0, 9, 0, 0));  // 9 AM auto-wake: the floor fades on toward 25%
    TEST_ASSERT_EQUAL((int)Director::Mode::Wake, (int)s.mode());
    assertPut(s.find(m, LIGHT_FLOOR), true, 25.83f, FADE_MS, 0);
    TEST_ASSERT_EQUAL(1, s.count(m));

    m = s.mark();
    s.runTo(T(0, 9, 0, 16));  // fade-in landed: one command for the rest of the wake
    assertPut(s.find(m, LIGHT_FLOOR), true, 75.0f, 885000);
    TEST_ASSERT_EQUAL(1, s.count(m));

    m = s.mark();
    s.runTo(T(0, 9, 5, 16));  // dresser joins at +5 min, in line with the floor
    assertPut(s.find(m, LIGHT_DRESSER), true, 42.5f, FADE_MS, 0);
    TEST_ASSERT_EQUAL(2, s.countFor(m, LIGHT_DRESSER));  // fade-in + glide
    TEST_ASSERT_EQUAL(2, s.count(m));

    m = s.mark();
    s.runTo(T(0, 9, 10, 16));  // chest at +10 min; the ceiling isn't due yet
    assertPut(s.find(m, LIGHT_CHEST), true, 59.17f, FADE_MS, 0);
    TEST_ASSERT_EQUAL(2, s.count(m));

    m = s.mark();
    s.runTo(T(0, 9, 59, 59));  // wake done, morning hold at 75%: silence
    TEST_ASSERT_EQUAL(0, s.count(m));
    TEST_ASSERT_EQUAL((int)Director::Mode::Active, (int)s.mode());

    m = s.mark();
    s.runTo(T(0, 10, 0, 16));  // 10:00: one 60-min command each; ceiling fades on, then joins
    assertPut(s.find(m, LIGHT_FLOOR), true, 100.0f, 3600000);
    assertPut(s.find(m, LIGHT_DRESSER), true, 100.0f, 3600000);
    assertPut(s.find(m, LIGHT_CHEST), true, 100.0f, 3600000);
    assertPut(s.find(m, LIGHT_CEIL_1), true, 75.1f, FADE_MS, 0);
    TEST_ASSERT_EQUAL(2, s.countFor(m, LIGHT_CEIL_1));
    TEST_ASSERT_EQUAL(5, s.count(m));

    m = s.mark();
    s.runTo(T(0, 16, 59, 59));  // 11:00 → 17:00 steady at 100%: nothing to send
    TEST_ASSERT_EQUAL(0, s.count(m));

    m = s.mark();
    s.runTo(T(0, 17, 0, 1));
    TEST_ASSERT_EQUAL(4, s.count(m));
    assertPut(s.find(m, LIGHT_CEIL_1), true, 75.0f, 3600000);

    m = s.mark();
    s.runTo(T(0, 19, 0, 1));  // ceiling off as the three that remain start down
    TEST_ASSERT_EQUAL(4, s.count(m));
    assertPut(s.find(m, LIGHT_CEIL_1), false, 0, FADE_MS, 0);
    assertPut(s.find(m, LIGHT_FLOOR), true, 40.0f, 3600000);

    m = s.mark();
    s.runTo(T(0, 20, 0, 1));
    TEST_ASSERT_EQUAL(1, s.count(m));
    assertPut(s.find(m, LIGHT_CHEST), false, 0, FADE_MS, 0);

    m = s.mark();
    s.runTo(T(0, 20, 45, 1));
    TEST_ASSERT_EQUAL(2, s.count(m));
    assertPut(s.find(m, LIGHT_DRESSER), true, 25.0f, 900000);

    m = s.mark();
    s.runTo(T(0, 22, 29, 59));  // 21:00 → 22:30 steady at 25%: nothing to send
    TEST_ASSERT_EQUAL(0, s.count(m));
    s.runTo(T(0, 22, 30, 1));
    TEST_ASSERT_EQUAL(1, s.count(m));
    assertPut(s.find(m, LIGHT_DRESSER), false, 0, FADE_MS, 0);

    m = s.mark();
    s.runTo(T(1, 1, 0, 1));  // 1 AM lights-out
    TEST_ASSERT_EQUAL(1, s.count(m));
    assertPut(s.find(m, LIGHT_FLOOR), false, 0, FADE_MS, 0);
    TEST_ASSERT_EQUAL((int)Director::Mode::Asleep, (int)s.mode());

    // The whole day was 24 commands — one per change, the bulbs did the fading.
    TEST_ASSERT_EQUAL(24, (int)s.env.puts.size());

    s.runTo(T(1, 9, 0, 1));  // and the next morning starts again on its own
    TEST_ASSERT_EQUAL((int)Director::Mode::Wake, (int)s.mode());
}

// ── wake triggers ───────────────────────────────────────────────────────────

void test_manual_wake_with_chest_rides_along_in_order() {
    Sim s;
    s.boot(T(0, 5, 0), 0);
    s.runTo(T(0, 7, 0, 0));
    size_t m = s.mark();
    s.userOn(LIGHT_CHEST, 40.0f);  // turning on any light after 6 AM starts the wake
    TEST_ASSERT_EQUAL((int)Director::Mode::Wake, (int)s.mode());
    assertPut(s.find(m, LIGHT_CHEST), true, 25.0f, JOIN_SNAP_MS, 0);  // snapped in line
    assertPut(s.find(m, LIGHT_FLOOR), true, 25.83f, FADE_MS, 0);     // floor fades on
    m = s.mark();
    s.runTo(T(0, 7, 5, 16));
    TEST_ASSERT_NOT_NULL(s.find(m, LIGHT_DRESSER));                  // dresser still at +5
    m = s.mark();
    s.runTo(T(0, 7, 10, 30));
    TEST_ASSERT_NULL(s.find(m, LIGHT_CHEST));                        // its own join is a no-op
}

void test_sleep_window_leaves_lights_alone_then_6am_wakes() {
    Sim s;
    s.boot(T(0, 22, 0), 0);  // everything off at 22:00 → asleep
    TEST_ASSERT_EQUAL((int)Director::Mode::Asleep, (int)s.mode());
    s.runTo(T(1, 3, 0));
    size_t m = s.mark();
    s.userOn(LIGHT_FLOOR, 30.0f);  // 3 AM bathroom trip: left alone
    s.runTo(T(1, 3, 5));
    s.userOff(LIGHT_FLOOR);
    s.runTo(T(1, 5, 50));
    s.userOn(LIGHT_FLOOR, 30.0f);  // up early
    s.runTo(T(1, 5, 59, 59));
    TEST_ASSERT_EQUAL(0, s.count(m));
    TEST_ASSERT_EQUAL((int)Director::Mode::Asleep, (int)s.mode());
    s.runTo(T(1, 6, 0, 1));  // a light already on at 6 AM starts the wake
    TEST_ASSERT_EQUAL((int)Director::Mode::Wake, (int)s.mode());
    assertPut(s.find(m, LIGHT_FLOOR), true, 25.83f, FADE_MS, 0);
}

void test_late_wake_after_cold_boot_fades_into_the_day() {
    Sim s;
    s.boot(T(0, 14, 0), 0);  // power came back mid-day with everything off: wait for you
    s.runTo(T(0, 14, 30));
    TEST_ASSERT_EQUAL(0, s.count(0));
    size_t m = s.mark();
    s.userOn(LIGHT_CHEST, 40.0f);
    assertPut(s.find(m, LIGHT_FLOOR), true, 26.25f, FADE_MS, 0);  // 25 → 100 over 15 min
    m = s.mark();
    s.runTo(T(0, 14, 40, 1));
    TEST_ASSERT_NOT_NULL(s.find(m, LIGHT_DRESSER));
    TEST_ASSERT_NOT_NULL(s.find(m, LIGHT_CEIL_1));  // the ceiling joins with the chest after 10:00
}

void test_dashboard_wake_ignored_at_night() {
    Sim s;
    s.boot(T(0, 3, 0), 0);
    s.command(Director::Cmd::Wake);
    TEST_ASSERT_EQUAL((int)Director::Mode::Asleep, (int)s.mode());
    TEST_ASSERT_EQUAL(0, s.count(0));
}

// ── your changes ────────────────────────────────────────────────────────────

void test_light_turned_on_mid_fade_snaps_in_line_then_follows() {
    Sim s;
    s.boot(T(0, 19, 29), FLOOR | DRESSER);
    s.runTo(T(0, 19, 30));
    size_t m = s.mark();
    s.userOn(LIGHT_CEIL_1, 75.0f);  // ceiling after its window: it joins, right level
    assertPut(s.find(m, LIGHT_CEIL_1), true, 57.5f, JOIN_SNAP_MS, 0);
    m = s.mark();
    s.runTo(T(0, 19, 30, 2));
    assertPut(s.find(m, LIGHT_CEIL_1), true, 40.0f, 1800000);
}

void test_hold_in_steady_lasts_until_next_fade_period() {
    Sim s;
    s.boot(T(0, 11, 59), ALL);
    s.runTo(T(0, 12, 1));
    size_t m = s.mark();
    s.userBri(LIGHT_FLOOR, 50.0f);
    TEST_ASSERT_EQUAL((int)Director::Mode::Hold, (int)s.mode());
    TEST_ASSERT_EQUAL(0, s.count(m));  // nothing mid-fade, nothing to freeze
    s.runTo(T(0, 16, 59, 59));
    TEST_ASSERT_EQUAL(0, s.count(m));  // held through the afternoon
    s.runTo(T(0, 17, 0, 1));           // the next fade period ends it: all → 75% by 18:00
    TEST_ASSERT_EQUAL((int)Director::Mode::Active, (int)s.mode());
    TEST_ASSERT_EQUAL(4, s.count(m));
    assertPut(s.find(m, LIGHT_FLOOR), true, 75.0f, 3600000);
}

void test_hold_mid_fade_freezes_others_and_defers_events() {
    Sim s;
    s.boot(T(0, 18, 59), FLOOR | DRESSER | CHEST);
    s.runTo(T(0, 19, 15));
    size_t m = s.mark();
    s.userBri(LIGHT_FLOOR, 60.0f);
    // the bulbs were mid-fade: dresser and chest stop where they are (66.25%)
    TEST_ASSERT_EQUAL(2, s.count(m));
    assertPut(s.find(m, LIGHT_DRESSER), true, 66.25f, FREEZE_MS, 0);
    assertPut(s.find(m, LIGHT_CHEST), true, 66.25f, FREEZE_MS, 0);
    TEST_ASSERT_NULL(s.find(m, LIGHT_FLOOR));

    m = s.mark();
    s.runTo(T(0, 20, 44, 59));  // the 20:00 chest-off waits for the hold
    TEST_ASSERT_EQUAL(0, s.count(m));
    TEST_ASSERT_TRUE(s.env.on[LIGHT_CHEST]);

    s.runTo(T(0, 20, 45, 1));  // hold over: catch up — chest off, the rest fade to 25%
    TEST_ASSERT_EQUAL(3, s.count(m));
    assertPut(s.find(m, LIGHT_CHEST), false, 0, FADE_MS, 0);
    assertPut(s.find(m, LIGHT_FLOOR), true, 25.0f, 900000);
    assertPut(s.find(m, LIGHT_DRESSER), true, 25.0f, 900000);
}

void test_resume_mid_fade_glides_for_the_time_left() {
    Sim s;
    s.boot(T(0, 19, 29), FLOOR | DRESSER);
    s.runTo(T(0, 19, 30));
    s.command(Director::Cmd::Hold);
    TEST_ASSERT_EQUAL((int)Director::Mode::Hold, (int)s.mode());
    s.runTo(T(0, 19, 40));
    size_t m = s.mark();
    s.command(Director::Cmd::Resume);  // 7:40 PM → 40% over the 20 min left
    TEST_ASSERT_EQUAL(2, s.count(m));
    assertPut(s.find(m, LIGHT_FLOOR), true, 40.0f, 1200000);
}

void test_resume_in_steady_fades_only_the_changed_light() {
    Sim s;
    s.boot(T(0, 11, 59), ALL);
    s.runTo(T(0, 12, 0));
    s.userBri(LIGHT_FLOOR, 50.0f);
    s.runTo(T(0, 12, 30));
    size_t m = s.mark();
    s.command(Director::Cmd::Resume);
    TEST_ASSERT_EQUAL(1, s.count(m));
    assertPut(s.find(m, LIGHT_FLOOR), true, 100.0f, FADE_MS, 0);
}

void test_hold_after_the_dresser_goes_off_ends_at_lights_out() {
    Sim s;
    s.boot(T(0, 23, 44), FLOOR);
    s.runTo(T(0, 23, 45));
    s.userBri(LIGHT_FLOOR, 50.0f);
    size_t m = s.mark();
    s.runTo(T(1, 0, 59, 59));
    TEST_ASSERT_EQUAL(0, s.count(m));
    s.runTo(T(1, 1, 0, 1));
    assertPut(s.find(m, LIGHT_FLOOR), false, 0, FADE_MS, 0);
    TEST_ASSERT_EQUAL((int)Director::Mode::Asleep, (int)s.mode());
}

void test_hold_after_last_fade_ends_when_the_dresser_goes_off() {
    Sim s;
    s.boot(T(0, 21, 29), FLOOR | DRESSER);
    s.runTo(T(0, 21, 30));
    s.userBri(LIGHT_FLOOR, 50.0f);
    TEST_ASSERT_EQUAL((int)Director::Mode::Hold, (int)s.mode());
    size_t m = s.mark();
    s.runTo(T(0, 22, 29, 59));
    TEST_ASSERT_EQUAL(0, s.count(m));
    s.runTo(T(0, 22, 30, 1));
    TEST_ASSERT_EQUAL((int)Director::Mode::Active, (int)s.mode());
    TEST_ASSERT_EQUAL(2, s.count(m));
    assertPut(s.find(m, LIGHT_DRESSER), false, 0, FADE_MS, 0);
    assertPut(s.find(m, LIGHT_FLOOR), true, 25.0f, FADE_MS, 0);
}

// ── reports on the plan's path aren't holds ─────────────────────────────────

void test_late_report_on_the_wake_path_is_not_a_hold() {
    Sim s;  // Oct 17: "Hold — Chest set to 60%", 28 s after the chest came on
    s.boot(T(0, 8, 59), 0);
    s.runTo(T(0, 9, 10, 29));
    TEST_ASSERT_EQUAL((int)Director::Mode::Wake, (int)s.mode());
    size_t m = s.mark();
    s.userBri(LIGHT_CHEST, s.d.inLineBri(s.now()) + 0.1f);
    TEST_ASSERT_EQUAL((int)Director::Mode::Wake, (int)s.mode());
    TEST_ASSERT_TRUE(s.logged("in line, no hold"));
    // Only that light is re-sent, and the others keep fading untouched.
    TEST_ASSERT_EQUAL(1, s.count(m));
    assertPut(s.find(m, LIGHT_CHEST), true, 75.0f, 271000, 3000);
}

void test_report_off_the_path_is_still_a_hold() {
    Sim s;
    s.boot(T(0, 8, 59), 0);
    s.runTo(T(0, 9, 10, 29));
    s.userBri(LIGHT_CHEST, s.d.inLineBri(s.now()) + IN_LINE_TOL_BRI + 1.0f);
    TEST_ASSERT_EQUAL((int)Director::Mode::Hold, (int)s.mode());
}

void test_in_line_nudge_in_steady_goes_back_to_the_level() {
    Sim s;
    s.boot(T(0, 11, 59), ALL);
    s.runTo(T(0, 12, 0));
    size_t m = s.mark();
    s.userBri(LIGHT_FLOOR, 98.0f);
    TEST_ASSERT_EQUAL((int)Director::Mode::Active, (int)s.mode());
    TEST_ASSERT_EQUAL(1, s.count(m));
    assertPut(s.find(m, LIGHT_FLOOR), true, 100.0f, FADE_MS, 0);
}

void test_in_line_report_during_a_hold_changes_nothing() {
    Sim s;
    s.boot(T(0, 11, 59), ALL);
    s.runTo(T(0, 12, 0));
    s.userBri(LIGHT_FLOOR, 50.0f);
    size_t m = s.mark();
    s.userBri(LIGHT_DRESSER, 100.0f);
    TEST_ASSERT_EQUAL((int)Director::Mode::Hold, (int)s.mode());
    TEST_ASSERT_EQUAL(0, s.count(m));
}

// ── wall switches ───────────────────────────────────────────────────────────

void test_power_lost_is_out_for_today() {
    Sim s;
    s.boot(T(0, 13, 59), ALL);
    s.runTo(T(0, 14, 0));
    size_t m = s.mark();
    s.powerLost(LIGHT_DRESSER);
    s.runTo(T(0, 14, 0, 5));
    TEST_ASSERT_TRUE(s.logged("Dresser lost power"));
    TEST_ASSERT_TRUE(s.logged("Dresser off — out for today"));
    TEST_ASSERT_TRUE((s.d.outMask() & DRESSER) != 0);
    s.runTo(T(0, 17, 0, 1));  // the next fade skips it
    TEST_ASSERT_EQUAL(0, s.countFor(m, LIGHT_DRESSER));
    TEST_ASSERT_EQUAL(1, s.countFor(m, LIGHT_FLOOR));
}

void test_power_back_fades_into_the_current_trajectory() {
    Sim s;
    s.boot(T(0, 19, 14), FLOOR | DRESSER | CHEST);
    s.runTo(T(0, 19, 15));
    s.powerLost(LIGHT_CHEST);
    s.runTo(T(0, 19, 20));
    size_t m = s.mark();
    s.powerBack(LIGHT_CHEST, 100.0f);  // back at its own level, nowhere near the fade
    TEST_ASSERT_TRUE(s.logged("Chest has power again"));
    TEST_ASSERT_EQUAL((int)Director::Mode::Active, (int)s.mode());
    const float at = 75.0f - 35.0f * (20.25f / 60.0f);  // the fade, 15 s from now
    const PutRec* p = s.find(m, LIGHT_CHEST);
    assertPut(p, true, at, FADE_MS, 0);
    TEST_ASSERT_EQUAL_UINT16(ctForBri(p->bri), p->ct);
    TEST_ASSERT_FALSE((s.d.outMask() & CHEST) != 0);
    m = s.mark();
    s.runTo(T(0, 19, 20, 17));  // landed: one command for the rest of the fade
    assertPut(s.find(m, LIGHT_CHEST), true, 40.0f, 2385000);
}

void test_power_lost_before_a_scheduled_on_stays_off() {
    Sim s;
    s.boot(T(0, 9, 29), FLOOR | DRESSER | CHEST);
    s.runTo(T(0, 9, 30));
    s.powerLost(LIGHT_CEIL_1);
    s.runTo(T(0, 9, 30, 5));
    size_t m = s.mark();
    s.runTo(T(0, 10, 0, 5));
    TEST_ASSERT_EQUAL(0, s.countFor(m, LIGHT_CEIL_1));
    TEST_ASSERT_TRUE(s.logged("Ceiling stays off — out for today"));
    m = s.mark();
    s.runTo(T(0, 10, 30));
    s.powerBack(LIGHT_CEIL_1, 40.0f);
    assertPut(s.find(m, LIGHT_CEIL_1), true, 75.0f + 25.0f * (30.25f / 60.0f), FADE_MS, 0);
}

void test_power_back_during_a_hold_waits_for_the_hold() {
    Sim s;
    s.boot(T(0, 11, 59), ALL);
    s.runTo(T(0, 12, 0));
    s.powerLost(LIGHT_CHEST);
    s.runTo(T(0, 12, 0, 5));
    s.userBri(LIGHT_FLOOR, 50.0f);
    size_t m = s.mark();
    s.powerBack(LIGHT_CHEST, 80.0f);
    TEST_ASSERT_EQUAL(0, s.countFor(m, LIGHT_CHEST));
    s.command(Director::Cmd::Resume);
    assertPut(s.find(m, LIGHT_CHEST), true, 100.0f, FADE_MS, 0);
}

void test_power_back_at_night_is_left_alone_and_in_the_morning_starts_the_wake() {
    Sim s;
    s.boot(T(0, 22, 0), 0);
    s.powerLost(LIGHT_CHEST);
    s.runTo(T(1, 3, 0));
    size_t m = s.mark();
    s.powerBack(LIGHT_CHEST, 80.0f);
    TEST_ASSERT_EQUAL(0, s.count(m));
    TEST_ASSERT_EQUAL((int)Director::Mode::Asleep, (int)s.mode());
    s.powerLost(LIGHT_CHEST);
    s.runTo(T(1, 7, 0));
    m = s.mark();
    s.powerBack(LIGHT_CHEST, 80.0f);
    TEST_ASSERT_EQUAL((int)Director::Mode::Wake, (int)s.mode());
    assertPut(s.find(m, LIGHT_CHEST), true, 25.0f + 50.0f * (15.0f / 900.0f), FADE_MS, 0);
}

void test_everything_off_before_9pm_is_a_nap() {
    Sim s;
    s.boot(T(0, 13, 59), ALL);
    s.runTo(T(0, 14, 0));
    for (int i = 0; i < LIGHT_COUNT; i++) s.userOff(i);  // a room toggle: one burst
    s.runTo(T(0, 14, 0, 4));
    TEST_ASSERT_EQUAL((int)Director::Mode::Nap, (int)s.mode());
    size_t m = s.mark();
    s.runTo(T(0, 14, 59, 59));
    TEST_ASSERT_EQUAL(0, s.count(m));
    s.runTo(T(0, 15, 0, 4));  // 60 min later: everything that was on fades back in
    TEST_ASSERT_EQUAL(4, s.count(m));
    assertPut(s.find(m, LIGHT_CEIL_1), true, 100.0f, FADE_MS, 0);
    TEST_ASSERT_EQUAL((int)Director::Mode::Active, (int)s.mode());
}

void test_light_on_during_nap_ends_it_and_brings_the_rest() {
    Sim s;
    s.boot(T(0, 13, 59), FLOOR | DRESSER);
    s.runTo(T(0, 14, 0));
    s.userOff(LIGHT_FLOOR);
    s.userOff(LIGHT_DRESSER);
    s.runTo(T(0, 14, 20));
    size_t m = s.mark();
    s.userOn(LIGHT_FLOOR, 30.0f);
    TEST_ASSERT_EQUAL((int)Director::Mode::Active, (int)s.mode());
    assertPut(s.find(m, LIGHT_FLOOR), true, 100.0f, JOIN_SNAP_MS, 0);
    assertPut(s.find(m, LIGHT_DRESSER), true, 100.0f, FADE_MS, 0);
}

void test_nap_running_at_9pm_becomes_sleep() {
    Sim s;
    s.boot(T(0, 20, 29), FLOOR | DRESSER | CHEST);
    s.runTo(T(0, 20, 30));
    s.userOff(LIGHT_FLOOR);
    s.userOff(LIGHT_DRESSER);
    s.userOff(LIGHT_CHEST);
    s.runTo(T(0, 20, 30, 4));
    TEST_ASSERT_EQUAL((int)Director::Mode::Nap, (int)s.mode());
    size_t m = s.mark();
    s.runTo(T(0, 21, 0, 1));
    TEST_ASSERT_EQUAL((int)Director::Mode::Asleep, (int)s.mode());
    s.runTo(T(0, 21, 31));
    TEST_ASSERT_EQUAL(0, s.count(m));  // no fade-in at 21:30 while you're in bed
}

void test_one_light_off_is_out_for_today() {
    Sim s;
    s.boot(T(0, 11, 59), ALL);
    s.runTo(T(0, 12, 0));
    s.userOff(LIGHT_DRESSER);
    s.runTo(T(0, 12, 0, 4));
    TEST_ASSERT_EQUAL((int)DRESSER, (int)s.d.outMask());
    size_t m = s.mark();
    s.runTo(T(0, 17, 0, 1));
    TEST_ASSERT_EQUAL(3, s.count(m));  // the 17:00 fade skips it
    TEST_ASSERT_NULL(s.find(m, LIGHT_DRESSER));
    s.runTo(T(0, 18, 0));
    m = s.mark();
    s.userOn(LIGHT_DRESSER, 100.0f);  // turning it back on brings it back, in line
    TEST_ASSERT_EQUAL(0, (int)s.d.outMask());
    assertPut(s.find(m, LIGHT_DRESSER), true, 75.0f, JOIN_SNAP_MS, 0);
}

void test_out_for_today_blocks_a_scheduled_join() {
    Sim s;
    s.boot(T(0, 8, 59), 0);
    s.runTo(T(0, 9, 1));
    s.userOn(LIGHT_DRESSER, 30.0f);
    s.runTo(T(0, 9, 2));
    s.userOff(LIGHT_DRESSER);
    s.runTo(T(0, 9, 2, 4));
    size_t m = s.mark();
    s.runTo(T(0, 9, 6));  // its +5 min join doesn't bring it back
    TEST_ASSERT_NULL(s.find(m, LIGHT_DRESSER));
    TEST_ASSERT_TRUE((s.d.outMask() & DRESSER) != 0);
}

void test_everything_off_after_9pm_is_sleep_and_lights_are_left_alone() {
    Sim s;
    s.boot(T(0, 21, 29), FLOOR | DRESSER);
    s.runTo(T(0, 21, 30));
    s.userOff(LIGHT_FLOOR);
    s.userOff(LIGHT_DRESSER);
    s.runTo(T(0, 21, 30, 4));
    TEST_ASSERT_EQUAL((int)Director::Mode::Asleep, (int)s.mode());
    size_t m = s.mark();
    s.runTo(T(0, 22, 0));
    s.userOn(LIGHT_FLOOR, 25.0f);
    s.runTo(T(0, 22, 5));
    TEST_ASSERT_EQUAL(0, s.count(m));
}

void test_nap_remaining_zero_ends_it() {
    Sim s;
    s.boot(T(0, 13, 59), FLOOR);
    s.runTo(T(0, 14, 0));
    s.userOff(LIGHT_FLOOR);
    s.runTo(T(0, 14, 0, 4));
    TEST_ASSERT_EQUAL((int)Director::Mode::Nap, (int)s.mode());
    size_t m = s.mark();
    s.command(Director::Cmd::NapRemaining, 0);
    s.runTo(T(0, 14, 0, 6));
    TEST_ASSERT_EQUAL((int)Director::Mode::Active, (int)s.mode());
    TEST_ASSERT_NOT_NULL(s.find(m, LIGHT_FLOOR));
}

void test_nap_command_fades_off_and_returns_on_time() {
    Sim s;
    s.boot(T(0, 13, 59), FLOOR | DRESSER);
    s.runTo(T(0, 14, 0));
    size_t m = s.mark();
    s.command(Director::Cmd::Nap, -1);
    TEST_ASSERT_EQUAL((int)Director::Mode::Nap, (int)s.mode());
    TEST_ASSERT_EQUAL(2, s.count(m));
    assertPut(s.find(m, LIGHT_FLOOR), false, 0.0f, FADE_MS, 0);
    assertPut(s.find(m, LIGHT_DRESSER), false, 0.0f, FADE_MS, 0);
    m = s.mark();
    s.runTo(T(0, 14, 59, 59));
    TEST_ASSERT_EQUAL(0, s.count(m));  // nothing until the default 60 min is up
    s.runTo(T(0, 15, 0, 4));
    TEST_ASSERT_EQUAL((int)Director::Mode::Active, (int)s.mode());
    assertPut(s.find(m, LIGHT_FLOOR), true, 100.0f, FADE_MS, 0);
    assertPut(s.find(m, LIGHT_DRESSER), true, 100.0f, FADE_MS, 0);
}

void test_nap_command_custom_length() {
    Sim s;
    s.boot(T(0, 13, 59), FLOOR);
    s.runTo(T(0, 14, 0));
    s.command(Director::Cmd::Nap, 90);
    s.runTo(T(0, 15, 29, 58));
    TEST_ASSERT_EQUAL((int)Director::Mode::Nap, (int)s.mode());
    s.runTo(T(0, 15, 30, 2));
    TEST_ASSERT_EQUAL((int)Director::Mode::Active, (int)s.mode());
}

void test_nap_command_replaces_a_hold() {
    Sim s;
    s.boot(T(0, 13, 59), FLOOR);
    s.runTo(T(0, 14, 0));
    s.userBri(LIGHT_FLOOR, 40.0f);
    TEST_ASSERT_EQUAL((int)Director::Mode::Hold, (int)s.mode());
    s.command(Director::Cmd::Nap, 30);
    TEST_ASSERT_EQUAL((int)Director::Mode::Nap, (int)s.mode());
    TEST_ASSERT_FALSE(s.env.on[LIGHT_FLOOR]);
}

void test_nap_command_refused_from_9pm_and_while_asleep_or_napping() {
    Sim s;
    s.boot(T(0, 21, 0, 1), FLOOR);
    s.runTo(T(0, 21, 0, 3));
    size_t m = s.mark();
    s.command(Director::Cmd::Nap, -1);
    TEST_ASSERT_EQUAL((int)Director::Mode::Active, (int)s.mode());
    TEST_ASSERT_EQUAL(0, s.count(m));
    TEST_ASSERT_TRUE(s.logged("Nap ignored"));

    Sim a;
    a.boot(T(0, 13, 59), FLOOR);
    a.command(Director::Cmd::Sleep);
    size_t am = a.mark();
    a.command(Director::Cmd::Nap, -1);
    TEST_ASSERT_EQUAL((int)Director::Mode::Asleep, (int)a.mode());
    TEST_ASSERT_EQUAL(0, a.count(am));

    Sim n;
    n.boot(T(0, 13, 59), FLOOR);
    n.command(Director::Cmd::Nap, 30);
    n.command(Director::Cmd::Nap, 90);  // keeps the first nap's length
    n.runTo(T(0, 14, 29, 50));
    TEST_ASSERT_EQUAL((int)Director::Mode::Active, (int)n.mode());
}

void test_nap_command_absorbs_an_off_you_just_made() {
    Sim s;
    s.boot(T(0, 13, 59), FLOOR | DRESSER);
    s.runTo(T(0, 14, 0));
    s.userOff(LIGHT_DRESSER);  // still being grouped when the command lands
    s.command(Director::Cmd::Nap, 90);
    s.runTo(T(0, 14, 0, 10));
    s.runTo(T(0, 15, 29, 50));  // the grouped off didn't restart it at 60 min
    TEST_ASSERT_EQUAL((int)Director::Mode::Nap, (int)s.mode());
    size_t m = s.mark();
    s.runTo(T(0, 15, 30, 4));
    TEST_ASSERT_NOT_NULL(s.find(m, LIGHT_DRESSER));  // and it comes back with the rest
    TEST_ASSERT_NOT_NULL(s.find(m, LIGHT_FLOOR));
}

void test_nap_command_retries_an_off_that_failed() {
    Sim s;
    s.boot(T(0, 13, 59), FLOOR);
    s.runTo(T(0, 14, 0));
    s.env.result = Director::Env::Failed;
    s.command(Director::Cmd::Nap, 30);
    TEST_ASSERT_TRUE(s.env.on[LIGHT_FLOOR]);
    s.env.result = Director::Env::Sent;
    s.runTo(T(0, 14, 5));
    TEST_ASSERT_FALSE(s.env.on[LIGHT_FLOOR]);
    TEST_ASSERT_EQUAL((int)Director::Mode::Nap, (int)s.mode());
}

void test_extend_adds_to_a_nap() {
    Sim s;
    s.boot(T(0, 13, 59), FLOOR);
    s.runTo(T(0, 14, 0));
    s.command(Director::Cmd::Nap, 30);
    s.command(Director::Cmd::Extend, 30);
    s.runTo(T(0, 14, 59, 58));
    TEST_ASSERT_EQUAL((int)Director::Mode::Nap, (int)s.mode());
    s.runTo(T(0, 15, 0, 2));
    TEST_ASSERT_EQUAL((int)Director::Mode::Active, (int)s.mode());
}

void test_extended_nap_still_becomes_asleep_at_9pm() {
    Sim s;
    s.boot(T(0, 20, 30), FLOOR);
    s.command(Director::Cmd::Nap, 20);
    s.command(Director::Cmd::Extend, 60);
    s.runTo(T(0, 21, 0, 2));
    TEST_ASSERT_EQUAL((int)Director::Mode::Asleep, (int)s.mode());
    TEST_ASSERT_FALSE(s.env.on[LIGHT_FLOOR]);
}

void test_extended_hold_stays_frozen_past_a_fade_then_catches_up() {
    Sim s;
    s.boot(T(0, 15, 59), FLOOR);
    s.runTo(T(0, 16, 0));
    s.userBri(LIGHT_FLOOR, 40.0f);  // would end at 17:00
    s.command(Director::Cmd::Extend, 120);  // now 19:00
    size_t m = s.mark();
    s.runTo(T(0, 18, 59, 58));
    TEST_ASSERT_EQUAL((int)Director::Mode::Hold, (int)s.mode());
    TEST_ASSERT_EQUAL(0, s.count(m));
    s.runTo(T(0, 19, 0, 2));
    TEST_ASSERT_EQUAL((int)Director::Mode::Active, (int)s.mode());
    TEST_ASSERT_NOT_NULL(s.find(m, LIGHT_FLOOR));
}

void test_lights_out_still_ends_an_extended_hold() {
    Sim s;
    s.boot(T(0, 23, 0), FLOOR);
    s.userBri(LIGHT_FLOOR, 10.0f);
    TEST_ASSERT_EQUAL((int)Director::Mode::Hold, (int)s.mode());
    s.command(Director::Cmd::Extend, 300);
    s.runTo(T(1, 1, 0, 2));
    TEST_ASSERT_EQUAL((int)Director::Mode::Asleep, (int)s.mode());
    TEST_ASSERT_FALSE(s.env.on[LIGHT_FLOOR]);
}

void test_extend_with_nothing_to_extend_is_ignored() {
    Sim s;
    s.boot(T(0, 13, 59), FLOOR);
    s.runTo(T(0, 14, 0));
    size_t m = s.mark();
    s.command(Director::Cmd::Extend, 30);
    TEST_ASSERT_EQUAL((int)Director::Mode::Active, (int)s.mode());
    TEST_ASSERT_EQUAL(0, s.count(m));
    TEST_ASSERT_TRUE(s.logged("Extend ignored"));
    s.userBri(LIGHT_FLOOR, 40.0f);
    s.command(Director::Cmd::Extend, 0);  // no minutes: the hold's end doesn't move
    TEST_ASSERT_TRUE(s.logged("needs a number"));
}

// ── one light from the dashboard (`dresser off`, `chest on`) ────────────────

void test_light_off_command_sends_the_off_and_is_out_for_today() {
    Sim s;
    s.boot(T(0, 11, 59), ALL);
    s.runTo(T(0, 12, 0));
    size_t m = s.mark();
    s.command(Director::Cmd::LightOff, LIGHT_DRESSER);
    TEST_ASSERT_EQUAL(1, s.count(m));
    assertPut(s.find(m, LIGHT_DRESSER), false, 0.0f, FADE_MS, 0);
    TEST_ASSERT_TRUE(s.logged("Dresser off for today — dashboard"));
    TEST_ASSERT_EQUAL((int)DRESSER, (int)s.d.outMask());
    TEST_ASSERT_EQUAL((int)Director::Mode::Active, (int)s.mode());
    m = s.mark();
    s.runTo(T(0, 17, 0, 1));  // the next fade skips it
    TEST_ASSERT_EQUAL(0, s.countFor(m, LIGHT_DRESSER));
    TEST_ASSERT_EQUAL(1, s.countFor(m, LIGHT_FLOOR));
}

void test_light_off_command_on_the_last_light_is_not_a_nap() {
    Sim s;
    s.boot(T(0, 13, 59), FLOOR);
    s.runTo(T(0, 14, 0));
    s.command(Director::Cmd::LightOff, LIGHT_FLOOR);
    s.runTo(T(0, 14, 0, 10));
    TEST_ASSERT_EQUAL((int)Director::Mode::Active, (int)s.mode());
    TEST_ASSERT_FALSE(s.env.on[LIGHT_FLOOR]);
    size_t m = s.mark();
    s.runTo(T(0, 15, 30));  // no nap to end, so nothing comes back
    TEST_ASSERT_EQUAL(0, s.countFor(m, LIGHT_FLOOR));
}

void test_light_off_command_after_9pm_on_the_last_light_is_asleep() {
    Sim s;
    s.boot(T(0, 22, 59), FLOOR);
    s.runTo(T(0, 23, 0));
    s.command(Director::Cmd::LightOff, LIGHT_FLOOR);
    TEST_ASSERT_EQUAL((int)Director::Mode::Asleep, (int)s.mode());  // the same rule as any last light out
}

void test_light_off_command_before_a_scheduled_on_keeps_it_off() {
    Sim s;
    s.boot(T(0, 9, 29), FLOOR | DRESSER | CHEST);
    s.runTo(T(0, 9, 30));
    size_t m = s.mark();
    s.command(Director::Cmd::LightOff, LIGHT_CEIL_1);  // already off: nothing to send
    TEST_ASSERT_EQUAL(0, s.count(m));
    s.runTo(T(0, 10, 0, 5));
    TEST_ASSERT_EQUAL(0, s.countFor(m, LIGHT_CEIL_1));
    TEST_ASSERT_TRUE(s.logged("Ceiling stays off — out for today"));
}

void test_light_on_command_brings_it_back_in_line_mid_fade() {
    Sim s;
    s.boot(T(0, 11, 59), ALL);
    s.runTo(T(0, 12, 0));
    s.command(Director::Cmd::LightOff, LIGHT_DRESSER);
    s.runTo(T(0, 17, 30));
    size_t m = s.mark();
    s.command(Director::Cmd::LightOn, LIGHT_DRESSER);
    TEST_ASSERT_TRUE(s.logged("Dresser on — dashboard"));
    TEST_ASSERT_EQUAL(0, (int)s.d.outMask());
    TEST_ASSERT_EQUAL(1, s.count(m));
    assertPut(s.find(m, LIGHT_DRESSER), true, 100.0f - 25.0f * (30.25f / 60.0f), FADE_MS, 0);  // 15 s fade-in
    m = s.mark();
    s.runTo(T(0, 17, 30, 16));  // landed: one command for the rest of the fade
    assertPut(s.find(m, LIGHT_DRESSER), true, 75.0f, 1785000);
}

void test_light_on_command_during_a_nap_ends_it() {
    Sim s;
    s.boot(T(0, 13, 59), FLOOR | DRESSER);
    s.runTo(T(0, 14, 0));
    s.command(Director::Cmd::Nap, -1);
    s.runTo(T(0, 14, 10));
    size_t m = s.mark();
    s.command(Director::Cmd::LightOn, LIGHT_DRESSER);
    TEST_ASSERT_EQUAL((int)Director::Mode::Active, (int)s.mode());
    TEST_ASSERT_TRUE(s.logged("Nap over — dashboard"));
    assertPut(s.find(m, LIGHT_DRESSER), true, 100.0f, FADE_MS, 0);
    assertPut(s.find(m, LIGHT_FLOOR), true, 100.0f, FADE_MS, 0);  // the rest of the nap set too
}

void test_light_commands_during_a_hold_switch_only_that_light() {
    Sim s;
    s.boot(T(0, 11, 59), ALL);
    s.runTo(T(0, 12, 0));
    s.command(Director::Cmd::LightOff, LIGHT_CHEST);
    s.runTo(T(0, 12, 0, 20));
    s.userBri(LIGHT_FLOOR, 50.0f);
    TEST_ASSERT_EQUAL((int)Director::Mode::Hold, (int)s.mode());
    size_t m = s.mark();
    s.command(Director::Cmd::LightOn, LIGHT_CHEST);
    s.command(Director::Cmd::LightOff, LIGHT_DRESSER);
    TEST_ASSERT_EQUAL((int)Director::Mode::Hold, (int)s.mode());
    TEST_ASSERT_EQUAL(2, s.count(m));  // nothing else moves
    assertPut(s.find(m, LIGHT_CHEST), true, 100.0f, FADE_MS, 0);
    assertPut(s.find(m, LIGHT_DRESSER), false, 0.0f, FADE_MS, 0);
    TEST_ASSERT_EQUAL((int)DRESSER, (int)s.d.outMask());
    s.runTo(T(0, 12, 1));
    m = s.mark();
    s.command(Director::Cmd::Resume);
    TEST_ASSERT_EQUAL(0, s.countFor(m, LIGHT_DRESSER));  // still out after the hold
}

void test_light_commands_while_asleep() {
    Sim s;
    s.boot(T(0, 21, 29), FLOOR | DRESSER);
    s.runTo(T(0, 21, 30));
    s.userOff(LIGHT_FLOOR);
    s.userOff(LIGHT_DRESSER);
    s.runTo(T(0, 21, 30, 4));
    TEST_ASSERT_EQUAL((int)Director::Mode::Asleep, (int)s.mode());
    s.runTo(T(0, 22, 0));
    size_t m = s.mark();
    s.command(Director::Cmd::LightOn, LIGHT_CHEST);  // no level to give it at night
    TEST_ASSERT_EQUAL(0, s.count(m));
    TEST_ASSERT_TRUE(s.logged("Chest on ignored — asleep"));
    s.command(Director::Cmd::LightOff, LIGHT_DRESSER);
    TEST_ASSERT_TRUE(s.logged("Dresser off ignored — already off"));
    s.userOn(LIGHT_FLOOR, 25.0f);  // left alone, but an off from the dashboard is sent
    m = s.mark();
    s.command(Director::Cmd::LightOff, LIGHT_FLOOR);
    TEST_ASSERT_EQUAL(1, s.count(m));
    assertPut(s.find(m, LIGHT_FLOOR), false, 0.0f, FADE_MS, 0);
    TEST_ASSERT_EQUAL((int)Director::Mode::Asleep, (int)s.mode());
    m = s.mark();
    s.command(Director::Cmd::LightOn, 7);  // not a light
    TEST_ASSERT_EQUAL(0, s.count(m));
    TEST_ASSERT_TRUE(s.logged("Light on ignored — no light 7"));
}

void test_light_on_command_asleep_in_the_day_starts_the_wake() {
    Sim s;
    s.boot(T(0, 13, 59), FLOOR);
    s.command(Director::Cmd::Sleep);
    s.runTo(T(0, 14, 0));
    size_t m = s.mark();
    s.command(Director::Cmd::LightOn, LIGHT_CHEST);
    TEST_ASSERT_EQUAL((int)Director::Mode::Wake, (int)s.mode());
    TEST_ASSERT_TRUE(s.logged("Wake — dashboard"));
    assertPut(s.find(m, LIGHT_FLOOR), true, 26.25f, FADE_MS, 0);  // 25 → 100 over 15 min
    assertPut(s.find(m, LIGHT_CHEST), true, 26.25f, FADE_MS, 0);  // rides it from the start
}

// ── robustness ──────────────────────────────────────────────────────────────

void test_failed_command_retries_with_backoff() {
    Sim s;
    s.boot(T(0, 16, 59), FLOOR);
    s.runTo(T(0, 16, 59, 59));
    s.env.result = Director::Env::Failed;  // the bridge stops taking commands
    size_t m = s.mark();
    s.runTo(T(0, 17, 0, 30));
    // 17:00:00 fails → retry at +5 s fails → retry at +15 s fails → next at +35 s
    TEST_ASSERT_EQUAL(3, s.count(m));
    s.env.result = Director::Env::Sent;
    m = s.mark();
    s.runTo(T(0, 17, 2, 0));
    TEST_ASSERT_EQUAL(1, s.count(m));  // one retry lands, still arriving at 18:00
    assertPut(s.find(m, LIGHT_FLOOR), true, 75.0f, 3565000);
}

void test_unreachable_bulb_backs_off_and_logs_once() {
    Sim s;
    s.boot(T(0, 9, 59), FLOOR | DRESSER | CHEST);
    s.env.deaf[LIGHT_CEIL_1] = true;  // wall switch off: 10:00's ceiling-on never lands
    s.runTo(T(0, 9, 59, 59));
    size_t m = s.mark();
    s.runTo(T(0, 10, 10, 0));
    int tries = s.countFor(m, LIGHT_CEIL_1);
    TEST_ASSERT_TRUE(tries >= 6 && tries <= 10);  // backoff 5 → 10 → 20 → 40 → 60 s, not every ~40 s
    int noted = 0;
    for (size_t k = 0; k < s.env.logs.size(); k++)
        if (s.env.logs[k].find("Ceiling didn't take") != std::string::npos) noted++;
    TEST_ASSERT_EQUAL(1, noted);  // said once, not every retry
    s.env.deaf[LIGHT_CEIL_1] = false;  // power restored: the next retry lands
    s.runTo(T(0, 10, 12, 0));
    TEST_ASSERT_TRUE(s.env.on[LIGHT_CEIL_1]);
}

void test_stale_revert_resends() {
    Sim s;
    s.boot(T(0, 11, 59), ALL);
    s.runTo(T(0, 12, 0));
    size_t m = s.mark();
    s.d.onStaleRevert(s.now(), LIGHT_DRESSER, true);
    s.deliver();
    assertPut(s.find(m, LIGHT_DRESSER), true, 100.0f, FADE_MS, 0);
}

void test_soft_reset_restores_without_switching_anything() {
    Sim a;
    a.boot(T(0, 11, 59), ALL);
    a.runTo(T(0, 12, 0, 30));
    DirectorState saved = a.d.state();

    Sim b;
    b.boot(T(0, 12, 0, 40), ALL, &saved);
    b.runTo(T(0, 16, 59, 59));
    TEST_ASSERT_EQUAL(0, b.count(0));  // picked up mid-day: nothing re-sent
    b.runTo(T(0, 17, 0, 1));
    TEST_ASSERT_EQUAL(4, b.count(0));
}

void test_soft_reset_catches_events_missed_while_down() {
    Sim a;
    a.boot(T(0, 18, 58), ALL);
    a.runTo(T(0, 18, 59, 50));
    DirectorState saved = a.d.state();

    Sim b;  // down for 40 s across 19:00 — the ceiling-off still happens
    b.boot(T(0, 19, 0, 30), ALL, &saved);
    b.runTo(T(0, 19, 0, 31));
    assertPut(b.find(0, LIGHT_CEIL_1), false, 0, FADE_MS, 0);
}

// ── power loss (MIRA_3.md §10) ──────────────────────────────────────────────

DayFacts factsAt(int64_t bootAt, LightMask physOn, int64_t saveAt) {
    Sim a;
    a.boot(bootAt, physOn);
    a.runTo(saveAt);
    return a.d.facts(a.now());
}

void test_power_loss_replays_the_evening_offs() {
    // Oct 15 exactly: last save after the 10:00 ceiling-on, dark until 23:36.
    DayFacts saved = factsAt(T(0, 9, 59), FLOOR | DRESSER | CHEST, T(0, 10, 0, 30));
    Sim b;
    b.boot(T(0, 23, 36), ALL, 0, &saved);
    TEST_ASSERT_TRUE(b.logged("power loss; caught up from 10:00AM"));
    b.runTo(T(0, 23, 36, 2));
    assertPut(b.find(0, LIGHT_CEIL_1), false, 0, FADE_MS, 0);
    assertPut(b.find(0, LIGHT_CHEST), false, 0, FADE_MS, 0);
    assertPut(b.find(0, LIGHT_DRESSER), false, 0, FADE_MS, 0);
    assertPut(b.find(0, LIGHT_FLOOR), true, 25.0f, FADE_MS, 0);
    TEST_ASSERT_EQUAL((int)Director::Mode::Active, (int)b.mode());
}

void test_power_loss_save_from_yesterday_infers() {
    DayFacts saved = factsAt(T(0, 11, 59), FLOOR, T(0, 12, 0));
    Sim b;
    b.boot(T(1, 12, 0), ALL, 0, &saved);
    TEST_ASSERT_FALSE(b.logged("caught up"));
    TEST_ASSERT_TRUE(b.logged("taking over"));
    TEST_ASSERT_EQUAL_UINT8(ALL, b.d.wantMask());
}

void test_power_loss_long_gap_same_mira_day() {
    // 07:00 → 05:00 the next morning: crossedLocal() reads this gap as a DST step.
    DayFacts saved = factsAt(T(0, 6, 59), FLOOR, T(0, 7, 0));
    Sim b;
    b.boot(T(1, 5, 0), FLOOR, 0, &saved);
    TEST_ASSERT_TRUE(b.logged("caught up"));
    TEST_ASSERT_EQUAL((int)Director::Mode::Asleep, (int)b.mode());
    assertPut(b.find(0, LIGHT_FLOOR), false, 0, FADE_MS, 0);
}

void test_power_loss_all_off_infers_instead_of_replaying() {
    DayFacts saved = factsAt(T(0, 9, 59), FLOOR, T(0, 10, 0));
    Sim late;
    late.boot(T(0, 22, 0), 0, 0, &saved);
    TEST_ASSERT_EQUAL((int)Director::Mode::Asleep, (int)late.mode());
    TEST_ASSERT_FALSE(late.logged("caught up"));

    Sim day;  // before 21:00: idle, and a light on starts a late wake
    day.boot(T(0, 14, 0), 0, 0, &saved);
    day.runTo(T(0, 15, 0));
    TEST_ASSERT_EQUAL(0, day.count(0));
    day.userOn(LIGHT_FLOOR, 50.0f);
    TEST_ASSERT_EQUAL((int)Director::Mode::Wake, (int)day.mode());
}

void test_power_loss_mid_hold_ends_the_hold() {
    Sim a;
    a.boot(T(0, 14, 59), ALL);
    a.runTo(T(0, 15, 0));
    a.userBri(LIGHT_FLOOR, 50.0f);
    DayFacts saved = a.d.facts(a.now());
    Sim b;
    b.boot(T(0, 17, 30), ALL, 0, &saved);
    b.runTo(T(0, 17, 30, 2));
    TEST_ASSERT_EQUAL((int)Director::Mode::Active, (int)b.mode());
    assertPut(b.find(0, LIGHT_FLOOR), true, 75.0f, 1800000);
}

void test_power_loss_keeps_out_for_today() {
    Sim a;
    a.boot(T(0, 13, 59), FLOOR | DRESSER | CHEST);
    a.runTo(T(0, 14, 0));
    a.userOff(LIGHT_CHEST);
    a.runTo(T(0, 14, 0, 5));
    DayFacts saved = a.d.facts(a.now());
    Sim b;
    b.boot(T(0, 16, 0), FLOOR | DRESSER, 0, &saved);
    TEST_ASSERT_TRUE((b.d.outMask() & CHEST) != 0);
    b.runTo(T(0, 16, 0, 2));
    TEST_ASSERT_EQUAL(0, b.countFor(0, LIGHT_CHEST));
}

void test_soft_reset_wins_over_the_saved_facts() {
    Sim a;
    a.boot(T(0, 11, 59), ALL);
    a.runTo(T(0, 12, 0, 30));
    DirectorState rtc = a.d.state();
    DayFacts saved = a.d.facts(a.now());
    Sim b;
    b.boot(T(0, 12, 0, 40), ALL, &rtc, &saved);
    TEST_ASSERT_TRUE(b.logged("picked up where it left off"));
    TEST_ASSERT_FALSE(b.logged("caught up"));
}

void test_a_full_day_saves_only_when_facts_change() {
    Sim s;
    s.boot(T(0, 8, 59), 0);
    DayFacts last = s.d.facts(s.now());
    int saves = 0, savesDuringFadeStarts = 0;
    while (s.ms < T(1, 1, 0, 30)) {
        s.runTo(s.ms + 1000);
        DayFacts f = s.d.facts(s.now());
        if (Director::sameFacts(f, last)) continue;
        saves++;
        last = f;
        const int32_t sec = s.now().localSec;
        if (sec >= HHMM(17, 0) && sec < HHMM(17, 0) + 5) savesDuringFadeStarts++;
    }
    TEST_ASSERT_LESS_OR_EQUAL(15, saves);
    TEST_ASSERT_GREATER_THAN(4, saves);
    TEST_ASSERT_EQUAL(0, savesDuringFadeStarts);
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_ct_map_matches_2x);
    RUN_TEST(test_profile_levels);
    RUN_TEST(test_next_fade_start_is_the_hold_end);
    RUN_TEST(test_crossing_midnight_and_dst);
    RUN_TEST(test_echo_early_window_accepts_the_whole_range);
    RUN_TEST(test_echo_mid_fade_only_near_the_current_position);
    RUN_TEST(test_echo_off_fade_and_short_commands);
    RUN_TEST(test_full_day_auto_wake_one_command_per_change);
    RUN_TEST(test_manual_wake_with_chest_rides_along_in_order);
    RUN_TEST(test_sleep_window_leaves_lights_alone_then_6am_wakes);
    RUN_TEST(test_late_wake_after_cold_boot_fades_into_the_day);
    RUN_TEST(test_dashboard_wake_ignored_at_night);
    RUN_TEST(test_light_turned_on_mid_fade_snaps_in_line_then_follows);
    RUN_TEST(test_hold_in_steady_lasts_until_next_fade_period);
    RUN_TEST(test_hold_mid_fade_freezes_others_and_defers_events);
    RUN_TEST(test_resume_mid_fade_glides_for_the_time_left);
    RUN_TEST(test_resume_in_steady_fades_only_the_changed_light);
    RUN_TEST(test_hold_after_the_dresser_goes_off_ends_at_lights_out);
    RUN_TEST(test_hold_after_last_fade_ends_when_the_dresser_goes_off);
    RUN_TEST(test_late_report_on_the_wake_path_is_not_a_hold);
    RUN_TEST(test_report_off_the_path_is_still_a_hold);
    RUN_TEST(test_in_line_nudge_in_steady_goes_back_to_the_level);
    RUN_TEST(test_in_line_report_during_a_hold_changes_nothing);
    RUN_TEST(test_power_lost_is_out_for_today);
    RUN_TEST(test_power_back_fades_into_the_current_trajectory);
    RUN_TEST(test_power_lost_before_a_scheduled_on_stays_off);
    RUN_TEST(test_power_back_during_a_hold_waits_for_the_hold);
    RUN_TEST(test_power_back_at_night_is_left_alone_and_in_the_morning_starts_the_wake);
    RUN_TEST(test_everything_off_before_9pm_is_a_nap);
    RUN_TEST(test_light_on_during_nap_ends_it_and_brings_the_rest);
    RUN_TEST(test_nap_running_at_9pm_becomes_sleep);
    RUN_TEST(test_one_light_off_is_out_for_today);
    RUN_TEST(test_out_for_today_blocks_a_scheduled_join);
    RUN_TEST(test_everything_off_after_9pm_is_sleep_and_lights_are_left_alone);
    RUN_TEST(test_nap_remaining_zero_ends_it);
    RUN_TEST(test_nap_command_fades_off_and_returns_on_time);
    RUN_TEST(test_nap_command_custom_length);
    RUN_TEST(test_nap_command_replaces_a_hold);
    RUN_TEST(test_nap_command_refused_from_9pm_and_while_asleep_or_napping);
    RUN_TEST(test_nap_command_absorbs_an_off_you_just_made);
    RUN_TEST(test_nap_command_retries_an_off_that_failed);
    RUN_TEST(test_extend_adds_to_a_nap);
    RUN_TEST(test_extended_nap_still_becomes_asleep_at_9pm);
    RUN_TEST(test_extended_hold_stays_frozen_past_a_fade_then_catches_up);
    RUN_TEST(test_lights_out_still_ends_an_extended_hold);
    RUN_TEST(test_extend_with_nothing_to_extend_is_ignored);
    RUN_TEST(test_light_off_command_sends_the_off_and_is_out_for_today);
    RUN_TEST(test_light_off_command_on_the_last_light_is_not_a_nap);
    RUN_TEST(test_light_off_command_after_9pm_on_the_last_light_is_asleep);
    RUN_TEST(test_light_off_command_before_a_scheduled_on_keeps_it_off);
    RUN_TEST(test_light_on_command_brings_it_back_in_line_mid_fade);
    RUN_TEST(test_light_on_command_during_a_nap_ends_it);
    RUN_TEST(test_light_commands_during_a_hold_switch_only_that_light);
    RUN_TEST(test_light_commands_while_asleep);
    RUN_TEST(test_light_on_command_asleep_in_the_day_starts_the_wake);
    RUN_TEST(test_failed_command_retries_with_backoff);
    RUN_TEST(test_unreachable_bulb_backs_off_and_logs_once);
    RUN_TEST(test_stale_revert_resends);
    RUN_TEST(test_soft_reset_restores_without_switching_anything);
    RUN_TEST(test_soft_reset_catches_events_missed_while_down);
    RUN_TEST(test_power_loss_replays_the_evening_offs);
    RUN_TEST(test_power_loss_save_from_yesterday_infers);
    RUN_TEST(test_power_loss_long_gap_same_mira_day);
    RUN_TEST(test_power_loss_all_off_infers_instead_of_replaying);
    RUN_TEST(test_power_loss_mid_hold_ends_the_hold);
    RUN_TEST(test_power_loss_keeps_out_for_today);
    RUN_TEST(test_soft_reset_wins_over_the_saved_facts);
    RUN_TEST(test_a_full_day_saves_only_when_facts_change);
    return UNITY_END();
}
