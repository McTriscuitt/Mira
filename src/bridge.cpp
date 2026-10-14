//
// bridge.cpp — Hue bridge I/O for Mira 3.0. See bridge.h.
//
// Carried over from 2.x (battle-tested, essentially unchanged): the NVS cert pin
// with the post-handshake byte compare (CA-mode pinning can't work when dialing
// the bridge by IP — see peerMatchesPinned), the persistent SSE stream drained on
// Core 0, the reconnect resync, the recent-command trajectory ring, the no-op
// filter, and stale-revert detection. New in 3.0: the classifier's job is no
// longer "should NORMAL soft-pause?" but "what did you just do?" — an on/off edge
// (yours or Mira's echo), a brightness change (yours → hold), or nothing. Color
// and color-temperature changes are deliberately not tracked (MIRA_3.md §5).
//
#include "bridge.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <mbedtls/x509_crt.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include "config.h"
#include "secrets.h"
#include "echo.h"
#include "events.h"

volatile bool holdPending = false;

// ── The roster's UUIDs, in LightIdx order ───────────────────────────────────

static const char* const LIGHT_UUIDS[LIGHT_COUNT] = {
    LIGHT_UUID_FLOOR,
    LIGHT_UUID_DRESSER,
    LIGHT_UUID_CHEST,
    LIGHT_UUID_CEIL_1,
    // LIGHT_UUID_CEIL_2,  // unused bulb — see LIGHT_CEIL_2 in lights.h
};

static int idxByUuid(const char* uuid) {
    if (!uuid) return -1;
    for (int i = 0; i < LIGHT_COUNT; i++) {
        if (strcmp(uuid, LIGHT_UUIDS[i]) == 0) return i;
    }
    return -1;
}

// ── Cache + trajectory ring (shared with the SSE task under dataMux) ────────

struct CachedLight {
    bool  on          = false;
    float bri         = 0.0f;
    int   ct          = 0;
    bool  initialized = false;
};
static CachedLight lightCache[LIGHT_COUNT];

// One slot per recent command: the trajectory from where the light was to where
// it was told to go. An event landing on a live trajectory is Mira's own echo.
struct RecentPut {
    bool          active     = false;
    bool          onTarget   = false;
    bool          priorOn    = false;
    float         priorBri   = 0.0f;
    float         targetBri  = 0.0f;
    unsigned long postedAtMs = 0;
    unsigned long durationMs = 0;
};
static RecentPut recentPuts[LIGHT_COUNT][RECENT_PUT_RING_SIZE];

static portMUX_TYPE dataMux = portMUX_INITIALIZER_UNLOCKED;

static unsigned long lastOnEdgeMs[LIGHT_COUNT] = {0};  // on-edge grace (ON_EDGE_GRACE_MS)
static unsigned long lastRevertMs[LIGHT_COUNT] = {0};  // a second revert inside the lookback isn't one

LightMask bridgeOnMask() {
    LightMask m = 0;
    portENTER_CRITICAL(&dataMux);
    for (int i = 0; i < LIGHT_COUNT; i++) if (lightCache[i].on) m |= lightBit(i);
    portEXIT_CRITICAL(&dataMux);
    return m;
}

LightSnapshot bridgeCached(int idx) {
    LightSnapshot s = {false, 0.0f, 0};
    if (idx < 0 || idx >= LIGHT_COUNT) return s;
    portENTER_CRITICAL(&dataMux);
    s.on  = lightCache[idx].on;
    s.bri = lightCache[idx].bri;
    s.ct  = lightCache[idx].ct;
    portEXIT_CRITICAL(&dataMux);
    return s;
}

// Record an outgoing command *before* it goes out — the bridge can echo back
// faster than HTTPClient returns. Returns the slot for the post-send refresh.
static int noteRecentPut(int idx, bool on, float bri, uint32_t durMs) {
    portENTER_CRITICAL(&dataMux);
    int  slot      = 0;
    bool foundFree = false;
    for (int s = 0; s < RECENT_PUT_RING_SIZE; s++) {
        const RecentPut& r = recentPuts[idx][s];
        bool expired = r.active && (millis() - r.postedAtMs > r.durationMs + RECENT_PUT_GRACE_MS);
        if (!r.active || expired) { slot = s; foundFree = true; break; }
    }
    if (!foundFree) {  // all live — overwrite the oldest
        unsigned long oldest = 0;
        for (int s = 0; s < RECENT_PUT_RING_SIZE; s++) {
            unsigned long age = millis() - recentPuts[idx][s].postedAtMs;
            if (age > oldest) { oldest = age; slot = s; }
        }
    }
    RecentPut& r = recentPuts[idx][slot];
    r.active     = true;
    r.onTarget   = on;
    r.priorOn    = lightCache[idx].on;
    // A light coming on from off fades up from ~0, not from its remembered level.
    r.priorBri   = lightCache[idx].on ? lightCache[idx].bri : 0.0f;
    r.targetBri  = on ? bri : 0.0f;
    r.postedAtMs = millis();
    r.durationMs = durMs;
    portEXIT_CRITICAL(&dataMux);
    return slot;
}

// Is this event one of our own echoes — on a live slot's path (echo.h)?
static bool matchesRecentPut(int idx, bool hasOn, bool evOn, bool hasBri, float evBri) {
    bool matched = false;
    portENTER_CRITICAL(&dataMux);
    unsigned long now = millis();
    for (int s = 0; s < RECENT_PUT_RING_SIZE; s++) {
        RecentPut& r = recentPuts[idx][s];
        if (!r.active) continue;
        unsigned long age = now - r.postedAtMs;
        if (age > r.durationMs + RECENT_PUT_GRACE_MS) { r.active = false; continue; }
        if (hasOn && evOn != r.onTarget) continue;
        if (hasBri && !echoOnPath(r.priorBri, r.targetBri, age, r.durationMs, evBri)) continue;
        matched = true;
        break;
    }
    portEXIT_CRITICAL(&dataMux);
    return matched;
}

// Stale-revert fingerprint: the event lands exactly where one of our recent
// commands *started* — the bulb never applied it and the bridge is walking back
// its optimistic echo (observed ~35 s out on the Signe, June 11). Expired slots
// count: the fingerprint outlives the trajectory window.
static bool matchesPriorPut(int idx, bool hasOn, bool evOn, bool hasBri, float evBri) {
    bool matched = false;
    portENTER_CRITICAL(&dataMux);
    for (int s = 0; s < RECENT_PUT_RING_SIZE; s++) {
        const RecentPut& r = recentPuts[idx][s];
        if (r.postedAtMs == 0) continue;
        if (millis() - r.postedAtMs > STALE_REVERT_LOOKBACK_MS) continue;
        if (hasOn && evOn != r.priorOn) continue;
        if (!hasOn && !r.priorOn) continue;  // a brightness revert needs a prior that was on
        if (hasBri && fabsf(evBri - r.priorBri) > TRAJECTORY_TOLERANCE_BRI) continue;
        matched = true;
        break;
    }
    portEXIT_CRITICAL(&dataMux);
    return matched;
}

// One re-send per lookback window. A second revert-shaped event inside it is
// treated as genuine: for on/off, your doing (never fight it twice); for
// brightness, a deaf bulb we stop chasing until the next command.
static bool revertAllowed(int idx) {
    return lastRevertMs[idx] == 0 || millis() - lastRevertMs[idx] > STALE_REVERT_LOOKBACK_MS;
}

void bridgeSeedTrajectory(int idx, float b0, float b1, uint32_t elapsedMs, uint32_t durMs) {
    if (idx < 0 || idx >= LIGHT_COUNT || elapsedMs >= durMs + RECENT_PUT_GRACE_MS) return;
    portENTER_CRITICAL(&dataMux);
    RecentPut& r = recentPuts[idx][0];
    r.active     = true;
    r.onTarget   = true;
    r.priorOn    = true;
    r.priorBri   = b0;
    r.targetBri  = b1;
    r.postedAtMs = millis() - elapsedMs;
    r.durationMs = durMs;
    portEXIT_CRITICAL(&dataMux);
}

// ── Diagnostics: every classified event, kept for the hold dump ─────────────

enum class Outcome : uint8_t {
    UnknownUuid = 0,
    NoFields,        // nothing Mira tracks (color, effects, ...)
    CtOnly,          // a CT change — not tracked in 3.0
    NoChange,        // "on" restating what we knew
    EdgeMira,        // on/off echo of our own command
    EdgeUser,        // you switched a light
    OffLightBri,     // dimming reported for a light that's off (presets)
    OnGrace,         // brightness right after a turn-on — part of the turn-on
    EchoMatch,       // brightness on a live trajectory
    NoOpEcho,        // brightness already matching the cache
    StaleRevert,     // the bridge walked back one of our commands → re-send
    RevertIgnored,   // a second revert inside the lookback — not chased
    UserBrightness,  // you changed brightness → hold
    SyntheticEdge,   // stream-resync diff
    COUNT
};

static const char* outcomeName(Outcome o) {
    switch (o) {
        case Outcome::UnknownUuid:    return "unknown-uuid";
        case Outcome::NoFields:       return "no-fields";
        case Outcome::CtOnly:         return "ct-only";
        case Outcome::NoChange:       return "no-change";
        case Outcome::EdgeMira:       return "edge-mira";
        case Outcome::EdgeUser:       return "EDGE-USER";
        case Outcome::OffLightBri:    return "off-light-bri";
        case Outcome::OnGrace:        return "on-grace";
        case Outcome::EchoMatch:      return "echo-match";
        case Outcome::NoOpEcho:       return "noop-echo";
        case Outcome::StaleRevert:    return "stale-revert";
        case Outcome::RevertIgnored:  return "revert-ignored";
        case Outcome::UserBrightness: return "USER-BRIGHTNESS";
        case Outcome::SyntheticEdge:  return "synthetic-edge";
        default:                      return "?";
    }
}

struct SseEventRecord {
    unsigned long timeMs   = 0;
    int           idx      = -1;
    bool          hasOn    = false;
    bool          evOn     = false;
    bool          hasBri   = false;
    float         evBri    = 0.0f;
    bool          cacheOn  = false;
    float         cacheBri = 0.0f;
    Outcome       outcome  = Outcome::NoFields;
};
static SseEventRecord sseEventRing[SSE_EVENT_RING_SIZE];
static int            sseEventRingHead = 0;
static unsigned long  outcomeCounts[(int)Outcome::COUNT] = {0};

static void recordEvent(int idx, bool hasOn, bool evOn, bool hasBri, float evBri,
                        bool cacheOn, float cacheBri, Outcome o) {
    SseEventRecord& r = sseEventRing[sseEventRingHead];
    r.timeMs   = millis();
    r.idx      = idx;
    r.hasOn    = hasOn;  r.evOn  = evOn;
    r.hasBri   = hasBri; r.evBri = evBri;
    r.cacheOn  = cacheOn;
    r.cacheBri = cacheBri;
    r.outcome  = o;
    sseEventRingHead = (sseEventRingHead + 1) % SSE_EVENT_RING_SIZE;
    outcomeCounts[(int)o]++;
#ifdef ECHO_TRACE
    Serial.printf("SSE %s on=%c%d bri=%c%.1f cache[on=%d bri=%.1f] -> %s\n",
                  idx >= 0 ? lightName(idx) : "?", hasOn ? '+' : ' ', evOn ? 1 : 0,
                  hasBri ? '+' : ' ', evBri, cacheOn ? 1 : 0, cacheBri, outcomeName(o));
#endif
}

void bridgeDumpDiagnostics(int idx) {
    if (idx < 0 || idx >= LIGHT_COUNT) return;
    Serial.printf("Hold diagnostic — %s\n", lightName(idx));
    for (int s = 0; s < RECENT_PUT_RING_SIZE; s++) {
        const RecentPut& r = recentPuts[idx][s];
        if (r.postedAtMs == 0) continue;
        Serial.printf("  recent[%d] %s on=%d %.1f→%.1f age=%lu/%lu\n", s, r.active ? "live" : "done",
                      r.onTarget ? 1 : 0, r.priorBri, r.targetBri,
                      millis() - r.postedAtMs, r.durationMs);
    }
    int shown = 0;
    for (int k = 1; k <= SSE_EVENT_RING_SIZE && shown < 6; k++) {
        const SseEventRecord& r = sseEventRing[(sseEventRingHead - k + SSE_EVENT_RING_SIZE) % SSE_EVENT_RING_SIZE];
        if (r.timeMs == 0 || r.idx != idx) continue;
        Serial.printf("  t-%lums", millis() - r.timeMs);
        if (r.hasOn)  Serial.printf(" on=%d", r.evOn ? 1 : 0);
        if (r.hasBri) Serial.printf(" bri=%.1f", r.evBri);
        Serial.printf(" (cache on=%d bri=%.1f) -> %s\n", r.cacheOn ? 1 : 0, r.cacheBri, outcomeName(r.outcome));
        shown++;
    }
    Serial.print("  counts:");
    for (int o = 0; o < (int)Outcome::COUNT; o++) {
        if (outcomeCounts[o]) Serial.printf(" %s=%lu", outcomeName((Outcome)o), outcomeCounts[o]);
    }
    Serial.println();
}

// ── TLS cert pin (2.x R3, unchanged) ────────────────────────────────────────

static Preferences prefs;
static String      bridgeCertPem;  // pinned bridge cert, loaded from NVS

// Encode DER bytes as a PEM certificate string.
static String derToPem(const uint8_t* der, size_t len) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    String out = "-----BEGIN CERTIFICATE-----\n";
    size_t lineLen = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)der[i] << 16;
        if (i + 1 < len) v |= (uint32_t)der[i + 1] << 8;
        if (i + 2 < len) v |= (uint32_t)der[i + 2];
        out += b64[(v >> 18) & 63];
        out += b64[(v >> 12) & 63];
        out += (i + 1 < len) ? b64[(v >> 6) & 63] : '=';
        out += (i + 2 < len) ? b64[v & 63]        : '=';
        lineLen += 4;
        if (lineLen >= 64) { out += '\n'; lineLen = 0; }
    }
    if (lineLen > 0) out += '\n';
    out += "-----END CERTIFICATE-----\n";
    return out;
}

// mbedtls_x509_time (UTC) → Unix timestamp, for the NVS expiry.
static unsigned long certTimeToEpoch(const mbedtls_x509_time& t) {
    static const int dom[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    unsigned long days = 0;
    for (int y = 1970; y < t.year; y++) {
        bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        days += leap ? 366 : 365;
    }
    for (int m = 1; m < t.mon; m++) {
        days += dom[m - 1];
        if (m == 2) {
            bool leap = (t.year % 4 == 0 && (t.year % 100 != 0 || t.year % 400 == 0));
            if (leap) days++;
        }
    }
    days += t.day - 1;
    return days * 86400UL + t.hour * 3600UL + t.min * 60UL + t.sec;
}

// CA-style pinning (setCACert) can't handshake here: we dial the bridge by IP,
// its cert's CN is the bridge ID, and the core always verifies the hostname when
// a CA is set (found live July 15). So the pin is a byte compare of the peer cert
// against the NVS copy, after the handshake and before anything is sent — on the
// boot probe and the SSE stream. HTTPClient paths transmit on connect, so they run
// insecure-mode TLS and inherit the boot-time identity check. With nothing pinned
// yet (failed first fetch): bootstrap trust.
static bool peerMatchesPinned(WiFiClientSecure& client) {
    if (!bridgeCertPem.length()) return true;
    const mbedtls_x509_crt* peer = client.getPeerCertificate();
    if (!peer) return false;
    return derToPem(peer->raw.p, peer->raw.len) == bridgeCertPem;
}

static bool fetchAndStoreBridgeCert() {
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10);
    if (!client.connect(HUE_BRIDGE_HOST, 443)) {
        Serial.println("Cert fetch: connect failed");
        return false;
    }
    client.print("GET /clip/v2/resource/light HTTP/1.0\r\n"
                 "Host: " HUE_BRIDGE_HOST "\r\n"
                 "hue-application-key: " HUE_API_KEY "\r\n"
                 "Connection: close\r\n\r\n");
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
    String        pem    = derToPem(cert->raw.p, cert->raw.len);
    unsigned long expiry = certTimeToEpoch(cert->valid_to);
    client.stop();

    prefs.begin("mira", false);
    prefs.putString("hueCert", pem);
    prefs.putULong("hueCertExpiry", expiry);
    prefs.end();
    bridgeCertPem = pem;
    Serial.printf("Bridge cert stored, expires %lu\n", expiry);
    return true;
}

// Load the pinned cert; re-fetch if missing, expired, within 30 days of expiry,
// or no longer matching the live bridge. Needs the clock (runs after SNTP).
static void ensureBridgeCert() {
    prefs.begin("mira", true);
    String        stored = prefs.getString("hueCert", "");
    unsigned long expiry = prefs.getULong("hueCertExpiry", 0);
    prefs.end();

    unsigned long now      = (unsigned long)time(nullptr);
    bool          needFetch = stored.isEmpty() || now >= expiry || (expiry - now < 30UL * 24 * 3600);
    if (!needFetch) {
        bridgeCertPem = stored;
        WiFiClientSecure probe;
        probe.setInsecure();
        probe.setTimeout(5);
        bool ok = probe.connect(HUE_BRIDGE_HOST, 443) && peerMatchesPinned(probe);
        probe.stop();
        if (ok) {
            Serial.println("Bridge cert loaded from NVS.");
            return;
        }
        Serial.println("Bridge cert NVS verify failed — re-fetching...");
    } else {
        Serial.println("Bridge cert missing or expiring — fetching...");
    }
    if (!fetchAndStoreBridgeCert() && !stored.isEmpty()) {
        bridgeCertPem = stored;
        Serial.println("Cert fetch failed — using stored cert.");
    }
}

// ── Bootstrap / resync ──────────────────────────────────────────────────────
// One v2 GET seeds every light. resync=false: at boot, before the stream exists.
// resync=true: from the SSE task after every reconnect — events during the gap
// are gone, so each on/off flip we missed becomes a synthetic edge (yours, unless
// it lands on one of our live trajectories).
static void bootstrapLightStates(bool resync) {
    WiFiClientSecure client;
    client.setInsecure();  // HTTPClient transmits on connect — see peerMatchesPinned
    HTTPClient http;
    http.begin(client, String(HUE_V2_BASE_URL) + "/resource/light");
    http.addHeader("hue-application-key", HUE_API_KEY);
    int code = http.GET();
    if (code != 200) {
        Serial.printf("bootstrap: HTTP %d\n", code);
        http.end();
        return;
    }
    String body = http.getString();
    http.end();

    JsonDocument filter;
    filter["data"][0]["id"]                         = true;
    filter["data"][0]["on"]["on"]                   = true;
    filter["data"][0]["dimming"]["brightness"]      = true;
    filter["data"][0]["color_temperature"]["mirek"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
        Serial.println("bootstrap: JSON parse failed");
        return;
    }
    for (JsonObject light : doc["data"].as<JsonArray>()) {
        int idx = idxByUuid(light["id"]);
        if (idx < 0) continue;
        bool  on  = light["on"]["on"] | false;
        float bri = light["dimming"]["brightness"] | 0.0f;
        int   ct  = light["color_temperature"]["mirek"] | 0;

        portENTER_CRITICAL(&dataMux);
        bool  prevOn  = lightCache[idx].on;
        float prevBri = lightCache[idx].bri;
        bool  wasInit = lightCache[idx].initialized;
        lightCache[idx].on          = on;
        lightCache[idx].bri         = bri;
        if (ct) lightCache[idx].ct  = ct;
        lightCache[idx].initialized = true;
        portEXIT_CRITICAL(&dataMux);

        if (!resync) {
            Serial.printf("Bootstrap %s — on=%d bri=%.1f ct=%d\n", lightName(idx), on ? 1 : 0, bri, ct);
        } else if (wasInit && on != prevOn) {
            bool mine = matchesRecentPut(idx, true, on, false, 0.0f);
            Serial.printf("SSE resync: %s went %s while the stream was down (%s).\n",
                          lightName(idx), on ? "on" : "off", mine ? "Mira's" : "yours");
            recordEvent(idx, true, on, false, 0.0f, prevOn, prevBri, Outcome::SyntheticEdge);
            if (on) lastOnEdgeMs[idx] = millis();
            Event ev;
            ev.type  = EvType::LightEdge;
            ev.a     = (uint8_t)idx;
            ev.flags = (uint8_t)((on ? 1 : 0) | (mine ? 0 : 2));
            ev.f     = bri;
            ev.tMs   = millis();
            postEvent(ev);
        }
    }
}

// ── Commands ────────────────────────────────────────────────────────────────

static int httpPut(int idx, const char* body) {
    WiFiClientSecure client;
    client.setInsecure();  // HTTPClient transmits on connect — see peerMatchesPinned
    HTTPClient http;
    char url[160];
    snprintf(url, sizeof(url), "%s/resource/light/%s", HUE_V2_BASE_URL, LIGHT_UUIDS[idx]);
    http.begin(client, url);
    http.setTimeout(4000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("hue-application-key", HUE_API_KEY);
    int code = http.PUT((uint8_t*)body, strlen(body));
    if (code > 0 && code != 200) {
        String resp = http.getString();
        Serial.printf("  bridge said: %s\n", resp.substring(0, 160).c_str());
    }
    http.end();
    return code;
}

BridgeResult bridgePut(int idx, bool on, float bri, uint16_t ct, uint32_t durMs) {
    // A brightness change you just made is waiting for the main task: sending the
    // rest of a batch now would land on top of it (2.x's mid-batch abort).
    if (holdPending) return BridgeResult::Aborted;
    if (idx < 0 || idx >= LIGHT_COUNT || WiFi.status() != WL_CONNECTED) return BridgeResult::Failed;

    char body[200];
    if (on) {
        snprintf(body, sizeof(body),
                 "{\"on\":{\"on\":true},\"dimming\":{\"brightness\":%.1f},"
                 "\"color_temperature\":{\"mirek\":%u},\"dynamics\":{\"duration\":%lu}}",
                 bri, (unsigned)ct, (unsigned long)durMs);
    } else {
        snprintf(body, sizeof(body), "{\"on\":{\"on\":false},\"dynamics\":{\"duration\":%lu}}",
                 (unsigned long)durMs);
    }

    int slot = noteRecentPut(idx, on, bri, durMs);  // before sending — echoes can beat the reply
    int code = httpPut(idx, body);
    // The grace window counts from when the command actually went out.
    portENTER_CRITICAL(&dataMux);
    recentPuts[idx][slot].postedAtMs = millis();
    portEXIT_CRITICAL(&dataMux);

    if (on) Serial.printf("PUT %s → %.1f%% %umk over %.1fs : %d\n", lightName(idx), bri, (unsigned)ct, durMs / 1000.0f, code);
    else    Serial.printf("PUT %s → off over %.1fs : %d\n", lightName(idx), durMs / 1000.0f, code);
    // 207: accepted with warnings (e.g. a "soft off" note) — the command still went in.
    return (code == 200 || code == 207) ? BridgeResult::Sent : BridgeResult::Failed;
}

void bridgePreset(int idx, float bri, uint16_t ct, uint32_t durMs) {
    if (holdPending || idx < 0 || idx >= LIGHT_COUNT || WiFi.status() != WL_CONNECTED) return;
    char body[160];
    snprintf(body, sizeof(body),
             "{\"dimming\":{\"brightness\":%.1f},\"color_temperature\":{\"mirek\":%u},"
             "\"dynamics\":{\"duration\":%lu}}",
             bri, (unsigned)ct, (unsigned long)durMs);
    int code = httpPut(idx, body);
    Serial.printf("PRESET %s (off) → %.1f%% over %.1fs : %d\n", lightName(idx), bri, durMs / 1000.0f, code);
}

// Hue v1 HSB (hue 0–65535, sat 0–254) → CIE xy for the v2 API.
static void hsbToXY(int hueV1, int satV1, float& x, float& y) {
    float h  = hueV1 * 360.0f / 65535.0f;
    float s  = satV1 / 254.0f;
    int   hi = (int)(h / 60.0f) % 6;
    float f  = h / 60.0f - (int)(h / 60.0f);
    float p  = 1.0f - s;
    float q  = 1.0f - f * s;
    float t  = 1.0f - (1.0f - f) * s;
    float r, g, b;
    switch (hi) {
        case 0:  r = 1; g = t; b = p; break;
        case 1:  r = q; g = 1; b = p; break;
        case 2:  r = p; g = 1; b = t; break;
        case 3:  r = p; g = q; b = 1; break;
        case 4:  r = t; g = p; b = 1; break;
        default: r = 1; g = p; b = q; break;
    }
    auto gamma = [](float c) -> float {
        return c > 0.04045f ? powf((c + 0.055f) / 1.055f, 2.4f) : c / 12.92f;
    };
    r = gamma(r); g = gamma(g); b = gamma(b);
    float X = r * 0.4124f + g * 0.3576f + b * 0.1805f;
    float Y = r * 0.2126f + g * 0.7152f + b * 0.0722f;
    float Z = r * 0.0193f + g * 0.1192f + b * 0.9505f;
    float sum = X + Y + Z;
    x = (sum > 0.0f) ? X / sum : 0.3f;
    y = (sum > 0.0f) ? Y / sum : 0.3f;
}

void bridgeFlourish(int idx) {
    if (idx < 0 || idx >= LIGHT_COUNT) return;
    LightSnapshot saved = bridgeCached(idx);
    float cx, cy;
    hsbToXY(48000, 200, cx, cy);  // deep purple
    char body[200];
    snprintf(body, sizeof(body),
             "{\"on\":{\"on\":true},\"dimming\":{\"brightness\":78.7},"
             "\"color\":{\"xy\":{\"x\":%.4f,\"y\":%.4f}},\"dynamics\":{\"duration\":1000}}",
             cx, cy);
    noteRecentPut(idx, true, 78.7f, 1000);
    Serial.printf("Flourish %s : %d\n", lightName(idx), httpPut(idx, body));
    delay(3000);
    int ct = saved.ct > 0 ? saved.ct : CT_WARM;
    bridgePut(idx, saved.on, saved.bri, (uint16_t)ct, 1000);  // back the way it was
}

// ── The event stream (Core-0 task) ──────────────────────────────────────────

static WiFiClientSecure sseClient;
static char             sseLine[4096];
static size_t           sseLen           = 0;
static bool             sseOverflow      = false;
static unsigned long    sseLastByteMs    = 0;
static unsigned long    sseLastConnectMs = 0;
static TaskHandle_t     sseTaskHandle    = nullptr;
static const unsigned long SSE_RECONNECT_DELAY_MS = 5000UL;
static const unsigned long SSE_STALE_TIMEOUT_MS   = 600000UL;  // Hue v2 sends no keepalive

uint32_t bridgeSseStackFree() {
    return sseTaskHandle ? (uint32_t)uxTaskGetStackHighWaterMark(sseTaskHandle) : 0;
}

// Classify one light update. Uses only this event's fields — the bridge splits
// combined changes across events, so a cache-merged view would invent changes.
// The cache is written after classification.
static void handleLightUpdate(JsonObjectConst upd) {
    int idx = idxByUuid(upd["id"]);
    if (idx < 0) {
        recordEvent(-1, false, false, false, 0.0f, false, 0.0f, Outcome::UnknownUuid);
        return;
    }
    JsonVariantConst onF  = upd["on"]["on"];
    JsonVariantConst briF = upd["dimming"]["brightness"];
    JsonVariantConst ctF  = upd["color_temperature"]["mirek"];  // null while in color mode
    bool  hasOn  = !onF.isNull();
    bool  hasBri = !briF.isNull();
    bool  hasCt  = !ctF.isNull();
    bool  evOn   = hasOn  ? onF.as<bool>()   : false;
    float evBri  = hasBri ? briF.as<float>() : 0.0f;
    int   evCt   = hasCt  ? ctF.as<int>()    : 0;

    portENTER_CRITICAL(&dataMux);
    bool  cOn  = lightCache[idx].on;
    float cBri = lightCache[idx].bri;
    int   cCt  = lightCache[idx].ct;
    portEXIT_CRITICAL(&dataMux);

    Outcome o;
    if (!hasOn && !hasBri) {
        o = hasCt ? Outcome::CtOnly : Outcome::NoFields;
    } else if (hasOn && evOn != cOn) {
        // An on/off edge (brightness riding in the same event is part of it):
        // Mira's own echo, or yours. No stale-revert reading here — in 3.0 your
        // on/off is a strong signal (out for today, nap, wake), and switching off
        // the ceiling that just came on at 10:00 looks exactly like the bridge
        // walking back our "on". Never fight you over it; a bulb that truly missed
        // a command just stays as it is until you flip it.
        o = matchesRecentPut(idx, true, evOn, false, 0.0f) ? Outcome::EdgeMira : Outcome::EdgeUser;
        if (evOn) lastOnEdgeMs[idx] = millis();
    } else if (hasBri) {
        if (!cOn)                                                   o = Outcome::OffLightBri;
        else if (millis() - lastOnEdgeMs[idx] < ON_EDGE_GRACE_MS)   o = Outcome::OnGrace;
        else if (matchesRecentPut(idx, false, false, true, evBri))  o = Outcome::EchoMatch;
        else if (fabsf(evBri - cBri) <= TRAJECTORY_TOLERANCE_BRI)   o = Outcome::NoOpEcho;
        else if (matchesPriorPut(idx, false, false, true, evBri))   o = revertAllowed(idx) ? Outcome::StaleRevert
                                                                                           : Outcome::RevertIgnored;
        else                                                        o = Outcome::UserBrightness;
    } else {
        o = Outcome::NoChange;
    }

    portENTER_CRITICAL(&dataMux);
    if (hasOn)  lightCache[idx].on  = evOn;
    if (hasBri) lightCache[idx].bri = evBri;
    if (hasCt)  lightCache[idx].ct  = evCt;
    portEXIT_CRITICAL(&dataMux);

    recordEvent(idx, hasOn, evOn, hasBri, evBri, cOn, cBri, o);

    Event ev;
    ev.a   = (uint8_t)idx;
    ev.tMs = millis();
    switch (o) {
        case Outcome::EdgeMira:
        case Outcome::EdgeUser:
            ev.type  = EvType::LightEdge;
            ev.flags = (uint8_t)((evOn ? 1 : 0) | (o == Outcome::EdgeUser ? 2 : 0));
            ev.f     = hasBri ? evBri : cBri;
            break;
        case Outcome::StaleRevert:
            lastRevertMs[idx] = millis();
            Serial.printf("SSE: %s reverted to its pre-command state — queueing a re-send\n", lightName(idx));
            ev.type  = EvType::StaleRevert;
            ev.flags = (uint8_t)((hasOn ? evOn : cOn) ? 1 : 0);
            break;
        case Outcome::UserBrightness:
            holdPending = true;  // synchronous half: any PUT the main task is about to send bails
            Serial.printf("SSE: you set %s to %.1f%% — hold\n", lightName(idx), evBri);
            ev.type = EvType::UserBrightness;
            ev.f    = evBri;
            ev.i    = hasCt ? evCt : cCt;
            break;
        default:
            return;
    }
    postEvent(ev);
}

// One SSE `data:` payload: an array of events, each with an array of updates.
static void handleSseEventData(const char* json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return;
    for (JsonObject event : doc.as<JsonArray>()) {
        if (strcmp(event["type"] | "", "update") != 0) continue;
        for (JsonObject upd : event["data"].as<JsonArray>()) {
            if (strcmp(upd["type"] | "", "light") != 0) continue;
            handleLightUpdate(upd);
        }
    }
}

static void sseConnect() {
    sseClient.stop();
    sseLen = 0;
    sseOverflow = false;
    sseClient.setInsecure();
    sseClient.setTimeout(5);
    if (!sseClient.connect(HUE_BRIDGE_HOST, 443)) {
        Serial.println("SSE: connect failed");
        return;
    }
    // The pin check, before anything (the application key included) is sent.
    if (!peerMatchesPinned(sseClient)) {
        Serial.println("SSE: bridge cert pin mismatch — dropping connection");
        sseClient.stop();
        return;
    }
    sseClient.print("GET /eventstream/clip/v2 HTTP/1.1\r\n"
                    "Host: " HUE_BRIDGE_HOST "\r\n"
                    "hue-application-key: " HUE_API_KEY "\r\n"
                    "Accept: text/event-stream\r\n"
                    "Connection: keep-alive\r\n\r\n");
    String        hdr;
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
                if (hdr.length() == 0) {
                    sseLastByteMs = millis();
                    Serial.println("SSE: connected.");
                    return;
                }
                hdr = "";
            } else if (c != '\r') {
                hdr += c;
            }
        }
        delay(5);
    }
    Serial.println("SSE: header read timeout");
    sseClient.stop();
}

static void sseTick() {
    if (!sseClient.connected()) {
        if (millis() - sseLastConnectMs >= SSE_RECONNECT_DELAY_MS) {
            sseLastConnectMs = millis();
            sseConnect();
            if (sseClient.connected()) bootstrapLightStates(true);  // catch what the gap hid
        }
        return;
    }
    // read() (not available()+read()): it pulls new TLS records off the socket.
    int b;
    while ((b = sseClient.read()) >= 0) {
        char c = (char)b;
        sseLastByteMs = millis();
        if (c == '\n') {
            if (sseOverflow) {
                Serial.println("SSE: line over 4 KB dropped");
                sseOverflow = false;
            } else if (sseLen > 0) {
                sseLine[sseLen] = '\0';
                if (strncmp(sseLine, "data:", 5) == 0) {
                    const char* p = sseLine + 5;
                    while (*p == ' ') p++;
                    handleSseEventData(p);
                }
            }
            sseLen = 0;
        } else if (c != '\r') {
            if (sseLen < sizeof(sseLine) - 1) sseLine[sseLen++] = c;
            else sseOverflow = true;
        }
    }
    if (millis() - sseLastByteMs >= SSE_STALE_TIMEOUT_MS) {
        Serial.println("SSE: stale — reconnecting");
        sseClient.stop();
    }
}

static void sseTask(void*) {
    for (;;) {
        sseTick();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

void bridgeBegin() {
    ensureBridgeCert();
    bootstrapLightStates(false);
}

void bridgeStart() {
    sseConnect();  // first handshake here; the task owns the stream from now on
    // Anything switched between the boot snapshot and this connect (the flourish
    // takes ~3 s) would otherwise go unseen until the next reconnect.
    if (sseClient.connected()) bootstrapLightStates(true);
    xTaskCreatePinnedToCore(sseTask, "sse", 12288, nullptr, 1, &sseTaskHandle, 0);
    Serial.println("SSE task started on core 0.");
}
