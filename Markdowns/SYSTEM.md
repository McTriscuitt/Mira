# SYSTEM.md — Walkthrough of `src/main.cpp`

A linear, in-depth tour of the only translation unit in the firmware. Every global, helper, and tick function is described in the order it appears, with the design rationale for each. SSE coverage is exhaustive — that's where most of the system's hard problems live.

Companion docs (`SSE.md`, `SSE v1.0 Awkward Structure.md`, `FIRMWARE.md`) cover the same code from focused angles; this doc is the file-internal view.

---

## 1. Includes & Globals (lines 1–119)

### 1.1 Includes

Standard ESP32 + Arduino set: `WiFi`, `HTTPClient`, `WiFiClientSecure`, `mbedtls/x509_crt.h` (for the cert-time → epoch conversion), `ArduinoJson`, `NTPClient`, `WiFiUDP`, `Preferences`, `Adafruit_VEML7700`, and the two project headers (`config.h`, `lightcurve.h`). `mbedtls` is pulled in only for the cert fetch — everywhere else we go through `WiFiClientSecure`.

### 1.2 `struct LightState` and `LightTarget sentTarget`

```cpp
struct LightState { bool on; float bri; int ct; };
LightTarget sentTarget = {-1.0f, 0};
```

`LightState` is a plain snapshot carrier returned by `cachedLight(idx)`. `LightTarget` itself is declared in `lightcurve.h`. `sentTarget` represents the last bri/ct Mira asked the bulbs to be at. It's initialized to `{-1.0f, 0}` — values *outside* the valid Hue ranges — so the first `shouldUpdate` check in `tickNormal` is unconditionally true on tick 1. This is a sentinel trick: no `firstRun` boolean needed, and the same trick is re-applied whenever a state transition needs to force-PUT (e.g. `forceState(NORMAL)` and the mode-button short-press path both reset `sentTarget = {-1.0f, 0}`).

### 1.3 `bool overheadsOn`

Single-byte firmware intent flag — "do we *think* the overheads should be on?" Not a physical-state mirror. It's read by every tick function that decides whether to PUT the overheads, written by `tickNormal`'s overhead edge detection, and *resynced from the cache* (`overheadsOn = cachedLight(LIGHT_CEIL_1).on`) on every re-entry to `NORMAL` (mode button, cycle button via `forceState`, soft pause auto-resume). That re-sync is essential — without it, a user manual change during `SOFT_PAUSE`/`HARD_OFF` would leave `overheadsOn` permanently stale.

### 1.4 `CachedLight lightCache[LIGHT_COUNT]`

```cpp
struct CachedLight {
    bool  on          = false;
    float bri         = 0.0f;
    int   ct          = 0;
    bool  initialized = false;
};
CachedLight lightCache[LIGHT_COUNT]; // [LIGHT_CHEST, LIGHT_DRESSER, LIGHT_CEIL_1, LIGHT_CEIL_2, LIGHT_FLOOR]
```

The local mirror of bridge state. Seeded once at boot by `bootstrapLightStates()`, kept fresh by SSE thereafter. The `initialized` field is currently written but not read — held in reserve for future "cache stale?" logic. **The cache contract is read-mostly: "best-known last-reported state."** It does *not* participate in override detection — see `recentPuts[]` (§1.10) and `handleLightUpdate()` (§7.1).

### 1.5 `ButtonState` × 2

```cpp
struct ButtonState {
    bool debounced, held, shortPress, longPress, longFired;
    unsigned long lastDebounceMs, pressStartMs;
};
ButtonState btnMode, btnCycle;
```

`pollButton()` (§11.1) takes a reference and operates on either button generically. Bundling the per-button state in a struct is what makes the polling code reusable.

### 1.6 `float lastLux = 1.0f`

Cached from the most recent VEML read so the cycle button can call `triggerWake(lastLux)` from inside the 30-second wait loop, where no fresh lux sample has been taken.

### 1.7 `enum class State` and the state machine globals

```cpp
enum class State { LOCKED_OUT, NORMAL, WAKE, WIND_DOWN, SOFT_PAUSE, HARD_OFF };
State state = State::LOCKED_OUT;
```

Scoped enum — `State::NORMAL`, not bare `NORMAL` — so the compiler catches any int→State implicit conversions.

Three clusters of state follow:

- **Wind-down:** `stableLuxCount` (counter that increments when `lux ≤ 10 && hour ≥ 21`, decrements otherwise with floor 0), `windDownStep` (0–120 over 60 min at 30 s/tick), `windDownStartBri` (bri snapshot at trigger time).
- **Wake:** `wakeStep`, `wakeStartTarget`, `wakeEndTarget`.
- **Soft pause:** `softPauseStart`, `pauseResumeActive`, `pauseResumeStep`, `pauseResumeStartTarget`.

`lastFloorOn` sits alone — it's the edge-detector seed for `checkFloorState()`.

### 1.8 `RecentPut recentPuts[LIGHT_COUNT][3]` — the echo-discrimination ring

```cpp
struct RecentPut {
    bool          active;
    bool          onTarget;
    float         priorBri;     // lightCache[idx].bri snapshot at PUT time
    float         targetBri;    // value we asked for
    int           priorCt;
    int           targetCt;
    unsigned long postedAtMs;
    unsigned long durationMs;
};
const int RECENT_PUT_RING_SIZE = 3;
RecentPut recentPuts[LIGHT_COUNT][RECENT_PUT_RING_SIZE];
```

A **3-slot ring per light**. Each outgoing PUT snapshots its `(prior, target)` trajectory into one of three slots so the SSE handler can recognize self-PUT echoes versus external overrides. This is **the** key piece of architecture in the file. Full treatment in §4 and §7.

Why three slots and not one (which is what the initial Phase 3 implementation shipped with): overlapping in-flight PUTs are common. A `tickWakeRamp` tick PUT with `dynamics.duration = 30000` is still settling when the next tick fires its own PUT; with a single slot, the second `noteRecentPut` overwrites the first and a late echo of the first PUT misclassifies as override. Same for a lux-driven PUT that lands on a bulb whose overhead-toggle PUT is still echoing. Three slots cover the common cases without growing the trajectory window so wide it masks small real overrides.

### 1.9 The tolerance constants

```cpp
const unsigned long RECENT_PUT_GRACE_MS  = 5000UL;
const float         TRAJECTORY_TOLERANCE_BRI = 5.0f;   // percent
const int           TRAJECTORY_TOLERANCE_CT  = 15;     // mirek
```

Note these are deliberately wider than the `STATE_TOLERANCE_*` constants in `config.h`. They have two different jobs:

- `STATE_TOLERANCE_BRI = 1.2f` / `STATE_TOLERANCE_CT = 3` — "is the curve drifted enough to send a new PUT?" (used in `tickNormal`). Tight on purpose.
- `TRAJECTORY_TOLERANCE_BRI = 5.0f` / `TRAJECTORY_TOLERANCE_CT = 15` — "is this incoming SSE echo within the slop we expect from Zigbee + bulb-side step quantization?" Wider on purpose: bulbs snap to discrete steps, and the cached `priorBri`/`priorCt` may itself be one step off the bulb's real pre-PUT state. Initial Phase 3 reused `STATE_TOLERANCE_*` for both jobs and false-positived on every NORMAL re-entry; fixed in commit `71ee541`.

`RECENT_PUT_GRACE_MS` is slack past `durationMs` for late echoes from slow Zigbee mesh settles.

### 1.10 SSE globals

```cpp
WiFiClientSecure sseClient;
String        sseBuf;                 // partial-line accumulator
unsigned long sseLastByteMs    = 0;
unsigned long sseLastConnectMs = 0;
const unsigned long SSE_RECONNECT_DELAY_MS = 5000UL;
const unsigned long SSE_STALE_TIMEOUT_MS   = 600000UL;  // 10 min
```

`sseClient` is the persistent TLS socket. `sseBuf` accumulates bytes between `\n`s — SSE bytes arrive in arbitrary chunks, so we have to buffer partial lines. `SSE_STALE_TIMEOUT_MS` is 10 minutes because Hue v2 sends no keepalive — long silence is normal.

### 1.11 The rest

`bool lastFloorOn` (edge detector), `WiFiUDP ntpUDP` + `NTPClient timeClient`, and `static String _bridgeCertPem` (file-scope cert PEM loaded from NVS).

---

## 2. TLS Cert Management (lines 124–238)

Three helpers, all file-static.

### 2.1 `_derToPem(der, len)`

Base64-encodes DER bytes with 64-char line wrapping and adds the `-----BEGIN/END CERTIFICATE-----` markers. Hand-rolled because pulling in mbedTLS's PEM writer would balloon the binary; the encoding is ~20 lines of inline base64.

### 2.2 `_certTimeToEpoch(mbedtls_x509_time&)`

Converts the cert's `valid_to` field to a Unix epoch so we can compare against `timeClient.getEpochTime()`. Inline day-of-month table with explicit leap-year handling. Stored in NVS as `hueCertExpiry`.

### 2.3 `_fetchAndStoreBridgeCert()`

Opens a one-off `WiFiClientSecure` with `setInsecure()`, issues a minimal `GET /clip/v2/resource/light` to drive the handshake (we don't care about the response body — we just need the handshake to complete so the peer cert is available), then calls `client.getPeerCertificate()` and encodes it to PEM. PEM + expiry are written to NVS namespace `"mira"`.

### 2.4 `ensureBridgeCert()`

Run at the top of `setup()` after NTP sync. Loads cert + expiry from NVS. If missing, expired, or within 30 days of expiry → refetch. Otherwise loads `_bridgeCertPem` from NVS and verifies it still authenticates against the live bridge (`test.setCACert(...); test.connect(...)`). If that verify fails, the cert has rotated and we refetch. If the refetch fails but a stored cert exists, we fall back to it rather than refuse to boot.

**Caveat:** the pinned cert is loaded into `_bridgeCertPem` but is currently *not consumed* by `setLight`, `setLightColor`, `bootstrapLightStates`, or `sseConnect` — all four call `setInsecure()`. That unification is an open item.

---

## 3. Light-Cache Helpers (lines 243–257)

```cpp
static int idxByUuid(const char* uuid);
LightState cachedLight(int idx);
```

`idxByUuid` is the only place that maps a v2 UUID string to a `lightCache[]` index. A handful of `strcmp`s (chest, dresser, ceiling 1, floor — ceiling 2 commented out), returns `-1` on unknown. A sibling `lightName(idx)` maps the index back to a friendly label for Serial/bootstrap logging. Called everywhere an incoming UUID needs to be routed (`handleLightUpdate`, `bootstrapLightStates`, `noteRecentPut`, `setLight*`).

`cachedLight(idx)` returns a `LightState` copy out of `lightCache[]`. This is the universal "what does the bulb look like right now?" read path — used by `triggerWake`, `checkFloorState`, every NORMAL/LOCKED_OUT re-entry resync, and the boot flourish snapshot.

---

## 4. The Recent-PUT Trajectory Ring + No-Op Filter

This is the heart of override discrimination. Two layers run in series on every incoming light event:

1. **Trajectory ring (`recentPuts[LIGHT_COUNT][3]`)** — does this event lie on the trajectory of any live in-flight PUT for this light?
2. **No-op event filter** — if the trajectory check missed, is the event effectively equal to the *pre-event cache* (i.e. a late settling echo whose slot has already expired)?

Anything that fails both checks is classified `Override` and fires `SOFT_PAUSE`.

### 4.1 `noteRecentPut(idx, on, bri, ct, durationMs)`

Called from `setLight()` and `setLightColor()` *before* the HTTP PUT goes out. Returns the slot index it wrote (used by the caller to refresh `postedAtMs` after the PUT actually returns — see §6.4).

Slot selection: scan the three slots for this light, prefer the first inactive/expired slot so live trajectories from earlier PUTs aren't stomped. If every slot is currently live, overwrite the oldest one — that's the entry whose remaining grace contributes least.

```cpp
RecentPut& r = recentPuts[idx][slot];
r.active     = true;
r.onTarget   = on;
r.priorBri   = lightCache[idx].bri;   // snapshot pre-PUT cache value
r.targetBri  = bri;
r.priorCt    = lightCache[idx].ct;
r.targetCt   = ct;
r.postedAtMs = millis();
r.durationMs = durationMs;
```

**Why this must happen before the HTTP call.** The bridge can emit an SSE echo of a PUT *faster than `HTTPClient::PUT()` returns control*. If `noteRecentPut` ran after the PUT, the echo could land first, find no record, and trip override detection on our own command. The ordering is counter-intuitive but mandatory.

**Why `postedAtMs` is refreshed after the PUT returns.** The HTTP PUT block itself can eat 100–500 ms (TLS handshake + bridge roundtrip). `priorBri` was correctly snapshotted before the PUT, but the grace budget should count from when the PUT actually went out, otherwise a chain of sequential `setLight` calls can let the first slot's grace expire before its own echo is drained from the SSE socket. The refresh fixes that — see §6.4.

### 4.2 `eventMatchesRecentPut(idx, hasOn, evOn, hasBri, evBri, hasCt, evCt)`

Iterates every live slot for `idx`; first match wins. For each present field on the incoming event, check if the value lies within `[min(prior, target), max(prior, target)] ± TRAJECTORY_TOLERANCE_*`. A match requires every present field to be in range; any field that's out of range disqualifies that slot, and we move to the next.

```cpp
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
    return true;
}
return false;
```

Auto-expiry happens inside the loop — there is no separate cleanup pass; expired slots are marked `active = false` the next time they're visited.

**Why trajectory matching, not endpoint matching.** A Hue PUT with `dynamics.duration > 0` ramps the bulb, and the bridge emits intermediate echoes during the ramp. A `1000 ms` PUT from `bri = 80` to `bri = 45` produces a sequence of echoes reporting roughly `bri = 78, 70, 60, 50, 45`. Matching against only the target `45` would flag all four intermediates as overrides. Matching against the interval `[45, 80] ± 5` absorbs every intermediate naturally — and a real user-driven override during the ramp (say, the user slides to `bri = 20`) lands outside the interval and still trips.

Trajectory framing converts a hard timing problem ("when does the ramp end?") into a simple set-membership check.

**Why match only fields present in the event.** The bridge splits state changes across multiple events — a `dimming`-only event can arrive separately from a `color_temperature`-only event for the same user action. If we merged the event with cached fields and matched the merged record, we'd compare a stale cached field against the current trajectory and frequently misfire. The "only present fields" rule keeps each event's match self-contained.

### 4.3 The No-Op Event Filter

If `eventMatchesRecentPut` returns `false`, before declaring `Override` the handler runs a second check: is the event's value within `TRAJECTORY_TOLERANCE_*` of the **pre-event cache** (snapshotted in `handleLightUpdate` before any cache write)?

```cpp
bool isNoOp = true;
if (hasOn  && evOn != cacheOnBefore)                                    isNoOp = false;
if (hasBri && fabsf(evBri - cacheBriBefore) > TRAJECTORY_TOLERANCE_BRI) isNoOp = false;
if (hasCt  && abs(evCt   - cacheCtBefore)    > TRAJECTORY_TOLERANCE_CT) isNoOp = false;
decision = isNoOp ? EchoOutcome::NoOpEcho : EchoOutcome::Override;
```

**The timing gap this closes.** When `tickNormal` decides "no change — skipping PUT" for several ticks in a row, nothing refreshes any `recentPuts` slot for that light. Meanwhile, the bridge can still emit late settling confirmation events ~30+ s after the last actual PUT (Zigbee mesh stragglers reporting their final `mirek` 1–2 units off the value the bridge has already settled to). With nothing in the ring to match, the trajectory check returns `false` and the event would historically fire `Override` — a misfire on what's effectively a bridge-internal cleanup event.

The no-op filter catches this: if the event is reporting a value already within tolerance of what we believe is true, it's not an override regardless of which PUT (if any) generated it. The semantic threshold for "real override" is unchanged — `TRAJECTORY_TOLERANCE_BRI = 5%` and `TRAJECTORY_TOLERANCE_CT = 15` mirek — so a real user-driven override (double-digit-percent bri or 50+ mirek) still trips correctly through either path.

---

## 5. `bootstrapLightStates()` (lines 313–342)

One-shot HTTPS GET to `/clip/v2/resource/light` using a throwaway `setInsecure()` client. Parses the JSON `data[]` array, maps each light's `id` UUID to its index, and copies `on.on`, `dimming.brightness`, and `color_temperature.mirek` into `lightCache[idx]`. Sets `initialized = true`. Prints a one-line debug for each light.

This is the *only* per-tick HTTP GET path that hits the lights endpoint. SSE is the source of truth for every subsequent state change. Open item: re-run this on every SSE reconnect to close the missed-events gap.

---

## 6. Hue v2 PUT Path (lines 346–451)

### 6.1 `setRGB(r, g, b)` and `connectWiFi()`

The onboard RGB on the Nano ESP32-S3 is active-LOW, so the helper inverts inside the function — every call site reads `setRGB(true, false, false)` as "red on." `connectWiFi()` is a blocking spin during `setup()`; red-blinks while connecting, solid green when associated.

### 6.2 `_hsbToXY(hueV1, satV1, x, y)`

HSB (v1 hue 0–65535, sat 0–254) → linear RGB via gamma 2.4 → CIE XYZ → normalized xy. Used only by `setLightColor` to convert the boot-flourish purple from v1's `hue/sat` calling convention to v2's required `color.xy`.

### 6.3 `setLightColor(uuid, on, bri, hueV1, satV1, durationMs)`

The color-mode PUT helper. Called exactly once in the codebase — for the boot purple flourish. Notable details:

```cpp
void setLightColor(...) {
    if (state == State::SOFT_PAUSE || state == State::HARD_OFF) return;   // mid-batch abort guard
    int idx = idxByUuid(uuid);
    int ctTarget = (idx >= 0) ? lightCache[idx].ct : 0;
    int slot = noteRecentPut(idx, on, bri, ctTarget, (unsigned long)durationMs);
    ...
}
```

Color mode doesn't change CT meaningfully, but the trajectory record requires a CT pair. Passing through the cached value makes the CT check a self-match — the echo's `mirek` (which the bridge will continue to report) lies on the degenerate trajectory `[cachedCt, cachedCt] ± tol`, so it's recognized as ours.

The body sends `on.on`, `dynamics.duration`, and (only if `on`) `dimming.brightness` + `color.xy`. The "only if on" is important: PUTting `dimming` while turning off causes the bridge to reject the whole request.

The mid-batch abort guard at the top is shared with `setLight()` — see §6.4 for the rationale. The boot purple flourish is unaffected because `setup()` runs the flourish in `State::NORMAL` (the static initializer) and `sseConnect()` hasn't opened yet, so no override can fire during it.

### 6.4 `setLight(uuid, on, bri, ct, durationMs)`

The white/CT PUT helper — the workhorse for every state-driven command. Same shape as `setLightColor` minus the HSB conversion. Body: `on.on`, `dynamics.duration`, and (only if `on`) `dimming.brightness` + `color_temperature.mirek`.

```cpp
void setLight(...) {
    if (state == State::SOFT_PAUSE || state == State::HARD_OFF) return;   // mid-batch abort guard
    int idx  = idxByUuid(uuid);
    int slot = noteRecentPut(idx, on, bri, ct, (unsigned long)durationMs);
    // ... HTTP PUT ...
    if (idx >= 0 && slot >= 0) recentPuts[idx][slot].postedAtMs = millis();   // postedAtMs refresh
    sseTick();                                                                 // drain echoes
}
```

Two ordering-critical pieces beyond the body itself:

- **Mid-batch abort guard (top of function).** Each `setLight*()` tail-calls `sseTick()` to drain pending echoes (last line above). `sseTick()` is synchronous — when it processes an `Override` event, `handleLightUpdate()` flips `state = SOFT_PAUSE` *before returning*. Without this guard, the next PUT in a chained tick (`tickNormal`'s 4-bulb batch, `tickWakeRamp`, `tickWindDown`) would proceed against the now-stale lux-curve target the user has already manually overridden. Bailing *before* `noteRecentPut` is essential — otherwise we'd leave an orphan trajectory slot with no matching PUT, and an unrelated future event for the same bulb could match against it during its `durationMs + RECENT_PUT_GRACE_MS` lifetime, silently misclassifying a real override as `EchoMatch`. See the May 13 patch writeup in `Bug Records/` for the full incident.
- **`postedAtMs` refresh (after PUT returns).** The TLS handshake + bridge roundtrip inside `http.PUT()` can eat 100–500 ms. `priorBri`/`priorCt` were correctly snapshotted before the PUT, but the grace budget should count from when the PUT actually went out — otherwise a chain of four sequential `setLight` calls in `tickNormal` can let the first slot's grace clock expire before its own echo is drained from the SSE socket. The refresh keeps each slot's age small relative to its grace window.

The tail `sseTick()` after the refresh is what gives override detection its low latency — without it, echoes pile up in the kernel buffer until the wait-loop drain on the next 50 ms cycle.

**Why `dynamics.duration` is in ms here.** v1 `transitiontime` was 100 ms units (`300` = 30 s). v2 `dynamics.duration` is ms (`30000` = 30 s). The migration replaced every call site; the constant differences are:

| Caller                               | Duration | Reason                                                              |
|:-------------------------------------|:---------|:--------------------------------------------------------------------|
| `tickNormal` lux-driven PUTs         | `1000`   | Snap to new target — drift check already gates frequency            |
| `tickWakeRamp` / `tickWindDown` PUTs | `30000`  | Equal to tick interval; bulb interpolates smoothly between commands |
| `tickWindDown` chest/dresser-off     | `10000`  | Gentle off — chest at midpoint (step 60), dresser at completion     |
| `tickNormal` overhead-off            | `500`    | Quick cut on lux crossing 200                                       |
| `tickSoftPause`-resume ramp PUTs     | `30000`  | Same as wake/wind-down rationale                                    |
| Boot flourish PUTs                   | `1000`   | Aesthetic                                                           |

---

## 7. SSE — The Core of `main.cpp` (lines 537–706)

Everything from `handleLightUpdate` down to `sseTick` exists to maintain a persistent, real-time channel of bridge state changes — replacing v1's per-tick polling. Reading order in the file matches data flow: `handleLightUpdate` consumes events, `handleSseEventData` parses payloads into events, `handleSseLine` filters protocol lines into payloads, `sseConnect` opens the socket, `sseTick` drives the read pump.

### 7.1 `handleLightUpdate(JsonObjectConst upd)` — the override-detection brain

Runs once per `light`-type event. Every event ends in exactly one of eight `EchoOutcome` classifications; only `Override` fires `SOFT_PAUSE`. Sequence:

1. Resolve `upd["id"]` via `idxByUuid`. Unknown UUID → `UnknownUuid`, return.
2. Extract `on.on`, `dimming.brightness`, `color_temperature.mirek` as `JsonVariantConst`. Compute `hasOn`/`hasBri`/`hasCt` via `.isNull()`. If all three absent → `NoFields`, return.
3. **Snapshot pre-event cache values** into local `cacheOnBefore` / `cacheBriBefore` / `cacheCtBefore`. These are preserved through the rest of the function for both the no-op filter (step 7) and the diagnostic record (step 9). This snapshot must happen *before* the cache write in step 8.
4. Classify into `EchoOutcome`. The decision cascade:
   - `state != NORMAL` → `SkipState`. Override detection is `NORMAL`-only.
   - `pauseResumeActive` → `SkipPauseResume`. The resume ramp is itself a sequence of self-PUTs.
   - `expectedOn == false` for this idx (`LIGHT_CEIL_*` when `overheadsOn == false`) → `SkipOffLight`. We don't fire override for lights we don't think should be on.
   - `eventMatchesRecentPut(idx, hasOn, evOn, hasBri, evBri, hasCt, evCt)` returns `true` → `EchoMatch`. Done.
5. **No-op filter (the second layer).** If we got past `EchoMatch` without classification, check whether the event's values are within `TRAJECTORY_TOLERANCE_*` of `cacheBefore`. If yes → `NoOpEcho` (late settling echo whose trajectory slot has already expired — see §4.3). If no → `Override`.
6. **Write the present fields to `lightCache[idx]`.** This happens *after* classification — the inverse of the original Phase 3 ordering. Why: the override decision in step 4–5 needs the *pre-event* cache for the no-op filter, and the diagnostic record in step 9 needs it too. The cache itself is the bridge's best-known last-reported state; one event of write-after-classify delay is invisible elsewhere because nothing reads the cache in this code path.
7. **`recordSseEvent(...)`** appends one entry to the `sseEventRing[16]` diagnostic ring with the event fields, the pre-event cache snapshot, and the classified outcome. Cumulative `echoOutcomeCounts[]` histogram also increments here.
8. If `decision != Override` → return.
9. **Override fired.** Set `state = SOFT_PAUSE`, `softPauseStart = millis()`, `pauseResumeActive = false`. Call `dumpOverrideDiagnostic()` which ships a multi-line dump to the dashboard via `sendLog()` containing the triggering event, every active `recentPuts[idx][s]` slot's full state and age, the last 6 ring entries for this idx, and the outcome histogram. Then `sendLog("Manual override — soft pause — …")`.

The diagnostic dump only fires on the override path — it's free in steady state, and invaluable when a misfire needs root-causing. Without it you'd have to guess which of the layered checks let the event through, and what the cache and trajectory state actually looked like at that instant. The dashboard log row contains embedded newlines; paste it back into a debugging conversation and the failure conditions are fully reconstructable without a USB cable.

**Why classification before cache write.** If we wrote the cache first (as Phase 3 originally did), the no-op filter would compare the event against itself and every event would be classified `NoOpEcho`. Inverting the order so `cacheBefore` reflects the pre-event state — and is the only thing the filter compares against — keeps the filter semantically correct without affecting downstream cache consumers.

### 7.2 `handleSseEventData(const char* json)` — the JSON dispatcher

The payload is an *array* of events (the bridge batches updates). For each event with `type == "update"`, walk its inner `data` array and dispatch each `type == "light"` resource update to `handleLightUpdate`. Everything else — `button`, `relative_rotary`, `motion`, `zigbee_connectivity`, `device_power` — is silently dropped today.

`JsonDocument` parses the payload as a value; failure (`deserializeJson != 0`) returns without logging. SSE payloads can be malformed if a TLS record gets truncated mid-line; silent skip is the right behavior.

### 7.3 `handleSseLine(const String& line)` — the protocol filter

SSE wire format has three line types:

- `data: <json>` — payload; we strip the prefix (and one optional leading space) and forward to `handleSseEventData`.
- `id: <event-id>` — bridge-assigned ID; we don't currently use it (no last-event-id replay).
- `: <comment>` — server comment; the bridge sends `: hi`-style heartbeats, ignored.

Empty lines aren't passed in (`sseTick` filters them) — only non-empty buffered lines reach this function.

### 7.4 `sseConnect()` — opening the persistent stream

Manual GET issued via raw `WiFiClientSecure.print()` rather than `HTTPClient`, because there's no clean `HTTPClient` API for "drain the headers then leave the socket open and hand it back to me." Steps:

1. `sseClient.stop()` + clear `sseBuf` — defensive reset.
2. `setInsecure()` + `setTimeout(5)`.
3. `connect(HUE_BRIDGE_HOST, 443)`. Fail → log and return.
4. Send the GET line + headers (`Host`, `hue-application-key`, `Accept: text/event-stream`, `Connection: keep-alive`).
5. Loop reading bytes byte-by-byte for up to 5 s, accumulating into `hdrLine`, splitting on `\n`. When we see an empty line (`hdrLine.length() == 0` after `\n`), the response-header section is complete; mark `sseLastByteMs = millis()` and return — the socket is now in the SSE body and ready to stream.
6. If 5 s elapses without seeing the empty terminator → log timeout and `stop()`.

A subtlety: the loop strips `\r` (`else if (c != '\r')`) so CRLF line endings collapse cleanly to LF.

### 7.5 `sseTick()` — the drain pump

Called every ~50 ms from the wait loop in `loop()`. Three responsibilities per call:

**(a) Reconnect.** If `!sseClient.connected()`, check the throttle:

```cpp
if (millis() - sseLastConnectMs >= SSE_RECONNECT_DELAY_MS) {
    sseLastConnectMs = millis();
    sseConnect();
}
```

The 5 s throttle keeps us from hammering the bridge during outages. Critically, `sseLastConnectMs` is updated *before* calling `sseConnect()` — that way a slow-connecting attempt doesn't bypass the throttle if it fails.

**(b) Drain bytes.** The key choice here is:

```cpp
int b;
while ((b = sseClient.read()) >= 0) { ... }
```

…and not the more familiar `while (sseClient.available()) { ... }`. **`WiFiClientSecure::available()` only inspects the already-decrypted internal buffer.** If new TLS records are sitting in the kernel TCP buffer waiting to be decrypted, `available()` returns 0 and the loop exits without pulling them. `read()` calls `mbedtls_ssl_read()`, which actively pulls and decrypts TLS records from the underlying TCP socket. The bridge sends heartbeats and small events as standalone TLS records; the `available()` pattern missed them and caused spurious 10-min stale timeouts. Fixed by switching to `read()`.

For each byte read: update `sseLastByteMs`, append to `sseBuf` (or terminate a line on `\n`), drop `\r`, and defensively reset `sseBuf` if it grows past 4096 bytes (never triggered in practice).

**(c) Stale-stream watchdog.** If `millis() - sseLastByteMs >= SSE_STALE_TIMEOUT_MS` (10 min), close the socket. The next call to `sseTick` will reconnect (subject to the throttle).

**Why 10 min.** Hue v2 sends no keepalive frames — long silence is legitimate during idle network periods. Tightening below 10 min produces spurious reconnects, each of which creates a missed-events gap where real changes are lost.

### 7.6 Where `sseTick()` runs — the latency picture

`sseTick()` is invoked *only* from the inter-tick wait loop. While `loop()` is blocked inside an HTTP call (`pollDashboardCommand`, `sendDashboardStatus`, `sendLog`, any `setLight*`), SSE bytes pile up in the kernel buffer and are not processed.

In practice this is fine for steady state — the wait loop is 30 s wide, HTTP work is ~1–2 s, and the bridge buffers bytes for us — but it means **override detection has up to ~2 s of worst-case latency**. The ESP32-S3's Core 1 is idle today. Moving `sseTick()` to a FreeRTOS task on Core 1 is the planned next step; the cache-contract simplification (cache is read-mostly, override semantics live in `recentPuts[]` only) shipped specifically to make that move low-risk.

### 7.7 Diagnostics — `EchoOutcome`, `sseEventRing`, `echoOutcomeCounts`, `dumpOverrideDiagnostic`

Echo discrimination is only as good as our ability to inspect it when it misclassifies. Three diagnostic surfaces are always on; one fires only on the override path.

**`EchoOutcome` — the classification vocabulary.**

```cpp
enum class EchoOutcome : uint8_t {
    UnknownUuid,       // event for a UUID outside lightCache[]
    NoFields,          // no on / dimming / color_temperature in payload
    SkipState,         // state != NORMAL
    SkipPauseResume,   // pauseResumeActive
    SkipOffLight,      // expectedOn == false for this idx
    EchoMatch,         // trajectory ring matched
    NoOpEcho,          // trajectory missed, cacheBefore matched (late settling echo)
    Override           // off-trajectory and off-cache — fires SOFT_PAUSE
};
```

Every call into `handleLightUpdate()` ends in exactly one outcome. The enum is the single vocabulary used by the ring, the histogram, and the override dump — they all speak the same eight values.

**`sseEventRing[16]` — last 16 classified events.**

```cpp
struct SseEventRecord {
    unsigned long timeMs;
    int           idx;
    bool          hasOn;   bool  evOn;
    bool          hasBri;  float evBri;
    bool          hasCt;   int   evCt;
    bool          cacheOnBefore;
    float         cacheBriBefore;
    int           cacheCtBefore;
    EchoOutcome   outcome;
};
SseEventRecord sseEventRing[16];
int            sseEventRingHead = 0;
```

`recordSseEvent()` appends one record per call. The `cacheBefore` triplet is captured before the cache write (§7.1 step 3) so the record reflects the delta between what we *thought* was true and what the event *reported*. Pasting one of these rings into a debugging conversation makes it possible to reconstruct exactly which path each event took without a USB cable.

**`echoOutcomeCounts[8]` — cumulative since boot.**

A simple `unsigned long` per outcome, incremented in `recordSseEvent`. Included in the override dump as a one-line histogram (`unknown-uuid=0 no-fields=0 skip-state=78 ...`). Useful signal:

- High `NoOpEcho` confirms the late-settling-echo pattern was widespread before the filter shipped.
- High `EchoMatch` confirms the trajectory ring is doing its job.
- Any nonzero `Override` is worth a look — at minimum to confirm whether it was user-driven or a misfire.

**`dumpOverrideDiagnostic()` — dashboard-bound full dump on Override.**

Fires from step 9 of §7.1. Ships a multi-line `sendLog()` containing:

- The triggering event's `hasOn/evOn`, `hasBri/evBri`, `hasCt/evCt`
- `cacheBefore` (pre-event cache snapshot)
- Every active `recentPuts[idx][s]` slot's full state and age vs `durationMs + grace` (active or expired)
- The last 6 ring entries for this idx, each annotated with its outcome
- The full outcome histogram

Because it only runs on the override path, it's free in steady state. The dashboard renders the multi-line message as a single log row; the line breaks survive into the database, so the dump can be reformatted for inspection later.

**`#define ECHO_TRACE 1`** — uncomment near the `EchoOutcome` enum to enable per-event Serial logging during tuning. Off by default because steady-state operation emits dozens of events per minute.

Full reference: `SSE.md` §Diagnostics.

---

## 8. Dashboard Helpers (lines 495–535)

### 8.1 `sendDashboardLog(message)` / `sendLog(message)`

Bearer-auth POST to `<DASHBOARD_BASE_URL>/api/log` with `{ "message": ... }`. `sendLog` is a one-line wrapper — kept around so callers don't need to know the underlying name, and so a second sink (Serial, SD) could be added in one place.

### 8.2 `sendDashboardStatus(lux)`

POST to `/api/status` with the full state object: `state` (string), `lux`, `bri`, `ct`, `overhead_on`, `wind_down_step`, `wake_step`, `wake_total`, and `soft_pause_remaining_s`.

The pause-remaining calculation is the one signed-math site in the file:

```cpp
long pauseRemaining = (state == State::SOFT_PAUSE) ?
    max(0L, ((long)SOFT_PAUSE_MS - (long)(millis() - softPauseStart)) / 1000L) : 0L;
```

The `(long)` casts before the subtraction matter: `SOFT_PAUSE_MS` and `millis()` are both `unsigned long`. Without the casts the subtraction wraps on underflow instead of going negative, and the `max(0L, ...)` clamp becomes useless.

---

## 9. `pollDashboardCommand()` (lines 710–753)

GET to `/api/command` with bearer auth. On 200, parses the response, reads `command`, `id`, `value`. If `command` is null, return.

Command dispatch: `NORMAL`, `SOFT_PAUSE`, `HARD_OFF`, `LOCKED_OUT` → `forceState`. `WIND_DOWN` / `WAKE` accept an optional `value` to seek into the ramp (with `windDownStep` capped at 119 — step 120 fires the transition to `LOCKED_OUT`, so seeking *to* completion is disallowed). `SET_WIND_DOWN_STEP` and `SET_WAKE_STEP` are state-gated seeks.

If `id >= 0`, POST to `/api/command/<id>/ack` to advance the queue. Forward declaration of `forceState` appears above this function because `forceState` is defined later in the file.

`pollDashboardCommand` runs *before* the state dispatch in `loop()`, so a command received this tick takes effect this tick — no one-tick lag.

---

## 10. State-Tick Functions (lines 755–949)

### 10.1 `saveLastState(bri, ct)`

Two-field NVS write under namespace `"mira"`: `savedBri` (float) and `savedCt` (uint16_t). Called on every PUT in `tickNormal` and once at wind-down completion. Currently *not* read on the read side anywhere — `triggerWake` reads from the cache, not NVS. Kept as a write-only journal in case a "restore on reboot" feature comes back.

### 10.2 `tickWakeRamp(lux)`

```cpp
wakeStep++;
float t = min(wakeStep / (float)WAKE_RAMP_TICKS, 1.0f);
float rampBri = wakeStartTarget.bri + t * (wakeEndTarget.bri - wakeStartTarget.bri);
int   rampCt  = (int)(wakeStartTarget.ct  + t * (wakeEndTarget.ct  - wakeStartTarget.ct));
```

Linear interpolation; `t` clamped at 1.0 so step 41+ doesn't overshoot. `rampBri` is `constrain`ed to `[1.0, 100.0]` and `rampCt` to `[CT_COOL, CT_WARM]` defensively.

The floor lamp PUTs every tick — it **leads** the ramp from tick 0 — with `dynamics.duration = 30000`. Chest + dresser join once `lux >= WAKE_SECONDARY_LUX (200) && rampBri >= WAKE_SECONDARY_BRI (40%)`. Overheads PUT only when `lux >= WAKE_OVERHEAD_LUX (600) && rampBri >= S2_BRI_LO (63%)` — a **higher** lux gate than NORMAL's `S3_LUX_HI` (200) — and `overheadsOn = true` flips when they do. Quarter-progress milestones get a `sendLog`. At `t >= 1.0`, state hands off to `NORMAL` with a "Wake complete" log. `sentTarget` is updated to the final ramp value so the next NORMAL tick's drift check uses the right baseline.

### 10.3 `tickWindDown()`

```cpp
windDownStep++;
if (overheadsOn) {
    setLight(LIGHT_UUID_CEIL_1, false, 0, 0, 1000);
    setLight(LIGHT_UUID_CEIL_2, false, 0, 0, 1000);
    overheadsOn = false;
}
float wdBri = windDownStartBri - (windDownStartBri - S4_FLOOR_BRI) * (windDownStep / 120.0f);
```

Overheads cut on the first tick unconditionally (defensive — if any of them was still on when wind-down triggered, kill them now). Linear bri ramp from `windDownStartBri` down to `S4_FLOOR_BRI` over 120 steps (60 min). CT held at `CT_WARM`. The floor lamp PUTs every tick — it's the night-light anchor that survives to `LOCKED_OUT`. Chest dims for the first half and switches off at the midpoint (step 60; the off is **cache-guarded** — `else if (cachedLight(LIGHT_CHEST).on)` — so a dashboard step-seek that jumps past 60 still kills it). Dresser dims the full hour. At step 120: dresser turns off, state → `LOCKED_OUT`, NVS save with the floor values, floor lamp left glowing at `S4_FLOOR_BRI`. Milestone logs at steps 30/60/90.

### 10.4 `tickNormal(lux, target)`

The most complex tick function. Order matters:

1. **`shouldUpdate` is computed inside `tickNormal`, not in `loop()`** — important because a forced sentinel reset earlier in the tick (via `pollDashboardCommand` → `forceState(NORMAL)`) would otherwise not be reflected in the delta check.
2. **Overhead edge detection.** `newOverheadsOn = lux > S3_LUX_HI`. On a falling edge (overheads were on, now should be off), explicit overhead-off PUTs with `dynamics.duration = 500`. On either edge, `shouldUpdate = true` is forced (the floor/chest/dresser targets jump across the curve's discontinuity at 200 lux).
3. **Pause-resume ramp.** If `pauseResumeActive`, run the linear interpolation from `pauseResumeStartTarget` to `target` over `PAUSE_RESUME_TICKS = 20` (10 min). PUTs all expected-on lights at the ramp value with `dynamics.duration = 30000`. Saves to NVS. Sets `pauseResumeActive = false` at completion. **Returns early** — none of the stable-lux / wind-down / lux-driven logic runs during the resume ramp.
4. **Stable-lux counter.** Increment if `lux <= 10 && hour >= 21`, otherwise decrement with floor 0. The decrement (not reset) is intentional weighted decay — brief lux spikes don't reset the 30-minute accumulation. At `stableLuxCount >= 60`, transition to `WIND_DOWN` (seed `windDownStartBri` from `sentTarget.bri`), log, return.
5. **Lux-driven PUT.** If `shouldUpdate`, PUT floor + chest + dresser (+ overheads if `overheadsOn`) at the curve target with `dynamics.duration = 1000`. Update `sentTarget`, `saveLastState`, log. If not, "No change — skipping PUT."

Override detection is not in this function — it's pushed entirely into the SSE handler.

**Mid-batch preemption.** The multi-bulb PUT chain in step 5 (floor + chest + dresser, plus overheads) is the primary place where a user override fired via SSE between PUTs would historically have driven the remaining un-PUT bulbs to the now-stale curve target. The mid-batch abort guard at the top of `setLight()` / `setLightColor()` (§6.4) short-circuits this: once `state` flips to `SOFT_PAUSE`, every remaining PUT in the chain no-ops. The post-batch `sentTarget = target` / `saveLastState` / "Bulbs updated." lines still run on the partial batch — cosmetically misleading but harmless, since the system is in `SOFT_PAUSE` for the next 60 min and won't read those values until the resume ramp re-evaluates everything against current lux. Same protection applies to step 2's overhead-off pair and step 3's pause-resume ramp PUTs.

### 10.5 `tickSoftPause()`

```cpp
if (millis() - softPauseStart >= SOFT_PAUSE_MS) {
    pauseResumeStartTarget = sentTarget;
    pauseResumeActive      = true;
    pauseResumeStep        = 0;
    overheadsOn            = cachedLight(LIGHT_CEIL_1).on;
    lastFloorOn            = cachedLight(LIGHT_FLOOR).on;
    state                  = State::NORMAL;
    ...
}
```

Single check: has 60 min elapsed? If yes, seed the resume ramp from `sentTarget` (the pre-pause baseline), arm `pauseResumeActive`, re-sync `overheadsOn` and `lastFloorOn` from the cache (the user may have changed lights during the pause), flip to `NORMAL`. The next `tickNormal` will see `pauseResumeActive` and run the ramp.

The cache re-sync at this re-entry point is the same defensive pattern used at every NORMAL/LOCKED_OUT re-entry — see §1.3.

### 10.6 `triggerWake(ambientLux)`

```cpp
LightState floorLamp = cachedLight(LIGHT_FLOOR);
float    startBri  = (floorLamp.on && floorLamp.bri > 0.0f) ? floorLamp.bri : S4_FLOOR_BRI;
uint16_t startCt   = (floorLamp.ct  > 0) ? (uint16_t)floorLamp.ct  : (uint16_t)CT_WARM;
```

Reads from the cache (no GET). The wake therefore starts from wherever the **floor lamp** actually is right now — it's the bulb the user physically flips on, and the rising edge that fired the wake. The `> 0` guards handle the "cache uninitialized" edge case by falling back to the night floor — though after `bootstrapLightStates()` this branch should never hit.

`wakeEndTarget = luxToTarget(ambientLux)`, `wakeStep = 0`, `state = WAKE`, logs.

### 10.7 `checkFloorState(lux)`

Edge detector for the **floor lamp** (it inherited this role from the old bedside). Two cases:

- **Rising edge in `LOCKED_OUT`** (`!lastFloorOn && floorLamp.on`) → `triggerWake(lux)`. This is the morning hand-off — the floor lamp being flipped on is what wakes the system up.
- **Falling edge after `LOCKOUT_RESET_HOUR`** → check the cache for chest + dresser + ceiling 1. If all are off, re-arm: `state = LOCKED_OUT`, reset `stableLuxCount` and `windDownStep`. The all-off confirmation prevents a partial shutdown from re-arming too early.

Called every tick in `LOCKED_OUT`, `NORMAL`, and `WIND_DOWN`. Not called in `WAKE`/`SOFT_PAUSE`/`HARD_OFF` — those states have their own re-entry semantics.

---

## 11. Buttons (lines 951–1060)

### 11.1 `pollButton(btn, pin)`

```cpp
bool raw = (digitalRead(pin) == LOW);
unsigned long now = millis();
if (raw == btn.debounced) {
    btn.lastDebounceMs = now;  // keep timer fresh while stable
} else if (now - btn.lastDebounceMs >= DEBOUNCE_MS) {
    btn.debounced = raw;
    if (raw) { btn.held = true; btn.pressStartMs = now; }
    else     { btn.held = false; if (!btn.longFired) btn.shortPress = true; btn.longFired = false; }
}
if (btn.held && !btn.longFired && now - btn.pressStartMs >= LONG_PRESS_MS) {
    btn.longPress = true;
    btn.longFired = true;
}
```

The debouncer keeps `lastDebounceMs` fresh *while the signal is stable*, so the elapsed clock only accumulates when the signal is *different* from the last committed state — i.e. while it's bouncing. Once `(now - lastDebounceMs) >= DEBOUNCE_MS`, the new state is committed.

Short press fires on **release** so the user can change their mind by holding longer. `longFired` is sticky for one press cycle so the release after a long-press doesn't also emit a short press.

### 11.2 `forceState(next)`

The canonical "set this state with all the right entry preconditions" function. Per case:

- `LOCKED_OUT`: reset counters; resync `lastFloorOn` from cache; set state.
- `NORMAL`: sentinel-reset `sentTarget = {-1.0f, 0}`; reset `stableLuxCount`; resync `overheadsOn` and `lastFloorOn` from cache; set state.
- `WAKE`: call `triggerWake(lastLux)` (which sets state internally).
- `WIND_DOWN`: seed `windDownStartBri` from `sentTarget.bri` if a PUT has gone out (`sentTarget.ct > 0` is the not-sentinel proof), else from `luxToTarget(lastLux).bri`; reset `windDownStep`; set state.
- `SOFT_PAUSE`: `softPauseStart = millis()`; set state.
- `HARD_OFF`: set state.

The pattern of resyncing flags from the cache on NORMAL/LOCKED_OUT entry is the firmware's main defense against user-induced state divergence during periods where the firmware isn't watching.

### 11.3 `handleCycleButton()`

Short press → `next = (State)(((int)state + 1) % 6)` → `forceState(next)` → log (except WAKE, which `triggerWake` already logs).

### 11.4 `handleButtonEvents()` — the mode button

- **Long press** (`HARD_OFF` toggle). From not-`HARD_OFF` → `HARD_OFF`. From `HARD_OFF` → `LOCKED_OUT` + resync `lastFloorOn` (the user may have flipped the floor lamp while we were dead, and the next morning's wake trigger depends on the edge detector being correctly seeded).
- **Short press from `HARD_OFF`** → ignored. Hard off is long-press-only.
- **Short press from `NORMAL`** → `SOFT_PAUSE`.
- **Short press from anything else** → `NORMAL` with sentinel reset of `sentTarget`, counter reset, and cache resync of both flags.

The short-press-to-`NORMAL` path duplicates `forceState(NORMAL)` inline rather than calling it. Minor cosmetic inconsistency; behaviorally identical.

---

## 12. `setup()` (lines 1062–1103)

Linear boot sequence — every step depends on the previous one. See §1 above for the rationale per step; the file order is:

1. Serial + LED pins + button pins.
2. `connectWiFi()` (blocking).
3. `timeClient.begin()` + `update()` → solid blue LED.
4. `veml.begin()` → fatal red-blink on failure.
5. `ensureBridgeCert()` — relies on NTP-synced time for expiry comparison.
6. `bootstrapLightStates()` — seeds the cache.
7. Boot flourish: `cachedLight(LIGHT_CHEST)` snapshot, purple fade-in, hold 3 s, restore. (Still on the **chest** lamp — the old bedside bulb — not the floor lamp.)
8. `lastFloorOn` and `overheadsOn` seeded from cache.
9. `sseConnect()` — opens the persistent stream.
10. `sendLog("Online — ...")`.

The order is rigid: NTP must precede cert; cert must precede `bootstrapLightStates` and `sseConnect` (both need TLS); boot flourish must follow `bootstrapLightStates` (it reads from the cache); SSE drain must wait until `loop()` is running (the wait loop is what calls `sseTick`).

---

## 13. `loop()` (lines 1105–1161)

```cpp
unsigned long tickStart = millis();   // anchor at very top

timeClient.update();
printStatus();
float lux = veml.readLux(VEML_LUX_AUTO);
lastLux = lux;
LightTarget target = luxToTarget(lux);

pollDashboardCommand();               // remote control runs BEFORE state dispatch

switch (state) {
    case State::LOCKED_OUT: checkFloorState(lux); break;
    case State::NORMAL:     checkFloorState(lux); tickNormal(lux, target); break;
    case State::WAKE:       tickWakeRamp(lux); break;
    case State::WIND_DOWN:  checkFloorState(lux); tickWindDown(); break;
    case State::SOFT_PAUSE: tickSoftPause(); break;
    case State::HARD_OFF:   break;
}

sendDashboardStatus(lux);             // heartbeat to Railway

while (millis() - tickStart < 30000UL) {
    pollButton(btnMode,  BTN_MODE);
    pollButton(btnCycle, BTN_CYCLE);
    handleButtonEvents();
    handleCycleButton();
    sseTick();                        // drain SSE bytes; reconnect if needed
    delay(50);
}
```

Three design choices to call out:

- **`tickStart` is anchored at the very top, not after the work.** This means the wait loop subtracts processing time (HTTP, JSON parse, SSE handling, dashboard POSTs) from the 30 s budget, producing consistent 30 s tick intervals. In v1 the loop did `delay(30000)` *after* the work, which compounded jitter into ~45 s intervals on busy ticks.
- **`pollDashboardCommand` runs before the state dispatch.** A dashboard command received this tick takes effect this tick — no one-tick lag.
- **The wait loop drains SSE 600 times per tick** (30 000 ms ÷ 50 ms). `sseTick()` is non-blocking; events are processed within ~50 ms of arrival *while the main loop is not in an HTTP call*. During HTTP work the drain pauses — this is the latency window discussed in §7.6.

`HARD_OFF` is the only case with no per-tick work at all — the only thing that runs is the wait loop's button polling and SSE drain.

---

## 14. `millis()` Rollover Safety

`millis()` wraps to 0 after ~49.7 days. Every comparison in the file uses the subtraction form:

```cpp
if (millis() - startTime >= interval)   // SAFE — unsigned wrap absorbs rollover
```

…and never the unsafe form `if (millis() > startTime + interval)`. Audit complete; the only signed-math site is `sendDashboardStatus`'s pause-remaining calculation (§8.2), which explicitly casts to `long` to allow the clamp-to-zero.

---

## 15. Related Docs

- `SSE.md` — focused SSE reference; overlaps with §7 here
- `SSE v1.0 Awkward Structure.md` — design critique of the Phase 3 SSE rollout, status of items (a)–(e)
- `FIRMWARE.md` — state-machine + polling-loop reference
- `DASHBOARD.md` — Flask dashboard contract
- `HANDOFF_v2_migration.md` — historical migration plan
- `LIGHTCURVE.md` — curve-tuning reference
- `System Explanation OLD.md` — obsolete; documents the v1 polling architecture that no longer exists