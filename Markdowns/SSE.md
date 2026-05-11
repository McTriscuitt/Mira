# SSE.md — Mira Server-Sent Events Subsystem

Companion to `FIRMWARE.md` (state machine, polling, buttons) and `DASHBOARD.md` (Flask dashboard). This file is the single reference for how Mira uses the Hue v2 Server-Sent Events stream — connection, cache, echo discrimination, override detection, bootstrap, and reconnect behavior.

---

## What SSE Replaces

Pre-v2, Mira polled the bridge every tick: up to four `getLightState()` GETs per 30 s tick to detect manual overrides, plus a final GET in `triggerWake()` to read the bedside's current bri/ct. With the bridge's TLS handshake cost and the per-tick fan-out, observed tick intervals stretched to ~45 s instead of the intended 30 s.

The v2 SSE stream replaces all of that:

- A single persistent HTTPS connection delivers bridge state changes as they happen
- `lightCache[]` is updated from incoming events; `cachedLight(idx)` is the new read path everywhere `getLightState()` used to be called
- Override detection runs inside the SSE handler instead of as a per-tick poll
- Wake trigger reads from the cache

Tick intervals are now consistent at 30 s.

---

## Connection

| Aspect | Detail |
|---|---|
| URL | `https://192.168.1.186/eventstream/clip/v2` (`HUE_V2_BASE_URL` + `/eventstream/clip/v2`) |
| Auth header | `hue-application-key: <HUE_API_KEY>` |
| TLS | `WiFiClientSecure` with the bridge cert loaded from NVS (`_bridgeCertPem`) — `setCACert()` not `setInsecure()` |
| Globals | `WiFiClientSecure sseClient`, `String sseBuf` (partial-line accumulator), `unsigned long sseLastByteMs`, `unsigned long sseLastConnectMs` |
| Reconnect throttle | `SSE_RECONNECT_DELAY_MS = 5000UL` — minimum gap between reconnect attempts |
| Stale-stream timeout | `SSE_STALE_TIMEOUT_MS = 600000UL` (10 min) — Hue v2 emits no keepalive, so we only treat the stream as dead after a long silence |

The connection is opened by `sseConnect()` at the end of `setup()`, after WiFi, NTP, VEML7700, cert refresh, and `bootstrapLightStates()`. It's drained by `sseTick()` inside the non-blocking wait loop between ticks.

---

## Bootstrap (one-time GET → cache seed)

SSE only delivers events from connect time forward — it does not push initial state. To start with a populated `lightCache[]`, `setup()` calls `bootstrapLightStates()` once before opening the stream:

1. `GET https://192.168.1.186/clip/v2/resource/light` with the application key
2. For each light in the response, look up its index via `idxByUuid()`
3. Write `on`, `dimming.brightness`, and `color_temperature.mirek` into `lightCache[idx]`
4. Mark `lightCache[idx].initialized = true`

After this, every subsequent state change is delivered via SSE and applied incrementally.

`bootstrapLightStates()` still uses `setInsecure()` today — see "Future Work" below for the planned unification with the SSE event handler (item (e) in `SSE v1.0 Awkward Structure.md`).

---

## Cache Architecture

```cpp
struct CachedLight {
    bool  on          = false;
    float bri         = 0.0f;   // percent 0.0–100.0
    int   ct          = 0;      // mirek
    bool  initialized = false;
};
CachedLight lightCache[4];      // indexed by LIGHT_BEDSIDE / LIGHT_DESK / LIGHT_CEIL_1 / LIGHT_CEIL_2
```

**Read path:** `cachedLight(idx)` returns a `LightState` copy. Used by `triggerWake()`, `checkBedsideState()`, all state-transition resync points (`overheadsOn = cachedLight(LIGHT_CEIL_1).on`, `lastBedsideOn = cachedLight(LIGHT_BEDSIDE).on`), the setup flourish (`saved = cachedLight(LIGHT_BEDSIDE)`), and `noteRecentPut()` for the trajectory `priorBri`/`priorCt`.

**Write path:** `handleLightUpdate()` writes `on`, `bri`, and/or `ct` from incoming SSE events. Each event may carry any subset of the three fields — the bridge splits state changes across multiple events (e.g. a `dimming` event followed by a separate `color_temperature` event), so writes are field-by-field, not whole-record replacements.

**Contract:** The cache is "best-known last-reported state." It is no longer used for override detection — that's `recentPuts[]`'s job. This split (item (d) in the awkward-structure critique) makes the cache read-mostly, which in turn makes a future move to an async SSE task (item (a)) low-risk.

---

## Echo Discrimination — the `recentPuts[]` Ring

The hard problem: when Mira PUTs a state change, the bridge echoes it back over SSE as an event. Naïvely, every Mira PUT would trigger SOFT_PAUSE. The old approach used a flat time-window mute (`muteOverride()` / `MUTE_AFTER_TRANSITION_MS = 5000UL`) — every PUT silenced override detection for ~5 s. That worked, but missed real overrides during the mute window and didn't degrade per-light.

Current approach: a per-light **trajectory ring** records every outgoing PUT. Incoming events are matched against the trajectory their target light is currently on; on-trajectory → echo, off-trajectory → external override.

### Struct

```cpp
struct RecentPut {
    bool          active     = false;
    bool          onTarget   = false;
    float         priorBri   = 0.0f;   // cache value at PUT time
    float         targetBri  = 0.0f;   // value we asked for
    int           priorCt    = 0;
    int           targetCt   = 0;
    unsigned long postedAtMs = 0;
    unsigned long durationMs = 0;
};
RecentPut recentPuts[4];
```

### Write — `noteRecentPut(idx, on, bri, ct, durationMs)`

Called from `setLight()` and `setLightColor()` **before** the HTTP PUT goes out. The bridge can emit an SSE echo faster than `HTTPClient::PUT()` returns, so the entry must already exist when the event arrives. Counter-intuitive but correct.

```cpp
r.active     = true;
r.onTarget   = on;
r.priorBri   = lightCache[idx].bri;   // snapshot pre-PUT cache value
r.targetBri  = bri;
r.priorCt    = lightCache[idx].ct;
r.targetCt   = ct;
r.postedAtMs = millis();
r.durationMs = durationMs;
```

### Read — `eventMatchesRecentPut(idx, hasOn, evOn, hasBri, evBri, hasCt, evCt)`

For each field the event carries, check if the value lies within the expected trajectory `[min(prior, target), max(prior, target)] ± TRAJECTORY_TOLERANCE_*`. Auto-expires entries past `postedAtMs + durationMs + RECENT_PUT_GRACE_MS`. Returns `true` if every present field is in range — then it's an echo, ignore. Returns `false` otherwise — that's an external change.

```cpp
if (!r.active) return false;
if (millis() - r.postedAtMs > r.durationMs + RECENT_PUT_GRACE_MS) { r.active = false; return false; }
if (hasOn  && evOn != r.onTarget) return false;
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
```

### Why Trajectory, Not Endpoint

A Hue PUT with `dynamics.duration > 0` ramps the bulb over time, and the bridge emits intermediate echoes during the ramp. A `1000 ms` PUT from `bri=80` → `bri=45` produces echoes reporting `bri=78, 70, 60, 50, 45` (approximate). Matching against just the target endpoint `45` would mark all the mid-ramp echoes as overrides. Matching against the full `[45, 80]` interval (plus tolerance) absorbs them naturally — and any value *outside* that interval (e.g. the user dragging the bulb to `bri=20` during our ramp) still trips override correctly. The trajectory framing turns a hard timing problem into a simple set-membership check.

### Two-Tier Tolerance

| Constant | Default | Purpose |
|---|---|---|
| `STATE_TOLERANCE_BRI` | `1.2f` (percent) | `tickNormal` drift check — "is the lux curve drifted enough to send a new PUT?" |
| `STATE_TOLERANCE_CT` | `3` (mirek) | Same for ct |
| `TRAJECTORY_TOLERANCE_BRI` | `5.0f` (percent) | `eventMatchesRecentPut` — "is this echo within the slop we expect from Zigbee/bulb-side step quantization?" |
| `TRAJECTORY_TOLERANCE_CT` | `15` (mirek) | Same for ct |
| `RECENT_PUT_GRACE_MS` | `5000UL` | Slack past `durationMs` for late echoes from slow mesh settles |

The original implementation reused `STATE_TOLERANCE_*` for both jobs. Wrong. The drift check wants tight thresholds so small lux changes still trigger PUTs; the echo discriminator needs wider thresholds because bulbs snap to discrete bri/ct steps (~1–3% bri, 5–10 mirek slop) and the cached `priorBri`/`priorCt` may itself be one quantization step off the bulb's true pre-PUT state. The fix: split the constants. A real user-driven override is double-digit-percent bri or 50+ mirek, comfortably outside the trajectory window.

---

## Override Detection Flow

`handleLightUpdate(JsonObjectConst upd)` runs once per `light`-type event in the parsed SSE payload. Sequence:

1. Resolve the event's `id` to a `lightCache[]` index via `idxByUuid()`. Unknown UUID → return.
2. Extract `on.on`, `dimming.brightness`, and `color_temperature.mirek`. If all three are absent → return.
3. **Write to cache** for whichever fields are present.
4. Guard: if `state != NORMAL` → return. (Override detection is a NORMAL-only behavior.)
5. Guard: if `pauseResumeActive` → return. (The resume ramp is itself a series of self-PUTs; we suppress detection for its duration.)
6. Compute `expectedOn` per light index:
   - `LIGHT_BEDSIDE` / `LIGHT_DESK` → always `true`
   - `LIGHT_CEIL_1` / `LIGHT_CEIL_2` → `overheadsOn` flag
   - Anything else → return.
7. If `!expectedOn` → return. (We don't trigger override for lights we don't expect to be on.)
8. Echo discrimination: `eventMatchesRecentPut(idx, hasOn, evOn, hasBri, evBri, hasCt, evCt)`. If `true` → echo, return.
9. Off-trajectory event reached. Dump a diagnostic `Serial.printf("Override fire: ...")` with the event vs `RecentPut` state side-by-side, then set `state = SOFT_PAUSE`, `softPauseStart = millis()`, `pauseResumeActive = false`. `sendLog("Manual override — soft pause — ...")`.

The diagnostic dump only fires on the override path, so it's free in steady state and invaluable when a misfire needs root-causing.

---

## SSE Wire Protocol

The Hue v2 SSE format is plain UTF-8 over the long-lived HTTPS connection. Lines we care about:

- `data: <json>` — the payload. JSON is an array of events; each event is `{ type, data: [...] }` where `data` is an array of resource updates.
- `id: <event-id>` — bridge-assigned event id; we don't currently use it.
- `: <comment>` — server comment (heartbeat-style). Ignored.

`handleSseLine()` filters for `data:`-prefixed lines and forwards the JSON body to `handleSseEventData()`. `handleSseEventData()` walks the outer event array, keeps only `type == "update"` entries, then walks the inner resource-update array and dispatches `type == "light"` updates to `handleLightUpdate()`. All other resource types are silently ignored today — `button`, `relative_rotary`, `motion`, `zigbee_connectivity`, etc. are not yet consumed (see `SSE_BRAINSTORM.md` for the catalogue and ideas).

---

## Reconnect Behavior

`sseTick()` does three things per call:

1. If the socket is disconnected and `millis() - sseLastConnectMs >= SSE_RECONNECT_DELAY_MS` → `sseConnect()`.
2. If the socket is connected, drain available bytes into `sseBuf` and split on `\n`. Each complete line goes to `handleSseLine()`. Partial trailing data stays in the buffer.
3. If the socket is connected but `millis() - sseLastByteMs >= SSE_STALE_TIMEOUT_MS` (10 min) → consider the stream dead, close and trigger reconnect on the next tick.

The 10-minute stale timeout is intentional — Hue v2 doesn't send keepalive frames, so long silence is normal for a quiet network. Tightening this would cause spurious reconnects during legitimate idle periods.

**Missed-events gap:** During a reconnect, any events the bridge emitted between disconnect and the new connect are lost. The cache may diverge from reality until the bulbs next emit any event. A planned mitigation (item (e) in `SSE v1.0 Awkward Structure.md`) is to re-run `bootstrapLightStates()` on every reconnect to resync the cache.

---

## Configuration Constants

All defined inline in `src/main.cpp` near the globals, not in `config.h`:

```cpp
const unsigned long SSE_RECONNECT_DELAY_MS  = 5000UL;     // gap between reconnect attempts
const unsigned long SSE_STALE_TIMEOUT_MS    = 600000UL;   // 10 min — no keepalive, long silence is normal
const float         TRAJECTORY_TOLERANCE_BRI = 5.0f;      // echo discrimination width, percent
const int           TRAJECTORY_TOLERANCE_CT  = 15;        // echo discrimination width, mirek
const unsigned long RECENT_PUT_GRACE_MS     = 5000UL;     // late-echo slack past dynamics.duration
```

`STATE_TOLERANCE_BRI` and `STATE_TOLERANCE_CT` live in `config.h` because they affect `tickNormal`'s drift check, not SSE.

---

## Open Issues / Future Work

See `SSE v1.0 Awkward Structure.md` for the full critique and implementation status. Open items relevant to SSE:

- **(a) Pin SSE to Core 1 via FreeRTOS task.** Today `sseTick()` only runs inside the wait loop, which means SSE drain pauses while the main loop is in any HTTP call (`sendDashboardStatus`, `pollDashboardCommand`, `sendLog`, `setLight`). The ESP32-S3 has a second core idle. Moving SSE handling there would give true real-time event processing regardless of what the main loop is doing. Now lower-risk because the cache is read-mostly post-(b)/(c)/(d).
- **(e) Unify bootstrap with SSE event handling.** Currently the one-time GET path and the persistent SSE path are separate code. A cleaner design: on every reconnect, re-fetch full state, treat all incoming SSE events as deltas. Eliminates the missed-events gap during reconnect.
- **Owner-aware policy.** The `owner` field on light events likely identifies the parent device, not the actor that caused the change. Verification needed — see `SSE_BRAINSTORM.md` section A for the action item.
- **Wider event vocabulary.** `button`, `relative_rotary`, `zigbee_connectivity`, `device_power`, `motion` events all flow through the same stream and are currently dropped. See `SSE_BRAINSTORM.md` for the full catalogue and the Tap Dial Switch deep-dive.

---

## Related Docs

- `FIRMWARE.md` — state machine, polling loop, button handling, lux→bri/ct mapping
- `DASHBOARD.md` — Flask dashboard, REST API contract
- `SSE_BRAINSTORM.md` — future SSE-driven features (real-time dashboard push, Tap Dial, adaptive curve learning, etc.)
- `SSE v1.0 Awkward Structure.md` — design critique of the Phase 3 SSE rollout and implementation status of fixes
- `HANDOFF_v2_migration.md` — the original v1→v2 migration plan, including the rationale for moving to SSE
