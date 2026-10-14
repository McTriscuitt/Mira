//
// test_legs.cpp — built with MAX_FADE_MS = 30 min (env:native_legs): an hour-long
// fade must go out as legs that still arrive on schedule.
// Run: pio test -e native_legs
//
#include <unity.h>
#include <utility>
#include <vector>
#include "config.h"
#include "director.h"
#include "timeline.h"

namespace {

struct PutRec { int64_t t; uint8_t idx; bool on; float bri; uint32_t dur; };

// Records commands and echoes on/off changes back, like the real bridge.
struct FakeEnv : Director::Env {
    std::vector<PutRec>                    puts;
    std::vector<std::pair<uint8_t, bool> > echoes;
    bool    on[LIGHT_COUNT] = {false};
    int64_t nowMs = 0;
    PutResult put(uint8_t idx, bool o, float bri, uint16_t, uint32_t dur) {
        PutRec r = { nowMs, idx, o, bri, dur };
        puts.push_back(r);
        if (on[idx] != o) { on[idx] = o; echoes.push_back(std::make_pair(idx, o)); }
        return Sent;
    }
    void log(const char*) {}
};

const int64_t DAY0 = 20000LL * 86400000LL;

Now at(int64_t ms) {
    Now n = { ms, (int32_t)((ms / 1000) % 86400) };
    return n;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_hour_fade_goes_out_in_two_legs() {
    TEST_ASSERT_EQUAL_UINT32(1800000UL, MAX_FADE_MS);
    FakeEnv  env;
    Director d;
    d.begin(&env);
    int64_t ms = DAY0 + (int64_t)(9 * 3600 + 59 * 60) * 1000LL;  // 09:59, floor on at 75%
    float bri[LIGHT_COUNT] = {75.0f, 0.0f, 0.0f, 0.0f};
    env.on[LIGHT_FLOOR] = true;
    env.nowMs = ms;
    d.boot(at(ms), 1 << LIGHT_FLOOR, bri, 0);

    const int64_t TEN = DAY0 + 10LL * 3600 * 1000;
    while (ms < DAY0 + 11LL * 3600 * 1000) {
        ms += 1000;
        env.nowMs = ms;
        d.tick(at(ms));
        while (!env.echoes.empty()) {
            std::pair<uint8_t, bool> e = env.echoes.front();
            env.echoes.erase(env.echoes.begin());
            d.onEdge(at(ms), e.first, e.second, false, 75.0f);
        }
    }
    std::vector<PutRec> floor;
    for (size_t k = 0; k < env.puts.size(); k++)
        if (env.puts[k].idx == LIGHT_FLOOR && env.puts[k].t >= TEN) floor.push_back(env.puts[k]);

    // 10:00 → first leg to 87.5% over 30 min; 10:30 → the rest, to 100% by 11:00.
    TEST_ASSERT_EQUAL(2, (int)floor.size());
    TEST_ASSERT_FLOAT_WITHIN(0.3f, 87.5f, floor[0].bri);
    TEST_ASSERT_EQUAL_UINT32(1800000UL, floor[0].dur);
    TEST_ASSERT_FLOAT_WITHIN(0.3f, 100.0f, floor[1].bri);
    TEST_ASSERT_UINT32_WITHIN(2000, 1800000UL, floor[1].dur);
    TEST_ASSERT_TRUE(floor[1].t - floor[0].t >= 1800000LL);  // the second waited for the first to land
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_hour_fade_goes_out_in_two_legs);
    return UNITY_END();
}
