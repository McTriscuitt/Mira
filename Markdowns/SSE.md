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
CachedLight lightCache[LIGHT_COUNT]; // LIGHT_CHEST / LIGHT_DRESSER / LIGHT_CEIL_1 / LIGHT_CEIL_2 / LIGHT_FLOOR
```

**Read path:** `cachedLight(idx)` returns a `LightState` copy. Used by `triggerWake()`, `checkFloorState()`, all state-transition resync points (`overheadsOn = cachedLight(LIGHT_CEIL_1).on`, `lastFloorOn = cachedLight(LIGHT_FLOOR).on`), the setup flourish (`saved = cachedLight(LIGHT_CHEST)`), and `noteRecentPut()` for the trajectory `priorBri`/`priorCt`.

**Write path:** `handleLightUpdate()` writes `on`, `bri`, and/or `ct` from incoming SSE events. Each event may carry any subset of the three fields — the bridge splits state changes across multiple events (e.g. a `dimming` event followed by a separate `color_temperature` event), so writes are field-by-field, not whole-record replacements.

**Contract:** The cache is "best-known last-reported state." It is no longer used for override detection — that's `recentPuts[]`'s job. This split (item (d) in the awkward-structure critique) makes the cache read-mostly, which in turn makes a future move to an async SSE task (item (a)) low-risk.

---

## Echo Discrimination — the `recentPuts[][]` Ring + No-Op Filter

The hard problem: when Mira PUTs a state change, the bridge echoes it back over SSE as an event. Naïvely, every Mira PUT would trigger SOFT_PAUSE. The old approach used a flat time-window mute (`muteOverride()` / `MUTE_AFTER_TRANSITION_MS = 5000UL`) — every PUT silenced override detection for ~5 s. That worked, but missed real overrides during the mute window and didn't degrade per-light.

Current approach: every incoming event is classified into one of nine `EchoOutcome` buckets. The override path is reached only when the event is off-trajectory of every live `recentPuts` slot, off-cache by more than `TRAJECTORY_TOLERANCE_*`, *and* not a stale revert of one of our own recent PUTs. Anything else is an echo (or a skip) and does not trigger SOFT_PAUSE.

Three layers do the work:

1. **Trajectory ring (`recentPuts[LIGHT_COUNT][3]`)** — every outgoing PUT writes one slot per light. An incoming event matches if its on/bri/ct values lie within `[min(prior, target), max(prior, target)] ± TRAJECTORY_TOLERANCE_*` for any live slot. Three slots per light let overlapping in-flight PUTs (e.g. wake-ramp tick + state-change PUT) keep their trajectories live simultaneously.
2. **No-op event filter** — if the trajectory check fails, before declaring `Override` the handler checks whether the event's values are within `TRAJECTORY_TOLERANCE_*` of the **pre-event cache**. If so, the bridge is reporting state we already think is true — a late settling echo whose slot expired before the event arrived. Classify as `NoOpEcho`. Closes the timing gap on "skipping PUT" ticks where nothing refreshed the slot.
3. **Stale-revert filter** — if the no-op check *also* fails, the handler compares the event against the **prior** (pre-PUT) values of every slot posted within `STALE_REVERT_LOOKBACK_MS` (90 s), including expired slots. A match means the lamp never applied (or rolled back) one of our recent PUTs and the bridge is correcting its optimistic echo — classify as `StaleRevert` and **re-assert** the slot's target instead of pausing. See "Stale-Revert Filter + Re-Assert" below.

### Struct + Ring

```cpp
struct RecentPut {
    bool          active     = false;
    bool          onTarget   = false;
    bool          priorOn    = false;  // cache on-state at PUT time (stale-revert fingerprint)
    float         priorBri   = 0.0f;   // cache value at PUT time
    float         targetBri  = 0.0f;   // value we asked for
    int           priorCt    = 0;
    int           targetCt   = 0;
    unsigned long postedAtMs = 0;
    unsigned long durationMs = 0;
};
const int RECENT_PUT_RING_SIZE = 3;
RecentPut recentPuts[LIGHT_COUNT][RECENT_PUT_RING_SIZE];
```

### Write — `noteRecentPut(idx, on, bri, ct, durationMs)`

Called from `setLight()` and `setLightColor()` **before** the HTTP PUT goes out. The bridge can emit an SSE echo faster than `HTTPClient::PUT()` returns, so the entry must already exist when the event arrives. Counter-intuitive but correct.

Slot selection: prefer the first inactive/expired slot so live trajectories from earlier PUTs aren't stomped. If every slot is currently live, overwrite the oldest — that's the entry whose remaining grace contributes least.

```cpp
// (slot selection: first inactive/expired wins; else oldest)
RecentPut& r = recentPuts[idx][slot];
r.active     = true;
r.onTarget   = on;
r.priorOn    = lightCache[idx].on;    // snapshot pre-PUT cache value
r.priorBri   = lightCache[idx].bri;   // snapshot pre-PUT cache value
r.targetBri  = bri;
r.priorCt    = lightCache[idx].ct;
r.targetCt   = ct;
r.postedAtMs = millis();
r.durationMs = durationMs;
```

### Read — `eventMatchesRecentPut(idx, hasOn, evOn, hasBri, evBri, hasCt, evCt)`

Iterates every live slot for `idx`; first match wins. For each present field the event carries, check if the value lies within the expected trajectory `[min(prior, target), max(prior, target)] ± TRAJECTORY_TOLERANCE_*`. Auto-expires entries past `postedAtMs + durationMs + RECENT_PUT_GRACE_MS` inline.

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
    return true; // any live slot's trajectory matched
}
return false;
```

### No-Op Event Filter

If `eventMatchesRecentPut()` returns false, the handler runs a second check before declaring an override. The question: did this event represent a state change *at all*? If the event's values are within `TRAJECTORY_TOLERANCE_*` of the **pre-event cache** (snapshotted just before the cache write), the bridge is reporting state effectively identical to what we already think is true — a late settling echo, not a user override.

```cpp
bool isNoOp = true;
if (hasOn  && evOn != cacheOnBefore)                                    isNoOp = false;
if (hasBri && fabsf(evBri - cacheBriBefore) > TRAJECTORY_TOLERANCE_BRI) isNoOp = false;
if (hasCt  && abs(evCt   - cacheCtBefore)    > TRAJECTORY_TOLERANCE_CT) isNoOp = false;
if (isNoOp)                                                       decision = EchoOutcome::NoOpEcho;
else if (eventMatchesPriorPut(idx, ..., revertSlot))              decision = EchoOutcome::StaleRevert;
else                                                              decision = EchoOutcome::Override;
```

This closes a timing gap that the trajectory check alone left open: when `tickNormal` decides "no change — skipping PUT", nothing refreshes the `recentPuts` slot, and the bridge's late settling confirmation event (often ~30+ s after the previous PUT) could land after the slot's `durationMs + grace` had expired. The trajectory check would miss it, and the event — typically a 1–2 mirek confirmation — would fire SOFT_PAUSE.

The semantic threshold for "real override" is unchanged: `TRAJECTORY_TOLERANCE_BRI` (5%) and `TRAJECTORY_TOLERANCE_CT` (15 mirek). Anywhere a value lands within either a recent PUT's trajectory **or** the pre-event cache counts as an echo. A real user override is double-digit-percent bri or 50+ mirek, comfortably outside both windows.

### Stale-Revert Filter + Re-Assert

The bridge's v2 event stream is **optimistic**: it emits the commanded values the instant it accepts a PUT, before the lamp confirms over Zigbee. If the lamp never applies the command (dropped Zigbee delivery — observed June 11 on the Signe floor lamp), the bridge discovers the truth at its next lamp poll and emits a *correction* event walking its resource state back — observed ~35 s after the PUT, far past any slot's `durationMs + grace` window.

That correction defeats both earlier layers: the trajectory slot has expired, and the cache holds the optimistic echo's values (the *commanded* state), so the correction differs from cache by the full size of the failed PUT. Pre-fix, this fired a phantom `Override` and froze the system in SOFT_PAUSE for an hour — triggered by nothing but a flaky bulb.

The fingerprint that separates a revert from a real override: **a user lands on arbitrary values; a revert lands exactly where our own PUT started.** `eventMatchesPriorPut()` compares the event's present fields against each slot's `priorOn`/`priorBri`/`priorCt` (within `TRAJECTORY_TOLERANCE_*`), scanning all slots posted within `STALE_REVERT_LOOKBACK_MS` (90 s) — *including expired slots*, whose data survives the `active = false` sweep.

On `StaleRevert`, the SSE task enqueues the event for the dispatcher **without setting `overridePending`** (a revert must not abort an in-progress PUT batch). The main-task dispatcher calls `reassertRecentPut(idx, slot)`, which re-PUTs the matched slot's target with a 500 ms snap. The re-PUT writes a fresh trajectory slot, so the retry's own echo classifies as `EchoMatch` — and because the cache was already updated with the reverted values before dispatch, the new slot's `prior*` reflects the lamp's true state.

Guards:

- **Per-light cooldown** (`lastReassertMs[]`, one re-assert per `STALE_REVERT_LOOKBACK_MS`): a persistently deaf lamp doesn't ping-pong with the bridge every poll cycle. After a failed retry, the normal 30 s curve tick re-drives the lamp once drift exceeds `STATE_TOLERANCE_*`.
- `setLight()`'s top guard drops the re-assert harmlessly if state moved to SOFT_PAUSE/HARD_OFF between classification and dispatch.
- Ordering protects against misuse: the no-op filter runs *first*, so an event merely re-stating the cache never reaches the revert check — `StaleRevert` only sees events that contradict what we believe **and** land on a recent PUT's starting point.

Accepted trade-off: a user who manually returns a lamp to its pre-PUT value within the 90 s lookback is misread as a revert and re-asserted once. Their next (different) adjustment fires `Override` normally.

### Why Trajectory, Not Endpoint

A Hue PUT with `dynamics.duration > 0` ramps the bulb over time, and the bridge emits intermediate echoes during the ramp. A `1000 ms` PUT from `bri=80` → `bri=45` produces echoes reporting `bri=78, 70, 60, 50, 45` (approximate). Matching against just the target endpoint `45` would mark all the mid-ramp echoes as overrides. Matching against the full `[45, 80]` interval (plus tolerance) absorbs them naturally — and any value *outside* that interval (e.g. the user dragging the bulb to `bri=20` during our ramp) still trips override correctly. The trajectory framing turns a hard timing problem into a simple set-membership check.

### Why a Ring, Not a Single Slot

Two scenarios where overlapping PUTs collide on a single-slot design:

- A wake-ramp tick PUT with `dynamics.duration=30000` is still settling when the next tick fires its own PUT. With one slot, the second `noteRecentPut()` overwrites the first; a late echo of the first PUT then has no live trajectory to match.
- A lux-driven PUT fires immediately after an overhead-toggle PUT (e.g. `S3_LUX_HI` crossing turns on `LIGHT_CEIL_1` + `LIGHT_CEIL_2` and the same tick adjusts `LIGHT_FLOOR`). On the floor slot this is fine; on the overheads, multiple settling echoes can collide.

Three slots cover the common cases without growing the trajectory window so wide it masks small real overrides.

### Two-Tier Tolerance

| Constant | Default | Purpose |
|---|---|---|
| `STATE_TOLERANCE_BRI` | `1.2f` (percent) | `tickNormal` drift check — "is the lux curve drifted enough to send a new PUT?" |
| `STATE_TOLERANCE_CT` | `3` (mirek) | Same for ct |
| `TRAJECTORY_TOLERANCE_BRI` | `5.0f` (percent) | `eventMatchesRecentPut` + no-op filter — "is this echo within the slop we expect from Zigbee/bulb-side step quantization?" |
| `TRAJECTORY_TOLERANCE_CT` | `15` (mirek) | Same for ct |
| `RECENT_PUT_GRACE_MS` | `5000UL` | Slack past `durationMs` for late echoes from slow mesh settles |

The original implementation reused `STATE_TOLERANCE_*` for both jobs. Wrong. The drift check wants tight thresholds so small lux changes still trigger PUTs; the echo discriminator needs wider thresholds because bulbs snap to discrete bri/ct steps (~1–3% bri, 5–10 mirek slop) and the cached `priorBri`/`priorCt` may itself be one quantization step off the bulb's true pre-PUT state. The fix: split the constants. A real user-driven override is double-digit-percent bri or 50+ mirek, comfortably outside the trajectory window. The no-op filter uses the same `TRAJECTORY_TOLERANCE_*` so the user-override threshold is identical regardless of which path catches the echo.

---

## Override Detection Flow

`handleLightUpdate(JsonObjectConst upd)` runs once per `light`-type event in the parsed SSE payload. Classification produces an `EchoOutcome` (see Diagnostics below); only `Override` triggers SOFT_PAUSE. Sequence:

1. Resolve the event's `id` to a `lightCache[]` index via `idxByUuid()`. Unknown UUID → `UnknownUuid`, return.
2. Extract `on.on`, `dimming.brightness`, and `color_temperature.mirek`. If all three are absent → `NoFields`, return.
3. **Snapshot pre-event cache values** (`cacheOnBefore`, `cacheBriBefore`, `cacheCtBefore`). These are preserved through the rest of the function for the diagnostic record and the no-op filter.
4. Classify into `EchoOutcome`:
   - `state != NORMAL` → `SkipState`
   - `pauseResumeActive` → `SkipPauseResume` (resume ramp is itself self-PUTs; suppress)
   - `expectedOn` is false for this idx (`LIGHT_CEIL_*` when `overheadsOn == false`) → `SkipOffLight`
   - `eventMatchesRecentPut()` returns true → `EchoMatch`
   - No-op filter passes (event values within `TRAJECTORY_TOLERANCE_*` of `cacheBefore`) → `NoOpEcho`
   - `eventMatchesPriorPut()` returns true (event lands on a recent slot's pre-PUT values) → `StaleRevert` (dispatcher re-asserts the slot's target)
   - None of the above → `Override`
5. **Write to cache** for whichever fields are present. (After classification so `cacheBefore` stays meaningful.)
6. `recordSseEvent()` appends the classified event to the diagnostic ring.
7. If `decision != Override` → return.
8. Set `state = SOFT_PAUSE`, `softPauseStart = millis()`, `pauseResumeActive = false`. Call `dumpOverrideDiagnostic()` (ships a full multi-line dump to the dashboard via `sendLog`). Then `sendLog("Manual override — soft pause — ...")`.

The override decision is still based on fields present in *this* event only — never on cache-merged state — because the bridge splits combined state changes across multiple events (a `dimming` event followed by a separate `color_temperature` event) and mixing cached fields into the trajectory comparison would generate false positives on every partial event.

### Mid-Batch Abort

The state flip in step 8 happens synchronously inside `handleLightUpdate()`, which is called from `sseTick()`, which is called from the **tail** of every `setLight()` and `setLightColor()` (after the PUT returns, before the function returns). That tail-drain is what gives the override its low latency — but it also means the flip can land **between** PUTs in a chained tick.

The vulnerable callers are the multi-PUT batches:

- `tickNormal`'s `if (shouldUpdate)` block — up to 4 PUTs (floor, chest, dresser, ceil_1)
- `tickNormal`'s overhead-off PUT when crossing `S3_LUX_HI` descending
- `tickNormal`'s pause-resume ramp (floor + chest + dresser, plus overheads)
- `tickWakeRamp` — floor leads, chest+dresser and overheads gated per their thresholds
- `tickWindDown` — floor + chest + dresser (chest off at step 60, dresser off at step 120)

If the user changed e.g. the chest-lamp brightness via the Hue app between PUT #1 (floor) and PUT #2 (chest), the bridge would echo chest back with the user's value, `eventMatchesRecentPut()` would miss (the value is outside the trajectory), the no-op filter would miss (the value is outside `TRAJECTORY_TOLERANCE_BRI` of the pre-event cache), and the override would fire. State is now SOFT_PAUSE — but `tickNormal` (or whoever) has no idea, and proceeds to PUT #2..#4 to the curve-derived target the user just overrode.

The fix is a guard at the top of both PUT helpers, *before* `noteRecentPut()` and the HTTP request:

```cpp
void setLight(const char* uuid, bool on, float bri, int ct, int durationMs) {
    // Mid-batch abort. setLight() tail-calls sseTick() which can flip state
    // to SOFT_PAUSE on an Override. Without this, the rest of a chained
    // tickNormal/tickWakeRamp/tickWindDown batch keeps PUTting after the
    // user's manual change has already preempted the system.
    if (state == State::SOFT_PAUSE || state == State::HARD_OFF) return;
    ...
}
```

Bailing *before* `noteRecentPut()` is important — if we recorded a trajectory slot and then skipped the actual PUT, an unrelated future event for the same bulb could match against an orphan slot during its `durationMs + RECENT_PUT_GRACE_MS` lifetime, silently classifying a real override as `EchoMatch`.

The guard is also valid for `HARD_OFF` for the same reason (HARD_OFF means "the firmware is taking no automated action on bulbs at all"). The boot purple flourish is unaffected — `setup()` runs the flourish in `State::NORMAL` (the static initializer at module scope) and `sseConnect()` doesn't open until after the flourish, so no override can fire during it.

Trade-off: the partial batch leaves some bulbs at their pre-PUT state and others at the new curve target — the lights are temporarily inconsistent. That's the right behavior given the user's manual change explicitly disagreed with the system's curve target; SOFT_PAUSE is about to hold things steady for 60 min anyway. Cosmetic side-effect: `tickNormal`'s post-batch `Serial.println("Bulbs updated.")` and `sendLog("Lux: ... → bri=... ct=...")` still fire even though only some bulbs were PUT. The "Manual override — soft pause" log immediately follows, so the sequence is still readable; cleaning up the cosmetic gap is a low-priority follow-up.

---

## Diagnostics — Event Ring, Outcome Counters, Override-Fire Dump

Echo discrimination is only as good as our ability to inspect it when it misclassifies. Three diagnostic surfaces are always on:

### `EchoOutcome` — every event is classified

Every call into `handleLightUpdate()` ends in exactly one outcome. The enum is the single vocabulary for what happened on each event:

| Outcome | Meaning |
|---|---|
| `UnknownUuid` | Event for a UUID outside `lightCache[]`. Not actionable. |
| `NoFields` | Event payload had no `on` / `dimming` / `color_temperature`. Heartbeat-ish. |
| `SkipState` | `state != NORMAL`. Override detection is NORMAL-only. |
| `SkipPauseResume` | `pauseResumeActive` — resume ramp is itself self-PUTs. |
| `SkipOffLight` | `expectedOn == false` for this idx (off overhead). |
| `EchoMatch` | Trajectory check against a live `recentPuts` slot matched. |
| `NoOpEcho` | Trajectory missed, but event values are within tolerance of pre-event cache. |
| `StaleRevert` | Event lands on a recent slot's *prior* values — lamp never applied our PUT; dispatcher re-asserts the target. |
| `Override` | Off-trajectory, off-cache, and not a stale revert — fires SOFT_PAUSE. |

### `sseEventRing[16]` — last 16 classified events

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
const int      SSE_EVENT_RING_SIZE = 16;
SseEventRecord sseEventRing[SSE_EVENT_RING_SIZE];
int            sseEventRingHead = 0;
```

`recordSseEvent()` appends one record per call into `handleLightUpdate()`. The pre-event cache snapshot is captured *before* the cache write so the record reflects the delta between what we thought was true and what the event reported. This is what makes post-hoc root-causing possible: when an override fires, you can see whether the cache was poisoned, whether earlier events were misclassified, whether the bridge was emitting many small confirmations, or whether a real drastic delta arrived.

### `echoOutcomeCounts[]` — cumulative since boot

`unsigned long echoOutcomeCounts[ECHO_OUTCOME_COUNT] = {0};` increments on every event. Included in the override-fire dump as a one-line histogram. High `NoOpEcho` counts confirm the late-settling-echo pattern was widespread before the filter was added; high `EchoMatch` counts show the ring is doing its job; any nonzero `Override` is genuinely worth investigating.

### `dumpOverrideDiagnostic()` — dashboard-bound full dump on Override

When `decision == Override`, the handler ships a multi-line message to the dashboard via `sendLog()` containing:

- The triggering event's `hasOn/evOn`, `hasBri/evBri`, `hasCt/evCt`
- `cacheBefore` (the pre-event cache snapshot)
- Every active `recentPuts[idx][s]` slot's full state and age vs `durationMs + grace`
- The last 6 ring entries with `idx == idx` of the override, each annotated with its outcome
- The full outcome histogram since boot

The dump fires only on the override path, so it's free in steady state. The dashboard log row is one event but the message contains embedded newlines — paste it back into a debugging conversation and the conditions are fully reconstructable without a USB cable.

### `ECHO_TRACE` — verbose per-event Serial output

```cpp
// #define ECHO_TRACE 1
```

Uncomment near the `EchoOutcome` enum to enable per-event Serial logging during tuning. Format: `SSE evt idx=N on=±X bri=±Y.YY ct=±Z cacheBefore[...] -> <outcome>`. Off by default because steady-state operation emits dozens of events per minute.

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
const unsigned long SSE_RECONNECT_DELAY_MS   = 5000UL;    // gap between reconnect attempts
const unsigned long SSE_STALE_TIMEOUT_MS     = 600000UL;  // 10 min — no keepalive, long silence is normal
const float         TRAJECTORY_TOLERANCE_BRI = 5.0f;      // echo discrimination width, percent — used by trajectory, no-op, and stale-revert filters
const int           TRAJECTORY_TOLERANCE_CT  = 15;        // echo discrimination width, mirek — used by trajectory, no-op, and stale-revert filters
const unsigned long STALE_REVERT_LOOKBACK_MS = 90000UL;   // stale-revert fingerprint window — covers the bridge's ~35 s lamp-poll correction with margin
const unsigned long RECENT_PUT_GRACE_MS      = 5000UL;    // late-echo slack past dynamics.duration
const int           RECENT_PUT_RING_SIZE     = 3;         // slots per light in the trajectory ring
const int           SSE_EVENT_RING_SIZE      = 16;        // classified-event diagnostic ring depth
const int           ECHO_OUTCOME_COUNT       = 8;         // size of echoOutcomeCounts[] histogram
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
