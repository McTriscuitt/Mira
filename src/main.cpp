//
// Project created by Tristan DeOrnellis on 4/13/2026.
// Name "Mira" designated on 4/22/2026.
// Mira 3.0 (October 2026): time-of-day lighting — see Markdowns/MIRA_3.md.
//
// main.cpp is the shell around the pure core. It owns the hardware and the
// clock, and wires three sources of events into the Director (director.cpp):
//   bridge.cpp    — the Core-0 SSE task: your on/off and brightness changes
//   dashboard.cpp — the Core-0 net task: dashboard commands (and status/log out)
//   the clock     — a 1 Hz tick for the timeline
// The Director answers by sending commands through BridgeEnv → bridgePut().
//
#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>
#include <sys/time.h>
#include <time.h>
#include <Adafruit_VEML7700.h>
#include "config.h"
#include "secrets.h"
#include "lights.h"
#include "timeline.h"
#include "director.h"
#include "events.h"
#include "bridge.h"
#include "dashboard.h"

QueueHandle_t evQueue = nullptr;

bool postEvent(const Event& ev) {
    return evQueue && xQueueSend(evQueue, &ev, 0) == pdTRUE;
}

static void sendLog(const char* msg);

// ── Onboard RGB (active LOW) ────────────────────────────────────────────────

static void setRGB(bool r, bool g, bool b) {
    digitalWrite(LEDR, r ? LOW : HIGH);
    digitalWrite(LEDG, g ? LOW : HIGH);
    digitalWrite(LEDB, b ? LOW : HIGH);
}

// ── Boot reason (Sept 12, 2026) ─────────────────────────────────────────────
// The Online log names why the board started — PANIC / TASK-WDT / BROWNOUT /
// power-on / sw-restart, plus the two restarts Mira does itself, which leave an
// RTC-memory marker (survives a soft reset, garbage after power-on).

RTC_NOINIT_ATTR static uint32_t bootMarker;
static const uint32_t BOOT_MARKER_WIFI_WDT = 0x57494644UL;  // "WIFD"
static const uint32_t BOOT_MARKER_NTP      = 0x4E545021UL;  // "NTP!"
static const char*    bootReason           = "unknown";

static const char* resetReasonStr() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_SW:        return "sw-restart";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT-WDT";
        case ESP_RST_TASK_WDT:  return "TASK-WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "other";
    }
}

static void captureBootReason() {
    bootReason = resetReasonStr();
    if (esp_reset_reason() == ESP_RST_SW) {
        if (bootMarker == BOOT_MARKER_WIFI_WDT) bootReason = "wifi-watchdog";
        if (bootMarker == BOOT_MARKER_NTP)      bootReason = "ntp-timeout";
    }
    bootMarker = 0;
    Serial.printf("Boot reason: %s\n", bootReason);
}

// Soft resets keep RTC memory, so Mira picks up the day exactly (MIRA_3.md §7).
static bool wasSoftReset() {
    switch (esp_reset_reason()) {
        case ESP_RST_SW:
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
            return true;
        default:
            return false;
    }
}

// ── WiFi + watchdog (Sept 12, 2026 — escalation ladder in config.h) ─────────

static void connectWiFi() {
    Serial.print("Connecting to WiFi");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        setRGB(true, false, false); delay(300);  // red blink = connecting
        setRGB(false, false, false); delay(300);
        Serial.print(".");
    }
    setRGB(false, true, false);  // green = connected
    Serial.printf("\nWiFi connected — IP: %s\n", WiFi.localIP().toString().c_str());
}

static uint32_t wifiLastCheckMs = 0;
static uint32_t wifiDownSinceMs = 0;
static uint32_t wifiLastKickMs  = 0;
static bool     wifiDown        = false;

static void wifiWatchdog() {
    if (millis() - wifiLastCheckMs < WIFI_CHECK_MS) return;
    wifiLastCheckMs = millis();

    if (WiFi.status() == WL_CONNECTED) {
        if (wifiDown) {
            wifiDown = false;
            char line[48];
            snprintf(line, sizeof(line), "WiFi reconnected after %lu s",
                     (unsigned long)((millis() - wifiDownSinceMs) / 1000UL));
            sendLog(line);
        }
        return;
    }
    if (!wifiDown) {
        wifiDown        = true;
        wifiDownSinceMs = millis();
        wifiLastKickMs  = millis();  // auto-reconnect gets the first WIFI_RETRY_MS uncontested
        Serial.println("WiFi: link down — watchdog armed");
        return;
    }
    if (millis() - wifiDownSinceMs >= WIFI_REBOOT_MS) {
        Serial.println("WiFi: down past the reboot threshold — restarting");
        bootMarker = BOOT_MARKER_WIFI_WDT;
        delay(100);
        ESP.restart();
    }
    if (millis() - wifiLastKickMs >= WIFI_RETRY_MS) {
        wifiLastKickMs = millis();
        Serial.println("WiFi: manual reconnect kick");
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
}

// ── Clock: SNTP + POSIX TZ (US Central, automatic DST) ──────────────────────

static bool clockValid() {
    return time(nullptr) > 1700000000L;  // anything before Nov 2023 is the unset epoch
}

static void syncClock() {
    configTzTime(MIRA_TZ, NTP_SERVER_1, NTP_SERVER_2);
    uint32_t t0 = millis();
    while (!clockValid()) {
        setRGB(false, false, true); delay(250);  // blue blink = waiting for the time
        setRGB(false, false, false); delay(250);
        if (millis() - t0 > NTP_SYNC_TIMEOUT_MS) {
            Serial.println("NTP: no sync — restarting");
            bootMarker = BOOT_MARKER_NTP;
            delay(100);
            ESP.restart();
        }
    }
    setRGB(false, false, true);  // solid blue = time synced
    Serial.println("Clock synced.");
}

static Now nowLocal() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    time_t secs = tv.tv_sec;
    struct tm lt;
    localtime_r(&secs, &lt);
    Now n;
    n.ms       = (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
    n.localSec = lt.tm_hour * 3600 + lt.tm_min * 60 + (lt.tm_sec > 59 ? 59 : lt.tm_sec);
    return n;
}

static String timeString() {
    static const char* const DAYS[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    time_t secs = time(nullptr);
    struct tm lt;
    localtime_r(&secs, &lt);
    char clock[12];
    fmtClock(lt.tm_hour * 3600 + lt.tm_min * 60, clock, sizeof(clock));
    return String(clock) + " " + DAYS[lt.tm_wday % 7];
}

// ── Logging: Serial + the dashboard event log ───────────────────────────────

static void sendLog(const char* msg) {
    char line[120];
    snprintf(line, sizeof(line), "%s — %s", msg, timeString().c_str());
    Serial.println(line);
    dashLog(line);
}

// ── The Director and its seams ──────────────────────────────────────────────

class BridgeEnv : public Director::Env {
public:
    PutResult put(uint8_t idx, bool on, float bri, uint16_t ct, uint32_t durMs) override {
        BridgeResult r = bridgePut(idx, on, bri, ct, durMs);
        if (r == BridgeResult::Sent)    return Sent;
        if (r == BridgeResult::Aborted) return Aborted;
        return Failed;
    }
    void preset(uint8_t idx, float bri, uint16_t ct, uint32_t durMs) override {
        bridgePreset(idx, bri, ct, durMs);
    }
    void log(const char* msg) override { sendLog(msg); }
};

static BridgeEnv env;
static Director  director;

// ── RTC persistence: a soft reset picks up the day where it left off ────────

struct RtcBlob {
    DirectorState state;
    uint32_t      check;
};
RTC_NOINIT_ATTR static RtcBlob rtcBlob;

static uint32_t stateCheck(const DirectorState& s) {
    uint32_t h = 2166136261UL ^ (uint32_t)sizeof(DirectorState);  // FNV-1a, seeded with the layout size
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&s);
    for (size_t i = 0; i < sizeof(s); i++) { h ^= p[i]; h *= 16777619UL; }
    return h;
}

static void persist() {
    rtcBlob.state = director.state();
    rtcBlob.check = stateCheck(rtcBlob.state);
}

static const DirectorState* rtcRestore(bool softReset) {
    if (!softReset) return nullptr;
    if (rtcBlob.check != stateCheck(rtcBlob.state)) return nullptr;
    if (rtcBlob.state.magic != Director::STATE_MAGIC) return nullptr;
    return &rtcBlob.state;  // the Director also checks its age
}

// Commands still running when the board reset: re-record them so the bridge's
// progress reports mid-fade (if it sends any) read as Mira's, not yours.
static void seedEchoRing(const Now& n) {
    const DirectorState& s = director.state();
    for (int i = 0; i < LIGHT_COUNT; i++) {
        const Command& c = s.cmd[i];
        if (!c.valid || !c.on) continue;
        int64_t elapsed = n.ms - c.t0Ms;
        if (elapsed < 0 || elapsed >= (int64_t)c.durMs + (int64_t)RECENT_PUT_GRACE_MS) continue;
        bridgeSeedTrajectory(i, c.b0, c.b1, (uint32_t)elapsed, c.durMs);
    }
}

// ── Status (dashboard) + Serial status line ─────────────────────────────────

static Adafruit_VEML7700 veml;
static bool  vemlOk  = false;
static float lastLux = -1.0f;  // reporting only — nothing in 3.0 reacts to it

// The current dashboard predates 3.0 and renders the 2.x state names (interim,
// MIRA_3.md §8); the 3.0 name rides along in "mode" for its rework.
static const char* legacyStateName(Director::Mode m) {
    switch (m) {
        case Director::Mode::Asleep: return "LOCKED_OUT";
        case Director::Mode::Wake:   return "WAKE";
        case Director::Mode::Hold:   return "SOFT_PAUSE";
        case Director::Mode::Nap:    return "SOFT_PAUSE";
        default:                     return "NORMAL";
    }
}

static void pushStatus() {
    Now n = nowLocal();
    StatusSnapshot s;
    memset(&s, 0, sizeof(s));
    Director::Mode m = director.mode(n);
    strlcpy(s.state, legacyStateName(m), sizeof(s.state));
    strlcpy(s.mode, Director::modeName(m), sizeof(s.mode));
    s.lux             = lastLux;
    s.bri             = director.inLineBri(n);
    s.ct              = s.bri > 0.0f ? ctForBri(s.bri) : 0;
    s.on              = bridgeOnMask();
    s.ceilingOn       = (s.on & lightBit(LIGHT_CEIL_1)) != 0;
    s.wakeStep        = director.wakeStep(n);
    s.wakeTotal       = WAKE_FADE_S / 30;
    s.pauseRemainingS = director.pauseRemainingS(n);
    s.out             = director.outMask();
    s.heapFree        = esp_get_free_heap_size();
    s.sseStackFree    = bridgeSseStackFree();
    dashStatus(s);
}

static void maskStr(LightMask m, char* buf) {  // "FDC-": Floor Dresser Chest Ceiling
    static const char LETTERS[] = "FDCX";
    for (int i = 0; i < LIGHT_COUNT; i++) buf[i] = (m & lightBit(i)) ? LETTERS[i % 4] : '-';
    buf[LIGHT_COUNT] = '\0';
}

static void printStatus() {
    Now n = nowLocal();
    char on[LIGHT_COUNT + 1], want[LIGHT_COUNT + 1], out[LIGHT_COUNT + 1];
    maskStr(bridgeOnMask(), on);
    maskStr(director.wantMask(), want);
    maskStr(director.outMask(), out);
    Serial.printf("[%s] %-6s in-line %5.1f%%  on=%s want=%s out=%s  lux=%.1f  heap=%u sse=%u\n",
                  timeString().c_str(), Director::modeName(director.mode(n)), director.inLineBri(n),
                  on, want, out, lastLux, (unsigned)esp_get_free_heap_size(), (unsigned)bridgeSseStackFree());
}

// Every TELEMETRY_TICK_S on the wall clock (:00/:30): read lux, print, push status.
static void serviceTelemetry() {
    static time_t next = 0;
    time_t t = time(nullptr);
    if (t < next) return;
    next = (t / TELEMETRY_TICK_S + 1) * TELEMETRY_TICK_S;
    if (vemlOk) lastLux = veml.readLux(VEML_LUX_AUTO);
    printStatus();
    pushStatus();
}

// ── Event dispatch + the loop ───────────────────────────────────────────────

static void dispatch(const Event& ev) {
    Now n = nowLocal();
    switch (ev.type) {
        case EvType::LightEdge:
            director.onEdge(n, ev.a, (ev.flags & 1) != 0, (ev.flags & 2) != 0, ev.f);
            break;
        case EvType::UserBrightness:
            holdPending = false;  // the hold is being applied — its freeze commands may go out
            bridgeDumpDiagnostics(ev.a);
            director.onUserBrightness(n, ev.a, ev.f, (uint16_t)ev.i);
            break;
        case EvType::StaleRevert:
            director.onStaleRevert(n, ev.a, (ev.flags & 1) != 0);
            break;
        case EvType::DashCmd:
            director.onCommand(n, (Director::Cmd)ev.a, ev.i);
            break;
    }
}

// Belt-and-braces: if the SSE task flagged a brightness change but its event
// never made it into a full queue, apply the hold anyway — otherwise the flag
// would block every command forever.
static void serviceHoldPending() {
    static uint32_t sinceMs = 0;
    if (!holdPending) { sinceMs = 0; return; }
    if (sinceMs == 0) { sinceMs = millis(); return; }
    if (millis() - sinceMs > 1000UL) {
        holdPending = false;
        sinceMs     = 0;
        director.onUserBrightness(nowLocal(), -1, 0.0f, 0);
    }
}

static void serviceDirector() {
    static uint32_t lastMs = 0;
    if (millis() - lastMs < 1000UL) return;
    lastMs = millis();
    director.tick(nowLocal());
}

void setup() {
    Serial.begin(115200);
    captureBootReason();
    pinMode(LEDR, OUTPUT);
    pinMode(LEDG, OUTPUT);
    pinMode(LEDB, OUTPUT);
    setRGB(false, false, false);

    connectWiFi();
    syncClock();

    evQueue = xQueueCreate(32, sizeof(Event));
    dashboardStart();  // early, so every boot log line rides the queue
    char line[64];
    snprintf(line, sizeof(line), "Online (%s) — Mira 3.0", bootReason);
    sendLog(line);

    vemlOk = veml.begin();
    if (!vemlOk) sendLog("Lux sensor not found — lux reporting off");  // no longer fatal

    bridgeBegin();  // cert pin + seed the light cache

    const bool soft = wasSoftReset();
    Now n = nowLocal();
    // The boot flourish only on a real power-up during the day — a 3 AM crash
    // shouldn't light up the chest.
    if (!soft && toMds(n.localSec) < toMds(SLEEP_CUTOFF)) bridgeFlourish(LIGHT_CHEST);

    float bri[LIGHT_COUNT];
    for (int i = 0; i < LIGHT_COUNT; i++) {
        LightSnapshot c = bridgeCached(i);
        bri[i] = c.on ? c.bri : 0.0f;
    }
    director.begin(&env);
    const DirectorState* restored = rtcRestore(soft);
    n = nowLocal();
    director.boot(n, bridgeOnMask(), bri, restored);
    if (restored) seedEchoRing(n);

    bridgeStart();  // the event stream + SSE task; from here your changes arrive as events
    persist();
    pushStatus();
    setRGB(false, false, false);  // boot complete
}

void loop() {
    Event ev;
    if (xQueueReceive(evQueue, &ev, pdMS_TO_TICKS(50)) == pdTRUE) dispatch(ev);
    serviceHoldPending();
    serviceDirector();
    serviceTelemetry();
    if (director.consumeDirty()) pushStatus();
    persist();  // RTC copy — a few hundred bytes, every pass
    wifiWatchdog();
}
