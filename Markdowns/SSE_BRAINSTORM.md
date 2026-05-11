# Mira — SSE Extensions Brainstorm

Phase 3 wired up the Hue v2 Server-Sent Events stream. Today we use it for one
thing: light state echoes that update the local cache and detect manual overrides.
The stream actually exposes a much wider event vocabulary, and the cache + handler
architecture we built is generic enough to react to most of it. This file
catalogues what's possible, captures user-direction notes, and ranks ideas by ROI.

---

## User Notes / Discuss Later

- **Zigbee connectivity** — revisit later. Open question for future discussion.
- **Owner-aware override has limited immediate value.** The user's observation:
  in the current setup, nothing else is linked to the system. No Hue automations
  configured, no Hue Sync, no voice assistant integration, no other apps controlling
  the bulbs. So if a state change shows up that wasn't initiated by Mira, it's
  effectively always the user. The owner field tells us *which surface* the user
  used (app vs. dimmer vs. voice), which is a refinement, not a new dimension.
  → Reprioritize once the ecosystem grows (e.g., once Hue dimmer paired, once
  Calendar/Siri webhooks land, once Hue Sync is in the loop).
- **Liked ideas:** D13, D14, D15 (real-time dashboard event push + activity feed +
  per-bulb status pills).
- **Want deeper exploration:** Tap Dial Switch, E16 (adaptive curve learning),
  G22 (instant lockout re-arm), G23 (drop `lastBedsideOn` accumulator). See the
  per-idea deep-dives below.
- These are all things i want to add soon. however, there is a bigger architecture question: how can we turn this into an sse/event driven/reactive system, so that the 30 second tick interval is solely for updating brightness?
- 
---

## What the Bridge Actually Emits

The `/eventstream/clip/v2` endpoint is multiplexed — every resource type the
bridge tracks emits events through it, not just lights. Below is the full
vocabulary, with rough relevance to Mira:

| Resource type            | What it reports                                 | Mira relevance |
|--------------------------|-------------------------------------------------|----------------|
| `light`                  | on/off, bri, ct, color, gradient, effects        | Currently used |
| `button`                 | Button press events on Hue accessories           | High (Tap Dial) |
| `relative_rotary`        | Tap dial rotation (direction + step count)       | High (Tap Dial) |
| `motion`                 | Motion sensor enter/exit                         | Medium |
| `light_level`            | Hue motion sensor's lux reading                  | Medium |
| `temperature`            | Hue motion sensor's temperature reading          | Low |
| `device_power`           | Battery percentage of battery-powered devices    | Medium |
| `zigbee_connectivity`    | Bulb online/offline transitions                  | High |
| `zigbee_device_discovery`| New device paired / unpaired                     | Low |
| `geofence_client`        | Phone enter/leave home (if configured)           | Medium |
| `behavior_instance`      | A Hue automation fired                           | Medium |
| `scene`                  | Scene activation                                 | Medium |
| `grouped_light` / `room` | Group operations                                  | Low |
| `entertainment_*`        | Hue Sync (gaming/movies) state                   | Medium |
| `bridge` / `bridge_home` | Bridge online status                             | Medium |

Every event also carries an **`owner`** field — a `{rid, rtype}` reference to
the resource that caused the change (a `device`, `behavior_script`,
`bridge_home`, etc.). That field is the basis of the "owner-aware" ideas below.

---

## Idea Buckets

### A. Owner-Aware Override Policy

The basic premise: stop treating all non-Mira state changes as the same thing.
The `owner` field tells us *who* did it.

1. **Coexist with Hue automations.** If `owner.rtype == "behavior_instance"`,
   the change came from a Hue-side automation (timer, geofence, etc.). Don't
   trigger SOFT_PAUSE — coexist.
2. **HARD_OFF on Hue Sync.** If owner is `entertainment_configuration`, the
   user started Hue Sync. SOFT_PAUSE would be wrong — Hue Sync hammers the
   bridge with rapid changes. HARD_OFF is the right response.
3. **App vs. accessory differentiation.** Different SOFT_PAUSE durations or
   policies based on whether the user used the app, a physical dimmer, or a
   voice assistant.
4. **Voice assistant hooks.** "Alexa, lights off" arrives with an Alexa-skill
   owner. We can react specifically (no auto-resume).

> User note (top of file): owner-aware policy is mostly redundant today because
> Mira is currently the only system controlling the lights. Revisit when the
> integration surface expands.

#### Verification needed before relying on `owner` for actor-identification

The ideas above (A1–A4) assume the `owner` field on a `light`-type SSE event
identifies the *actor* that triggered the change (Mira vs. Hue app vs.
behavior_instance vs. entertainment_configuration). That assumption is not yet
confirmed for this bridge's firmware.

On Hue v2, the `owner` field on a *light* resource is documented as a reference
to the parent resource the light belongs to (typically the bulb's `device`),
which is identification of *what changed*, not *who caused it*. Actor info, if
exposed at all, may live in other fields (e.g., `service_id`, application
references on `auth_v1` resources) and is not consistent across bridge firmware
versions.

**Action item — when investigating A:** add temporary `Serial.printf` of the
full event JSON inside `handleLightUpdate()`. Trigger three changes back-to-back
and compare event shapes:

1. A Mira-driven change (let lux trigger a PUT, or use the dashboard).
2. A Hue-app change from the phone (tap a bulb in the Hue app).
3. A physical change (toggle the bulb at the wall switch, or use a paired
   accessory if one exists).

If any field reliably differs across the three sources, owner-aware policy is
viable using that field. If they're indistinguishable, owner-aware policy as
described above is not implementable on light events, and we should either
(a) accept that scope for owner-aware logic is limited to `button` /
`relative_rotary` / `behavior_instance` events whose owner *does* identify the
originating accessory, or (b) lean entirely on the recent-PUT ring buffer
(`SSE v1.0 Awkward Structure.md` option (b)) for echo discrimination, which is
closed-loop and doesn't depend on bridge-supplied actor metadata.

### B. Hue Accessories as Physical UI

Replace or extend the two GPIO buttons with wireless Hue accessories.

5. **Tap Dial Switch (4 buttons + rotary).** See deep-dive below.
6. **Hue Dimmer Switch (4 buttons).** Cheaper alternative — 4 buttons, no dial.
7. **Long press → toggle override mute.** Forces NORMAL until next press.
8. **Double press → custom scene** (e.g., "movie mode").

### C. Bulb / Bridge Health

9. **Track `zigbee_connectivity` events** to flag offline bulbs in the cache.
   Skip PUTs to offline bulbs. Surface in dashboard and via firmware LED.
10. **Battery monitoring** for paired battery devices (sensors, dimmers).
    Dashboard pill: "Tap Dial: 14% — replace soon."
11. **Bridge offline detection.** SSE drop or `bridge` event reports offline →
    LED red, dashboard banner. Currently we silently fail PUTs.
12. **Anomaly logger.** Persist SSE events with timestamp + UUID + delta. Use
    for forensics (RF interference, broken bulbs, retry storms).

### D. Real-Time Dashboard *(user-prioritized)*

See deep-dive below.

13. **Push events from firmware → dashboard.** Forward each SSE event through a
    new `/api/event` endpoint instead of relying on the 30s heartbeat.
14. **Live activity feed.** Rolling timeline: "9:42:13 — bedside dimmed to 45%."
15. **Per-bulb status pills with last-event timestamp** in the dashboard header.

### E. Adaptive / Learning Behaviors

See deep-dive on E16 below.

16. **Override-driven curve calibration.** Log every override as
    `(lux, sentTarget, userChose)`. Surface curve-vs-preference deltas.
17. **Inactivity-based soft-pause resume.** Replace fixed 60-min timeout with
    "no user-initiated events for 10 min → resume."
18. **Presence inference.** Frequent user-initiated events → user is active,
    don't WIND_DOWN even if conditions match.

### F. External Coordination

19. **Geofence integration.** Hue's `geofence_client` events on enter/leave home
    drive HARD_OFF / WIND_DOWN. Replaces or complements Calendar/Siri.
20. **Hue motion sensor as Mira input.** Adds bedroom-mounted lux + motion
    signals. Cross-checks VEML7700.
21. **Multi-device coordination.** A second ESP32 in another room subscribed to
    the same SSE could mirror or follow bedroom state.

### G. Architectural Simplifications *(user-prioritized)*

See deep-dives on G22 and G23 below.

22. **Instant lockout re-arm** via SSE handler instead of 30 s tick polling.
23. **Drop `lastBedsideOn` edge accumulator.** SSE provides real edges.
24. **Drop `cachedLight()` synchronous reads from tick paths.** Trust the cache.

### H. Speculative / Longer-Horizon

25. **Scene-as-input.** Treat user-saved scene activation as an explicit mode
    signal rather than an override.
26. **Event-replay test harness.** Record real SSE streams; replay against
    firmware in a host build for state machine testing.
27. **Push notifications on suspicious events.** "Lights toggled at 3:14 AM."
28. **External webhook bridge.** Forward owner-tagged events to IFTTT / Zapier
    / Home Assistant.

---

## Deep Dive — Tap Dial Switch (B5)

**Hardware:** Philips Hue Tap Dial Switch (model RDM005). ~$50. Battery-powered
(CR2032), Zigbee. Has 4 momentary buttons in the corners and a rotary dial in
the middle. Wireless, no wiring required.

**Setup steps (one-time, manual):**

1. Pair to the Hue Bridge using the Hue mobile app — same flow as adding any
   accessory. Mira plays no role here.
2. Once paired, run discovery against the bridge to find UUIDs:
   - `GET https://192.168.1.186/clip/v2/resource/device` — finds the device
     resource (the dial as a whole).
   - `GET https://192.168.1.186/clip/v2/resource/button` — finds the four
     button resources, each with its own UUID.
   - `GET https://192.168.1.186/clip/v2/resource/relative_rotary` — finds the
     rotary resource UUID.
3. Hardcode UUIDs in `config.h` next to the existing bulb UUIDs. The dial's
   `metadata.control_id` field disambiguates buttons (1–4); record those too.

**SSE event shapes:**

Button event (delivered through the same stream we already drain):
```json
{
  "id": "<button-uuid>",
  "type": "button",
  "owner": { "rid": "<device-uuid>", "rtype": "device" },
  "button": {
    "last_event": "short_release",
    "button_report": {
      "updated": "2026-05-10T22:14:33.000Z",
      "event": "short_release"
    }
  }
}
```

`last_event` values: `initial_press`, `repeat`, `short_release`, `long_release`,
`long_press`, `double_short_release`.

Rotary event:
```json
{
  "id": "<rotary-uuid>",
  "type": "relative_rotary",
  "relative_rotary": {
    "last_event": {
      "action": "start",
      "rotation": {
        "direction": "clock_wise",
        "duration": 200,
        "steps": 12
      }
    }
  }
}
```

**Firmware changes:**

1. **`config.h`** — add UUIDs and a button-to-action mapping:
   ```cpp
   #define HUE_TAPDIAL_BTN_1   "..."
   #define HUE_TAPDIAL_BTN_2   "..."
   #define HUE_TAPDIAL_BTN_3   "..."
   #define HUE_TAPDIAL_BTN_4   "..."
   #define HUE_TAPDIAL_ROTARY  "..."
   ```
2. **`handleSseEventData()`** — extend the dispatch:
   ```cpp
   if (strcmp(updType, "button") == 0)         handleHueButton(upd);
   else if (strcmp(updType, "relative_rotary") == 0) handleHueRotary(upd);
   else if (strcmp(updType, "light") == 0)     handleLightUpdate(upd);
   ```
3. **`handleHueButton(upd)`** — map UUID + `last_event` to a state action.
   Suggested mapping:
   - Button 1 short → NORMAL
   - Button 1 long  → HARD_OFF toggle
   - Button 2 short → SOFT_PAUSE
   - Button 3 short → cycle state (matches BTN_CYCLE)
   - Button 4 short → trigger WAKE
4. **`handleHueRotary(upd)`** — accumulate a `briOffset` (percent) applied to
   `target.bri` before each PUT. Clockwise = brighter, counter-clockwise =
   dimmer. Decay or reset on next state transition. Lets the user nudge the
   curve without triggering override.

**Battery awareness:** The Tap Dial reports battery via `device_power` events.
Listen for those, surface percentage in the dashboard, alert below 20%.

**Tradeoffs:**

- Adds an annual battery-replacement chore (small).
- One-time setup is in the Hue app, not in Mira's flow — slightly inconsistent UX.
- Zigbee adds ~50–100 ms latency vs. a wired GPIO press. Not noticeable for
  state-change actions; might be visible for live brightness slewing.
- ~$50 hardware cost.
- Could fully replace the two GPIO buttons → frees `D9` and `D10` for future
  sensors (PIR, sound, LDR, anything).

**Migration path:** Keep both wired buttons and Tap Dial active simultaneously
during the transition. Wire Tap Dial first, validate the SSE handlers work,
then optionally remove the GPIO buttons.

---

## Deep Dive — Real-Time Dashboard (D13 / D14 / D15)

The current dashboard is heartbeat-driven. `sendDashboardStatus()` posts once
per 30 s tick with the latest snapshot. Between ticks, the dashboard is stale.
With SSE in firmware we now know about every state change within ~100 ms — this
trio of ideas brings that liveness to the dashboard.

### D13 — Push events firmware → dashboard

**Schema:** new endpoint `POST /api/event` accepting:
```json
{
  "ts": "2026-05-10T22:14:33.123Z",
  "uuid": "6cccebb8-...",
  "idx": 0,
  "on": true,
  "bri": 47.3,
  "ct": 366,
  "owner_rtype": "device",
  "owner_rid": "...",
  "state": "NORMAL",
  "sent_target_bri": 65.0,
  "sent_target_ct": 320
}
```
Authenticated by the existing `ESP32_API_KEY` bearer token.

**Firmware side:** wrap `handleLightUpdate()` to also POST to `/api/event` after
updating the cache. Keep these POSTs short and async (fire-and-forget) so they
don't stall the SSE drain loop. If the dashboard is unreachable, drop the event
silently — don't queue or retry; firmware-side state stays correct without it.

**Dashboard side:** `events` table in Postgres:
```sql
CREATE TABLE events (
  id BIGSERIAL PRIMARY KEY,
  ts TIMESTAMPTZ NOT NULL DEFAULT now(),
  light_idx SMALLINT NOT NULL,
  light_uuid TEXT NOT NULL,
  on_state BOOLEAN,
  bri REAL,
  ct INT,
  owner_rtype TEXT,
  owner_rid TEXT,
  mira_state TEXT,
  sent_target_bri REAL,
  sent_target_ct INT
);
CREATE INDEX events_ts_idx ON events (ts DESC);
CREATE INDEX events_light_idx ON events (light_uuid, ts DESC);
```

### D14 — Live activity feed

A scrolling timeline component on the dashboard, top-of-page or as a
collapsible panel. Each row: timestamp, light name, what changed, owner,
delta from sentTarget. Example:

```
22:14:33  Bedside    bri 65 → 47%   user     -18%
22:13:47  Ceil 1     off                       
22:11:02  Desk       bri 65 → 65%   Mira       (heartbeat)
```

Filter chips: "All", "User actions only", "Mira actions only", "Override
events". Color-code rows: gray for Mira-driven, blue for user-driven, red for
override-triggered.

**Browser delivery:** server-side SSE. The dashboard exposes
`GET /api/events/stream`. Each connected browser tab opens an `EventSource` on
that URL. When the firmware POSTs to `/api/event`, the Flask handler writes to
Postgres *and* pushes to the in-process SSE fan-out. All browsers update in
real time without polling.

**Implementation note:** Flask + SSE on Railway works fine. Use an in-process
queue (e.g., `threading.Queue` or `asyncio` if running async). For multi-worker
gunicorn, use a Postgres `LISTEN/NOTIFY` channel as the fan-out so events
broadcast across worker processes.

### D15 — Per-bulb status pills

Header strip showing each of the four bulbs with current state at a glance:

```
┌─ Bedside ─┬─ Desk ────┬─ Ceil 1 ──┬─ Ceil 2 ──┐
│ ON 47.3%  │ ON 47.3%  │ OFF       │ OFF       │
│ 12s ago   │ 12s ago   │ 4m ago    │ 4m ago    │
└───────────┴───────────┴───────────┴───────────┘
```

Backed by a small `light_status` table or a materialized view of the latest
event per UUID:
```sql
CREATE VIEW light_status AS
  SELECT DISTINCT ON (light_uuid)
    light_uuid, light_idx, on_state, bri, ct, ts
  FROM events
  ORDER BY light_uuid, ts DESC;
```

The pills subscribe to the same SSE stream as the activity feed; on every
event for their UUID, redraw with new values + reset "X seconds ago" counter.

**Total surface area for D13–D15:** ~150 lines firmware (POST helper +
event-shaping), ~250 lines Flask (endpoint + SSE fan-out + Postgres writes),
~200 lines dashboard JS (EventSource subscription + activity feed + pills).
Schema migration for the `events` table. Should fit in a single PR.

---

## Deep Dive — Adaptive Curve Learning (E16)

**The premise:** Every override event is a data point that says "the curve was
wrong here." Specifically: at the moment the user dimmed/brightened, we know
what `luxToTarget()` predicted (`sentTarget`) and what the user actually
preferred (the bri they dialed in). Aggregate these data points over time and
two valuable things become possible:

1. **Surface curve-vs-preference disagreement** in the dashboard so you can
   tune `lightcurve.h` constants by hand with real evidence rather than gut.
2. **Auto-suggest curve adjustments** that minimize the average disagreement.

### Data capture

When `handleLightUpdate()` decides an override has occurred, log the event
shape needed for analysis:

```json
{
  "ts": "...",
  "lux_at_override": 78.4,
  "target_bri": 65.0,
  "target_ct": 320,
  "chosen_bri": 47.3,
  "chosen_ct": 320,
  "delta_bri": -17.7,
  "owner_rtype": "device",
  "minutes_since_state_entry": 12.3,
  "minute_of_day": 1334
}
```

POST to a new `/api/override_event` endpoint (separate from the regular event
firehose so it has clear semantics). Postgres table `override_events`.

### What "override" means in practice

The current SOFT_PAUSE trigger fires on the *first* event that exceeds
tolerance. But the user might keep adjusting (initial press → dial down a few
clicks → settle). The clean data point is the *settled* value, not the first
divergence. Two options:

1. **Capture-and-update.** On override trigger, capture (lux, sentTarget). Then
   over the next ~30 seconds, keep updating `chosen_bri/ct` from incoming SSE
   events. After 30 s of no new events, mark the data point as settled and POST.
2. **Capture-on-resume.** Defer logging until SOFT_PAUSE auto-resumes. At
   resume time, the bulb's current bri/ct is the user's chosen value. Simpler
   to implement, but loses information if the user changes their mind multiple
   times within a single pause window.

Option 1 is more accurate; option 2 is simpler. Start with option 2.

### Counter-evidence — when the curve was right

Override-only data is biased: we only sample when the curve was wrong. To
balance, we also need positive samples:

- **Held-without-override events.** If lux stays in a 10-lux bucket for 15 min
  with no override, log a positive sample at the curve's predicted bri. Same
  table, with `chosen_bri = target_bri` and a `was_override = false` flag.
- Frequency: too many positives drown out the (sparser) negatives. Sample
  positives at a lower rate (e.g., one per 30 min of stable lux per bucket).

### Analysis

A dashboard page bins data by lux range (10-lux buckets, e.g., 0–10, 10–20, …,
2990–3000) and computes per-bucket:
- Mean / median user-chosen bri
- Mean / median curve-predicted bri
- Sample count
- Disagreement = chosen − predicted (in percent)

Plot as overlay on the existing lux curve page: the firmware curve in solid
line, observed user preference as a scatter + smoothed mean line, error bars
sized by sample count.

### Suggestion

The current curve is piecewise (`lightcurve.h`'s 4 segments). To suggest
adjustments, fit each segment's parameters to the observed data using
least-squares against the segment's existing mathematical form. Constrain so
adjacent segments stay continuous (or match the existing intentional
discontinuity at lux=200). Output: a candidate `lightcurve.h` with new
constants and a "what changed" diff.

UI: dashboard shows current and proposed curves side-by-side with a "Apply to
firmware" button. Application path: dashboard PUTs new constants to a firmware
endpoint; firmware persists them to NVS (alongside the cert) and uses NVS
values in `luxToTarget()` instead of compile-time constants. Reflashing
becomes optional for curve tuning.

### Caveats and edge cases

- **Time-of-day matters.** User preference at 8 AM and 11 PM at the same lux
  is probably different. Stratify analysis by hour bucket or include
  `minute_of_day` as a feature.
- **State context matters.** Overrides during WAKE/WIND_DOWN are different
  beasts from NORMAL overrides. Filter analysis to NORMAL-only by default.
- **Volume.** Single-user dataset. Months of data needed for confident
  suggestions in less-trafficked lux buckets (e.g., very high daytime lux).
- **Dashboard testing pollution.** Overrides from the dashboard's "test"
  controls shouldn't count. Tag events from dashboard-issued commands with a
  `source = "dashboard"` flag so they can be excluded from the dataset.

### Minimum viable version

Skip the auto-suggestion entirely at first. Just:

1. Log overrides + held-without-override samples to Postgres.
2. Dashboard renders them on the existing `/lux` page as a scatter overlay.
3. Hand-tune `lightcurve.h` constants based on what the data shows; reflash
   normally.

That alone is a big improvement over the current "nudge constants and see what
happens." The auto-fitting is a phase-2 nice-to-have.

---

## Deep Dive — G22: Instant Lockout Re-Arm

**Current behavior.** `checkBedsideState()` runs once per 30 s tick. It checks:

```
if (lastBedsideOn && !bedside.on && hour >= LOCKOUT_RESET_HOUR) {
    if all other lights are off → state = LOCKED_OUT
}
```

This means re-arming the morning lockout has up to 30 s of latency from when
the last light actually turns off. Not a bug, but a perception lag — the user
turns off the lights, then sees "still NORMAL" on the dashboard for half a
minute.

**With SSE.** Every off-event arrives within ~100 ms. We can run the re-arm
check in `handleLightUpdate()` after each cache update. Pseudocode addition:

```cpp
static void handleLightUpdate(JsonObjectConst upd) {
    // ... existing cache update logic ...

    // Existing override detection guarded on state == NORMAL ...

    // New: instant lockout re-arm check (all states where re-arm is valid)
    if ((state == State::NORMAL || state == State::WIND_DOWN || state == State::SOFT_PAUSE)
        && timeClient.getHours() >= LOCKOUT_RESET_HOUR
        && !lightCache[LIGHT_BEDSIDE].on
        && !lightCache[LIGHT_DESK].on
        && !lightCache[LIGHT_CEIL_1].on
        && !lightCache[LIGHT_CEIL_2].on)
    {
        state          = State::LOCKED_OUT;
        stableLuxCount = 0;
        windDownStep   = 0;
        sendLog("All lights off — locked out for the morning — " + getTimeString());
    }
}
```

**Sequencing concern with SOFT_PAUSE.** Current behavior: user manually turns
off all lights → first off-event triggers SOFT_PAUSE override → subsequent
off-events arrive but don't reach the override branch (already in SOFT_PAUSE).
Re-arm path (in `checkBedsideState`) runs only from NORMAL/WIND_DOWN tick
dispatch — never fires from SOFT_PAUSE. So today, manually turning off all
lights gets you stuck in SOFT_PAUSE for the full hour.

The SSE handler version *can* fire from SOFT_PAUSE if we add it to the
allowed-from-states list (as in the pseudocode above). This is actually
desired behavior: turning off all lights manually after hour 21 is a sleep
signal, not a "leave me alone" signal. Re-arm should win over SOFT_PAUSE.

**Edge cases:**

- Bootstrap: the initial cache populate could spuriously fire re-arm if the
  bridge's initial state happens to be all-off + after hour 21. Solution:
  set a `bootEpochMs` and skip re-arm checks for the first ~5 s after boot, or
  gate on `lightCache[i].initialized` to require the SSE event has been seen
  (not just bootstrap).
- Race with WAKE: if all lights are off and re-arm fires, we go LOCKED_OUT.
  Good. No race.

**Lines of code:** ~10. One block addition to `handleLightUpdate()`. Could
keep the existing `checkBedsideState()` re-arm path as a redundant safety net
or remove it entirely (G23 leans on this).

---

## Deep Dive — G23: Drop `lastBedsideOn` Edge Accumulator

**Current behavior.** `lastBedsideOn` is a global bool that tracks the
previous bedside on/off state across ticks. It's the rising/falling edge
detector for two paths:

1. **Wake trigger:** `state == LOCKED_OUT && !lastBedsideOn && bedside.on`
   → triggerWake().
2. **Lockout re-arm:** `lastBedsideOn && !bedside.on && hour >= 21` → re-arm.

It has to be re-synced from the bridge at every NORMAL/LOCKED_OUT re-entry
point (button press, soft-pause auto-resume, hard-off clear, etc.) to prevent
false edges. There are seven such sync calls in `main.cpp` today.

**With SSE, the edge is delivered directly.** When a light's `on` state
changes, the bridge emits an event whose payload includes the new `on.on`
value. There's no need to remember the previous state ourselves — the event
*is* the edge.

**Refactored wake trigger.** Move into `handleLightUpdate()`:

```cpp
static void handleLightUpdate(JsonObjectConst upd) {
    int idx = idxByUuid(upd["id"]);
    if (idx < 0) return;

    bool prevOn = lightCache[idx].on;  // remember pre-update value
    // ... apply update to cache ...
    bool nowOn  = lightCache[idx].on;

    // Wake trigger: bedside rising edge during LOCKED_OUT
    if (state == State::LOCKED_OUT && idx == LIGHT_BEDSIDE && !prevOn && nowOn) {
        triggerWake(lastLux);
    }

    // ... rest of override detection / re-arm logic ...
}
```

Note that `prevOn` is captured *before* the cache update, so we have access to
both the old and new state for edge detection. Same pattern works for re-arm
in G22.

**Removing the global.** `lastBedsideOn` and every `lastBedsideOn = ...; //
sync from actual bridge state` line goes away. That's seven sync points
deleted from `main.cpp`. The remaining `checkBedsideState()` function shrinks
to just the lockout re-arm logic (which itself moves to the SSE handler under
G22), so `checkBedsideState()` can be deleted entirely.

The dispatch in `loop()` shrinks too:
```cpp
case State::LOCKED_OUT:
    checkBedsideState(lux);   // gone
    break;
case State::NORMAL:
    checkBedsideState(lux);   // gone
    tickNormal(lux, target, shouldUpdate);
    break;
case State::WIND_DOWN:
    checkBedsideState(lux);   // gone
    tickWindDown();
    break;
```

→
```cpp
case State::LOCKED_OUT: break;
case State::NORMAL:     tickNormal(lux, target, shouldUpdate); break;
case State::WIND_DOWN:  tickWindDown(); break;
```

The state machine becomes a pure dispatcher; bedside-edge logic is wholly
event-driven.

**Edge cases:**

- **Bootstrap.** The first time the SSE handler sees an event for the
  bedside, `prevOn` should equal whatever was loaded from
  `bootstrapLightStates()`. So `prevOn` from cache (post-bootstrap) is correct
  on first SSE event. No special-casing needed — just make sure bootstrap runs
  before SSE connect, which we already do.
- **Missed events during reconnect.** If the SSE socket drops and reconnects,
  we miss any events during the gap. The next event we see has the latest
  state, but the cache has the pre-drop state. If the bedside was off, then
  briefly turned on during the gap, then turned off again, we'd see "off →
  off" (no change) and miss the wake trigger. Mitigation: on SSE reconnect,
  re-run `bootstrapLightStates()` and check for bedside rising-edge against
  the pre-reconnect cache. Add this to `sseConnect()` as a post-handshake step.
- **Initial-state-after-boot.** If the bedside is already on at boot in
  LOCKED_OUT (e.g., user left it on overnight), we shouldn't fire wake.
  Bootstrap captures `bedside.on = true`. SSE delivers no event because nothing
  changed. No spurious wake trigger.

**Lines of code:** ~30 deletions, ~15 additions. Net simplification.

**Coordination with G22.** G22 and G23 are best done together. G22's re-arm
check uses cache values, which G23 already maintains correctly. The deletion
of `checkBedsideState()` is the union of both refactors.

---

## Top Picks — ROI Ranking (post-feedback)

Given the user-direction notes (owner-aware deprioritized, D13–15 +
Tap Dial + E16 + G22/G23 prioritized), the recommended order:

1. **G22 + G23 together.** Code simplification + UX win (instant re-arm).
   ~1 hour of work. Very low risk. Should happen soon.
2. **D13 + D14 + D15 together.** Real-time dashboard. Largest perceptible
   improvement to the user experience. ~1–2 days of work across firmware and
   dashboard. Ships as one PR.
3. **C9 (zigbee_connectivity → cache).** Trivial addition; meaningful
   diagnostics. Can ride along with G22/G23 if convenient.
4. **B5 (Tap Dial).** Hardware purchase + UUID discovery + ~50 lines of
   firmware. Wait until D13–15 lands so you have dashboard visibility into
   button events for debugging.
5. **E16 — minimum viable version.** Just log override events to Postgres and
   render them on the existing `/lux` page. Iterate from there.
6. **Owner-aware override (A).** Defer until the integration surface grows
   (Calendar, Siri, Hue automations). Currently low value per user note.
7. **Everything else** — pick opportunistically as needs arise.
