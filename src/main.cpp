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
// Chest is now a conditionally-driven lamp like the overheads: on only above the
// S3_LUX_HI (250) curve breakpoint, off below it (floor+dresser carry the dim band).
// Firmware *intent*, not physical state — edge-detected in tickNormal and synced from
// cache on every NORMAL/LOCKED_OUT re-entry, exactly like overheadsOn.
bool chestOn = true;

// Per-light cycle exclusion (dashboard-toggled, separate from soft pause). A light
// marked excluded is turned off once and then dropped from every driving state's PUTs
// until it's re-included or the daily lockout re-arm clears it. Exclusion does NOT
// suppress override detection — a manual Hue-app change to an excluded light still
// fires the global soft pause (see expectedOn handling in handleLightUpdate). Indexed
// by the LIGHT_* cache indices.
bool excludedLight[LIGHT_COUNT] = {false};

// Phase 3 — light state cache. Mirror of bridge state; bootstrapped via v2 GET at startup,
// kept in sync by SSE events. Replaces the old per-tick HTTP polling of getLightState().
struct CachedLight {
    bool  on          = false;
    float bri         = 0.0f;
    int   ct          = 0;
    bool  initialized = false;
};
CachedLight lightCache[LIGHT_COUNT]; // indexed by LIGHT_CHEST / LIGHT_DRESSER / LIGHT_CEIL_1 / LIGHT_CEIL_2 / LIGHT_FLOOR

// [buttons removed 2026-06 — see N1_MIGRATION.md] Physical buttons retired per
// the June 11 decision; code commented out (not deleted) so the hardware can be
// re-activated. EvType::ButtonPress stays in the event enum for the future.
// struct ButtonState {
//     bool debounced       = false;
//     bool held            = false;
//     bool shortPress      = false;
//     bool longPress       = false;
//     bool longFired       = false;
//     unsigned long lastDebounceMs = 0;
//     unsigned long pressStartMs   = 0;
// };
//
// ButtonState btnMode;
// ButtonState btnCycle;

float lastLux = 1.0f; // last lux reading — seeds triggerWake() when forceState(WAKE) fires between ticks

enum class State { LOCKED_OUT, NORMAL, WAKE, WIND_DOWN, SOFT_PAUSE, HARD_OFF };
// Written ONLY by the main task (dispatcher / tick paths). Read cross-core by
// the Core-0 SSE task during event classification, hence volatile — the SSE
// task tolerates a stale-by-microseconds read, but must not see a
// register-cached one. (N1 Stage 1 — see Markdowns/N1_MIGRATION.md.)
volatile State state = State::NORMAL;

// Wind-down state
int   stableLuxCount  = 0;      // weighted counter: increments when lux ≤ 10 after WIND_DOWN_GATE_HOUR, decrements otherwise (floor 0)
int   windDownStep    = 0;      // 0–120 (120 steps × 30 s = 60 min)
float windDownStartBri = 0.0f;  // bri at the moment wind-down triggered

// Wake sequence state
int         wakeStep        = 0;
LightTarget wakeStartTarget = {0, 0};
LightTarget wakeEndTarget   = {0, 0};

// Soft pause state
unsigned long softPauseStart = 0;
// Live (mutable) pause length. Seeded to SOFT_PAUSE_MS on every soft-pause entry,
// then adjustable at runtime: the dashboard "extend" button grows it (+10 min/press)
// and the remaining-time slider rewrites it. Kept separate from the fixed
// SOFT_PAUSE_MS so an extend past 60 min just enlarges the target rather than
// pushing softPauseStart into the future (which would wrap the unsigned millis()
// subtraction and fire an immediate resume). TIMER_PAUSE_EXPIRY (via
// syncSoftTimers) and queueDashboardStatus() measure against this, not the constant.
unsigned long softPauseDurationMs = SOFT_PAUSE_MS;

// Soft pause resume ramp
// volatile: read by the Core-0 SSE task (SkipPauseResume classification).
volatile bool pauseResumeActive    = false;
int         pauseResumeStep        = 0;
LightTarget pauseResumeStartTarget = {0, 0}; // bri/ct snapshot at soft pause entry — interpolated on resume

// Recent-PUT trajectory record — per-light ring (slot count below). Replaces
// the old time-window override mute. Each outgoing PUT snapshots its
// (prior, target) trajectory into one slot so the SSE handler can discriminate
// self-PUT echoes (events that land on the trajectory) from external overrides
// (events that don't). Closed-loop: depends only on what we just told the
// bridge, not on bridge metadata. Per-light, so an in-flight chest-lamp PUT can't
// mask a real override on the overheads. A no-op event filter in
// handleLightUpdate() catches late settling echoes that arrive after the slot's
// grace expires (see Markdowns/SSE.md).
struct RecentPut {
    bool          active     = false;
    bool          onTarget   = false;
    bool          priorOn    = false;
    float         priorBri   = 0.0f;
    float         targetBri  = 0.0f;
    int           priorCt    = 0;
    int           targetCt   = 0;
    unsigned long postedAtMs = 0;
    unsigned long durationMs = 0;
};
// 3-slot ring per light. Lets multiple in-flight PUTs (a wake-ramp tick with
// 30 s dynamics overlapping a state-transition PUT, or a lux-driven PUT firing
// right after an overhead toggle) keep their trajectories live simultaneously
// so a late echo from any of them can still match. noteRecentPut() prefers an
// inactive/expired slot; if all three are live, the oldest is overwritten.
const int RECENT_PUT_RING_SIZE = 3;
RecentPut recentPuts[LIGHT_COUNT][RECENT_PUT_RING_SIZE];
const unsigned long RECENT_PUT_GRACE_MS = 5000UL; // post-ramp slack for late echoes (Zigbee mesh can be slow)
// Stale-revert lookback. The bridge echoes a PUT optimistically, before the lamp
// confirms over Zigbee; if the lamp never applies it, the bridge walks its
// resource state back at its next lamp poll — observed ~35 s out, far past any
// slot's duration+grace window. An event landing on a slot's *prior* values
// within this window is the lamp reverting to its pre-PUT state, not a user
// override (a user lands on arbitrary values; a revert lands exactly where our
// own PUT started). Expired slots keep their data, so the fingerprint outlives
// the trajectory window.
const unsigned long STALE_REVERT_LOOKBACK_MS = 90000UL;
// Trajectory-match tolerances, intentionally wider than STATE_TOLERANCE_*:
// - STATE_TOLERANCE_* sizes "is the curve drifted enough to send a new PUT?"
// - TRAJECTORY_TOLERANCE_* sizes "is the bridge's settled value within the slop
//   we expect from Zigbee/bulb-side quantization?" Bulbs snap to discrete bri/ct
//   steps (~1-2% bri, ~5-10 mirek), and the cached priorBri/Ct may itself be a
//   step off the bulb's real pre-PUT state. A user-driven override is typically
//   double-digit percent or 50+ mirek, well outside this window.
const float TRAJECTORY_TOLERANCE_BRI = 5.0f;
const int   TRAJECTORY_TOLERANCE_CT  = 15;

// ── Echo-discrimination diagnostics ─────────────────────────────────────────
// Every SSE light event is classified into one of these outcomes. Counts are
// kept since boot and included in the override-fire dashboard dump so we can
// see the *distribution* of decisions, not just the one that fired SOFT_PAUSE.
// Uncomment ECHO_TRACE for verbose per-event Serial output during tuning.
// (Was on July 15–16 for the Stage 3 edge-dispatch soak; off again at the
// Stage 4 flash per plan.)
// #define ECHO_TRACE 1

enum class EchoOutcome : uint8_t {
    UnknownUuid = 0,    // event for a UUID outside lightCache[]
    NoFields,           // event payload had no on/bri/ct
    SkipState,          // state != NORMAL — override only checked in NORMAL
    SkipPauseResume,    // pauseResumeActive — resume ramp suppresses detection
    SkipOffLight,       // expectedOn == false for this idx
    EchoMatch,          // trajectory match against a live recentPuts slot
    NoOpEcho,           // trajectory missed but event delta from cache is <= TRAJECTORY_TOLERANCE_*
    StaleRevert,        // event matches a recent slot's *prior* values — lamp reverted to its pre-PUT state → re-assert, don't pause
    Override,           // off-trajectory and off-cache → fires SOFT_PAUSE
    SyntheticEdge       // Stage 3 — reconnect-resync diff, not a real bridge event; carries an on/off edge the stream missed while down
};
const int ECHO_OUTCOME_COUNT = 10;

static const char* echoOutcomeName(EchoOutcome o) {
    switch (o) {
        case EchoOutcome::UnknownUuid:     return "unknown-uuid";
        case EchoOutcome::NoFields:        return "no-fields";
        case EchoOutcome::SkipState:       return "skip-state";
        case EchoOutcome::SkipPauseResume: return "skip-pauseresume";
        case EchoOutcome::SkipOffLight:    return "skip-offlight";
        case EchoOutcome::EchoMatch:       return "echo-match";
        case EchoOutcome::NoOpEcho:        return "noop-echo";
        case EchoOutcome::StaleRevert:     return "stale-revert";
        case EchoOutcome::Override:        return "OVERRIDE";
        case EchoOutcome::SyntheticEdge:   return "synthetic-edge";
        default:                           return "?";
    }
}

struct SseEventRecord {
    unsigned long timeMs         = 0;
    int           idx            = -1;
    bool          hasOn          = false;
    bool          evOn           = false;
    bool          hasBri         = false;
    float         evBri          = 0.0f;
    bool          hasCt          = false;
    int           evCt           = 0;
    bool          cacheOnBefore  = false;
    float         cacheBriBefore = 0.0f;
    int           cacheCtBefore  = 0;
    EchoOutcome   outcome        = EchoOutcome::NoFields;
};
const int      SSE_EVENT_RING_SIZE = 16;
SseEventRecord sseEventRing[SSE_EVENT_RING_SIZE];
int            sseEventRingHead = 0;
unsigned long  echoOutcomeCounts[ECHO_OUTCOME_COUNT] = {0};

// ── N1 Stage 1 — event queue + SSE-task plumbing ────────────────────────────
// The SSE stream is drained by a dedicated FreeRTOS task pinned to Core 0
// (Arduino's loopTask already owns Core 1; WiFi/lwIP idle plenty on Core 0).
// The task writes lightCache[]/recentPuts[] under dataMux and talks to the
// main task exclusively through evQueue + overridePending — it NEVER mutates
// `state`. Full design: Markdowns/N1_MIGRATION.md.

enum class EvType : uint8_t {
    LuxTick,        // Stage 2 — 30 s curve tick
    SseLight,       // classified light event from the SSE task
    SseAccessory,   // future: button / relative_rotary / zigbee_connectivity
    ButtonPress,    // future (physical buttons being retired)
    DashboardCmd,   // Stage 5 — command pulled by the network task
    TimerFire       // Stage 4 — soft-timer expiry (ramp step, pause expiry)
};

struct Event {
    EvType   type  = EvType::LuxTick;
    uint8_t  a     = 0;    // light idx / button id / command enum / timer id
    uint8_t  flags = 0;    // SseLight: bit0=prevOn, bit1=nowOn, bits2-5=EchoOutcome
    int32_t  i     = 0;    // value (sseEventRing slot, cmdValue, steps, …)
    float    f     = 0.0f; // lux / bri
    uint32_t tMs   = 0;    // millis() at enqueue
};

// SseLight flag packing (Stage 3). The on/off edge rides in bits 0-1 so the
// dispatcher can make wake/re-arm decisions from the event itself rather than
// re-reading a cache that may have moved on by dispatch time; the
// classification sits in bits 2-5 (EchoOutcome fits in 4 bits).
static inline uint8_t packSseFlags(EchoOutcome o, bool prevOn, bool nowOn) {
    return (uint8_t)(((uint8_t)o << 2) | (nowOn ? 2 : 0) | (prevOn ? 1 : 0));
}
static inline EchoOutcome sseFlagsOutcome(uint8_t f) { return (EchoOutcome)(f >> 2); }
static inline bool        sseFlagsPrevOn(uint8_t f)  { return (f & 0x01) != 0; }
static inline bool        sseFlagsNowOn(uint8_t f)   { return (f & 0x02) != 0; }

QueueHandle_t evQueue       = nullptr; // created in setup() before the SSE task starts
TaskHandle_t  sseTaskHandle = nullptr;

// N1 Stage 2 — millis() deadline for the next LuxTick enqueue. Armed by
// scheduleNextLuxTick() (aligned to :00/:30 under ALIGNED_TICKS — N5), checked
// by serviceLuxTickSchedule() in loop() with rollover-safe signed subtraction.
uint32_t nextLuxTickDueMs = 0;

// ── N1 Stage 4 — soft timers ────────────────────────────────────────────────
// Ramp advancement and soft-pause expiry run on their own deadlines so the
// LuxTick's only job is the curve. Nothing arms or disarms these at the
// transition sites: syncSoftTimers() reconciles the set of active timers with
// (state, pauseResumeActive) on every loop() pass, so a transition made
// anywhere — forceState, wake/wind-down completion, SSE re-arm, override
// pause, pause expiry — picks up the right timers within ~50 ms and can never
// leak one. Ramp timers arm with an immediate first fire, so a dashboard
// WIND_DOWN/WAKE command (or an SSE wake edge) starts its ramp on the next
// loop() pass instead of waiting out the 30 s tick boundary.
enum : uint8_t {
    TIMER_WAKE_STEP = 0,   // 30 s periodic — advances tickWakeRamp()
    TIMER_WINDDOWN_STEP,   // 30 s periodic — advances tickWindDown()
    TIMER_RESUME_STEP,     // 30 s periodic — advances the pause-resume ramp
    TIMER_PAUSE_EXPIRY,    // one-shot — due at softPauseStart + softPauseDurationMs
    SOFT_TIMER_COUNT
};
struct SoftTimer {
    bool     active   = false;
    uint32_t dueMs    = 0;  // absolute millis() deadline (signed-diff compared)
    uint32_t periodMs = 0;  // 0 → one-shot
};
SoftTimer softTimers[SOFT_TIMER_COUNT];
const uint32_t RAMP_STEP_MS = 30000UL; // step cadence for all three ramps

// ── N1 Stage 5 — dashboard networking task ──────────────────────────────────
// All DASHBOARD_BASE_URL HTTP (log POSTs, status POSTs, command polling) lives
// on netTask (Core 0, priority 1) so a slow or unreachable Railway endpoint
// can never stall a tick — the LuxTick's only remaining network I/O is bridge
// PUTs. Main task ↔ netTask traffic is fixed-size PODs over FreeRTOS queues
// (no String across tasks, same rule as the SSE task):
//   logQueue    — LogMsg, drop-OLDEST on full (the main task never blocks)
//   statusQueue — StatusSnapshot, depth 1 + xQueueOverwrite (latest wins; a
//                 stale snapshot is worthless the moment a newer one exists)
// Inbound: netTask polls /api/command every NET_CMD_POLL_MS, maps the wire
// string to a DashCmd, and enqueues a DashboardCmd event. Command *application*
// stays on the main task (dispatchDashboardCmd) because it mutates state.
// The ack POSTs from netTask right after a successful enqueue — same
// at-least-once semantics as the old synchronous poll (enqueue failure or a
// dropped ack → the command is re-fetched on the next poll).
struct LogMsg { char text[120]; };

// Everything the /api/status POST needs, snapshotted on the main task — every
// field is main-task-owned, so no locking. netTask serializes + POSTs it and
// stamps its own stack watermark (netStackFree) at send time.
struct StatusSnapshot {
    char     state[12];
    float    lux             = 0.0f;
    float    bri             = 0.0f;
    int      ct              = 0;
    bool     overheadsOn     = false;
    int      windDownStep    = 0;
    int      wakeStep        = 0;
    int      wakeTotal       = 0;
    long     pauseRemainingS = 0;
    int      stableLuxCount  = 0;
    bool     excluded[LIGHT_COUNT] = {false};
    // Stage 5 soak telemetry — heap + task-stack watermarks ride every status
    // POST so long soaks read from the dashboard instead of a serial tether
    // (three overnight captures in a row died to laptop sleep/update reboots).
    uint32_t heapFree        = 0;
    uint32_t sseStackFree    = 0;
    // (net_stack_free is netTask's own watermark — it samples it directly at
    // POST time rather than riding in the snapshot.)
};

// Dashboard command vocabulary. netTask parses the wire string into one of
// these so the queued Event stays a fixed-size POD (cmd in ev.a, value in ev.i).
enum class DashCmd : uint8_t {
    Normal, SoftPause, WindDown, Wake, HardOff, LockedOut,
    SetWindDownStep, SetWakeStep, SetSoftPauseRemaining, SetStableLuxCount,
    ExcludeLight, IncludeLight
};

QueueHandle_t logQueue      = nullptr;
QueueHandle_t statusQueue   = nullptr;
TaskHandle_t  netTaskHandle = nullptr;

// Mid-batch abort flag. Set by the SSE task the instant it classifies an
// Override; checked at the top of setLight()/setLightColor(). The Override
// *event* waits in evQueue until the dispatcher runs — without this flag a
// chained tick (tickNormal's multi-bulb batch, tickWakeRamp, tickWindDown)
// would finish driving the remaining bulbs to the now-stale pre-override
// target before the dispatcher could flip state. Cleared by the dispatcher
// when it applies the soft pause (applyOverridePause).
volatile bool overridePending = false;

// Guards lightCache[] + recentPuts[] cross-core access. Critical sections are
// tiny field copies only — never prints, JSON parsing, or HTTP.
portMUX_TYPE dataMux = portMUX_INITIALIZER_UNLOCKED;

// SSE event stream — persistent HTTPS connection over which the bridge pushes state changes.
WiFiClientSecure sseClient;
String        sseBuf;            // partial-line accumulator for incoming SSE bytes
unsigned long sseLastByteMs    = 0;
unsigned long sseLastConnectMs = 0;
const unsigned long SSE_RECONNECT_DELAY_MS = 5000UL;
const unsigned long SSE_STALE_TIMEOUT_MS   = 600000UL; // Hue v2 sends no keepalive; only reconnect on long silence

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
    if (!client.connect(HUE_BRIDGE_HOST, 443)) {
        Serial.println("Cert fetch: connect failed");
        return false;
    }
    // Send a minimal GET so the TLS handshake completes and the peer cert is available.
    client.print("GET /clip/v2/resource/light HTTP/1.0\r\n"
                 "Host: " HUE_BRIDGE_HOST "\r\n"
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

static bool peerMatchesPinned(WiFiClientSecure& client); // defined below — R3 manual pin check

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
        // Verify the stored cert still matches the live bridge — catches cert
        // rotation. Manual pin comparison, not setCACert(): CA-mode dies on CN
        // mismatch when dialing by IP (see peerMatchesPinned below), which
        // made the old probe fail — and silently re-fetch — on every boot.
        WiFiClientSecure test;
        test.setInsecure();
        test.setTimeout(5);
        bool ok = test.connect(HUE_BRIDGE_HOST, 443) && peerMatchesPinned(test);
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

// R3 postscript (July 15, found live): CA-style pinning via setCACert() is
// unworkable on this stack. We dial the bridge by IP, the bridge cert's CN is
// the bridge ID, and core 2.0.0's ssl_client always calls
// mbedtls_ssl_set_hostname with the connect host (ssl_client.cpp:257) with
// verification REQUIRED — so every handshake dies on CN mismatch (confirmed:
// "SSE: connect failed" loop on the first R3 flash). Pinning is enforced
// instead by byte-comparing the live peer certificate against the NVS-pinned
// PEM after the handshake, on the connections where nothing is transmitted
// until we've checked: the boot probe in ensureBridgeCert() and the
// persistent SSE stream (the application key goes out only after this passes).
// The HTTPClient paths (setLight*/bootstrap GET) send their request
// immediately on connect, so they run insecure-mode TLS and inherit the
// boot-time identity check. Returns true when nothing is pinned yet (failed
// first-boot fetch) — bootstrap trust, same as _fetchAndStoreBridgeCert().
static bool peerMatchesPinned(WiFiClientSecure& client) {
    if (!_bridgeCertPem.length()) return true;
    const mbedtls_x509_crt* peer = client.getPeerCertificate();
    if (!peer) return false;
    return _derToPem(peer->raw.p, peer->raw.len) == _bridgeCertPem;
}

// ── Light cache helpers ─────────────────────────────────────────────────────

// Map a v2 UUID string to its lightCache[] index. Returns -1 for unknown UUIDs.
static int idxByUuid(const char* uuid) {
    if (!uuid) return -1;
    if (strcmp(uuid, LIGHT_UUID_CHEST)   == 0) return LIGHT_CHEST;
    if (strcmp(uuid, LIGHT_UUID_DRESSER) == 0) return LIGHT_DRESSER;
    if (strcmp(uuid, LIGHT_UUID_CEIL_1)  == 0) return LIGHT_CEIL_1;
    // if (strcmp(uuid, LIGHT_UUID_CEIL_2)  == 0) return LIGHT_CEIL_2;
    if (strcmp(uuid, LIGHT_UUID_FLOOR)   == 0) return LIGHT_FLOOR;
    return -1;
}

// Friendly name for a lightCache[] index — used in Serial/diagnostic output so
// logs read "Floor" instead of an opaque UUID prefix.
static const char* lightName(int idx) {
    switch (idx) {
        case LIGHT_CHEST:   return "Chest";
        case LIGHT_DRESSER: return "Dresser";
        case LIGHT_CEIL_1:  return "Ceiling_1";
        case LIGHT_CEIL_2:  return "Ceiling_2";
        case LIGHT_FLOOR:   return "Floor";
        default:            return "Unknown";
    }
}

// Inverse of idxByUuid(): map a cache index to its v2 UUID for outgoing PUTs.
// Used by the cycle-exclusion enforcement (turn an excluded light off by index).
static const char* lightUuid(int idx) {
    switch (idx) {
        case LIGHT_CHEST:   return LIGHT_UUID_CHEST;
        case LIGHT_DRESSER: return LIGHT_UUID_DRESSER;
        case LIGHT_CEIL_1:  return LIGHT_UUID_CEIL_1;
        case LIGHT_CEIL_2:  return LIGHT_UUID_CEIL_2;
        case LIGHT_FLOOR:   return LIGHT_UUID_FLOOR;
        default:            return nullptr;
    }
}

// Read the cached state for a bulb. Replaces the old HTTP-polling getLightState().
// The copy runs under dataMux: the Core-0 SSE task is the cache's writer, and
// a torn read (bri from one event, ct from another mid-write) would be silent.
LightState cachedLight(int idx) {
    if (idx < 0 || idx >= LIGHT_COUNT) return {false, 0.0f, 0};
    portENTER_CRITICAL(&dataMux);
    LightState out = {lightCache[idx].on, lightCache[idx].bri, lightCache[idx].ct};
    portEXIT_CRITICAL(&dataMux);
    return out;
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
// event lands. Returns the slot index it wrote to (or -1 for invalid idx) so
// the caller can refresh postedAtMs after the blocking PUT returns; the
// blocking time spent in TLS handshake + roundtrip would otherwise eat the
// grace window before the echo can be drained.
static int noteRecentPut(int idx, bool on, float bri, int ct, unsigned long durationMs) {
    if (idx < 0 || idx >= LIGHT_COUNT) return -1;
    // Whole body under dataMux: recentPuts is read/expired by the Core-0 SSE
    // task (eventMatchesRecentPut), and priorBri/Ct are read from the cache the
    // SSE task writes.
    portENTER_CRITICAL(&dataMux);
    // Slot selection: prefer the first inactive/expired slot so live trajectories
    // from earlier PUTs (e.g. a wake-ramp tick still settling) aren't stomped. If
    // every slot is currently live, overwrite the oldest — that's the entry whose
    // remaining grace contributes least.
    int  slot      = 0;
    bool foundFree = false;
    for (int s = 0; s < RECENT_PUT_RING_SIZE; s++) {
        const RecentPut& r = recentPuts[idx][s];
        bool expired = r.active && (millis() - r.postedAtMs > r.durationMs + RECENT_PUT_GRACE_MS);
        if (!r.active || expired) { slot = s; foundFree = true; break; }
    }
    if (!foundFree) {
        unsigned long oldestAge = 0;
        for (int s = 0; s < RECENT_PUT_RING_SIZE; s++) {
            unsigned long age = millis() - recentPuts[idx][s].postedAtMs;
            if (age > oldestAge) { oldestAge = age; slot = s; }
        }
    }
    RecentPut& r = recentPuts[idx][slot];
    r.active     = true;
    r.onTarget   = on;
    r.priorOn    = lightCache[idx].on;
    r.priorBri   = lightCache[idx].bri;
    r.targetBri  = bri;
    r.priorCt    = lightCache[idx].ct;
    r.targetCt   = ct;
    r.postedAtMs = millis();
    r.durationMs = durationMs;
    portEXIT_CRITICAL(&dataMux);
    return slot;
}

// Does an incoming SSE event lie on the trajectory of the most recent PUT for
// this light? Auto-expires entries past their dynamics window + grace.
static bool eventMatchesRecentPut(int idx,
                                  bool hasOn,  bool   evOn,
                                  bool hasBri, float  evBri,
                                  bool hasCt,  int    evCt) {
    if (idx < 0 || idx >= LIGHT_COUNT) return false;
    // Runs on the Core-0 SSE task; recentPuts is written by the main task in
    // noteRecentPut(), so the slot scan runs under dataMux.
    bool matched = false;
    portENTER_CRITICAL(&dataMux);
    // Try every live slot; first match wins. Auto-expire stale entries inline.
    for (int s = 0; s < RECENT_PUT_RING_SIZE; s++) {
        RecentPut& r = recentPuts[idx][s];
        if (!r.active) continue;
        if (millis() - r.postedAtMs > r.durationMs + RECENT_PUT_GRACE_MS) {
            r.active = false;
            continue;
        }
        if (hasOn && evOn != r.onTarget) continue;
        if (hasBri) {
            float lo = fminf(r.priorBri, r.targetBri) - TRAJECTORY_TOLERANCE_BRI;
            float hi = fmaxf(r.priorBri, r.targetBri) + TRAJECTORY_TOLERANCE_BRI;
            if (evBri < lo || evBri > hi) continue;
        }
        if (hasCt) {
            int lo = min(r.priorCt, r.targetCt) - TRAJECTORY_TOLERANCE_CT;
            int hi = max(r.priorCt, r.targetCt) + TRAJECTORY_TOLERANCE_CT;
            if (evCt < lo || evCt > hi) continue;
        }
        matched = true;
        break;
    }
    portEXIT_CRITICAL(&dataMux);
    return matched;
}

// Stale-revert fingerprint: does this event match the *prior* (pre-PUT) values
// of a slot posted within STALE_REVERT_LOOKBACK_MS — including expired slots?
// Runs after the trajectory + no-op checks have both missed, so the event is
// already known to contradict both live trajectories and the cache. Landing
// exactly on a recent PUT's starting point means the lamp never applied (or
// rolled back) that PUT and the bridge is correcting its optimistic echo.
// slotOut receives the matched slot so the dispatcher can re-assert its target.
// Trade-off, accepted: a user who manually returns the lamp to its pre-PUT
// value within the lookback is misread as a revert and re-asserted once; their
// next (different) adjustment fires Override normally.
static bool eventMatchesPriorPut(int idx,
                                 bool hasOn,  bool  evOn,
                                 bool hasBri, float evBri,
                                 bool hasCt,  int   evCt,
                                 int& slotOut) {
    if (idx < 0 || idx >= LIGHT_COUNT) return false;
    bool matched = false;
    portENTER_CRITICAL(&dataMux);
    for (int s = 0; s < RECENT_PUT_RING_SIZE; s++) {
        const RecentPut& r = recentPuts[idx][s];
        if (r.postedAtMs == 0) continue; // slot never written since boot
        if (millis() - r.postedAtMs > STALE_REVERT_LOOKBACK_MS) continue;
        if (hasOn  && evOn != r.priorOn)                                    continue;
        if (hasBri && fabsf(evBri - r.priorBri) > TRAJECTORY_TOLERANCE_BRI) continue;
        if (hasCt  && abs(evCt   - r.priorCt)   > TRAJECTORY_TOLERANCE_CT)  continue;
        matched = true;
        slotOut = s;
        break;
    }
    portEXIT_CRITICAL(&dataMux);
    return matched;
}

// ── Bootstrap / resync of the light cache ───────────────────────────────────
// One v2 GET seeds every cached field. Two callers (Stage 3):
//  - setup(), enqueueEdges=false: boot seed, before the SSE task exists. Never
//    enqueues, so a floor lamp already on at boot does NOT fire wake.
//  - sseTick() on every reconnect, enqueueEdges=true: the stream was down and
//    any events in the gap are gone, so diff fresh state against the
//    pre-fetch cache and enqueue a synthetic edge event for each on/off flip
//    we missed — wake/re-arm stay correct across bridge outages. This path
//    runs on the Core-0 SSE task, hence the dataMux around cache writes.
static int recordSseEvent(int idx, bool hasOn, bool evOn, bool hasBri, float evBri,
                          bool hasCt, int evCt, bool cacheOnBefore, float cacheBriBefore,
                          int cacheCtBefore, EchoOutcome outcome); // defined with the SSE handlers below

static void bootstrapLightStates(bool enqueueEdges = false) {
    WiFiClientSecure client;
    client.setInsecure(); // HTTPClient transmits on connect — pin check is on the boot probe + SSE stream (see peerMatchesPinned)
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
        bool  on  = light["on"]["on"]                   | false;
        float bri = light["dimming"]["brightness"].as<float>();
        int   ct  = light["color_temperature"]["mirek"] | 0;

        portENTER_CRITICAL(&dataMux);
        bool  prevOn  = lightCache[idx].on;
        float prevBri = lightCache[idx].bri;
        int   prevCt  = lightCache[idx].ct;
        bool  wasInit = lightCache[idx].initialized;
        lightCache[idx].on  = on;
        lightCache[idx].bri = bri;
        lightCache[idx].ct  = ct;
        lightCache[idx].initialized = true;
        portEXIT_CRITICAL(&dataMux);

        if (!enqueueEdges) {
            Serial.printf("Bootstrap %s — on=%d bri=%.1f ct=%d\n",
                          lightName(idx), on ? 1 : 0, bri, ct);
        } else if (wasInit && on != prevOn) {
            Serial.printf("SSE resync: %s flipped %s while the stream was down — synthetic edge.\n",
                          lightName(idx), on ? "on" : "off");
            recordSseEvent(idx, true, on, false, 0.0f, false, 0,
                           prevOn, prevBri, prevCt, EchoOutcome::SyntheticEdge);
            Event ev = {};
            ev.type  = EvType::SseLight;
            ev.a     = (uint8_t)idx;
            ev.flags = packSseFlags(EchoOutcome::SyntheticEdge, prevOn, on);
            ev.i     = -1;
            ev.tMs   = millis();
            if (evQueue) xQueueSend(evQueue, &ev, 0);
        }
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
    // Mid-batch abort. The Core-0 SSE task classifies overrides in real time
    // and sets overridePending; the SOFT_PAUSE transition itself waits in
    // evQueue for the dispatcher. Without this guard, the remaining PUTs in a
    // chain (tickNormal's multi-bulb batch, tickWakeRamp, tickWindDown) would
    // still drive the bulbs to the *pre-override* target — contradicting the
    // user's manual change that just fired the override. Bail before
    // noteRecentPut() so we don't leave an orphan trajectory slot.
    if (overridePending || state == State::SOFT_PAUSE || state == State::HARD_OFF) return;

    // Record trajectory so SSE echoes are recognized as ours. Color mode doesn't
    // change CT meaningfully — pass through the cached value so the ct check is a
    // self-match.
    int idx = idxByUuid(uuid);
    int ctTarget = (idx >= 0) ? lightCache[idx].ct : 0;
    int slot = noteRecentPut(idx, on, bri, ctTarget, (unsigned long)durationMs);

    float cx, cy;
    _hsbToXY(hueV1, satV1, cx, cy);

    WiFiClientSecure client;
    client.setInsecure(); // HTTPClient transmits on connect — pin check is on the boot probe + SSE stream (see peerMatchesPinned)
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
    Serial.println("setLightColor(" + String(lightName(idx)) + ") → HTTP " + code);
    http.end();

    // Restart the trajectory grace window from now: the http.PUT block ate up
    // TLS handshake + roundtrip time. priorBri was correctly snapshotted before
    // the PUT (so the bridge can echo back without losing the race), but the
    // grace budget should count from when the PUT actually went out. (No tail
    // sseTick() anymore — the Core-0 SSE task drains echoes continuously,
    // including while http.PUT() blocks.)
    if (idx >= 0 && slot >= 0) {
        portENTER_CRITICAL(&dataMux);
        recentPuts[idx][slot].postedAtMs = millis();
        portEXIT_CRITICAL(&dataMux);
    }
}

// v2 HTTPS white/CT PUT. bri is percent 0.0–100.0. durationMs in ms.
void setLight(const char* uuid, bool on, float bri, int ct, int durationMs) {
    // Mid-batch abort. The Core-0 SSE task classifies overrides in real time
    // and sets overridePending; the SOFT_PAUSE transition itself waits in
    // evQueue for the dispatcher. Without this guard, the remaining PUTs in a
    // chain (tickNormal's multi-bulb batch, tickWakeRamp, tickWindDown) would
    // still drive the bulbs to the *pre-override* target — contradicting the
    // user's manual change that just fired the override. Bail before
    // noteRecentPut() so we don't leave an orphan trajectory slot.
    if (overridePending || state == State::SOFT_PAUSE || state == State::HARD_OFF) return;

    // Record trajectory so the SSE echo of this PUT is recognized as ours.
    int idx  = idxByUuid(uuid);
    int slot = noteRecentPut(idx, on, bri, ct, (unsigned long)durationMs);

    WiFiClientSecure client;
    client.setInsecure(); // HTTPClient transmits on connect — pin check is on the boot probe + SSE stream (see peerMatchesPinned)
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
    Serial.println("setLight(" + String(lightName(idx)) + ") → HTTP " + code);
    http.end();

    // Restart the trajectory grace window from now: the http.PUT block ate up
    // TLS handshake + roundtrip time. priorBri was correctly snapshotted before
    // the PUT (so the bridge can echo back without losing the race), but the
    // grace budget should count from when the PUT actually went out. (No tail
    // sseTick() anymore — the Core-0 SSE task drains echoes continuously,
    // including while http.PUT() blocks.)
    if (idx >= 0 && slot >= 0) {
        portENTER_CRITICAL(&dataMux);
        recentPuts[idx][slot].postedAtMs = millis();
        portEXIT_CRITICAL(&dataMux);
    }
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
    // N1 Stage 1 soak telemetry (Serial): task stack headroom in bytes (should
    // stay comfortably above ~1 KB) + free heap. Since Stage 5 the same values
    // also ride every /api/status POST, so soaks don't need this tether.
    if (sseTaskHandle) {
        Serial.printf("sse stack free=%u net stack free=%u heap free=%u\n",
                      (unsigned)uxTaskGetStackHighWaterMark(sseTaskHandle),
                      netTaskHandle ? (unsigned)uxTaskGetStackHighWaterMark(netTaskHandle) : 0,
                      (unsigned)esp_get_free_heap_size());
    }
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

// N1 Stage 5 — queue a log line for netTask to POST. Truncate-copies into a
// fixed LogMsg so nothing heap-backed crosses the task boundary; non-blocking.
// Queue full → drop the OLDEST entry (the main task never blocks on the
// dashboard). sendLog is main-task-only, so the receive-then-send swap can't
// race another producer. The direct-send fallback covers any call before the
// queue exists (early boot).
void sendLog(const String& message) {
    if (!logQueue) { sendDashboardLog(message); return; }
    LogMsg m;
    strlcpy(m.text, message.c_str(), sizeof(m.text));
    if (xQueueSend(logQueue, &m, 0) != pdTRUE) {
        LogMsg dropped;
        xQueueReceive(logQueue, &dropped, 0);
        xQueueSend(logQueue, &m, 0);
    }
}

// N1 Stage 5 — main-task half of the status POST: snapshot every field into a
// POD and overwrite the depth-1 status queue; netTask serializes + POSTs.
// Called each LuxTick and after every applied dashboard command — the latter
// shrinks the June 11 ack-before-status window: the snapshot reflecting a
// commanded state now chases the ack by ~one netTask pass (~100 ms) instead of
// waiting out the rest of the 30 s tick.
void queueDashboardStatus(float lux) {
    StatusSnapshot s;
    strlcpy(s.state, stateName(), sizeof(s.state));
    s.lux             = lux;
    s.bri             = sentTarget.bri;
    s.ct              = sentTarget.ct;
    s.overheadsOn     = overheadsOn;
    s.windDownStep    = windDownStep;
    s.wakeStep        = wakeStep;
    s.wakeTotal       = WAKE_RAMP_TICKS;
    s.pauseRemainingS = (state == State::SOFT_PAUSE) ?
        max(0L, ((long)softPauseDurationMs - (long)(millis() - softPauseStart)) / 1000L) : 0L;
    s.stableLuxCount  = stableLuxCount;
    for (int i = 0; i < LIGHT_COUNT; i++) s.excluded[i] = excludedLight[i];
    s.heapFree     = esp_get_free_heap_size();
    s.sseStackFree = sseTaskHandle ? (uint32_t)uxTaskGetStackHighWaterMark(sseTaskHandle) : 0;
    if (statusQueue) xQueueOverwrite(statusQueue, &s);
}

// netTask half: serialize + POST one snapshot. Runs on Core 0 — touches
// nothing but the snapshot it was handed (and its own stack watermark).
static void postDashboardStatus(const StatusSnapshot& s) {
    HTTPClient http;
    http.begin(String(DASHBOARD_BASE_URL) + "/api/status");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + String(ESP32_API_KEY));
    JsonDocument doc;
    doc["state"]       = (const char*)s.state;
    doc["lux"]         = s.lux;
    doc["bri"]         = s.bri;
    doc["ct"]          = s.ct;
    doc["overhead_on"] = s.overheadsOn;
    doc["wind_down_step"] = s.windDownStep;
    doc["wake_step"]      = s.wakeStep;
    doc["wake_total"]     = s.wakeTotal;
    doc["soft_pause_remaining_s"] = s.pauseRemainingS;
    doc["stable_lux_count"]      = s.stableLuxCount;
    // Cycle-exclusion state for the dashboard's "lights in cycle" chips.
    JsonObject excl = doc["excluded"].to<JsonObject>();
    excl["floor"]   = s.excluded[LIGHT_FLOOR];
    excl["chest"]   = s.excluded[LIGHT_CHEST];
    excl["dresser"] = s.excluded[LIGHT_DRESSER];
    excl["ceiling"] = s.excluded[LIGHT_CEIL_1];
    // Stage 5 soak telemetry (see StatusSnapshot).
    doc["heap_free"]      = s.heapFree;
    doc["sse_stack_free"] = s.sseStackFree;
    doc["net_stack_free"] = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
    String body;
    serializeJson(doc, body);
    int code = http.POST(body);
    if (code != 200) Serial.println("Dashboard status POST → HTTP " + String(code));
    http.end();
}

// ── SSE event handling ──────────────────────────────────────────────────────
// The Hue v2 bridge pushes resource updates over a long-lived HTTPS stream.
// The Core-0 SSE task (sseTask, N1 Stage 1) drains the socket continuously,
// parses `data:` lines as JSON arrays, updates the cache, and on a real manual
// override sets overridePending + enqueues an event for the main-task
// dispatcher to apply the SOFT_PAUSE.

// Append a classified SSE event to the rolling ring. Called exactly once per
// event in handleLightUpdate() regardless of outcome. The ring is dumped on
// override-fire so the conditions leading up to a misfire are visible without
// a serial connection. Returns the ring slot written — on Override the slot
// index travels in the queued Event so the dispatcher (main task) can feed the
// triggering event's details to dumpOverrideDiagnostic().
static int recordSseEvent(int idx,
                           bool  hasOn,  bool  evOn,
                           bool  hasBri, float evBri,
                           bool  hasCt,  int   evCt,
                           bool  cacheOnBefore,
                           float cacheBriBefore,
                           int   cacheCtBefore,
                           EchoOutcome outcome) {
    int slot = sseEventRingHead;
    SseEventRecord& r = sseEventRing[sseEventRingHead];
    r.timeMs         = millis();
    r.idx            = idx;
    r.hasOn          = hasOn;  r.evOn  = evOn;
    r.hasBri         = hasBri; r.evBri = evBri;
    r.hasCt          = hasCt;  r.evCt  = evCt;
    r.cacheOnBefore  = cacheOnBefore;
    r.cacheBriBefore = cacheBriBefore;
    r.cacheCtBefore  = cacheCtBefore;
    r.outcome        = outcome;
    sseEventRingHead = (sseEventRingHead + 1) % SSE_EVENT_RING_SIZE;
    echoOutcomeCounts[(int)outcome]++;
#ifdef ECHO_TRACE
    Serial.printf(
        "SSE evt idx=%d on=%c%d bri=%c%.2f ct=%c%d  cacheBefore[on=%d bri=%.2f ct=%d]  -> %s\n",
        idx,
        hasOn  ? '+' : ' ', evOn  ? 1 : 0,
        hasBri ? '+' : ' ', evBri,
        hasCt  ? '+' : ' ', evCt,
        cacheOnBefore ? 1 : 0, cacheBriBefore, cacheCtBefore,
        echoOutcomeName(outcome));
#endif
    return slot;
}

// Format and ship an extended diagnostic dump on override-fire. Includes the
// triggering event, pre-event cache, every active recentPuts slot for this
// light, the last few classified SSE events for this idx, and cumulative
// outcome counters since boot. Sent to both Serial and the dashboard so the
// conditions are captured without needing a USB connection.
static void dumpOverrideDiagnostic(int idx,
                                   bool  hasOn,  bool  evOn,
                                   bool  hasBri, float evBri,
                                   bool  hasCt,  int   evCt,
                                   bool  cacheOnBefore,
                                   float cacheBriBefore,
                                   int   cacheCtBefore) {
    String d;
    d.reserve(896);
    d += "Override fire idx=" + String(idx) + "\n";
    d += "  evt:";
    if (hasOn)  d += " on="  + String(evOn ? 1 : 0);
    if (hasBri) d += " bri=" + String(evBri, 2);
    if (hasCt)  d += " ct="  + String(evCt);
    d += "\n  cacheBefore: on=" + String(cacheOnBefore ? 1 : 0)
       + " bri=" + String(cacheBriBefore, 2)
       + " ct="  + String(cacheCtBefore);

    // Print every slot that has ever been written (postedAtMs > 0), regardless
    // of active flag — eventMatchesRecentPut clears active on expiration sweep,
    // so an "inactive" slot may still hold the trajectory we just posted. Tag
    // each as ACTIVE / EXPIRED / CLEARED so we can see whether the slot existed
    // at all and how stale it was when the override decision ran.
    int touchedSlots = 0;
    for (int s = 0; s < RECENT_PUT_RING_SIZE; s++) {
        const RecentPut& r = recentPuts[idx][s];
        if (r.postedAtMs == 0) continue;
        touchedSlots++;
        unsigned long age = millis() - r.postedAtMs;
        bool          past = age > r.durationMs + RECENT_PUT_GRACE_MS;
        const char*   tag  = r.active ? (past ? "EXPIRED" : "ACTIVE") : "CLEARED";
        d += "\n  recent[" + String(s) + "] " + tag
           + " on="          + String(r.onTarget ? 1 : 0)
           + " priorBri="    + String(r.priorBri, 2)
           + " targetBri="   + String(r.targetBri, 2)
           + " priorCt="     + String(r.priorCt)
           + " targetCt="    + String(r.targetCt)
           + " age="         + String(age) + "/" + String(r.durationMs)
           + "+grace="       + String(RECENT_PUT_GRACE_MS);
    }
    if (touchedSlots == 0) d += "\n  recent: (no slots ever written for this idx)";

    d += "\n  history(idx=" + String(idx) + "):";
    int dumped = 0;
    for (int i = 1; i <= SSE_EVENT_RING_SIZE && dumped < 6; i++) {
        int slot = (sseEventRingHead - i + SSE_EVENT_RING_SIZE) % SSE_EVENT_RING_SIZE;
        const SseEventRecord& r = sseEventRing[slot];
        if (r.timeMs == 0) continue;
        if (r.idx    != idx) continue;
        unsigned long ago = millis() - r.timeMs;
        d += "\n    t-" + String(ago) + "ms";
        if (r.hasOn)  d += " on="  + String(r.evOn ? 1 : 0);
        if (r.hasBri) d += " bri=" + String(r.evBri, 2);
        if (r.hasCt)  d += " ct="  + String(r.evCt);
        d += " (cache on=" + String(r.cacheOnBefore ? 1 : 0)
           + " bri=" + String(r.cacheBriBefore, 2)
           + " ct="  + String(r.cacheCtBefore) + ") -> "
           + echoOutcomeName(r.outcome);
        dumped++;
    }
    if (dumped == 0) d += "\n    (no prior history for this idx)";

    d += "\n  counts:";
    for (int i = 0; i < ECHO_OUTCOME_COUNT; i++) {
        d += " " + String(echoOutcomeName((EchoOutcome)i)) + "=" + String(echoOutcomeCounts[i]);
    }

    // Verbose dump goes to Serial only — the dashboard event log gets just the
    // concise "Manual override — soft pause" line from handleLightUpdate(). The
    // multi-line diagnostic (recentPuts slots, event history, outcome histogram)
    // was cluttering the dashboard, so it's Serial-only now.
    Serial.println(d);
}

// Apply a single light update event to the cache; classify into an EchoOutcome;
// flag + enqueue only on Override. Runs on the Core-0 SSE task (N1 Stage 1):
// never mutates `state`, never does HTTP — on Override it sets overridePending
// (synchronous mid-batch abort) and enqueues an SseLight event; the dispatcher
// on the main task performs the SOFT_PAUSE transition, diagnostic dump, and
// dashboard log. Ordering: (1) snapshot pre-event cache so the diagnostic
// record shows what we thought before the event landed, (2) classify,
// (3) update cache, (4) record into ring, (5) flag + enqueue if Override.
//
// The override decision is still based on fields present in *this* event only,
// not on cache-merged state. The bridge splits combined state changes across
// multiple events (a dimming event followed by a separate color_temperature
// event), so for a single event the cache is a Frankenstein of new + stale
// fields; mixing cached fields into the trajectory comparison would generate
// false positives every time a partial event landed during a steady-state PUT.
//
// New: a no-op event filter runs *after* the trajectory check. If the event's
// values are within TRAJECTORY_TOLERANCE_* of the pre-event cache, the event
// is reporting state we already think is true — a late settling echo whose
// recentPuts slot already expired. Closes the timing gap that previously
// caused SOFT_PAUSE misfires on "skipping PUT" ticks where nothing refreshed
// the slot before the bridge's confirmation event arrived.
static void handleLightUpdate(JsonObjectConst upd) {
    const char* uuid = upd["id"];
    int idx = idxByUuid(uuid);
    if (idx < 0) {
        recordSseEvent(-1, false, false, false, 0.0f, false, 0,
                       false, 0.0f, 0, EchoOutcome::UnknownUuid);
        return;
    }

    JsonVariantConst onField  = upd["on"]["on"];
    JsonVariantConst briField = upd["dimming"]["brightness"];
    JsonVariantConst ctField  = upd["color_temperature"]["mirek"];
    bool  hasOn  = !onField.isNull();
    bool  hasBri = !briField.isNull();
    bool  hasCt  = !ctField.isNull();
    bool  evOn   = hasOn  ? onField.as<bool>()   : false;
    float evBri  = hasBri ? briField.as<float>() : 0.0f;
    int   evCt   = hasCt  ? ctField.as<int>()    : 0;

    portENTER_CRITICAL(&dataMux);
    bool  cacheOnBefore  = lightCache[idx].on;
    float cacheBriBefore = lightCache[idx].bri;
    int   cacheCtBefore  = lightCache[idx].ct;
    portEXIT_CRITICAL(&dataMux);

    if (!hasOn && !hasBri && !hasCt) {
        recordSseEvent(idx, hasOn, evOn, hasBri, evBri, hasCt, evCt,
                       cacheOnBefore, cacheBriBefore, cacheCtBefore,
                       EchoOutcome::NoFields);
        return;
    }

    EchoOutcome decision;
    int revertSlot = -1; // recentPuts slot matched by the stale-revert check
    if (state != State::NORMAL) {
        decision = EchoOutcome::SkipState;
    } else if (pauseResumeActive) {
        decision = EchoOutcome::SkipPauseResume;
    } else {
        bool expectedOn;
        switch (idx) {
            case LIGHT_DRESSER:
            case LIGHT_FLOOR:   expectedOn = true;        break;
            case LIGHT_CHEST:   expectedOn = chestOn;     break;
            case LIGHT_CEIL_1:
            // case LIGHT_CEIL_2:
                                expectedOn = overheadsOn; break;
            default:            expectedOn = false;       break;
        }
        // Cycle-excluded lights stay override-watched: Mira no longer drives them, but
        // a manual Hue-app change must still fire the global soft pause. Forcing
        // expectedOn true means the change is classified (echo vs override) rather than
        // skipped as an "off light". Mira's own exclusion-off PUT is still an EchoMatch.
        if (excludedLight[idx]) expectedOn = true;
        if (!expectedOn) {
            decision = EchoOutcome::SkipOffLight;
        } else if (eventMatchesRecentPut(idx, hasOn, evOn, hasBri, evBri, hasCt, evCt)) {
            decision = EchoOutcome::EchoMatch;
        } else {
            // Trajectory check missed. Fall back to a no-op check: is the event
            // reporting state already-consistent with our pre-event cache, within
            // the same tolerance the trajectory check uses? If so, it's a late
            // settling echo of a now-expired PUT, not a user override.
            bool isNoOp = true;
            if (hasOn  && evOn != cacheOnBefore)                                    isNoOp = false;
            if (hasBri && fabsf(evBri - cacheBriBefore) > TRAJECTORY_TOLERANCE_BRI) isNoOp = false;
            if (hasCt  && abs(evCt   - cacheCtBefore)    > TRAJECTORY_TOLERANCE_CT) isNoOp = false;
            if (isNoOp) {
                decision = EchoOutcome::NoOpEcho;
            } else if (eventMatchesPriorPut(idx, hasOn, evOn, hasBri, evBri,
                                            hasCt, evCt, revertSlot)) {
                // Off-trajectory AND off-cache, but landing on a recent PUT's
                // starting values: the bridge correcting its optimistic echo
                // after the lamp failed to apply our PUT (observed June 11 on
                // the Signe, ~35 s out). Re-assert the slot's target rather
                // than soft-pausing on a phantom override.
                decision = EchoOutcome::StaleRevert;
            } else {
                decision = EchoOutcome::Override;
            }
        }
    }

    // Cache write happens after the override decision so cacheBefore stays
    // meaningful for the diagnostic record and for the no-op filter above. The
    // override check itself doesn't use the cache, so this reorder is
    // semantically neutral for the trajectory path.
    portENTER_CRITICAL(&dataMux);
    if (hasOn)  lightCache[idx].on  = evOn;
    if (hasBri) lightCache[idx].bri = evBri;
    if (hasCt)  lightCache[idx].ct  = evCt;
    portEXIT_CRITICAL(&dataMux);

    int ringSlot = recordSseEvent(idx, hasOn, evOn, hasBri, evBri, hasCt, evCt,
                                  cacheOnBefore, cacheBriBefore, cacheCtBefore, decision);

    // Stage 3 — the on/off edge rides on *every* enqueued event, whatever the
    // classification (an off-flip during LOCKED_OUT classifies SkipState, but
    // it's exactly the edge the wake/re-arm dispatcher needs). prevOn is the
    // pre-event cache; nowOn falls back to it when the event carried no on
    // field (bri/ct-only events are never edges).
    bool prevOn = cacheOnBefore;
    bool nowOn  = hasOn ? evOn : cacheOnBefore;
    bool edge   = (prevOn != nowOn);

    if (decision == EchoOutcome::StaleRevert) {
        // No overridePending: a revert must not abort an in-progress PUT batch.
        // The dispatcher (main task) re-PUTs the matched slot's target; the
        // cache already holds the reverted values, so the retry's noteRecentPut
        // snapshots the lamp's true state as its prior.
        Serial.printf("SSE stale revert — %s reported pre-PUT values; queueing re-assert.\n",
                      lightName(idx));
    } else if (decision == EchoOutcome::Override) {
        // Synchronous half of the abort: any setLight*() call — including one
        // the main task is partway through a chained tick on — bails from here
        // on. The dispatcher applies the SOFT_PAUSE transition, ships the
        // diagnostic dump, and clears overridePending.
        overridePending = true;
        Serial.println("SSE override classified — queued for dispatcher.");
    } else if (!edge) {
        return; // routine echo with no on/off edge — nothing for the dispatcher
    }

    // ev.i carries the slot the dispatcher needs: the sseEventRing record of
    // the triggering event for Override (diagnostic dump), the recentPuts slot
    // for StaleRevert (re-assert target), -1 for edge-only events.
    Event ev = {};
    ev.type  = EvType::SseLight;
    ev.a     = (uint8_t)idx;
    ev.flags = packSseFlags(decision, prevOn, nowOn);
    ev.i     = (decision == EchoOutcome::Override)    ? ringSlot
             : (decision == EchoOutcome::StaleRevert) ? revertSlot
             : -1;
    ev.tMs   = millis();
    if (evQueue) xQueueSend(evQueue, &ev, 0);
}

// ── N1 Stage 1 — event dispatch (main task) ─────────────────────────────────

// Clobber repair. Override classification is real-time, and the mid-batch
// abort stops every *subsequent* PUT in a chain — but a PUT already committed
// to the wire can't be recalled. If the user's change targets the same light
// as that in-flight PUT, the PUT lands ~0.5–1 s later and overwrites the
// change (observed June 11: floor lamp flipped off mid-chain; the in-flight
// floor PUT turned it back on, then SOFT_PAUSE froze it that way for an hour).
// Detector: a live recentPuts slot whose target contradicts the triggering
// event's fields — slot lifetime (duration + grace) is exactly the window in
// which one of our PUTs can land after the user's change. Repair: re-PUT the
// user's own values (event fields, cache fill-in for absent ones). If the
// suspected clobber never actually landed, the re-PUT just re-asserts the
// bulb's current state — harmless. Must run while state is still NORMAL so
// setLight()'s guard doesn't eat the repair PUT.
static void restoreUserOverride(const SseEventRecord& r) {
    if (r.idx < 0 || r.idx >= LIGHT_COUNT) return;
    bool contradicted = false;
    portENTER_CRITICAL(&dataMux);
    for (int s = 0; s < RECENT_PUT_RING_SIZE; s++) {
        const RecentPut& p = recentPuts[r.idx][s];
        if (!p.active) continue;
        if (millis() - p.postedAtMs > p.durationMs + RECENT_PUT_GRACE_MS) continue;
        if (r.hasOn  && r.evOn != p.onTarget)                                    { contradicted = true; break; }
        if (r.hasBri && fabsf(r.evBri - p.targetBri) > TRAJECTORY_TOLERANCE_BRI) { contradicted = true; break; }
        if (r.hasCt  && abs(r.evCt   - p.targetCt)   > TRAJECTORY_TOLERANCE_CT)  { contradicted = true; break; }
    }
    portEXIT_CRITICAL(&dataMux);
    if (!contradicted) return;

    LightState cur = cachedLight(r.idx); // best-known physical state for absent fields
    bool  on  = r.hasOn  ? r.evOn  : cur.on;
    float bri = r.hasBri ? r.evBri : cur.bri;
    int   ct  = r.hasCt  ? r.evCt  : (cur.ct > 0 ? cur.ct : (int)CT_WARM);
    Serial.printf("Override repair — re-asserting user values to %s (on=%d bri=%.1f ct=%d)\n",
                  lightName(r.idx), on ? 1 : 0, bri, ct);
    setLight(lightUuid(r.idx), on, bri, ct, 500);
}

// Apply an Override classified by the SSE task: the actual SOFT_PAUSE
// transition, clobber repair, diagnostic dump, and dashboard log happen here,
// on the main task. ringSlot indexes the sseEventRing record of the triggering
// event (-1 = unknown, used by the flag-only belt-and-braces path).
static void applyOverridePause(int ringSlot) {
    overridePending = false;
    // Classified in NORMAL, but the state may have moved on in the ≤50 ms
    // before dispatch (dashboard command, pause already applied by an earlier
    // queued Override). Matches the old synchronous semantics: only NORMAL
    // pauses.
    if (state != State::NORMAL) return;

    // Copy: the ring is written by the SSE task; 16 entries make a wrap
    // within the dispatch latency implausible, and the record is used for
    // repair + diagnostics only.
    bool haveRecord = (ringSlot >= 0 && ringSlot < SSE_EVENT_RING_SIZE);
    SseEventRecord r;
    if (haveRecord) r = sseEventRing[ringSlot];

    // Repair before the state flip — setLight() returns early in SOFT_PAUSE.
    if (haveRecord) restoreUserOverride(r);

    state               = State::SOFT_PAUSE;
    softPauseStart      = millis();
    softPauseDurationMs = SOFT_PAUSE_MS;
    pauseResumeActive   = false;
    Serial.println("SSE override — soft pause.");

    if (haveRecord) {
        dumpOverrideDiagnostic(r.idx, r.hasOn, r.evOn, r.hasBri, r.evBri,
                               r.hasCt, r.evCt, r.cacheOnBefore,
                               r.cacheBriBefore, r.cacheCtBefore);
    }
    sendLog("Manual override — soft pause — " + getTimeString());
}

// Re-assert a PUT the lamp apparently never applied (StaleRevert classification).
// Re-sends the matched recentPuts slot's target; setLight() writes a fresh
// trajectory slot, so the retry's echo is covered, and its top guard drops the
// PUT harmlessly if the state moved to SOFT_PAUSE/HARD_OFF since classification.
// Per-light cooldown: if the lamp drops the retry too, its next revert event
// inside the cooldown is logged but not re-PUT — the 30 s curve tick re-drives
// the lamp anyway once drift exceeds STATE_TOLERANCE_*, so we don't ping-pong
// with a persistently deaf bulb every bridge poll.
static void reassertRecentPut(int idx, int slot) {
    static unsigned long lastReassertMs[LIGHT_COUNT] = {0};
    if (idx < 0 || idx >= LIGHT_COUNT || slot < 0 || slot >= RECENT_PUT_RING_SIZE) return;
    if (lastReassertMs[idx] != 0 &&
        millis() - lastReassertMs[idx] < STALE_REVERT_LOOKBACK_MS) {
        Serial.printf("Stale revert — %s reverted again within cooldown; leaving it to the next tick.\n",
                      lightName(idx));
        return;
    }
    portENTER_CRITICAL(&dataMux);
    RecentPut r = recentPuts[idx][slot];
    portEXIT_CRITICAL(&dataMux);
    if (r.postedAtMs == 0) return; // slot recycled/never written — nothing to re-assert
    lastReassertMs[idx] = millis();
    Serial.printf("Stale revert — re-asserting %s to on=%d bri=%.1f ct=%d\n",
                  lightName(idx), r.onTarget ? 1 : 0, r.targetBri, r.targetCt);
    setLight(lightUuid(idx), r.onTarget, r.targetBri, r.targetCt, 500);
    sendLog("Stale revert — re-sent " + String(lightName(idx)) + " — " + getTimeString());
}

static void dispatchLuxTick(); // defined below the tick functions — N1 Stage 2
static void dispatchTimerFire(const Event& ev); // defined below the tick functions — Stage 4
void triggerWake(float ambientLux); // defined with the tick functions — Stage 3 edge dispatch

// Stage 3 (G22+G23) — SseLight dispatch on the main task. Applies the Stage 1
// Override/StaleRevert actions, then runs the on/off-edge state decisions that
// used to live in the per-tick floor-lamp poll: wake trigger and lockout
// re-arm now land within ~100 ms of the bridge event instead of waiting for
// the next tick. Edge bits come from the event itself (pre-event cache vs
// event), so a burst of queued edges replays in order even though the cache
// has already moved on.
static void dispatchSseLight(const Event& ev) {
    EchoOutcome outcome = sseFlagsOutcome(ev.flags);
    bool prevOn = sseFlagsPrevOn(ev.flags);
    bool nowOn  = sseFlagsNowOn(ev.flags);
    int  idx    = ev.a;

    if      (outcome == EchoOutcome::Override)    applyOverridePause(ev.i);
    else if (outcome == EchoOutcome::StaleRevert) reassertRecentPut(idx, ev.i);

    if (prevOn == nowOn) return; // no on/off edge — nothing below applies

    // Wake (G23): rising edge on the floor lamp while locked out = "good
    // morning". lastLux is at most one tick (30 s) old — the same seed
    // forceState(WAKE) uses.
    if (state == State::LOCKED_OUT && idx == LIGHT_FLOOR && nowOn) {
        triggerWake(lastLux);
        return;
    }

    // Re-arm (G22): falling edge after the reset hour with every driven light
    // now off = the user has gone to bed. Fires from ANY state except HARD_OFF
    // (June 11 decision — all-off after 21:00 is a sleep signal, so it wins
    // over SOFT_PAUSE and a running ramp). The cache already reflects this
    // event, so the all-off check sees the light that just went out. If an
    // Override rode in on this same event, the pause it just applied is
    // superseded here. From LOCKED_OUT itself the block is a harmless
    // re-clear (counters + exclusions), same as the old poll.
    if (!nowOn && state != State::HARD_OFF &&
        timeClient.getHours() * 60 + timeClient.getMinutes() >= LOCKOUT_RESET_MIN_OF_DAY &&
        !cachedLight(LIGHT_FLOOR).on && !cachedLight(LIGHT_CHEST).on &&
        !cachedLight(LIGHT_DRESSER).on && !cachedLight(LIGHT_CEIL_1).on) {
        bool wasLockedOut = (state == State::LOCKED_OUT);
        state             = State::LOCKED_OUT;
        stableLuxCount    = 0;
        windDownStep      = 0;
        pauseResumeActive = false; // cancel any resume ramp in flight
        // New day's cycle: clear exclusions so every light rejoins tomorrow.
        for (int i = 0; i < LIGHT_COUNT; i++) excludedLight[i] = false;
        if (!wasLockedOut) {
            Serial.println("All lights off after reset hour — lockout re-armed.");
            sendLog("Lockout re-armed — " + getTimeString());
        }
    }
}

static void dispatchDashboardCmd(const Event& ev); // Stage 5 — defined with the netTask block below

static void dispatchEvent(const Event& ev) {
    switch (ev.type) {
        case EvType::LuxTick:
            dispatchLuxTick();
            break;
        case EvType::SseLight:
            dispatchSseLight(ev);
            break;
        case EvType::TimerFire:
            dispatchTimerFire(ev);
            break;
        case EvType::DashboardCmd:
            dispatchDashboardCmd(ev);
            break;
        default:
            break;
    }
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
    // R3 — manual pin check before anything (incl. the application key) is
    // sent. A mismatched peer means someone else is answering the bridge's IP.
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
// Called only by sseTask (Core 0) after boot — see N1 Stage 1.
static void sseTick() {
    if (!sseClient.connected()) {
        if (millis() - sseLastConnectMs >= SSE_RECONNECT_DELAY_MS) {
            sseLastConnectMs = millis();
            sseConnect();
            // Stage 3 — the stream was down and any events in the gap are
            // gone. Re-pull all light state; on/off flips we missed become
            // synthetic edge events so wake/re-arm still fire (G22/G23
            // reconnect-gap mitigation). On a routine stale-timeout reconnect
            // nothing changed, so the diff enqueues nothing.
            if (sseClient.connected()) bootstrapLightStates(true);
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

// N1 Stage 1 — SSE drain task, pinned to Core 0 (Arduino's loopTask owns
// Core 1). Replaces the wait-loop sseTick() call and the setLight*() tail-call
// drains: the stream is serviced continuously, so override classification
// happens within ~20 ms of the bridge emitting the event, regardless of what
// the main task is doing (Railway HTTP, bridge PUTs, JSON serialization).
// Owns sseClient/sseBuf and the reconnect path exclusively after boot.
static void sseTask(void*) {
    for (;;) {
        sseTick();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void forceState(State next); // defined later — forward declaration for dispatchDashboardCmd
void queueDashboardStatus(float lux);

// ── N1 Stage 5 — netTask: command poll ──────────────────────────────────────
// GET /api/command, map the wire string to a DashCmd, enqueue a DashboardCmd
// event for the main task, then ack. Runs on netTask (Core 0) — never touches
// state; application happens in dispatchDashboardCmd on the main task.
// R11's bounded drain is kept: up to 8 queued commands per poll, so a flooded
// dashboard queue can't wedge this loop. Ack-after-enqueue preserves the
// at-least-once semantics of the old synchronous poll — an enqueue failure
// (evQueue full) skips the ack so the command is re-fetched next poll.
static void pollDashboardCommands() {
  for (int n = 0; n < 8; n++) {
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

    static const struct { const char* name; DashCmd cmd; } CMD_MAP[] = {
        {"NORMAL",                   DashCmd::Normal},
        {"SOFT_PAUSE",               DashCmd::SoftPause},
        {"WIND_DOWN",                DashCmd::WindDown},
        {"WAKE",                     DashCmd::Wake},
        {"HARD_OFF",                 DashCmd::HardOff},
        {"LOCKED_OUT",               DashCmd::LockedOut},
        {"SET_WIND_DOWN_STEP",       DashCmd::SetWindDownStep},
        {"SET_WAKE_STEP",            DashCmd::SetWakeStep},
        {"SET_SOFT_PAUSE_REMAINING", DashCmd::SetSoftPauseRemaining},
        {"SET_STABLE_LUX_COUNT",     DashCmd::SetStableLuxCount},
        {"EXCLUDE_LIGHT",            DashCmd::ExcludeLight},
        {"INCLUDE_LIGHT",            DashCmd::IncludeLight},
    };
    bool known = false;
    Event ev;
    ev.type = EvType::DashboardCmd;
    ev.i    = cmdValue;
    ev.tMs  = millis();
    for (auto& m : CMD_MAP) {
        if (cmd == m.name) { ev.a = (uint8_t)m.cmd; known = true; break; }
    }
    if (known) {
        Serial.println("Dashboard command: " + cmd + (cmdValue >= 0 ? " value=" + String(cmdValue) : ""));
        if (!evQueue || xQueueSend(evQueue, &ev, 0) != pdTRUE) return; // full → no ack, retry next poll
    } else {
        // Unknown command: nothing to enqueue, but still ack it below or it
        // blocks the dashboard's pending queue forever — same fall-through
        // the old synchronous if-chain had.
        Serial.println("Unknown dashboard command: " + cmd);
    }

    // No id → can't ack; looping again would refetch the same command forever.
    if (cmdId < 0) return;
    HTTPClient ack;
    ack.begin(String(DASHBOARD_BASE_URL) + "/api/command/" + String(cmdId) + "/ack");
    ack.addHeader("Authorization", "Bearer " + String(ESP32_API_KEY));
    ack.POST("");
    ack.end();
  }
}

// Main-task side: apply one dashboard command (the body of the old synchronous
// pollDashboardCommand if-chain, switch-form). Runs from the dispatcher, so
// everything it mutates is main-task-owned. The per-command state guards
// re-check `state` here because classification happened against whatever the
// dashboard believed when the button was pressed, seconds ago.
static void dispatchDashboardCmd(const Event& ev) {
    DashCmd cmd  = (DashCmd)ev.a;
    int cmdValue = ev.i;

    switch (cmd) {
        case DashCmd::Normal:    forceState(State::NORMAL);     break;
        case DashCmd::SoftPause: forceState(State::SOFT_PAUSE); break;
        case DashCmd::WindDown:
            forceState(State::WIND_DOWN);
            if (cmdValue >= 0) windDownStep = min(cmdValue, 119);
            break;
        case DashCmd::Wake:
            forceState(State::WAKE);
            if (cmdValue >= 0) wakeStep = cmdValue;
            break;
        case DashCmd::HardOff:   forceState(State::HARD_OFF);   break;
        case DashCmd::LockedOut: forceState(State::LOCKED_OUT); break;
        case DashCmd::SetWindDownStep:
            if (state == State::WIND_DOWN && cmdValue >= 0) {
                windDownStep = min(cmdValue, 119);
                Serial.println("Wind-down seek → step " + String(windDownStep));
            }
            break;
        case DashCmd::SetWakeStep:
            if (state == State::WAKE && cmdValue >= 0) {
                wakeStep = cmdValue;
                Serial.println("Wake seek → step " + String(wakeStep));
            }
            break;
        // Soft-pause remaining-time control (slider scrub + "+10 min" extend button).
        // cmdValue is the absolute minutes-remaining target from *now*. Recomputed as
        // (already-elapsed + requested remaining) so softPauseStart stays put and the
        // unsigned millis() math never wraps — extends past 60 min just enlarge the
        // target. Dragging to 0 puts the TIMER_PAUSE_EXPIRY deadline in the past, so
        // the pause ends on the next loop() pass (Stage 4 — was the next tick).
        // The extend button is absolute (displayed + 10) on the dashboard side,
        // so rapid taps converge instead of cancelling to +10.
        case DashCmd::SetSoftPauseRemaining:
            if (state == State::SOFT_PAUSE && cmdValue >= 0) {
                softPauseDurationMs = (millis() - softPauseStart) + (unsigned long)cmdValue * 60000UL;
                Serial.println("Soft pause remaining → " + String(cmdValue) + " min");
            }
            break;
        case DashCmd::SetStableLuxCount:
            if (state == State::NORMAL && cmdValue >= 0) {
                stableLuxCount = min(cmdValue, 59);
                Serial.println("Stable lux count → " + String(stableLuxCount));
            }
            break;
        // Cycle-exclusion toggles (cmdValue = LIGHT_* cache index). Setting the flag is
        // all that's needed: tickNormal enforces the off (cache-guarded) and every driving
        // state skips excluded lights. INCLUDE resets sentTarget to the sentinel so the next
        // NORMAL tick re-PUTs the re-admitted light at the current curve target.
        case DashCmd::ExcludeLight:
            if (cmdValue >= 0 && cmdValue < LIGHT_COUNT) {
                excludedLight[cmdValue] = true;
                Serial.println("Excluded " + String(lightName(cmdValue)) + " from cycle");
            }
            break;
        case DashCmd::IncludeLight:
            if (cmdValue >= 0 && cmdValue < LIGHT_COUNT) {
                excludedLight[cmdValue] = false;
                sentTarget = {-1.0f, 0};  // force a refresh so the light re-joins next tick
                Serial.println("Re-included " + String(lightName(cmdValue)) + " into cycle");
            }
            break;
    }

    // Push a fresh snapshot so the dashboard sees the commanded state on
    // netTask's next pass (~100 ms) instead of at the next 30 s tick. Shrinks
    // the June 11 ack-before-status display window to near zero.
    queueDashboardStatus(lastLux);
}

// The dashboard networking task (Core 0, priority 1 — same as sseTask). Each
// pass: drain queued log lines, POST the latest status snapshot, poll for
// commands on the NET_CMD_POLL_MS cadence. Railway round-trips block only this
// task — the main task's tick cost drops to bridge-PUT time.
static void netTask(void*) {
    uint32_t lastCmdPollMs = millis() - NET_CMD_POLL_MS; // first poll fires immediately
    for (;;) {
        LogMsg m;
        while (xQueueReceive(logQueue, &m, 0) == pdTRUE) sendDashboardLog(m.text);

        StatusSnapshot s;
        if (xQueueReceive(statusQueue, &s, 0) == pdTRUE) postDashboardStatus(s);

        if (millis() - lastCmdPollMs >= NET_CMD_POLL_MS) {
            lastCmdPollMs = millis();
            pollDashboardCommands();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void saveLastState(float bri, uint16_t ct) {
    prefs.begin("mira", false);
    prefs.putFloat("savedBri", bri);
    prefs.putUShort("savedCt", ct);
    prefs.end();
}

// Cycle PUT: send to a light only if it's still part of the cycle. Cycle-excluded
// lights receive nothing from any driving state (NORMAL / WAKE / WIND_DOWN); their
// single turn-off is handled by enforceExclusions(). Centralizing the check here keeps
// every driving state's PUT logic uniform.
static inline void cyclePut(int idx, bool on, float bri, int ct, int durationMs) {
    if (idx < 0 || idx >= LIGHT_COUNT) return;
    if (excludedLight[idx]) return;
    const char* uuid = lightUuid(idx);
    if (uuid) setLight(uuid, on, bri, ct, durationMs);
}

// Turn off any excluded light that's still on, once. Cache-guarded so the resulting
// SSE echo (which updates the cache to off) prevents a repeat off-PUT next tick.
// Called at the top of tickNormal so an exclusion toggled during any state — including
// a soft pause, where the command's PUT would have been guarded out — is enforced as
// soon as NORMAL is running again.
static void enforceExclusions() {
    for (int i = 0; i < LIGHT_COUNT; i++) {
        if (i == LIGHT_CEIL_2) continue;            // unused bulb
        if (excludedLight[i] && cachedLight(i).on) {
            const char* uuid = lightUuid(i);
            if (uuid) setLight(uuid, false, 0, 0, 1000);
        }
    }
}

void tickWakeRamp(float lux) {
    wakeStep++;
    float t = min(wakeStep / (float)WAKE_RAMP_TICKS, 1.0f);

    float rampBri = wakeStartTarget.bri + t * (wakeEndTarget.bri - wakeStartTarget.bri);
    int   rampCt  = (int)(wakeStartTarget.ct  + t * (wakeEndTarget.ct  - wakeStartTarget.ct));

    rampBri = constrain(rampBri, 1.0f, 100.0f);
    rampCt  = constrain(rampCt, (int)CT_COOL, (int)CT_WARM);

    // Sunrise staircase. Floor lamp leads — on from tick 0, ramps the whole way.
    // (cyclePut skips any light the user has excluded from the cycle.)
    cyclePut(LIGHT_FLOOR, true, rampBri, rampCt, 30000);
    // Chest + dresser join once the ramp is meaningfully lit and the morning is bright.
    if (lux >= WAKE_SECONDARY_LUX && rampBri >= WAKE_SECONDARY_BRI) {
        cyclePut(LIGHT_CHEST,   true, rampBri, rampCt, 30000);
        cyclePut(LIGHT_DRESSER, true, rampBri, rampCt, 30000);
        chestOn = true;  // keep the NORMAL chest-edge flag consistent with what wake lit
    }
    // Overheads come last, and only on a bright morning (higher lux gate than above).
    if (lux >= WAKE_OVERHEAD_LUX && rampBri >= S2_BRI_LO) {
        cyclePut(LIGHT_CEIL_1, true, rampBri, rampCt, 30000);
        //cyclePut(LIGHT_CEIL_2, true, rampBri, rampCt, 30000);
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
        cyclePut(LIGHT_CEIL_1, false, 0, 0, 1000);
        //cyclePut(LIGHT_CEIL_2, false, 0, 0, 1000);
        overheadsOn = false;
    }

    float wdBri = windDownStartBri - (windDownStartBri - S4_FLOOR_BRI) * (windDownStep / 120.0f);
    wdBri = max(wdBri, S4_FLOOR_BRI);
    int wdCt  = (int)CT_WARM;

    Serial.println("Wind-down " + String(windDownStep * 0.5f, 1) + "/60.0 min — bri=" + String(wdBri, 1) + "%");
    if (windDownStep == 30 || windDownStep == 60 || windDownStep == 90) {
        sendLog("Wind-down " + String(windDownStep / 2) + "/60 min — bri=" + String(wdBri, 1) + "% — " + getTimeString());
    }

    // Floor lamp is the night-light anchor — dims to the floor and stays on.
    // (cyclePut skips any light excluded from the cycle.)
    cyclePut(LIGHT_FLOOR, true, wdBri, wdCt, 30000);

    // Chest collapses inward first: dims for the first half, then off for the
    // second half (step ≥ 60 / 30 min). Guarding on the cache (rather than a
    // one-shot at == 60) means a dashboard step-seek that jumps past 60 still
    // turns it off, and once it's off the echo keeps us from re-PUTting each tick.
    if (windDownStep < 60) {
        cyclePut(LIGHT_CHEST, true, wdBri, wdCt, 30000);
    } else if (cachedLight(LIGHT_CHEST).on) {
        cyclePut(LIGHT_CHEST, false, 0, 0, 10000);
    }

    // Dresser dims the full hour, off at the end (step 120 / 60 min), handing off
    // to LOCKED_OUT with only the floor lamp left glowing at the floor.
    if (windDownStep < 120) {
        cyclePut(LIGHT_DRESSER, true, wdBri, wdCt, 30000);
    } else {
        cyclePut(LIGHT_DRESSER, false, 0, 0, 10000);
        state = State::LOCKED_OUT;
        saveLastState(S4_FLOOR_BRI, (uint16_t)CT_WARM);
        Serial.println("Wind-down complete. Chest + dresser off, floor lamp at floor.");
        sendLog("Wind-down complete — " + getTimeString());
    }
    sentTarget = {wdBri, (uint16_t)wdCt};
}

void tickNormal(float lux, LightTarget target) {
    // Override detection now happens asynchronously inside the SSE handler
    // (handleLightUpdate), so tickNormal is no longer responsible for polling.

    // Turn off (once) any light the user has excluded from the cycle, before the
    // edge/update logic runs. cyclePut() below then skips those lights entirely.
    enforceExclusions();

    // Compute shouldUpdate here, not in loop(), so a forceState()-driven
    // sentTarget reset (via pollDashboardCommand earlier this tick) is reflected.
    bool shouldUpdate = fabsf(target.bri - sentTarget.bri) > STATE_TOLERANCE_BRI
                     || abs(target.ct - sentTarget.ct)    > STATE_TOLERANCE_CT;

    // Overhead edge — now at S2_LUX_HI (500). Off as lux drops below 500, on above.
    bool newOverheadsOn = lux > S2_LUX_HI;
    if (overheadsOn && !newOverheadsOn) {
        cyclePut(LIGHT_CEIL_1, false, 0, 0, 500);
        // cyclePut(LIGHT_CEIL_2, false, 0, 0, 500);
        shouldUpdate = true;
    } else if (!overheadsOn && newOverheadsOn) {
        shouldUpdate = true;
    }
    overheadsOn = newOverheadsOn;

    // Chest edge — at S3_LUX_HI (250), same pattern as the overheads. Below 250 the
    // chest cuts and the curve's discontinuity steps floor+dresser up to compensate;
    // above 250 it re-joins (turned on in the shouldUpdate block below). The off-PUT
    // fires only on the on→off transition, so a user who manually lit the chest in the
    // dim band keeps it (chestOn is already false → no repeated off-PUT).
    bool newChestOn = lux > S3_LUX_HI;
    if (chestOn && !newChestOn) {
        cyclePut(LIGHT_CHEST, false, 0, 0, 500);
        shouldUpdate = true;
    } else if (!chestOn && newChestOn) {
        shouldUpdate = true;
    }
    chestOn = newChestOn;

    // Resume-ramp advancement lives on TIMER_RESUME_STEP now (Stage 4 —
    // advanceResumeRamp() below). While the ramp runs, the curve tick still
    // does the edge detection above but must not count stable lux or fight
    // the ramp's PUTs, so it bows out here just like it always did.
    if (pauseResumeActive) return;

    if (lux <= 10 && timeClient.getHours() >= WIND_DOWN_GATE_HOUR) {
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
        cyclePut(LIGHT_FLOOR,   true, target.bri, target.ct, 1000);
        if (chestOn) cyclePut(LIGHT_CHEST, true, target.bri, target.ct, 1000);
        cyclePut(LIGHT_DRESSER, true, target.bri, target.ct, 1000);
        if (overheadsOn) {
            cyclePut(LIGHT_CEIL_1, true, target.bri, target.ct, 1000);
            // cyclePut(LIGHT_CEIL_2, true, target.bri, target.ct, 1000);
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

// Stage 4 — one step of the 10-min soft-pause resume ramp, fired by
// TIMER_RESUME_STEP. Body unchanged from its old home inside tickNormal();
// the ambient target now comes from lastLux (≤30 s old) instead of the
// enclosing tick's fresh read.
void advanceResumeRamp() {
    LightTarget target = luxToTarget(lastLux);
    pauseResumeStep++;
    float t       = min(pauseResumeStep / (float)PAUSE_RESUME_TICKS, 1.0f);
    float rampBri = pauseResumeStartTarget.bri + t * (target.bri - pauseResumeStartTarget.bri);
    int   rampCt  = (int)(pauseResumeStartTarget.ct  + t * (target.ct  - pauseResumeStartTarget.ct));
    rampBri       = constrain(rampBri, 1.0f, 100.0f);
    rampCt        = constrain(rampCt, (int)CT_COOL, (int)CT_WARM);
    cyclePut(LIGHT_FLOOR,   true, rampBri, rampCt, 30000);
    if (chestOn) cyclePut(LIGHT_CHEST, true, rampBri, rampCt, 30000);
    cyclePut(LIGHT_DRESSER, true, rampBri, rampCt, 30000);
    if (overheadsOn) {
        cyclePut(LIGHT_CEIL_1, true, rampBri, rampCt, 30000);
        // cyclePut(LIGHT_CEIL_2, true, rampBri, rampCt, 30000);
    }
    sentTarget = {rampBri, (uint16_t)rampCt};
    saveLastState(rampBri, (uint16_t)rampCt);
    Serial.println("Resume ramp " + String(pauseResumeStep) + "/" + String(PAUSE_RESUME_TICKS) +
                   " — bri=" + String(rampBri, 1) + "%, ct=" + String(rampCt));
    if (pauseResumeStep >= PAUSE_RESUME_TICKS) {
        pauseResumeActive = false;
        Serial.println("Resume ramp complete.");
    }
}

// Stage 4 — soft-pause expiry, fired by the one-shot TIMER_PAUSE_EXPIRY
// (replaces the per-tick elapsed poll tickSoftPause()). The elapsed re-check
// guards a queued-but-stale fire: if a SET_SOFT_PAUSE_REMAINING extend was
// processed after this event was enqueued, the pause isn't actually over —
// bail out and syncSoftTimers() re-arms the one-shot at the new deadline.
void expireSoftPause() {
    if (millis() - softPauseStart < softPauseDurationMs) return;
    // R10 — seed the resume ramp from where the user left the lights, not
    // from the pre-pause sentTarget. A pause usually *starts* because the
    // user changed the lights; interpolating from sentTarget made the first
    // resume tick snap back toward pre-pause brightness and then "fade".
    // Floor lamp is the room proxy (always-driven core light; per-light
    // seeding comes with R5). Fall back to sentTarget if the floor is off
    // or its cache is unpopulated.
    LightState f = cachedLight(LIGHT_FLOOR);
    pauseResumeStartTarget = {
        (f.on && f.bri > 0.0f) ? f.bri : sentTarget.bri,
        (uint16_t)((f.ct > 0)  ? f.ct  : sentTarget.ct)
    };
    pauseResumeActive      = true;
    pauseResumeStep        = 0;
    overheadsOn            = cachedLight(LIGHT_CEIL_1).on;  // sync from actual bridge state
    chestOn                = cachedLight(LIGHT_CHEST).on;   // sync from actual bridge state
    state                  = State::NORMAL;
    Serial.println("Soft pause expired — beginning 10-min resume ramp.");
    sendLog("Soft pause expired — resuming — " + getTimeString());
}

void triggerWake(float ambientLux) {
    // The floor lamp is the wake "start point" — the user physically flips it on,
    // and that rising edge is what fires this. Seed the ramp from its actual state.
    LightState floorLamp = cachedLight(LIGHT_FLOOR);
    float    startBri  = (floorLamp.on && floorLamp.bri > 0.0f) ? floorLamp.bri : S4_FLOOR_BRI;
    uint16_t startCt   = (floorLamp.ct  > 0) ? (uint16_t)floorLamp.ct  : (uint16_t)CT_WARM;
    wakeStartTarget    = {startBri, startCt};
    wakeEndTarget      = luxToTarget(ambientLux);
    wakeStep           = 0;
    state              = State::WAKE;
    Serial.println("Wake triggered — bri " + String(startBri) + " → " + String(wakeEndTarget.bri) +
                   ", ct " + String(startCt) + " → " + String(wakeEndTarget.ct));
    sendLog("Wake sequence started — " + getTimeString());
}

// The old floor-lamp poll (with its previous-state accumulator and seven sync
// sites) was deleted in Stage 3 (July 15) — wake trigger and lockout re-arm
// moved to dispatchSseLight(), driven by SSE on/off edges instead of a 30 s
// cache poll. See Markdowns/N1_MIGRATION.md Stage 3.

// [buttons removed 2026-06 — see N1_MIGRATION.md]
// void pollButton(ButtonState& btn, int pin) {
//     bool raw = (digitalRead(pin) == LOW); // convert to bool: true = pressed
//     unsigned long now = millis();
//
//     if (raw == btn.debounced) {
//         // Signal is stable — keep timer fresh so it's ready when signal next changes
//         btn.lastDebounceMs = now;
//     } else if (now - btn.lastDebounceMs >= DEBOUNCE_MS) {
//         // Signal has differed from last accepted state for long enough — commit it
//         btn.debounced = raw;
//         if (raw) {
//             btn.held         = true;
//             btn.pressStartMs = now;
//         } else {
//             btn.held = false;
//             if (!btn.longFired) btn.shortPress = true; // short press fires on release
//             btn.longFired = false;
//         }
//     }
//
//     // Long press: fire once when held past threshold
//     if (btn.held && !btn.longFired && now - btn.pressStartMs >= LONG_PRESS_MS) {
//         btn.longPress = true;
//         btn.longFired = true;
//     }
// }

void forceState(State next) {
    switch (next) {
        case State::LOCKED_OUT:
            stableLuxCount = 0;
            windDownStep   = 0;
            state          = State::LOCKED_OUT;
            break;
        case State::NORMAL:
            sentTarget        = {-1.0f, 0};
            stableLuxCount    = 0;
            overheadsOn       = cachedLight(LIGHT_CEIL_1).on;  // sync from actual bridge state
            chestOn           = cachedLight(LIGHT_CHEST).on;   // sync from actual bridge state
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
            softPauseStart      = millis();
            softPauseDurationMs = SOFT_PAUSE_MS;
            state               = State::SOFT_PAUSE;
            break;
        case State::HARD_OFF:
            state = State::HARD_OFF;
            break;
    }
    Serial.println("forceState → " + String(stateName()));
}

// [buttons removed 2026-06 — see N1_MIGRATION.md]
// void handleCycleButton() {
//     if (btnCycle.shortPress) {
//         btnCycle.shortPress = false;
//         State next = (State)(((int)state + 1) % 6);
//         forceState(next);
//         if (next != State::WAKE) { // triggerWake already sends discord
//             sendLog("Cycle button → " + String(stateName()) + " — " + getTimeString());
//         }
//     }
// }
//
// void handleButtonEvents() {
//     // Long press: hard off toggle
//     if (btnMode.longPress) {
//         btnMode.longPress = false;
//         if (state != State::HARD_OFF) {
//             state = State::HARD_OFF;
//             Serial.println("Button long press — hard off.");
//             sendLog("Hard off — " + getTimeString());
//         } else {
//             state         = State::LOCKED_OUT;
//             Serial.println("Button long press — hard off cleared.");
//             sendLog("Hard off cleared — " + getTimeString());
//         }
//     }
//
//     // Short press: go to NORMAL if not already there, otherwise soft pause
//     if (btnMode.shortPress) {
//         btnMode.shortPress = false;
//         if (state == State::HARD_OFF) {
//             // hard off is long-press only — ignore short press
//         } else if (state == State::NORMAL) {
//             state               = State::SOFT_PAUSE;
//             softPauseStart      = millis();
//             softPauseDurationMs = SOFT_PAUSE_MS;
//             Serial.println("Button short press — soft pause.");
//             sendLog("Soft pause (button) — " + getTimeString());
//         } else {
//             String prev = stateName();
//             state             = State::NORMAL;
//             sentTarget        = {-1.0f, 0};
//             stableLuxCount    = 0;
//             overheadsOn       = cachedLight(LIGHT_CEIL_1).on;  // sync from actual bridge state
//             chestOn           = cachedLight(LIGHT_CHEST).on;   // sync from actual bridge state
//             Serial.println("Button short press — NORMAL (was " + prev + ").");
//             sendLog("Returned to NORMAL by button — " + getTimeString());
//         }
//     }
// }

void setup() {
    Serial.begin(115200);
    pinMode(LEDR, OUTPUT);
    pinMode(LEDG, OUTPUT);
    pinMode(LEDB, OUTPUT);
    setRGB(false, false, false);

    // [buttons removed 2026-06 — see N1_MIGRATION.md]
    // pinMode(BTN_MODE,  INPUT_PULLUP);
    // pinMode(BTN_CYCLE, INPUT_PULLUP);

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
    LightState saved = cachedLight(LIGHT_CHEST);
    setLightColor(LIGHT_UUID_CHEST, true, 78.7f, 48000, 200, 1000); // fade in purple over 1s
    delay(3000);                                                        // hold for 3s
    setLight(LIGHT_UUID_CHEST, saved.on, saved.bri, saved.ct, 1000); // restore over 1s

    overheadsOn   = cachedLight(LIGHT_CEIL_1).on;  // seed from actual state — prevents false override and bad dashboard reporting
    chestOn       = cachedLight(LIGHT_CHEST).on;   // seed from actual state — same reason as overheadsOn

    sseConnect(); // open the persistent SSE event stream — handshake on the main task

    // N1 Stage 1 — event queue + Core-0 SSE task. Created after sseConnect()
    // so the initial handshake happens here; from this point the task owns the
    // stream (reconnects included) and the main task only consumes evQueue.
    evQueue = xQueueCreate(32, sizeof(Event));
    xTaskCreatePinnedToCore(sseTask, "sse", 12288, nullptr, 1, &sseTaskHandle, 0);
    Serial.println("SSE task started on core 0.");

    // N1 Stage 5 — dashboard networking task + its queues, started before the
    // Online log so even the boot log rides the queue (setup() no longer
    // blocks on a Railway round-trip).
    logQueue    = xQueueCreate(12, sizeof(LogMsg));
    statusQueue = xQueueCreate(1,  sizeof(StatusSnapshot)); // depth 1 — xQueueOverwrite, latest wins
    xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, &netTaskHandle, 0);
    Serial.println("Net task started on core 0.");

    sendLog("Online — " + getTimeString());

    // N1 Stage 2 — first LuxTick fires immediately (boot status/log lands right
    // away); scheduleNextLuxTick() aligns every subsequent tick to :00/:30.
    nextLuxTickDueMs = millis();
}

// ── N1 Stage 2 — LuxTick: the old monolithic tick body, now just an event ───
// Status print, lux read, state switch, status snapshot. The wait loop is gone
// (pacing lives in the loop() scheduler below) and since Stage 5 so is all
// dashboard HTTP: commands arrive as DashboardCmd events from netTask, and the
// status POST is a queue overwrite netTask ships from Core 0 — the only
// network I/O left on this path is bridge PUTs inside the state ticks.
static void dispatchLuxTick() {
    timeClient.update();
    printStatus();

    float lux = veml.readLux(VEML_LUX_AUTO);
    lastLux = lux;
    LightTarget target = luxToTarget(lux);
    Serial.print("Lux: "); Serial.print(lux, 1);
    Serial.print(" → bri="); Serial.print(target.bri, 1); Serial.print("%");
    Serial.print(", ct="); Serial.println(target.ct);

    switch (state) {
        case State::LOCKED_OUT:
            // Wake trigger + lockout re-arm are SSE-edge-driven now (Stage 3,
            // dispatchSseLight) — the tick has nothing to poll here.
            break;

        case State::NORMAL:
            tickNormal(lux, target);
            break;

        // Stage 4 — WAKE/WIND_DOWN ramp steps and SOFT_PAUSE expiry advance on
        // soft timers (dispatchTimerFire below), not the curve tick. The tick
        // still runs above for every state: status print, fresh lastLux for
        // the ramp handlers, command poll, status POST.
        case State::WAKE:
        case State::WIND_DOWN:
        case State::SOFT_PAUSE:
        case State::HARD_OFF:
            break;

    }

    queueDashboardStatus(lux);
}

// Compute the next LuxTick deadline. With ALIGNED_TICKS (N5), ticks land on
// wall-clock :00/:30 boundaries (±1 s — NTPClient is second-granular); without
// it, flash-relative 30 s from now. Called at enqueue time, so tick processing
// cost is naturally absorbed into the wait (same effect as the old
// tickStart-anchored wait loop). A short slot (< 5 s) means the tick that just
// fired *was* the boundary tick, landing a hair early against the truncated
// epoch (or displaced by an NTP resync / long tick) — scheduling the remainder
// would double-fire the same boundary (observed live July 14: :59+:03 tick
// pairs every few minutes), so skip to the following boundary instead.
static void scheduleNextLuxTick() {
#if ALIGNED_TICKS
    unsigned long secsIntoSlot = timeClient.getEpochTime() % 30UL;
    unsigned long waitMs = (30UL - secsIntoSlot) * 1000UL;
    if (waitMs < 5000UL) {
        Serial.printf("Tick align: %lu ms to boundary — just-fired tick claims it, skipping ahead.\n", waitMs);
        waitMs += 30000UL;
    }
#else
    unsigned long waitMs = 30000UL;
#endif
    nextLuxTickDueMs = millis() + waitMs;
}

// Enqueue a LuxTick when its deadline passes. Signed-difference comparison is
// rollover-safe. If the queue is momentarily full the deadline stays armed and
// the send retries on the next loop() pass — a tick is never silently skipped.
static void serviceLuxTickSchedule() {
    if ((int32_t)(millis() - nextLuxTickDueMs) < 0) return;
    Event ev;
    ev.type = EvType::LuxTick;
    ev.tMs  = millis();
    if (!evQueue || xQueueSend(evQueue, &ev, 0) != pdTRUE) return;
    scheduleNextLuxTick();
}

// ── N1 Stage 4 — soft-timer service ─────────────────────────────────────────

// Reconcile the active-timer set with (state, pauseResumeActive). Runs every
// loop() pass, so no transition site anywhere has to arm or disarm anything —
// a state change made in a tick body, forceState(), the SSE dispatcher, or an
// expiry handler is picked up within ~50 ms. Ramp timers arm due-now so entry
// into a ramp state PUTs on the next pass instead of waiting out a 30 s slot;
// the wakeStep/windDownStep/pauseResumeStep counters stay the state
// representation (dashboard seeks just rewrite them, cadence unaffected).
static void reconcileTimer(uint8_t id, bool shouldRun, uint32_t periodMs) {
    SoftTimer& t = softTimers[id];
    if (shouldRun && !t.active) {
        t.active   = true;
        t.dueMs    = millis(); // immediate first fire
        t.periodMs = periodMs;
    } else if (!shouldRun && t.active) {
        t.active = false;
    }
}

static void syncSoftTimers() {
    reconcileTimer(TIMER_WAKE_STEP,     state == State::WAKE,      RAMP_STEP_MS);
    reconcileTimer(TIMER_WINDDOWN_STEP, state == State::WIND_DOWN, RAMP_STEP_MS);
    reconcileTimer(TIMER_RESUME_STEP,
                   state == State::NORMAL && pauseResumeActive,    RAMP_STEP_MS);

    // Pause expiry is a one-shot at a *moving* deadline: SET_SOFT_PAUSE_REMAINING
    // and re-entry both rewrite softPauseStart/softPauseDurationMs, so recompute
    // the target every pass and re-aim whenever it differs. Dragging the slider
    // to 0 lands the deadline in the past → fires on the next pass.
    SoftTimer& pe = softTimers[TIMER_PAUSE_EXPIRY];
    if (state == State::SOFT_PAUSE) {
        uint32_t target = (uint32_t)(softPauseStart + softPauseDurationMs);
        if (!pe.active || pe.dueMs != target) {
            pe.active   = true;
            pe.dueMs    = target;
            pe.periodMs = 0;
        }
    } else {
        pe.active = false;
    }
}

// Enqueue TimerFire for each due timer. Queue-full leaves the deadline armed
// (retry next pass — a fire is never silently dropped, same policy as the
// LuxTick scheduler). Periodic re-arm is beat-anchored (dueMs += period) for
// drift-free cadence, but skips beats missed during a long stall (blocking
// HTTP) rather than burst-firing to catch up.
static void softTimersTick() {
    if (!evQueue) return;
    for (uint8_t id = 0; id < SOFT_TIMER_COUNT; id++) {
        SoftTimer& t = softTimers[id];
        if (!t.active || (int32_t)(millis() - t.dueMs) < 0) continue;
        Event ev;
        ev.type = EvType::TimerFire;
        ev.a    = id;
        ev.tMs  = millis();
        if (xQueueSend(evQueue, &ev, 0) != pdTRUE) return;
        if (t.periodMs) {
            t.dueMs += t.periodMs;
            if ((int32_t)(millis() - t.dueMs) >= 0) t.dueMs = millis() + t.periodMs;
        } else {
            t.active = false;
        }
    }
}

// TimerFire dispatch. Every handler re-checks state: the event may have been
// queued just before a transition (queue latency), and syncSoftTimers() only
// disarms on the next pass — a stale fire must be a no-op, not a wrong PUT.
static void dispatchTimerFire(const Event& ev) {
    switch (ev.a) {
        case TIMER_WAKE_STEP:
            if (state == State::WAKE) tickWakeRamp(lastLux);
            break;
        case TIMER_WINDDOWN_STEP:
            if (state == State::WIND_DOWN) tickWindDown();
            break;
        case TIMER_RESUME_STEP:
            if (state == State::NORMAL && pauseResumeActive) advanceResumeRamp();
            break;
        case TIMER_PAUSE_EXPIRY:
            if (state == State::SOFT_PAUSE) expireSoftPause();
            break;
        default:
            break;
    }
}

// N1 Stage 2 — loop() is now the consumer/dispatcher. Blocks up to 50 ms on
// the event queue (yields the core), dispatches whatever the SSE task or the
// scheduler enqueued, arms the next LuxTick, and services the Stage 4 soft
// timers. The overridePending check is belt-and-braces: if the SSE task's
// xQueueSend failed on a full queue, the flag alone still forces the pause —
// otherwise it would block every PUT forever (see the guard in setLight()).
void loop() {
    Event ev;
    if (evQueue && xQueueReceive(evQueue, &ev, pdMS_TO_TICKS(50)) == pdTRUE) {
        dispatchEvent(ev);
    } else if (overridePending) {
        applyOverridePause(-1);
    }
    serviceLuxTickSchedule();
    syncSoftTimers();  // reconcile before ticking so a just-entered state arms first
    softTimersTick();
}