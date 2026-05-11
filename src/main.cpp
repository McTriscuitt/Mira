//
// Project created by Tristan DeOrnellis on 4/13/2026.
// Name "Mira" designated on 4/22/2026
//

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <mbedtls/x509_crt.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUDP.h>
#include <Preferences.h>
#include <Adafruit_VEML7700.h>
#include "config.h"
#include "lightcurve.h"

struct LightState {
    bool  on;
    float bri;  // percent 0.0–100.0 (Hue API v2 native units)
    int   ct;
};

Adafruit_VEML7700 veml;
Preferences prefs;
LightTarget sentTarget = {-1.0f, 0}; // sentinel: forces first update to always send (-1/0 are outside valid ranges)
bool overheadsOn = false;

// Phase 3 — light state cache. Mirror of bridge state; bootstrapped via v2 GET at startup,
// kept in sync by SSE events. Replaces the old per-tick HTTP polling of getLightState().
struct CachedLight {
    bool  on          = false;
    float bri         = 0.0f;
    int   ct          = 0;
    bool  initialized = false;
};
CachedLight lightCache[4]; // indexed by LIGHT_BEDSIDE / LIGHT_DESK / LIGHT_CEIL_1 / LIGHT_CEIL_2

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

// Soft pause resume ramp
bool        pauseResumeActive      = false;
int         pauseResumeStep        = 0;
LightTarget pauseResumeStartTarget = {0, 0}; // bri/ct snapshot at soft pause entry — interpolated on resume

// Recent-PUT trajectory record — one slot per light index. Replaces the old
// time-window override mute. Each outgoing PUT snapshots its (prior, target)
// trajectory here so the SSE handler can discriminate self-PUT echoes (events
// that land on the trajectory) from external overrides (events that don't).
// Closed-loop: depends only on what we just told the bridge, not on bridge
// metadata. Per-light, so an in-flight bedside PUT doesn't mask a real override
// on the overheads.
struct RecentPut {
    bool          active     = false;
    bool          onTarget   = false;
    float         priorBri   = 0.0f;
    float         targetBri  = 0.0f;
    int           priorCt    = 0;
    int           targetCt   = 0;
    unsigned long postedAtMs = 0;
    unsigned long durationMs = 0;
};
RecentPut recentPuts[4];
const unsigned long RECENT_PUT_GRACE_MS = 5000UL; // post-ramp slack for late echoes (Zigbee mesh can be slow)
// Trajectory-match tolerances, intentionally wider than STATE_TOLERANCE_*:
// - STATE_TOLERANCE_* sizes "is the curve drifted enough to send a new PUT?"
// - TRAJECTORY_TOLERANCE_* sizes "is the bridge's settled value within the slop
//   we expect from Zigbee/bulb-side quantization?" Bulbs snap to discrete bri/ct
//   steps (~1-2% bri, ~5-10 mirek), and the cached priorBri/Ct may itself be a
//   step off the bulb's real pre-PUT state. A user-driven override is typically
//   double-digit percent or 50+ mirek, well outside this window.
const float TRAJECTORY_TOLERANCE_BRI = 5.0f;
const int   TRAJECTORY_TOLERANCE_CT  = 15;

// SSE event stream — persistent HTTPS connection over which the bridge pushes state changes.
WiFiClientSecure sseClient;
String        sseBuf;            // partial-line accumulator for incoming SSE bytes
unsigned long sseLastByteMs    = 0;
unsigned long sseLastConnectMs = 0;
const unsigned long SSE_RECONNECT_DELAY_MS = 5000UL;
const unsigned long SSE_STALE_TIMEOUT_MS   = 600000UL; // Hue v2 sends no keepalive; only reconnect on long silence

// Bedside edge detection
bool lastBedsideOn = false;  // previous bedside poll — detects manual on/off flips

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", UTC_OFFSET_SEC);

static String _bridgeCertPem; // TLS cert loaded from NVS; used by all v2 HTTPS calls



// Encode DER bytes as a PEM certificate string (no mbedTLS dependency).
static String _derToPem(const uint8_t* der, size_t len) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    String out = "-----BEGIN CERTIFICATE-----\n";
    size_t lineLen = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)der[i] << 16;
        if (i+1 < len) v |= (uint32_t)der[i+1] << 8;
        if (i+2 < len) v |= (uint32_t)der[i+2];
        out += b64[(v >> 18) & 63];
        out += b64[(v >> 12) & 63];
        out += (i+1 < len) ? b64[(v >> 6) & 63] : '=';
        out += (i+2 < len) ? b64[v & 63]         : '=';
        lineLen += 4;
        if (lineLen >= 64) { out += '\n'; lineLen = 0; }
    }
    if (lineLen > 0) out += '\n';
    out += "-----END CERTIFICATE-----\n";
    return out;
}

// Convert mbedtls_x509_time (UTC) to a Unix timestamp for NVS expiry storage.
static unsigned long _certTimeToEpoch(const mbedtls_x509_time& t) {
    static const int dom[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned long days = 0;
    for (int y = 1970; y < t.year; y++) {
        bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        days += leap ? 366 : 365;
    }
    for (int m = 1; m < t.mon; m++) {
        days += dom[m-1];
        if (m == 2) {
            bool leap = (t.year % 4 == 0 && (t.year % 100 != 0 || t.year % 400 == 0));
            if (leap) days++;
        }
    }
    days += t.day - 1;
    return days * 86400UL + t.hour * 3600UL + t.min * 60UL + t.sec;
}

// Connect once with setInsecure(), grab the bridge TLS cert, and write it to NVS.
static bool _fetchAndStoreBridgeCert() {
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10);
    if (!client.connect("192.168.1.186", 443)) {
        Serial.println("Cert fetch: connect failed");
        return false;
    }
    // Send a minimal GET so the TLS handshake completes and the peer cert is available.
    client.print("GET /clip/v2/resource/light HTTP/1.0\r\n"
                 "Host: 192.168.1.186\r\n"
                 "hue-application-key: " HUE_API_KEY "\r\n"
                 "Connection: close\r\n\r\n");
    // Drain just enough to ensure the handshake finished.
    unsigned long t0 = millis();
    while (client.connected() && millis() - t0 < 3000UL) {
        if (client.available()) { client.read(); break; }
    }

    const mbedtls_x509_crt* cert = client.getPeerCertificate();
    if (!cert) {
        Serial.println("Cert fetch: no peer cert");
        client.stop();
        return false;
    }

    String pem    = _derToPem(cert->raw.p, cert->raw.len);
    unsigned long expiry = _certTimeToEpoch(cert->valid_to);
    client.stop();

    prefs.begin("mira", false);
    prefs.putString("hueCert",       pem);
    prefs.putULong ("hueCertExpiry", expiry);
    prefs.end();

    _bridgeCertPem = pem;
    Serial.println("Bridge cert stored. Expiry epoch: " + String(expiry));
    return true;
}

// Load cert from NVS; re-fetch if missing, expired, or within 30 days of expiry.
// Must be called AFTER NTP sync (relies on timeClient.getEpochTime()).
static void ensureBridgeCert() {
    prefs.begin("mira", true);
    String        stored = prefs.getString("hueCert",       "");
    unsigned long expiry = prefs.getULong ("hueCertExpiry", 0);
    prefs.end();

    unsigned long now      = timeClient.getEpochTime();
    bool          needFetch = stored.isEmpty()
                           || (now >= expiry)
                           || (expiry - now < 30UL * 24 * 3600);

    if (!needFetch) {
        _bridgeCertPem = stored;
        // Verify the stored cert still validates against the live bridge.
        // Catches cert rotation (bridge issues new cert before old one expires).
        WiFiClientSecure test;
        test.setCACert(_bridgeCertPem.c_str());
        test.setTimeout(5);
        bool ok = test.connect(HUE_BRIDGE_HOST, 443);
        test.stop();
        if (ok) {
            Serial.println("Bridge cert loaded from NVS.");
            return;
        }
        Serial.println("Bridge cert NVS verify failed — re-fetching...");
    } else {
        Serial.println("Bridge cert missing or expiring — fetching...");
    }
    if (!_fetchAndStoreBridgeCert() && !stored.isEmpty()) {
        _bridgeCertPem = stored;
        Serial.println("Cert fetch failed — using stored cert.");
    }
}

// ── Light cache helpers ─────────────────────────────────────────────────────

// Map a v2 UUID string to its lightCache[] index. Returns -1 for unknown UUIDs.
static int idxByUuid(const char* uuid) {
    if (!uuid) return -1;
    if (strcmp(uuid, LIGHT_UUID_BEDSIDE) == 0) return LIGHT_BEDSIDE;
    if (strcmp(uuid, LIGHT_UUID_DESK)    == 0) return LIGHT_DESK;
    if (strcmp(uuid, LIGHT_UUID_CEIL_1)  == 0) return LIGHT_CEIL_1;
    if (strcmp(uuid, LIGHT_UUID_CEIL_2)  == 0) return LIGHT_CEIL_2;
    return -1;
}

// Read the cached state for a bulb. Replaces the old HTTP-polling getLightState().
LightState cachedLight(int idx) {
    if (idx < 0 || idx >= 4) return {false, 0.0f, 0};
    const CachedLight& c = lightCache[idx];
    return {c.on, c.bri, c.ct};
}

// ── Recent-PUT trajectory matching ──────────────────────────────────────────
// Replaces the old time-window override mute. The key insight: a self-PUT echo
// arrives reporting bri/ct values that lie on the trajectory between our pre-PUT
// state and our PUT target. An external override arrives reporting values that
// don't. Matching on trajectory (not just endpoint) absorbs the bridge's
// intermediate ramp echoes without needing to time-window them.

// Snapshot an outgoing PUT's trajectory. Called from setLight*/setLightColor
// *before* the HTTP request goes out — the bridge can echo back faster than
// HTTPClient::PUT() returns, so the entry must already exist when the SSE
// event lands.
static void noteRecentPut(int idx, bool on, float bri, int ct, unsigned long durationMs) {
    if (idx < 0 || idx >= 4) return;
    RecentPut& r = recentPuts[idx];
    r.active     = true;
    r.onTarget   = on;
    r.priorBri   = lightCache[idx].bri;
    r.targetBri  = bri;
    r.priorCt    = lightCache[idx].ct;
    r.targetCt   = ct;
    r.postedAtMs = millis();
    r.durationMs = durationMs;
}

// Does an incoming SSE event lie on the trajectory of the most recent PUT for
// this light? Auto-expires entries past their dynamics window + grace.
static bool eventMatchesRecentPut(int idx,
                                  bool hasOn,  bool   evOn,
                                  bool hasBri, float  evBri,
                                  bool hasCt,  int    evCt) {
    if (idx < 0 || idx >= 4) return false;
    RecentPut& r = recentPuts[idx];
    if (!r.active) return false;
    if (millis() - r.postedAtMs > r.durationMs + RECENT_PUT_GRACE_MS) {
        r.active = false;
        return false;
    }
    if (hasOn && evOn != r.onTarget) return false;
    if (hasBri) {
        float lo = fminf(r.priorBri, r.targetBri) - TRAJECTORY_TOLERANCE_BRI;
        float hi = fmaxf(r.priorBri, r.targetBri) + TRAJECTORY_TOLERANCE_BRI;
        if (evBri < lo || evBri > hi) return false;
    }
    if (hasCt) {
        int lo = min(r.priorCt, r.targetCt) - TRAJECTORY_TOLERANCE_CT;
        int hi = max(r.priorCt, r.targetCt) + TRAJECTORY_TOLERANCE_CT;
        if (evCt < lo || evCt > hi) return false;
    }
    return true;
}

// ── One-time bootstrap of the light cache ───────────────────────────────────
// Hits the v2 endpoint once at startup to seed every cached field; SSE keeps it
// fresh thereafter.
static void bootstrapLightStates() {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, String(HUE_V2_BASE_URL) + "/resource/light");
    http.addHeader("hue-application-key", HUE_API_KEY);
    int code = http.GET();
    if (code != 200) {
        Serial.println("bootstrap: HTTP " + String(code));
        http.end();
        return;
    }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) { Serial.println("bootstrap: JSON parse failed"); return; }
    for (JsonObject light : doc["data"].as<JsonArray>()) {
        const char* uuid = light["id"];
        int idx = idxByUuid(uuid);
        if (idx < 0) continue;
        lightCache[idx].on  = light["on"]["on"]                          | false;
        lightCache[idx].bri = light["dimming"]["brightness"].as<float>();
        lightCache[idx].ct  = light["color_temperature"]["mirek"]        | 0;
        lightCache[idx].initialized = true;
        Serial.printf("Bootstrap idx=%d on=%d bri=%.1f ct=%d (%s)\n",
                      idx, lightCache[idx].on, lightCache[idx].bri, lightCache[idx].ct,
                      String(uuid).substring(0, 8).c_str());
    }
}



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

// Convert Hue v1 HSB (hue 0–65535, sat 0–254) to CIE xy for the v2 API.
static void _hsbToXY(int hueV1, int satV1, float& x, float& y) {
    float h = hueV1 * 360.0f / 65535.0f;
    float s = satV1 / 254.0f;
    int   hi = (int)(h / 60.0f) % 6;
    float f  = h / 60.0f - (int)(h / 60.0f);
    float p  = 1.0f - s;
    float q  = 1.0f - f * s;
    float t  = 1.0f - (1.0f - f) * s;
    float r, g, b;
    switch (hi) {
        case 0: r=1; g=t; b=p; break;
        case 1: r=q; g=1; b=p; break;
        case 2: r=p; g=1; b=t; break;
        case 3: r=p; g=q; b=1; break;
        case 4: r=t; g=p; b=1; break;
        default:r=1; g=p; b=q; break;
    }
    auto gamma = [](float c) -> float {
        return c > 0.04045f ? powf((c + 0.055f) / 1.055f, 2.4f) : c / 12.92f;
    };
    r = gamma(r); g = gamma(g); b = gamma(b);
    float X = r*0.4124f + g*0.3576f + b*0.1805f;
    float Y = r*0.2126f + g*0.7152f + b*0.0722f;
    float Z = r*0.0193f + g*0.1192f + b*0.9505f;
    float sum = X + Y + Z;
    x = (sum > 0.0f) ? X / sum : 0.3f;
    y = (sum > 0.0f) ? Y / sum : 0.3f;
}

// v2 HTTPS color PUT (HSB color mode). bri is percent 0.0–100.0. durationMs in ms.
void setLightColor(const char* uuid, bool on, float bri, int hueV1, int satV1, int durationMs) {
    // Record trajectory so SSE echoes are recognized as ours. Color mode doesn't
    // change CT meaningfully — pass through the cached value so the ct check is a
    // self-match.
    int idx = idxByUuid(uuid);
    int ctTarget = (idx >= 0) ? lightCache[idx].ct : 0;
    noteRecentPut(idx, on, bri, ctTarget, (unsigned long)durationMs);

    float cx, cy;
    _hsbToXY(hueV1, satV1, cx, cy);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, String(HUE_V2_BASE_URL) + "/resource/light/" + uuid);
    http.addHeader("Content-Type",      "application/json");
    http.addHeader("hue-application-key", HUE_API_KEY);

    String body = "{\"on\":{\"on\":" + String(on ? "true" : "false") + "}";
    body += ",\"dynamics\":{\"duration\":" + String(durationMs) + "}";
    if (on) {
        body += ",\"dimming\":{\"brightness\":" + String(bri, 1) + "}";
        body += ",\"color\":{\"xy\":{\"x\":" + String(cx, 4) + ",\"y\":" + String(cy, 4) + "}}";
    }
    body += "}";

    int code = http.PUT(body);
    Serial.println("setLightColor(" + String(uuid).substring(0,8) + "…) → HTTP " + code);
    http.end();
}

// v2 HTTPS white/CT PUT. bri is percent 0.0–100.0. durationMs in ms.
void setLight(const char* uuid, bool on, float bri, int ct, int durationMs) {
    // Record trajectory so the SSE echo of this PUT is recognized as ours.
    noteRecentPut(idxByUuid(uuid), on, bri, ct, (unsigned long)durationMs);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, String(HUE_V2_BASE_URL) + "/resource/light/" + uuid);
    http.addHeader("Content-Type",      "application/json");
    http.addHeader("hue-application-key", HUE_API_KEY);

    String body = "{\"on\":{\"on\":" + String(on ? "true" : "false") + "}";
    body += ",\"dynamics\":{\"duration\":" + String(durationMs) + "}";
    if (on) {
        body += ",\"dimming\":{\"brightness\":" + String(bri, 1) + "}";
        body += ",\"color_temperature\":{\"mirek\":" + String(ct) + "}";
    }
    body += "}";

    int code = http.PUT(body);
    Serial.println("setLight(" + String(uuid).substring(0,8) + "…) → HTTP " + code);
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
    Serial.print("[");
    Serial.print(hour12); Serial.print(":");
    if (m < 10) Serial.print("0"); Serial.print(m); Serial.print(":");
    if (s < 10) Serial.print("0"); Serial.print(s);
    Serial.print(ampm); Serial.print("] ");
    Serial.print(days[day]);
    Serial.print(" ["); Serial.print(stateName()); Serial.println("]");
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
    doc["wake_total"]     = WAKE_RAMP_TICKS;
    long pauseRemaining = (state == State::SOFT_PAUSE) ?
        max(0L, ((long)SOFT_PAUSE_MS - (long)(millis() - softPauseStart)) / 1000L) : 0L;
    doc["soft_pause_remaining_s"] = pauseRemaining;
    String body;
    serializeJson(doc, body);
    int code = http.POST(body);
    if (code != 200) Serial.println("Dashboard status POST → HTTP " + String(code));
    http.end();
}

// ── SSE event handling ──────────────────────────────────────────────────────
// The Hue v2 bridge pushes resource updates over a long-lived HTTPS stream. We
// drain the socket from the main wait loop, parse `data:` lines as JSON arrays,
// update the cache, and trigger SOFT_PAUSE on a real manual override.

// Apply a single light update event to the cache; check override if in NORMAL.
// Override is evaluated ONLY against fields present in this event — not against
// cache-merged state. The bridge splits state changes across multiple events
// (e.g. a `dimming` event followed by a separate `color_temperature` event),
// so the cache is a Frankenstein of new + stale fields between arrivals, and
// using cached fields would generate false positives every time a partial event
// landed during a steady-state lux-driven PUT.
static void handleLightUpdate(JsonObjectConst upd) {
    const char* uuid = upd["id"];
    int idx = idxByUuid(uuid);
    if (idx < 0) return;

    JsonVariantConst onField  = upd["on"]["on"];
    JsonVariantConst briField = upd["dimming"]["brightness"];
    JsonVariantConst ctField  = upd["color_temperature"]["mirek"];
    bool hasOn  = !onField.isNull();
    bool hasBri = !briField.isNull();
    bool hasCt  = !ctField.isNull();
    if (!hasOn && !hasBri && !hasCt) return;

    if (hasOn)  lightCache[idx].on  = onField.as<bool>();
    if (hasBri) lightCache[idx].bri = briField.as<float>();
    if (hasCt)  lightCache[idx].ct  = ctField.as<int>();

    if (state != State::NORMAL) return;
    if (pauseResumeActive)      return;

    bool expectedOn;
    switch (idx) {
        case LIGHT_BEDSIDE:
        case LIGHT_DESK:    expectedOn = true;        break;
        case LIGHT_CEIL_1:
        case LIGHT_CEIL_2:  expectedOn = overheadsOn; break;
        default:            return;
    }
    if (!expectedOn) return;

    // Echo discrimination: if this event lies on the trajectory of a recent PUT
    // for this light, it's our own echo — ignore it. Anything off-trajectory is
    // an external change by construction.
    bool  evOn  = hasOn  ? onField.as<bool>()   : false;
    float evBri = hasBri ? briField.as<float>() : 0.0f;
    int   evCt  = hasCt  ? ctField.as<int>()    : 0;
    if (eventMatchesRecentPut(idx, hasOn, evOn, hasBri, evBri, hasCt, evCt)) return;

    // Diagnostic dump: capture the exact misfire conditions. Cheap, fires only
    // immediately before SOFT_PAUSE entry; useful for tuning trajectory windows
    // and identifying real-vs-spurious overrides during the SSE rollout.
    const RecentPut& r = recentPuts[idx];
    unsigned long age = millis() - r.postedAtMs;
    Serial.printf(
        "Override fire: idx=%d  event[hasOn=%d evOn=%d hasBri=%d evBri=%.2f hasCt=%d evCt=%d]  "
        "recent[active=%d onTarget=%d priorBri=%.2f targetBri=%.2f priorCt=%d targetCt=%d age=%lu/%lu+grace=%lu]\n",
        idx,
        hasOn?1:0, evOn?1:0, hasBri?1:0, evBri, hasCt?1:0, evCt,
        r.active?1:0, r.onTarget?1:0, r.priorBri, r.targetBri, r.priorCt, r.targetCt,
        age, r.durationMs, RECENT_PUT_GRACE_MS);

    state             = State::SOFT_PAUSE;
    softPauseStart    = millis();
    pauseResumeActive = false;
    Serial.println("SSE override — soft pause.");
    sendLog("Manual override — soft pause — " + getTimeString());
}

// Parse one SSE `data:` payload — an array of events, each with a nested array of resource updates.
static void handleSseEventData(const char* json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return;
    for (JsonObject event : doc.as<JsonArray>()) {
        const char* type = event["type"] | "";
        if (strcmp(type, "update") != 0) continue;
        for (JsonObject upd : event["data"].as<JsonArray>()) {
            const char* updType = upd["type"] | "";
            if (strcmp(updType, "light") != 0) continue;
            handleLightUpdate(upd);
        }
    }
}

// Process one complete SSE protocol line. Lines starting with `:` are comments
// (the bridge sends `: hi`-style heartbeats); `id:` carries the event ID; only
// `data:` lines carry the JSON payload we care about.
static void handleSseLine(const String& line) {
    if (line.startsWith("data:")) {
        const char* p = line.c_str() + 5;
        while (*p == ' ') p++;
        handleSseEventData(p);
    }
}

// Open the persistent SSE connection. Sends the GET, drains response headers
// non-blocking-ish (bounded 5s loop) until the empty header-terminator line.
static void sseConnect() {
    sseClient.stop();
    sseBuf = "";
    sseClient.setInsecure();
    sseClient.setTimeout(5);
    if (!sseClient.connect(HUE_BRIDGE_HOST, 443)) {
        Serial.println("SSE: connect failed");
        return;
    }
    sseClient.print("GET /eventstream/clip/v2 HTTP/1.1\r\n"
                    "Host: " HUE_BRIDGE_HOST "\r\n"
                    "hue-application-key: " HUE_API_KEY "\r\n"
                    "Accept: text/event-stream\r\n"
                    "Connection: keep-alive\r\n\r\n");

    String hdrLine;
    unsigned long t0 = millis();
    while (millis() - t0 < 5000UL) {
        if (!sseClient.connected()) {
            Serial.println("SSE: dropped during headers");
            sseClient.stop();
            return;
        }
        while (sseClient.available()) {
            char c = sseClient.read();
            if (c == '\n') {
                if (hdrLine.length() == 0) {
                    sseLastByteMs = millis();
                    Serial.println("SSE: connected.");
                    return;
                }
                hdrLine = "";
            } else if (c != '\r') {
                hdrLine += c;
            }
        }
        delay(5);
    }
    Serial.println("SSE: header read timeout");
    sseClient.stop();
}

// Drain available SSE bytes; reconnect on disconnect or stale stream.
// Called inside the main wait loop alongside button polling.
static void sseTick() {
    if (!sseClient.connected()) {
        if (millis() - sseLastConnectMs >= SSE_RECONNECT_DELAY_MS) {
            sseLastConnectMs = millis();
            sseConnect();
        }
        return;
    }
    // Use read() instead of available()+read(): read() calls mbedtls_ssl_read()
    // which pulls new TLS records from the TCP socket. available() alone only
    // checks the already-decrypted buffer and misses buffered heartbeats.
    int b;
    while ((b = sseClient.read()) >= 0) {
        char c = (char)b;
        sseLastByteMs = millis();
        if (c == '\n') {
            if (sseBuf.length() > 0) handleSseLine(sseBuf);
            sseBuf = "";
        } else if (c != '\r') {
            sseBuf += c;
            if (sseBuf.length() > 4096) sseBuf = "";
        }
    }
    if (millis() - sseLastByteMs >= SSE_STALE_TIMEOUT_MS) {
        Serial.println("SSE: stale — reconnecting");
        sseClient.stop();
    }
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
    else if (cmd == "WIND_DOWN") {
        forceState(State::WIND_DOWN);
        if (cmdValue >= 0) windDownStep = min(cmdValue, 119);
    }
    else if (cmd == "WAKE") {
        forceState(State::WAKE);
        if (cmdValue >= 0) wakeStep = cmdValue;
    }
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

void saveLastState(float bri, uint16_t ct) {
    prefs.begin("mira", false);
    prefs.putFloat("savedBri", bri);
    prefs.putUShort("savedCt", ct);
    prefs.end();
}

void tickWakeRamp(float lux) {
    wakeStep++;
    float t = min(wakeStep / (float)WAKE_RAMP_TICKS, 1.0f);

    float rampBri = wakeStartTarget.bri + t * (wakeEndTarget.bri - wakeStartTarget.bri);
    int   rampCt  = (int)(wakeStartTarget.ct  + t * (wakeEndTarget.ct  - wakeStartTarget.ct));

    rampBri = constrain(rampBri, 1.0f, 100.0f);
    rampCt  = constrain(rampCt, (int)CT_COOL, (int)CT_WARM);

    setLight(LIGHT_UUID_BEDSIDE, true, rampBri, rampCt, 30000);
    setLight(LIGHT_UUID_DESK,    true, rampBri, rampCt, 30000);
    if (lux >= S3_LUX_HI && rampBri >= S2_BRI_LO) {
        setLight(LIGHT_UUID_CEIL_1, true, rampBri, rampCt, 30000);
        setLight(LIGHT_UUID_CEIL_2, true, rampBri, rampCt, 30000);
        overheadsOn = true;
    }

    if (wakeStep == WAKE_RAMP_TICKS / 4 || wakeStep == WAKE_RAMP_TICKS / 2 || wakeStep == (3 * WAKE_RAMP_TICKS / 4)) {
        sendLog("Wake " + String(wakeStep * 100 / WAKE_RAMP_TICKS) + "% — bri=" + String(rampBri, 1) + "% — " + getTimeString());
    }

    if (t >= 1.0f) {
        state             = State::NORMAL;
        Serial.println("Wake complete — handing off to normal.");
        sendLog("Wake complete — " + getTimeString());
    }
    sentTarget = {rampBri, (uint16_t)rampCt};

    Serial.println("Wake " + String(wakeStep) + "/" + String(WAKE_RAMP_TICKS) +
                   " — bri=" + String(rampBri, 1) + "%/" + String(wakeEndTarget.bri, 1) + "%");
}

void tickWindDown() {
    windDownStep++;

    if (overheadsOn) {
        setLight(LIGHT_UUID_CEIL_1, false, 0, 0, 1000);
        setLight(LIGHT_UUID_CEIL_2, false, 0, 0, 1000);
        overheadsOn = false;
    }

    float wdBri = windDownStartBri - (windDownStartBri - S4_FLOOR_BRI) * (windDownStep / 120.0f);
    wdBri = max(wdBri, S4_FLOOR_BRI);
    int wdCt  = (int)CT_WARM;

    Serial.println("Wind-down " + String(windDownStep * 0.5f, 1) + "/60.0 min — bri=" + String(wdBri, 1) + "%");
    if (windDownStep == 30 || windDownStep == 60 || windDownStep == 90) {
        sendLog("Wind-down " + String(windDownStep / 2) + "/60 min — bri=" + String(wdBri, 1) + "% — " + getTimeString());
    }

    setLight(LIGHT_UUID_BEDSIDE, true, wdBri, wdCt, 30000);
    if (windDownStep < 120) {
        setLight(LIGHT_UUID_DESK, true, wdBri, wdCt, 30000);
    } else {
        setLight(LIGHT_UUID_DESK, false, wdBri, wdCt, 10000);
        state = State::LOCKED_OUT;
        saveLastState(S4_FLOOR_BRI, (uint16_t)CT_WARM);
        Serial.println("Wind-down complete. Desk off, bedside at floor.");
        sendLog("Wind-down complete — " + getTimeString());
    }
    sentTarget = {wdBri, (uint16_t)wdCt};
}

void tickNormal(float lux, LightTarget target) {
    // Override detection now happens asynchronously inside the SSE handler
    // (handleLightUpdate), so tickNormal is no longer responsible for polling.

    // Compute shouldUpdate here, not in loop(), so a forceState()-driven
    // sentTarget reset (via pollDashboardCommand earlier this tick) is reflected.
    bool shouldUpdate = fabsf(target.bri - sentTarget.bri) > STATE_TOLERANCE_BRI
                     || abs(target.ct - sentTarget.ct)    > STATE_TOLERANCE_CT;

    bool newOverheadsOn = lux > S3_LUX_HI;
    if (overheadsOn && !newOverheadsOn) {
        setLight(LIGHT_UUID_CEIL_1, false, 0, 0, 500);
        setLight(LIGHT_UUID_CEIL_2, false, 0, 0, 500);
        shouldUpdate = true;
    } else if (!overheadsOn && newOverheadsOn) {
        shouldUpdate = true;
    }
    overheadsOn = newOverheadsOn;

    // Soft pause resume ramp — drift from pre-pause bri/ct to current ambient over 10 min
    if (pauseResumeActive) {
        pauseResumeStep++;
        float t       = min(pauseResumeStep / (float)PAUSE_RESUME_TICKS, 1.0f);
        float rampBri = pauseResumeStartTarget.bri + t * (target.bri - pauseResumeStartTarget.bri);
        int   rampCt  = (int)(pauseResumeStartTarget.ct  + t * (target.ct  - pauseResumeStartTarget.ct));
        rampBri       = constrain(rampBri, 1.0f, 100.0f);
        rampCt        = constrain(rampCt, (int)CT_COOL, (int)CT_WARM);
        setLight(LIGHT_UUID_BEDSIDE, true, rampBri, rampCt, 30000);
        setLight(LIGHT_UUID_DESK,    true, rampBri, rampCt, 30000);
        if (overheadsOn) {
            setLight(LIGHT_UUID_CEIL_1, true, rampBri, rampCt, 30000);
            setLight(LIGHT_UUID_CEIL_2, true, rampBri, rampCt, 30000);
        }
        sentTarget = {rampBri, (uint16_t)rampCt};
        saveLastState(rampBri, (uint16_t)rampCt);
        Serial.println("Resume ramp " + String(pauseResumeStep) + "/" + String(PAUSE_RESUME_TICKS) +
                       " — bri=" + String(rampBri, 1) + "%, ct=" + String(rampCt));
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
        sendLog("Wind-down starting — " + getTimeString());
        return;
    }

    if (shouldUpdate) {
        setLight(LIGHT_UUID_BEDSIDE, true, target.bri, target.ct, 1000);
        setLight(LIGHT_UUID_DESK,    true, target.bri, target.ct, 1000);
        if (overheadsOn) {
            setLight(LIGHT_UUID_CEIL_1, true, target.bri, target.ct, 1000);
            setLight(LIGHT_UUID_CEIL_2, true, target.bri, target.ct, 1000);
        }
        sentTarget = target;
        saveLastState(target.bri, target.ct);
        Serial.println("Bulbs updated.");
        sendLog(getTimeString() +
                    " | Lux: " + String(lux, 1) +
                    " → bri=" + String(target.bri, 1) + "%" +
                    ", ct=" + String(target.ct));
    } else {
        Serial.println("No change — skipping PUT.");
    }
}

void tickSoftPause() {
    if (millis() - softPauseStart >= SOFT_PAUSE_MS) {
        pauseResumeStartTarget = sentTarget;
        pauseResumeActive      = true;
        pauseResumeStep        = 0;
        overheadsOn            = cachedLight(LIGHT_CEIL_1).on;  // sync from actual bridge state
        lastBedsideOn          = cachedLight(LIGHT_BEDSIDE).on; // sync from actual bridge state
        state                  = State::NORMAL;
        Serial.println("Soft pause expired — beginning 10-min resume ramp.");
        sendLog("Soft pause expired — resuming — " + getTimeString());
    }
}

void triggerWake(float ambientLux) {
    LightState bedside = cachedLight(LIGHT_BEDSIDE);
    float    startBri  = (bedside.on && bedside.bri > 0.0f) ? bedside.bri : S4_FLOOR_BRI;
    uint16_t startCt   = (bedside.ct  > 0) ? (uint16_t)bedside.ct  : (uint16_t)CT_WARM;
    wakeStartTarget    = {startBri, startCt};
    wakeEndTarget      = luxToTarget(ambientLux);
    wakeStep           = 0;
    state              = State::WAKE;
    Serial.println("Wake triggered — bri " + String(startBri) + " → " + String(wakeEndTarget.bri) +
                   ", ct " + String(startCt) + " → " + String(wakeEndTarget.ct));
    sendLog("Wake sequence started — " + getTimeString());
}

void checkBedsideState(float lux) {
    LightState bedside = cachedLight(LIGHT_BEDSIDE);

    if (state == State::LOCKED_OUT && !lastBedsideOn && bedside.on) {
        triggerWake(lux);
    }

    if (lastBedsideOn && !bedside.on && timeClient.getHours() >= LOCKOUT_RESET_HOUR) {
        LightState desk  = cachedLight(LIGHT_DESK);
        LightState ceil1 = cachedLight(LIGHT_CEIL_1);
        LightState ceil2 = cachedLight(LIGHT_CEIL_2);
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
            lastBedsideOn  = cachedLight(LIGHT_BEDSIDE).on; // sync from actual bridge state
            state          = State::LOCKED_OUT;
            break;
        case State::NORMAL:
            sentTarget        = {-1.0f, 0};
            stableLuxCount    = 0;
            overheadsOn       = cachedLight(LIGHT_CEIL_1).on;  // sync from actual bridge state
            lastBedsideOn     = cachedLight(LIGHT_BEDSIDE).on; // sync from actual bridge state
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
            sendLog("Cycle button → " + String(stateName()) + " — " + getTimeString());
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
            sendLog("Hard off — " + getTimeString());
        } else {
            state         = State::LOCKED_OUT;
            lastBedsideOn = cachedLight(LIGHT_BEDSIDE).on; // sync from actual bridge state
            Serial.println("Button long press — hard off cleared.");
            sendLog("Hard off cleared — " + getTimeString());
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
            sendLog("Soft pause (button) — " + getTimeString());
        } else {
            String prev = stateName();
            state             = State::NORMAL;
            sentTarget        = {-1.0f, 0};
            stableLuxCount    = 0;
            overheadsOn       = cachedLight(LIGHT_CEIL_1).on;  // sync from actual bridge state
            lastBedsideOn     = cachedLight(LIGHT_BEDSIDE).on; // sync from actual bridge state
            Serial.println("Button short press — NORMAL (was " + prev + ").");
            sendLog("Returned to NORMAL by button — " + getTimeString());
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

    ensureBridgeCert();    // must be after NTP sync; loads or fetches the bridge TLS cert
    bootstrapLightStates(); // one-time v2 GET seeds lightCache[]; SSE keeps it fresh thereafter

    setRGB(false, false, false); // LED off — boot complete

    // Startup flash — deep purple, then restore previous state
    LightState saved = cachedLight(LIGHT_BEDSIDE);
    setLightColor(LIGHT_UUID_BEDSIDE, true, 78.7f, 48000, 200, 1000); // fade in purple over 1s
    delay(3000);                                                        // hold for 3s
    setLight(LIGHT_UUID_BEDSIDE, saved.on, saved.bri, saved.ct, 1000); // restore over 1s

    lastBedsideOn = cachedLight(LIGHT_BEDSIDE).on; // seed edge detection — prevents false wake trigger on first tick
    overheadsOn   = cachedLight(LIGHT_CEIL_1).on;  // seed from actual state — prevents false override and bad dashboard reporting

    sseConnect(); // open the persistent SSE event stream — drained in the wait loop each tick

    sendLog("Online — " + getTimeString());
}

void loop() {
    // Anchor tickStart at the very top of the loop so the wait loop subtracts
    // processing time (HTTP calls, SSE event handling, etc.) from the 30s budget,
    // yielding consistent 30s tick intervals regardless of how long processing took.
    unsigned long tickStart = millis();

    timeClient.update();
    printStatus();

    float lux = veml.readLux(VEML_LUX_AUTO);
    lastLux = lux;
    LightTarget target = luxToTarget(lux);
    Serial.print("Lux: "); Serial.print(lux, 1);
    Serial.print(" → bri="); Serial.print(target.bri, 1); Serial.print("%");
    Serial.print(", ct="); Serial.println(target.ct);

    pollDashboardCommand();

    switch (state) {
        case State::LOCKED_OUT:
            checkBedsideState(lux);
            break;

        case State::NORMAL:
            checkBedsideState(lux);
            tickNormal(lux, target);
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

    while (millis() - tickStart < 30000UL) {
        pollButton(btnMode,  BTN_MODE);
        pollButton(btnCycle, BTN_CYCLE);
        handleButtonEvents();
        handleCycleButton();
        sseTick(); // drain SSE bytes; reconnect on disconnect or stale stream
        delay(50);
    }
}