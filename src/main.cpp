//
// Project created by Tristan DeOrnellis on 4/13/2026.
// Name "Mira" designated on 4/22/2026
//

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUDP.h>
#include <Preferences.h>
#include <Adafruit_VEML7700.h>
#include "config.h"
#include "lightcurve.h"

struct LightState {
    bool on;
    int  bri;
    int  ct;
};

// Per-weekday day types: 0=WORK, 1=RELAXED. Index 0=Sunday, 6=Saturday
int DAY_TYPES[7] = {1, 1, 0, 1, 0, 1, 0};

Adafruit_VEML7700 veml;
Preferences prefs;
LightTarget sentTarget = {255, 0}; // sentinel: forces first update to always send (255/0 are outside valid ranges)
LightTarget prevSentTarget = {255, 0}; // sentTarget before the most recent PUT — lets checkOverride ignore slow-applying bulbs
bool overheadsOn = false;

struct ButtonState {
    bool debounced       = false;
    bool held            = false;
    bool shortPress      = false;
    bool longPress       = false;
    bool longFired       = false;
    unsigned long lastDebounceMs = 0;
    unsigned long pressStartMs   = 0;
};

ButtonState btnMode;
ButtonState btnCycle;

float lastLux = 1.0f; // last lux reading — used by cycle button to seed triggerWake() from the wait loop

enum class State { LOCKED_OUT, NORMAL, WAKE, WIND_DOWN, SOFT_PAUSE, HARD_OFF };
State state = State::LOCKED_OUT;

// Wind-down state
int   stableLuxCount  = 0;      // weighted counter: increments when lux in [2, 8] after 9 PM, decrements otherwise (floor 0)
int   windDownStep    = 0;      // 0–120 (120 steps × 30 s = 60 min)
float windDownStartBri = 0.0f;  // bri at the moment wind-down triggered

// Wake sequence state
int         wakeStep        = 0;
LightTarget wakeStartTarget = {0, 0};
LightTarget wakeEndTarget   = {0, 0};

// Soft pause state
unsigned long softPauseStart = 0;
bool skipOverrideCheck = false;

// Soft pause resume ramp
bool        pauseResumeActive      = false;
int         pauseResumeStep        = 0;
LightTarget pauseResumeStartTarget = {0, 0}; // skip first checkOverride after wake — lights still mid-transition

// Bedside edge detection
bool lastBedsideOn = false;  // previous bedside poll — detects manual on/off flips

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", UTC_OFFSET_SEC);



void setRGB(bool r, bool g, bool b) {
    // Nano ESP32-S3 onboard RGB LED — active LOW (LOW = on, HIGH = off)
    digitalWrite(LEDR, r ? LOW : HIGH);
    digitalWrite(LEDG, g ? LOW : HIGH);
    digitalWrite(LEDB, b ? LOW : HIGH);
}

void connectWiFi() {
    Serial.print("Connecting to WiFi");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        setRGB(true, false, false); delay(300); // red blink = connecting
        setRGB(false, false, false); delay(300);
        Serial.print(".");
    }
    setRGB(false, true, false); // solid green = connected
    Serial.println("\nWiFi connected — IP: " + WiFi.localIP().toString());
    delay(800);
}

LightState getLightState(int id) {
    HTTPClient http;
    String url = String(HUE_BASE_URL) + "/lights/" + id;
    http.begin(url);
    LightState result = {false, 0, 0};
    if (http.GET() == 200) {
        JsonDocument doc;
        deserializeJson(doc, http.getString());
        result.on  = doc["state"]["on"].as<bool>();
        result.bri = doc["state"]["bri"].as<int>();
        result.ct  = doc["state"]["ct"].as<int>();
    }
    http.end();
    return result;
}

void setLightColor(int id, bool on, int bri, int hue, int sat, int transitiontime) {
    HTTPClient http;
    String url = String(HUE_BASE_URL) + "/lights/" + id + "/state";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    String body = "{\"on\":" + String(on ? "true" : "false") +
                  ", \"bri\": " + String(bri) +
                  ", \"hue\": " + String(hue) +
                  ", \"sat\": " + String(sat) +
                  ", \"transitiontime\": " + String(transitiontime) + "}";
    int code = http.PUT(body);
    Serial.println("setLightColor(" + String(id) + ") → HTTP " + code);
    http.end();
}

void setLight(int id, bool on, int bri, int ct, int transitiontime) {
    HTTPClient http;
    String url = String(HUE_BASE_URL) + "/lights/" + id + "/state";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    String body = "{\"on\":" + String(on ? "true" : "false") +
                ", \"bri\": " + String(bri) +
                ", \"ct\": " + String(ct) +
                ", \"transitiontime\": " + String(transitiontime) + "}";
    int code = http.PUT(body);
    Serial.println("setLight(" + String(id) + ") → HTTP " + code);

    http.end();
}

String getTimeString() {
    const char* days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    int day = timeClient.getDay();
    int h   = timeClient.getHours();
    int m   = timeClient.getMinutes();
    int hour12 = h % 12;
    if (hour12 == 0) hour12 = 12;
    const char* ampm = h < 12 ? "AM" : "PM";
    String t = String(hour12) + ":" + (m < 10 ? "0" : "") + String(m) + ampm;
    return t + " " + days[day];
}

const char* stateName() {
    switch (state) {
        case State::LOCKED_OUT: return "LOCKED_OUT";
        case State::NORMAL:     return "NORMAL";
        case State::WAKE:       return "WAKE";
        case State::WIND_DOWN:  return "WIND_DOWN";
        case State::SOFT_PAUSE: return "SOFT_PAUSE";
        case State::HARD_OFF:   return "HARD_OFF";
        default:                return "UNKNOWN";
    }
}

void printStatus() {
    int day = timeClient.getDay();
    int h   = timeClient.getHours();
    int m   = timeClient.getMinutes();
    int s   = timeClient.getSeconds();
    int hour12 = h % 12;
    if (hour12 == 0) hour12 = 12;
    const char* ampm = h < 12 ? "AM" : "PM";
    const char* days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    const char* type = DAY_TYPES[day] == 0 ? "WORK" : "RELAXED";

    Serial.print("[");
    Serial.print(hour12); Serial.print(":");
    if (m < 10) Serial.print("0"); Serial.print(m); Serial.print(":");
    if (s < 10) Serial.print("0"); Serial.print(s);
    Serial.print(ampm); Serial.print("] ");
    Serial.print(days[day]); Serial.print(" — "); Serial.print(type);
    Serial.print(" ["); Serial.print(stateName()); Serial.println("]");
}

void sendDiscord(const String& message) {
    HTTPClient http;
    http.begin(DISCORD_WEBHOOK_URL);
    http.addHeader("Content-Type", "application/json");
    JsonDocument doc;
    doc["content"] = message;
    String body;
    serializeJson(doc, body);
    int code = http.POST(body);
    if (code != 204) Serial.println("Discord POST → HTTP " + String(code));
    http.end();
}

void sendDashboardLog(const String& message) {
    HTTPClient http;
    http.begin(String(DASHBOARD_BASE_URL) + "/api/log");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + String(ESP32_API_KEY));
    JsonDocument doc;
    doc["message"] = message;
    String body;
    serializeJson(doc, body);
    int code = http.POST(body);
    if (code != 200) Serial.println("Dashboard log POST → HTTP " + String(code));
    http.end();
}

void sendLog(const String& message) {
    sendDiscord(message);
    sendDashboardLog(message);
}

void sendDashboardStatus(float lux) {
    HTTPClient http;
    http.begin(String(DASHBOARD_BASE_URL) + "/api/status");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + String(ESP32_API_KEY));
    JsonDocument doc;
    doc["state"]       = stateName();
    doc["lux"]         = lux;
    doc["bri"]         = sentTarget.bri;
    doc["ct"]          = sentTarget.ct;
    doc["overhead_on"] = overheadsOn;
    doc["wind_down_step"] = windDownStep;
    doc["wake_step"]      = wakeStep;
    int totalWakeTicks = DAY_TYPES[timeClient.getDay()] == 0 ? WAKE_RAMP_TICKS_WORK : WAKE_RAMP_TICKS_RELAXED;
    doc["wake_total"]     = totalWakeTicks;
    long pauseRemaining = (state == State::SOFT_PAUSE) ?
        max(0L, ((long)SOFT_PAUSE_MS - (long)(millis() - softPauseStart)) / 1000L) : 0L;
    doc["soft_pause_remaining_s"] = pauseRemaining;
    String body;
    serializeJson(doc, body);
    int code = http.POST(body);
    if (code != 200) Serial.println("Dashboard status POST → HTTP " + String(code));
    http.end();
}

void forceState(State next); // defined later — forward declaration for pollDashboardCommand

void pollDashboardCommand() {
    HTTPClient http;
    http.begin(String(DASHBOARD_BASE_URL) + "/api/command");
    http.addHeader("Authorization", "Bearer " + String(ESP32_API_KEY));
    if (http.GET() != 200) { http.end(); return; }
    JsonDocument doc;
    deserializeJson(doc, http.getString());
    http.end();

    if (doc["command"].isNull()) return;
    String cmd    = doc["command"].as<String>();
    int cmdId     = doc["id"]    | -1;
    int cmdValue  = doc["value"] | -1;

    Serial.println("Dashboard command: " + cmd + (cmdValue >= 0 ? " value=" + String(cmdValue) : ""));
    if      (cmd == "NORMAL")     forceState(State::NORMAL);
    else if (cmd == "SOFT_PAUSE") forceState(State::SOFT_PAUSE);
    else if (cmd == "WIND_DOWN")  forceState(State::WIND_DOWN);
    else if (cmd == "WAKE")       forceState(State::WAKE);
    else if (cmd == "HARD_OFF")   forceState(State::HARD_OFF);
    else if (cmd == "LOCKED_OUT") forceState(State::LOCKED_OUT);
    else if (cmd == "SET_WIND_DOWN_STEP" && state == State::WIND_DOWN && cmdValue >= 0) {
        windDownStep = min(cmdValue, 119);
        Serial.println("Wind-down seek → step " + String(windDownStep));
    }
    else if (cmd == "SET_WAKE_STEP" && state == State::WAKE && cmdValue >= 0) {
        wakeStep = cmdValue;
        Serial.println("Wake seek → step " + String(wakeStep));
    }

    if (cmdId >= 0) {
        HTTPClient ack;
        ack.begin(String(DASHBOARD_BASE_URL) + "/api/command/" + String(cmdId) + "/ack");
        ack.addHeader("Authorization", "Bearer " + String(ESP32_API_KEY));
        ack.POST("");
        ack.end();
    }
}

void saveLastState(uint8_t bri, uint16_t ct) {
    prefs.begin("mira", false);
    prefs.putUChar("savedBri", bri);
    prefs.putUShort("savedCt", ct);
    prefs.end();
}

void tickWakeRamp(float lux) {
    wakeStep++;
    int   totalTicks = DAY_TYPES[timeClient.getDay()] == 0 ? WAKE_RAMP_TICKS_WORK : WAKE_RAMP_TICKS_RELAXED;
    float t          = min(wakeStep / (float)totalTicks, 1.0f);

    int rampBri = wakeStartTarget.bri + t * (wakeEndTarget.bri - wakeStartTarget.bri);
    int rampCt = wakeStartTarget.ct + t * (wakeEndTarget.ct - wakeStartTarget.ct);

    rampBri = constrain(rampBri, 1, 254);
   rampCt = constrain(rampCt, int(CT_COOL), int(CT_WARM));

    setLight(LIGHT_BEDSIDE, true, rampBri, rampCt, 300);
    setLight(LIGHT_DESK, true, rampBri, rampCt, 300);
    if (lux >= S3_LUX_HI) {
        setLight(LIGHT_CEIL_1, true, rampBri, rampCt, 300);
        setLight(LIGHT_CEIL_2, true, rampBri, rampCt, 300);
        overheadsOn = true;
    }

    if (wakeStep == totalTicks / 4 || wakeStep == totalTicks / 2 || wakeStep == (3 * totalTicks / 4)) {
        sendLog("[Mira] Wake " + String(wakeStep * 100 / totalTicks) + "% — bri=" + String(rampBri) + " — " + getTimeString());
    }

    if (t >= 1.0f) {
        state             = State::NORMAL;
        skipOverrideCheck = true;
        Serial.println("Wake complete — handing off to normal.");
        sendLog("[Mira] Wake complete — " + getTimeString());
    }
    sentTarget = {(uint8_t)rampBri, (uint16_t)rampCt};

    Serial.println("Wake " + String(wakeStep) + "/" + String(totalTicks) +
                " — bri=" + String(rampBri) + "/" + String(wakeEndTarget.bri));

}

void tickWindDown() {
    windDownStep++;
    float windDownBri = windDownStartBri - (windDownStartBri - S4_FLOOR_BRI) * (windDownStep / 120.0f);
    windDownBri = max(windDownBri, (float)S4_FLOOR_BRI);
    int wdBri = (int)windDownBri;
    int wdCt  = (int)CT_WARM;

    Serial.println("Wind-down " + String(windDownStep * 0.5f, 1) + "/60.0 min — bri=" + String(wdBri));
    if (windDownStep == 30 || windDownStep == 60 || windDownStep == 90) {
        sendLog("[Mira] Wind-down " + String(windDownStep / 2) + "/60 min — bri=" + String(wdBri) + " — " + getTimeString());
    }

    setLight(LIGHT_BEDSIDE, true, wdBri, wdCt, 300);
    if (windDownStep < 120) {
        setLight(LIGHT_DESK, true, wdBri, wdCt, 300);
    } else {
        setLight(LIGHT_DESK, false, wdBri, wdCt, 100);
        state = State::LOCKED_OUT;
        saveLastState((uint8_t)S4_FLOOR_BRI, (uint16_t)CT_WARM);
        Serial.println("Wind-down complete. Desk off, bedside at floor.");
        sendLog("[Mira] Wind-down complete — " + getTimeString());
    }
    sentTarget = {(uint8_t)wdBri, (uint16_t)wdCt};
}

bool isManualOverride(LightState ls, bool expectedOn) {
    if (expectedOn && !ls.on) return true;
    if (ls.on) {
        bool matchesCurrent = abs(ls.bri - sentTarget.bri) <= STATE_TOLERANCE &&
                              abs(ls.ct  - sentTarget.ct)  <= STATE_TOLERANCE;
        bool matchesPrev    = abs(ls.bri - prevSentTarget.bri) <= STATE_TOLERANCE &&
                              abs(ls.ct  - prevSentTarget.ct)  <= STATE_TOLERANCE;
        if (!matchesCurrent && !matchesPrev) return true;
    }
    return false;
}

void checkOverride() {
    if (skipOverrideCheck) { skipOverrideCheck = false; return; }

    auto debugLight = [](const char* name, LightState ls) {
        Serial.print("  "); Serial.print(name);
        Serial.print(": on="); Serial.print(ls.on);
        Serial.print(" bri="); Serial.print(ls.bri);
        Serial.print(" ct="); Serial.println(ls.ct);
    };

    LightState bedside = getLightState(LIGHT_BEDSIDE);
    LightState desk    = getLightState(LIGHT_DESK);

    Serial.print("checkOverride — sentTarget bri="); Serial.print(sentTarget.bri);
    Serial.print(" ct="); Serial.println(sentTarget.ct);
    debugLight("bedside", bedside);
    debugLight("desk",    desk);

    bool overridden = isManualOverride(bedside, true) || isManualOverride(desk, true);
    if (overheadsOn) {
        LightState ceil1 = getLightState(LIGHT_CEIL_1);
        LightState ceil2 = getLightState(LIGHT_CEIL_2);
        debugLight("ceil1", ceil1);
        debugLight("ceil2", ceil2);
        overridden = overridden || isManualOverride(ceil1, true) || isManualOverride(ceil2, true);
    }

    if (overridden) {
        state              = State::SOFT_PAUSE;
        softPauseStart     = millis();
        pauseResumeActive  = false;
        Serial.println("Manual override detected — soft pause.");
        sendLog("[Mira] Manual override — soft pause — " + getTimeString());
    }
}

void tickNormal(float lux, LightTarget target, bool shouldUpdate) {
    if (!pauseResumeActive) checkOverride();
    if (state == State::SOFT_PAUSE) return;

    bool newOverheadsOn = lux > S3_LUX_HI;
    if (overheadsOn && !newOverheadsOn) {
        setLight(LIGHT_CEIL_1, false, 0, 0, 5);
        setLight(LIGHT_CEIL_2, false, 0, 0, 5);
        shouldUpdate = true;
    } else if (!overheadsOn && newOverheadsOn) {
        shouldUpdate = true;
    }
    overheadsOn = newOverheadsOn;

    // Soft pause resume ramp — drift from pre-pause bri/ct to current ambient over 10 min
    if (pauseResumeActive) {
        pauseResumeStep++;
        float t      = min(pauseResumeStep / (float)PAUSE_RESUME_TICKS, 1.0f);
        int rampBri  = (int)(pauseResumeStartTarget.bri + t * (target.bri - pauseResumeStartTarget.bri));
        int rampCt   = (int)(pauseResumeStartTarget.ct  + t * (target.ct  - pauseResumeStartTarget.ct));
        rampBri      = constrain(rampBri, 1, 254);
        rampCt       = constrain(rampCt, 153, 447);
        setLight(LIGHT_BEDSIDE, true, rampBri, rampCt, 300);
        setLight(LIGHT_DESK,    true, rampBri, rampCt, 300);
        if (overheadsOn) {
            setLight(LIGHT_CEIL_1, true, rampBri, rampCt, 300);
            setLight(LIGHT_CEIL_2, true, rampBri, rampCt, 300);
        }
        prevSentTarget = sentTarget;
        sentTarget = {(uint8_t)rampBri, (uint16_t)rampCt};
        saveLastState((uint8_t)rampBri, (uint16_t)rampCt);
        Serial.println("Resume ramp " + String(pauseResumeStep) + "/" + String(PAUSE_RESUME_TICKS) +
                       " — bri=" + String(rampBri) + " (" + String(rampBri * 100 / 254) + "%), ct=" + String(rampCt));
        if (pauseResumeStep >= PAUSE_RESUME_TICKS) {
            pauseResumeActive = false;
            Serial.println("Resume ramp complete.");
        }
        return;
    }

    if (lux <= 10 && timeClient.getHours() >= 21) {
        stableLuxCount++;
        Serial.print("stableLuxCount: "); Serial.println(stableLuxCount);
    } else {
        stableLuxCount = max(stableLuxCount - 1, 0);
    }

    if (stableLuxCount >= 60) {
        state            = State::WIND_DOWN;
        windDownStep     = 0;
        windDownStartBri = sentTarget.bri;
        sendLog("[Mira] Wind-down starting — " + getTimeString());
        return;
    }

    if (shouldUpdate) {
        setLight(LIGHT_BEDSIDE, true, target.bri, target.ct, 10);
        setLight(LIGHT_DESK,    true, target.bri, target.ct, 10);
        if (overheadsOn) {
            setLight(LIGHT_CEIL_1, true, target.bri, target.ct, 10);
            setLight(LIGHT_CEIL_2, true, target.bri, target.ct, 10);
        }
        prevSentTarget = sentTarget;
        sentTarget = target;
        saveLastState(target.bri, target.ct);
        Serial.println("Bulbs updated.");
        sendLog("[Mira] " + getTimeString() +
                    " | Lux: " + String(lux, 1) +
                    " → bri=" + String(target.bri) + " (" + String(target.bri * 100 / 254) + "%)" +
                    ", ct=" + String(target.ct) +
                    " | Bulbs updated");
    } else {
        Serial.println("No change — skipping PUT.");
    }
}

void tickSoftPause() {
    if (millis() - softPauseStart >= SOFT_PAUSE_MS) {
        pauseResumeStartTarget = sentTarget;
        pauseResumeActive      = true;
        pauseResumeStep        = 0;
        state                  = State::NORMAL;
        skipOverrideCheck      = true;
        Serial.println("Soft pause expired — beginning 10-min resume ramp.");
        sendLog("[Mira] Soft pause expired — resuming — " + getTimeString());
    }
}

void triggerWake(float ambientLux) {
    LightState bedside = getLightState(LIGHT_BEDSIDE);
    uint8_t  startBri  = (bedside.on && bedside.bri > 0) ? (uint8_t)bedside.bri : (uint8_t)S4_FLOOR_BRI;
    uint16_t startCt   = (bedside.ct  > 0) ? (uint16_t)bedside.ct  : (uint16_t)CT_WARM;
    wakeStartTarget    = {startBri, startCt};
    wakeEndTarget      = luxToTarget(ambientLux);
    wakeStep           = 0;
    state              = State::WAKE;
    Serial.println("Wake triggered — bri " + String(startBri) + " → " + String(wakeEndTarget.bri) +
                   ", ct " + String(startCt) + " → " + String(wakeEndTarget.ct));
    sendLog("[Mira] Wake sequence started — " + getTimeString());
}

void checkBedsideState(float lux) {
    LightState bedside = getLightState(LIGHT_BEDSIDE);

    if (state == State::LOCKED_OUT && !lastBedsideOn && bedside.on) {
        triggerWake(lux);
    }

    if (lastBedsideOn && !bedside.on && timeClient.getHours() >= LOCKOUT_RESET_HOUR) {
        LightState desk  = getLightState(LIGHT_DESK);
        LightState ceil1 = getLightState(LIGHT_CEIL_1);
        LightState ceil2 = getLightState(LIGHT_CEIL_2);
        if (!desk.on && !ceil1.on && !ceil2.on) {
            state          = State::LOCKED_OUT;
            stableLuxCount = 0;
            windDownStep   = 0;
        }
    }

    lastBedsideOn = bedside.on;
}

void pollButton(ButtonState& btn, int pin) {
    bool raw = (digitalRead(pin) == LOW); // convert to bool: true = pressed
    unsigned long now = millis();

    if (raw == btn.debounced) {
        // Signal is stable — keep timer fresh so it's ready when signal next changes
        btn.lastDebounceMs = now;
    } else if (now - btn.lastDebounceMs >= DEBOUNCE_MS) {
        // Signal has differed from last accepted state for long enough — commit it
        btn.debounced = raw;
        if (raw) {
            btn.held         = true;
            btn.pressStartMs = now;
        } else {
            btn.held = false;
            if (!btn.longFired) btn.shortPress = true; // short press fires on release
            btn.longFired = false;
        }
    }

    // Long press: fire once when held past threshold
    if (btn.held && !btn.longFired && now - btn.pressStartMs >= LONG_PRESS_MS) {
        btn.longPress = true;
        btn.longFired = true;
    }
}

void forceState(State next) {
    switch (next) {
        case State::LOCKED_OUT:
            stableLuxCount = 0;
            windDownStep   = 0;
            state          = State::LOCKED_OUT;
            break;
        case State::NORMAL:
            sentTarget        = {255, 0};
            skipOverrideCheck = true;
            state             = State::NORMAL;
            break;
        case State::WAKE:
            triggerWake(lastLux); // sets state = WAKE internally
            break;
        case State::WIND_DOWN:
            windDownStartBri = (sentTarget.ct > 0) ? sentTarget.bri : luxToTarget(lastLux).bri;
            windDownStep     = 0;
            state            = State::WIND_DOWN;
            break;
        case State::SOFT_PAUSE:
            softPauseStart = millis();
            state          = State::SOFT_PAUSE;
            break;
        case State::HARD_OFF:
            state = State::HARD_OFF;
            break;
    }
    Serial.println("forceState → " + String(stateName()));
}

void handleCycleButton() {
    if (btnCycle.shortPress) {
        btnCycle.shortPress = false;
        State next = (State)(((int)state + 1) % 6);
        forceState(next);
        if (next != State::WAKE) { // triggerWake already sends discord
            sendLog("[Mira] Cycle button → " + String(stateName()) + " — " + getTimeString());
        }
    }
}

void handleButtonEvents() {
    // Long press: hard off toggle
    if (btnMode.longPress) {
        btnMode.longPress = false;
        if (state != State::HARD_OFF) {
            state = State::HARD_OFF;
            Serial.println("Button long press — hard off.");
            sendLog("[Mira] Hard off — " + getTimeString());
        } else {
            state = State::LOCKED_OUT;
            Serial.println("Button long press — hard off cleared.");
            sendLog("[Mira] Hard off cleared — " + getTimeString());
        }
    }

    // Short press: go to NORMAL if not already there, otherwise soft pause
    if (btnMode.shortPress) {
        btnMode.shortPress = false;
        if (state == State::HARD_OFF) {
            // hard off is long-press only — ignore short press
        } else if (state == State::NORMAL) {
            state = State::SOFT_PAUSE;
            softPauseStart = millis();
            Serial.println("Button short press — soft pause.");
            sendLog("[Mira] Soft pause (button) — " + getTimeString());
        } else {
            String prev = stateName();
            state             = State::NORMAL;
            sentTarget        = {255, 0};
            skipOverrideCheck = true;
            Serial.println("Button short press — NORMAL (was " + prev + ").");
            sendLog("[Mira] Returned to NORMAL by button — " + getTimeString());
        }
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(LEDR, OUTPUT);
    pinMode(LEDG, OUTPUT);
    pinMode(LEDB, OUTPUT);
    setRGB(false, false, false);

    pinMode(BTN_MODE,  INPUT_PULLUP);
    pinMode(BTN_CYCLE, INPUT_PULLUP);

    connectWiFi();

    timeClient.begin();
    timeClient.update();
    setRGB(false, false, true); // solid blue = NTP synced
    Serial.println("NTP synced.");
    delay(800);

    if (!veml.begin()) {
        Serial.println("VEML7700 not found — check wiring!");
        while (1) { setRGB(true, false, false); delay(300); setRGB(false, false, false); delay(300); }
    }
    Serial.println("VEML7700 ready.");

    setRGB(false, false, false); // LED off — boot complete

    // Startup flash — deep purple, then restore previous state
    LightState saved = getLightState(LIGHT_BEDSIDE);
    setLightColor(LIGHT_BEDSIDE, true, 200, 48000, 200, 10); // fade in purple over 1s
    delay(3000);                                              // hold for 3s
    setLight(LIGHT_BEDSIDE, saved.on, saved.bri, saved.ct, 10); // restore over 1s

    lastBedsideOn = getLightState(LIGHT_BEDSIDE).on; // seed edge detection — prevents false wake trigger on first tick
    overheadsOn   = getLightState(LIGHT_CEIL_1).on;  // seed from actual state — prevents false override and bad dashboard reporting

    sendLog("[Mira] Online — " + getTimeString());
}

void loop() {
    timeClient.update();
    printStatus();

    float lux = veml.readLux(VEML_LUX_AUTO);
    lastLux = lux;
    LightTarget target = luxToTarget(lux);
    Serial.print("Lux: "); Serial.print(lux, 1);
    Serial.print(" → bri="); Serial.print(target.bri);
    Serial.print(" ("); Serial.print(target.bri * 100 / 254); Serial.print("%)");
    Serial.print(", ct="); Serial.println(target.ct);

    bool shouldUpdate = abs(target.bri - sentTarget.bri) > STATE_TOLERANCE || abs(target.ct - sentTarget.ct) > STATE_TOLERANCE;

    pollDashboardCommand();

    switch (state) {
        case State::LOCKED_OUT:
            checkBedsideState(lux);
            break;

        case State::NORMAL:
            checkBedsideState(lux);
            tickNormal(lux, target, shouldUpdate);
            break;

        case State::WAKE:
            tickWakeRamp(lux);
            break;

        case State::WIND_DOWN:
            checkBedsideState(lux);
            tickWindDown();
            break;

        case State::SOFT_PAUSE:
            tickSoftPause();
            break;

        case State::HARD_OFF:
            break;

    }

    sendDashboardStatus(lux);

    unsigned long tickStart = millis();
    while (millis() - tickStart < 30000UL) {
        pollButton(btnMode,  BTN_MODE);
        pollButton(btnCycle, BTN_CYCLE);
        handleButtonEvents();
        handleCycleButton();
        delay(50);
    }
}