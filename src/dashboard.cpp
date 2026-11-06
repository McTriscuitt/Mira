//
// dashboard.cpp — the Railway side (see dashboard.h). Main task ↔ netTask traffic
// is fixed-size PODs over FreeRTOS queues: logQueue drops the oldest line when
// full (the main task never blocks), statusQueue is depth 1 with overwrite (a
// stale snapshot is worthless once a newer one exists). Commands go the other
// way as DashCmd events; applying them stays on the main task.
//
// The schedule (timeline.cpp + config.h) is posted once at boot, retried until the
// site takes it, and again when the dashboard sends SEND_SCHEDULE (`schedule update`).
// The site shows the board's copy over its own, so the firmware is the source of truth.
//
#include "dashboard.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "config.h"
#include "secrets.h"
#include "director.h"
#include "events.h"
#include "timeline.h"

struct LogMsg { char text[120]; };

static QueueHandle_t logQueue      = nullptr;
static QueueHandle_t statusQueue   = nullptr;
static TaskHandle_t  netTaskHandle = nullptr;

void dashLog(const char* line) {
    if (!logQueue) return;
    LogMsg m;
    strlcpy(m.text, line, sizeof(m.text));
    if (xQueueSend(logQueue, &m, 0) != pdTRUE) {  // full: drop the oldest, keep the newest
        LogMsg dropped;
        xQueueReceive(logQueue, &dropped, 0);
        xQueueSend(logQueue, &m, 0);
    }
}

void dashStatus(const StatusSnapshot& s) {
    if (statusQueue) xQueueOverwrite(statusQueue, &s);
}

static void postLog(const char* text) {
    HTTPClient http;
    http.begin(String(DASHBOARD_BASE_URL) + "/api/log");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " ESP32_API_KEY);
    JsonDocument doc;
    doc["message"] = text;
    String body;
    serializeJson(doc, body);
    int code = http.POST(body);
    if (code != 200) Serial.printf("Dashboard log POST → HTTP %d\n", code);
    http.end();
}

static void postStatus(const StatusSnapshot& s) {
    HTTPClient http;
    http.begin(String(DASHBOARD_BASE_URL) + "/api/status");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " ESP32_API_KEY);
    JsonDocument doc;
    doc["state"]                  = (const char*)s.state;
    doc["mode"]                   = (const char*)s.mode;
    if (s.lux >= 0.0f) doc["lux"] = s.lux;
    doc["bri"]                    = s.bri;
    doc["ct"]                     = s.ct;
    doc["overhead_on"]            = s.ceilingOn;
    doc["wake_step"]              = s.wakeStep;
    doc["wake_total"]             = s.wakeTotal;
    doc["soft_pause_remaining_s"] = s.pauseRemainingS;
    // Retired 2.x fields the current dashboard still reads — sent as zero.
    doc["wind_down_step"]   = 0;
    doc["stable_lux_count"] = 0;
    // "Out for today" in the 2.x exclusion chips (interim until the dashboard rework).
    JsonObject excl = doc["excluded"].to<JsonObject>();
    excl["floor"]   = (s.out & lightBit(LIGHT_FLOOR)) != 0;
    excl["chest"]   = (s.out & lightBit(LIGHT_CHEST)) != 0;
    excl["dresser"] = (s.out & lightBit(LIGHT_DRESSER)) != 0;
    excl["ceiling"] = (s.out & lightBit(LIGHT_CEIL_1)) != 0;
    JsonObject on = doc["lights_on"].to<JsonObject>();
    on["floor"]   = (s.on & lightBit(LIGHT_FLOOR)) != 0;
    on["chest"]   = (s.on & lightBit(LIGHT_CHEST)) != 0;
    on["dresser"] = (s.on & lightBit(LIGHT_DRESSER)) != 0;
    on["ceiling"] = (s.on & lightBit(LIGHT_CEIL_1)) != 0;
    doc["heap_free"]      = s.heapFree;
    doc["sse_stack_free"] = s.sseStackFree;
    doc["net_stack_free"] = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
    String body;
    serializeJson(doc, body);
    int code = http.POST(body);
    if (code != 200) Serial.printf("Dashboard status POST → HTTP %d\n", code);
    http.end();
}

// ── Schedule ────────────────────────────────────────────────────────────────
// Both flags live on netTask only (pollCommands sets scheduleAsked), so no locking.
static bool     scheduleBootSent  = false;
static bool     scheduleAsked     = false;  // SEND_SCHEDULE: send now, and log the result
static bool     scheduleTried     = false;
static uint32_t scheduleLastTryMs = 0;

static const char* roleWire(Role r) {
    switch (r) {
        case Role::Floor:   return "floor";
        case Role::Dresser: return "dresser";
        case Role::Chest:   return "chest";
        case Role::Ceiling: return "ceiling";
    }
    return "floor";
}

static const char* eventWire(EventKind k) {
    switch (k) {
        case EventKind::WakeWindow:  return "wake_window";
        case EventKind::AutoWake:    return "auto_wake";
        case EventKind::LightOn:     return "light_on";
        case EventKind::LightOff:    return "light_off";
        case EventKind::SleepCutoff: return "sleep_cutoff";
        case EventKind::LightsOut:   return "lights_out";
    }
    return "wake_window";
}

// Times are local seconds since midnight; the site's schedule.py checks every field.
static int postSchedule() {
    JsonDocument doc;
    JsonArray periods = doc["periods"].to<JsonArray>();
    for (int i = 0; i < periodCount(); i++) {
        Period p = periodByIndex(i);
        JsonObject o = periods.add<JsonObject>();
        o["start"] = mdsToLocal(p.startMds);
        o["end"]   = mdsToLocal(p.endMds);
        o["from"]  = p.b0;
        o["to"]    = p.b1;
    }
    JsonArray events = doc["events"].to<JsonArray>();
    for (int i = 0; i < TIMELINE_EVENT_COUNT; i++) {
        const TimelineEvent& e = TIMELINE_EVENTS[i];
        JsonObject o = events.add<JsonObject>();
        o["at"]   = e.localSec;
        o["kind"] = eventWire(e.kind);
        if (e.kind == EventKind::LightOn || e.kind == EventKind::LightOff) o["light"] = roleWire(e.role);
    }
    JsonObject wake = doc["wake"].to<JsonObject>();
    wake["from"]   = WAKE_START_BRI;
    wake["to"]     = MORNING_BRI;
    wake["fade_s"] = WAKE_FADE_S;
    JsonObject joins = wake["joins"].to<JsonObject>();
    joins["dresser"] = WAKE_JOIN_DRESSER_S;
    joins["chest"]   = WAKE_JOIN_CHEST_S;
    JsonObject ct = doc["ct"].to<JsonObject>();
    ct["cool"]     = CT_COOL;
    ct["warm"]     = CT_WARM;
    ct["warm_bri"] = CT_WARM_BRI;
    doc["nap_s"] = (uint32_t)(NAP_MS / 1000UL);

    HTTPClient http;
    http.begin(String(DASHBOARD_BASE_URL) + "/api/schedule");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " ESP32_API_KEY);
    String body;
    serializeJson(doc, body);
    int code = http.POST(body);
    http.end();
    return code;
}

// At boot until the site takes it (every SCHEDULE_RETRY_MS), and right away when asked.
static void maybePostSchedule() {
    if (scheduleBootSent && !scheduleAsked) return;
    if (scheduleTried && !scheduleAsked && millis() - scheduleLastTryMs < SCHEDULE_RETRY_MS) return;
    scheduleTried     = true;
    scheduleLastTryMs = millis();
    int code = postSchedule();
    if (code == 200) {
        scheduleBootSent = true;
        Serial.println("Schedule sent to the dashboard.");
        if (scheduleAsked) dashLog("Schedule sent — dashboard");
    } else {
        Serial.printf("Dashboard schedule POST → HTTP %d\n", code);
        if (scheduleAsked) {
            char line[64];
            snprintf(line, sizeof(line), "Schedule not sent — HTTP %d", code);
            dashLog(line);
        }
    }
    scheduleAsked = false;
}

// Wire name → 3.0 command. The 2.x names stay accepted so the current dashboard
// works until its rework; the new names let that rework switch over without a
// reflash. Retired commands are acked (so they don't clog the queue) and logged.
// SEND_SCHEDULE never reaches the Director: netTask answers it itself.
#define CMD_NET_SCHEDULE -3
struct CmdName { const char* name; int cmd; };  // cmd −1: retired in 3.0
static const CmdName CMD_NAMES[] = {
    {"NORMAL",                   (int)Director::Cmd::Resume},
    {"RESUME",                   (int)Director::Cmd::Resume},
    {"SOFT_PAUSE",               (int)Director::Cmd::Hold},
    {"HOLD",                     (int)Director::Cmd::Hold},
    {"WAKE",                     (int)Director::Cmd::Wake},
    {"LOCKED_OUT",               (int)Director::Cmd::Sleep},
    {"SLEEP",                    (int)Director::Cmd::Sleep},
    {"SET_SOFT_PAUSE_REMAINING", (int)Director::Cmd::NapRemaining},
    {"NAP_REMAINING",            (int)Director::Cmd::NapRemaining},
    {"NAP",                      (int)Director::Cmd::Nap},     // value: minutes, or none for the default
    {"EXTEND",                   (int)Director::Cmd::Extend},  // value: minutes added to the hold or nap
    {"LIGHT_ON",                 (int)Director::Cmd::LightOn},   // value: roster index (lights.h)
    {"LIGHT_OFF",                (int)Director::Cmd::LightOff},  // value: roster index (lights.h)
    {"SEND_SCHEDULE",            CMD_NET_SCHEDULE},              // post the schedule again (netTask)
    {"HARD_OFF",                 -1},
    {"WIND_DOWN",                -1},
    {"SET_WIND_DOWN_STEP",       -1},
    {"SET_WAKE_STEP",            -1},
    {"SET_STABLE_LUX_COUNT",     -1},
    {"EXCLUDE_LIGHT",            -1},
    {"INCLUDE_LIGHT",            -1},
};

// GET /api/command → DashCmd event → ack. Up to 8 per poll so a flooded queue
// can't wedge the task; ack-after-enqueue keeps at-least-once delivery (a full
// event queue skips the ack, so the command is fetched again next poll).
static void pollCommands() {
    for (int n = 0; n < 8; n++) {
        HTTPClient http;
        http.begin(String(DASHBOARD_BASE_URL) + "/api/command");
        http.addHeader("Authorization", "Bearer " ESP32_API_KEY);
        if (http.GET() != 200) { http.end(); return; }
        JsonDocument doc;
        deserializeJson(doc, http.getString());
        http.end();
        if (doc["command"].isNull()) return;

        const char* name  = doc["command"] | "";
        int         id    = doc["id"] | -1;
        int         value = doc["value"] | -1;
        int         cmd   = -2;  // −2: unknown
        for (size_t k = 0; k < sizeof(CMD_NAMES) / sizeof(CMD_NAMES[0]); k++) {
            if (strcmp(name, CMD_NAMES[k].name) == 0) { cmd = CMD_NAMES[k].cmd; break; }
        }
        if (cmd == CMD_NET_SCHEDULE) {
            Serial.println("Dashboard command: SEND_SCHEDULE");
            scheduleAsked = true;
        } else if (cmd >= 0) {
            Serial.printf("Dashboard command: %s%s\n", name, value >= 0 ? " (with value)" : "");
            Event ev;
            ev.type = EvType::DashCmd;
            ev.a    = (uint8_t)cmd;
            ev.i    = value;
            ev.tMs  = millis();
            if (!postEvent(ev)) return;  // full → no ack, retried next poll
        } else {
            char line[96];
            snprintf(line, sizeof(line), "Dashboard command %s %s — ignored", name,
                     cmd == -1 ? "is retired" : "is unknown");
            Serial.println(line);
            dashLog(line);
        }
        if (id < 0) return;  // can't ack it; polling again would refetch it forever
        HTTPClient ack;
        ack.begin(String(DASHBOARD_BASE_URL) + "/api/command/" + String(id) + "/ack");
        ack.addHeader("Authorization", "Bearer " ESP32_API_KEY);
        ack.POST("");
        ack.end();
    }
}

static void netTask(void*) {
    uint32_t lastPollMs = millis() - NET_CMD_POLL_MS;  // first poll right away
    for (;;) {
        if (WiFi.status() == WL_CONNECTED) {
            LogMsg m;
            while (xQueueReceive(logQueue, &m, 0) == pdTRUE) postLog(m.text);
            StatusSnapshot s;
            if (xQueueReceive(statusQueue, &s, 0) == pdTRUE) postStatus(s);
            if (millis() - lastPollMs >= NET_CMD_POLL_MS) {
                lastPollMs = millis();
                pollCommands();
            }
            maybePostSchedule();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void dashboardStart() {
    logQueue    = xQueueCreate(12, sizeof(LogMsg));
    statusQueue = xQueueCreate(1, sizeof(StatusSnapshot));
    xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, &netTaskHandle, 0);
    Serial.println("Net task started on core 0.");
}
