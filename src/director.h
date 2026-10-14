#pragma once
//
// director.h — Mira 3.0's behavior engine (Markdowns/MIRA_3.md §1–§7).
// Pure logic: no Arduino, no network, no clock of its own. The shell (main.cpp)
// hands it the time and what the bridge saw; it answers by calling Env::put().
// Built on the host too, where test/test_core drives it through simulated days.
//
// The model in one paragraph: per light, a "want on" bit (you and the schedule
// both write it; the last writer wins) and an "out for today" bit (the schedule
// can't turn that light back on). The timeline gives the in-line level for right
// now. The only job is reconcile(): lights that want on are on, following the
// period in force — and a command goes out only when what's planned differs from
// what was last sent, so each fade is one command the bulbs carry out themselves.
// Three conditions pause that job — asleep, nap, hold — and while paused the
// schedule keeps writing "want on", so catching up afterwards is one reconcile.
//
#include <stdint.h>
#include "lights.h"

struct Now {
    int64_t ms;        // epoch milliseconds
    int32_t localSec;  // local seconds since midnight (DST-aware) — the same instant as ms
};

// What Mira last told one light: a trajectory the bulb is now carrying out.
struct Command {
    uint8_t  valid;   // 0: unknown — never sent, failed, reverted, or you changed the light
    uint8_t  on;
    uint8_t  join;    // a join step (fade-in or snap): align with the period once it lands
    uint8_t  pad;
    float    b0;      // estimated brightness when sent
    float    b1;      // target brightness
    int64_t  t0Ms;    // when sent
    uint32_t durMs;
};

// The remembered facts. Plain data, so the shell keeps a copy in RTC memory and a
// soft reset (panic / watchdog / wifi-watchdog) picks up exactly where it left off.
struct DirectorState {
    uint32_t  magic;
    uint8_t   asleep;
    uint8_t   wakeFlags;    // bit0: +5 min join done · bit1: +10 min join done · bit2: completion logged
    LightMask want;         // want on
    LightMask out;          // out for today
    float     wakeEndBri;   // where the wake fade lands (the profile level at its end)
    int64_t   wakeStartMs;  // 0: no wake since the last sleep
    int64_t   napUntilMs;   // 0: no nap
    int64_t   holdUntilMs;  // 0: no hold
    int64_t   lastTickMs;
    int32_t   lastLocalSec;
    Command   cmd[LIGHT_COUNT];
};

class Director {
public:
    // The seams to the outside world (real bridge on the board, fakes in tests).
    class Env {
    public:
        enum PutResult { Sent, Failed, Aborted };
        virtual ~Env() {}
        // One command to one light. Aborted: not sent at all (a hold is landing).
        // Failed: the bridge didn't take it, or we can't tell.
        virtual PutResult put(uint8_t idx, bool on, float bri, uint16_t ct, uint32_t durMs) = 0;
        // PRESET_OFF_LIGHTS only: dimming + CT to a light that's off, leaving it off.
        virtual void preset(uint8_t idx, float bri, uint16_t ct, uint32_t durMs) {
            (void)idx; (void)bri; (void)ct; (void)durMs;
        }
        virtual void log(const char* msg) = 0;
    };

    enum class Mode : uint8_t { Asleep, Wake, Active, Hold, Nap };
    enum class Cmd  : uint8_t { Resume, Hold, Wake, Sleep, NapRemaining };

    // 'M' 'I' · major 3 · DirectorState layout v1. Bump the last byte whenever
    // DirectorState changes, so a soft reset across a firmware update infers
    // from the bulbs instead of misreading the old layout.
    static const uint32_t STATE_MAGIC = 0x4D490301UL;

    Director();
    void begin(Env* env) { env_ = env; }
    // physOn/bri: the bridge's view at boot (bri[LIGHT_COUNT], percent). restored:
    // the RTC copy from before a soft reset, or nullptr (power-on → infer).
    void boot(const Now& now, LightMask physOn, const float* bri, const DirectorState* restored);

    void tick(const Now& now);  // ~1 Hz: timed events, timers, reconcile
    void onEdge(const Now& now, uint8_t idx, bool on, bool byUser, float bri);
    void onUserBrightness(const Now& now, int idx, float bri, uint16_t ct);  // idx −1: unknown light
    void onStaleRevert(const Now& now, uint8_t idx, bool nowOn);
    void onCommand(const Now& now, Cmd cmd, int32_t value);

    Mode      mode(const Now& now) const;
    float     inLineBri(const Now& now) const;        // 0 while nothing is driven
    int32_t   pauseRemainingS(const Now& now) const;  // nap or hold, else 0
    int       wakeStep(const Now& now) const;         // 0–30 (30 s steps) while waking
    LightMask outMask() const    { return s_.out; }
    LightMask wantMask() const   { return s_.want; }
    LightMask physOnMask() const { return physOn_; }
    const DirectorState& state() const { return s_; }
    bool      consumeDirty();     // true once after any change worth a status push
    static const char* modeName(Mode m);

private:
    struct Active {    // the period in force at one instant, in absolute time
        bool    driven;
        bool    fade;
        bool    wake;
        int64_t startMs;
        int64_t endMs;
        float   b0;
        float   b1;
    };

    int64_t mdsMsAt(const Now& now, int64_t atMs) const;
    int32_t mdsNow(const Now& now) const;
    Active  profileAt(const Now& now, int64_t atMs) const;
    Active  activeAt(const Now& now, int64_t atMs) const;
    static float levelOf(const Active& a, int64_t atMs);
    float   currentBri(const Now& now, int i) const;

    bool napActive() const  { return s_.napUntilMs != 0; }
    bool holdActive() const { return s_.holdUntilMs != 0; }
    bool paused() const     { return s_.asleep || napActive() || holdActive(); }

    void processEvents(const Now& now);
    void processTimers(const Now& now);
    void noteFadeStart(const Now& now);
    void reconcile(const Now& now);
    void reconcileLight(const Now& now, int i, const Active& p);
    void presetLight(const Now& now, int i, const Active& p);
    void send(const Now& now, int i, bool on, float bri, uint32_t durMs, bool join);
    void joinLight(const Now& now, int i, uint32_t durMs);
    void backoff(const Now& now, int i);

    void startWake(const Now& now, int triggerIdx, const char* why);
    void startHold(const Now& now, int changedIdx, float userBri, uint16_t userCt, const char* why);
    void endHold(const char* why);
    void startNap(const Now& now, LightMask group);
    void endNap(const char* why);
    void goToSleep(const char* why);
    void lightsOut(const Now& now, const char* at);
    void scheduleRole(Role role, bool on);
    void wakeJoin(const Now& now, Role role);
    void resolveOffGroup(const Now& now);
    void note(const char* fmt, ...);

    Env*          env_;
    DirectorState s_;
    LightMask     physOn_;
    LightMask     pendingOff_;           // your offs still inside the grouping window
    int64_t       pendingOffAtMs_;
    uint32_t      joinDurMs_[LIGHT_COUNT];  // 0: none; else the next reconcile joins with this duration
    float         knownBri_[LIGHT_COUNT];   // last brightness the bridge reported
    int64_t       retryAtMs_[LIGHT_COUNT];
    uint8_t       fails_[LIGHT_COUNT];
    float         presetB1_[LIGHT_COUNT];   // PRESET_OFF_LIGHTS bookkeeping
    int64_t       presetEndMs_[LIGHT_COUNT];
    int64_t       lastFadeLoggedMs_;
    bool          dirty_;
};
