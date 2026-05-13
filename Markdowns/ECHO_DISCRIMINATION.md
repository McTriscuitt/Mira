# Mira — Echo Discrimination Strategies

> **What shipped (post-brainstorm):** This doc was written when the firmware
> still used a single-slot `prevTarget`/`lastTarget` double-buffer. The
> trajectory-ring rewrite shipped in commit `8811d5c` already addressed the
> "three known failure modes" the doc enumerates below — double-echo emissions,
> split events, and bulb rounding all fall out of trajectory matching with
> per-field tolerance.
>
> Commit `756d5dc` then closed a *fourth* failure mode this doc didn't
> enumerate: a late settling echo arriving after a `recentPuts` slot expired
> on a "skipping PUT" tick. Two changes shipped together:
>
> 1. **3-slot ring per light** (`recentPuts[4][3]`) so overlapping in-flight
>    PUTs keep their trajectories live simultaneously. Strengthens Signal 2.
> 2. **No-op event filter** — a new signal not enumerated below: after the
>    trajectory check fails, classify as echo if the event's values are within
>    `TRAJECTORY_TOLERANCE_*` of the **pre-event cache**. Equivalent to asking
>    "did this event represent a state change at all?" relative to what we
>    thought was true. Closes the seam where neither a live trajectory nor a
>    grace window covered the arriving event.
>
> Diagnostics shipped alongside: `EchoOutcome` enum, 16-entry
> `sseEventRing[]`, cumulative `echoOutcomeCounts[]` since boot, and a
> dashboard-bound `dumpOverrideDiagnostic()` on Override fire.
>
> The full design space catalogue below is preserved for context. Signals 1
> (lattice watermark), 3 (scene-mediated dispatch), and 4 (event-ID causality)
> are still on the table if the current two-layer design proves insufficient.
> Signal 5 (magnitude / E17 framing) is the next likely addition. See
> `Markdowns/SSE.md` for the canonical current-state reference.

---

Companion document to `SSE_BRAINSTORM.md`. Captures approaches to the
"who sent this command" problem on the Hue v2 SSE stream. The current
`prevTarget` / `lastTarget` double-buffer lets echoes slip through in
three known failure modes; this doc enumerates more robust strategies
and a fusion classifier that combines them.

---

## The Problem

The Hue bridge knows exactly which application_key issued every state
change — it's in the URL path on every PUT — but it doesn't expose that
information on the SSE stream. Every `update` event arrives anonymous.
Echo discrimination has to be reconstructed from side channels.

The current double-buffer was a reasonable first cut but breaks on:

1. **Double-echo emissions.** The bridge often emits *two* events per
   command: an optimistic event reflecting the commanded state, then a
   Zigbee-confirmation event reflecting the bulb's actual reported state
   (which may differ by ±1 in quantized units). A single-slot buffer
   matches the first and treats the second as an override.
2. **Split events.** A combined `on:true + brightness + mirek` command
   can produce 1, 2, or 3 separate SSE events. A single-tuple buffer
   matches only one of them.
3. **Bulb rounding.** The bulb's reported brightness rarely matches the
   commanded value exactly. Tolerance-less matching misses these.

All three failure modes manifest as Mira's own echoes being
misclassified as user overrides → spurious SOFT_PAUSE.

---

## Six Signals

### 1. Steganographic brightness watermarking

**Idea.** Encode "Mira issued this" into the value itself. Pre-compute
the ~254 possible echoed brightness values (one per Zigbee level) and
pick a subset as the "Mira lattice." Every Mira PUT snaps target
brightness to the nearest lattice value. Echo detection: *is this
brightness on the lattice?*

**Why it works.** The Hue app, dimmer switches, and voice assistants
all send round-number brightnesses (50.0, 75.0). The bridge quantizes
those to whatever Zigbee level is nearest, which lands off-lattice
with high probability if the lattice is chosen with the right offset.

**Data structure.**
```cpp
constexpr float MIRA_LATTICE[NUM_LATTICE_POINTS] = {
    /* every 7th Zigbee level's reported brightness — calibrate on boot */
};
bool isOnLattice(float bri, float tolerance = 0.05f);
```

**Pros.** Stateless. Works even if SSE delivery is reordered or
delayed. Survives reboots without rehydration.

**Cons.** Brightness space is only ~254 values. Sparser lattice → fewer
brightness values Mira can target. Requires empirical Zigbee-level →
reported-percentage mapping on first boot (the exact formula depends on
bulb firmware).

**Effort.** ~1 hour calibration + ~30 lines firmware.

---

### 2. Pending-state set with tolerance and TTL

**Idea.** Direct replacement for the double-buffer. After each PUT,
push the expected post-state onto a per-light *set* (not a single slot)
with a TTL of ~10 s. Each SSE event tolerance-matches against all
entries in the set; matches consume the entry.

**Data structure.**
```cpp
struct PendingState {
    bool      on;
    float     briPercent;     // expected echoed value (post-quantization)
    uint16_t  mirek;
    uint32_t  expiresAtMs;
    bool      valid;
};

constexpr int PENDING_SET_SIZE = 5;
PendingState pending[NUM_LIGHTS][PENDING_SET_SIZE];

bool consumeMatch(int lightIdx, const SSEEvent& evt,
                  float briTol = 1.0f, int ctTol = 2);
```

**Tolerance values.** `briTol = ±1%`, `ctTol = ±2 mirek`, exact match
on `on`.

**Handles all three failure modes.**
- *Double-echo:* push the expected state with a hit counter of 2, or
  push twice on PUT. Both the optimistic and Zigbee-confirmation events
  consume entries.
- *Split events:* push one entry per sub-state (one for `on`, one for
  `brightness`, one for `mirek`). Each arrives and consumes
  independently.
- *Rounding:* tolerance match absorbs ±1 quantization error.

**Pros.** Most rigorous standalone approach. Drop-in replacement for
the current `prevTarget`/`lastTarget` buffer. Set semantics don't care
about event ordering.

**Cons.** Two different commands targeting the same end-state are
indistinguishable — if Mira commands 50% and the user immediately sets
50% via the Hue app, both look like echoes. Mitigated by combining
with signal 1 (lattice) or signal 3 (scene marker).

**Effort.** ~80 lines firmware. Largest single rewrite of
`handleLightUpdate()`.

---

### 3. Scene-mediated dispatch

**Idea.** Create a dedicated dynamic scene named `mira_internal`. To
change lights, PUT the scene's `actions` array with the new target
states, then recall the scene with `recall: { action: "active" }`. The
bridge emits a `scene` event before the resulting `light` events for
the same `creationtime`, giving a clean "Mira's command starts here"
marker on the SSE stream.

**Logic.**
```cpp
uint32_t lastMiraSceneCreationTime = 0;

void onSseEvent(const SSEEvent& evt) {
    if (evt.type == "scene" && evt.resourceId == MIRA_SCENE_UUID) {
        lastMiraSceneCreationTime = evt.creationtime;
        return;
    }
    if (evt.type == "light") {
        bool causedByMira = (evt.creationtime == lastMiraSceneCreationTime);
        // ... feed into classifier ...
    }
}
```

**Pros.** Cleanest categorical signal the API offers. Scene events are
explicit and unambiguous when the scene name is unique to Mira. The
Hue app's brightness slider and physical dimmer switches do *not*
produce scene events, so their direct light events never carry the
marker.

**Cons.** Adds a second HTTP call per command (PUT scene actions, then
recall). On local network this is single-digit ms, so not painful.
Mira's per-bulb sequenced transitions (WAKE, WIND_DOWN) would need to
be re-expressed as scene updates, which is awkward for staggered
fades.

**Effort.** ~100 lines firmware refactor of the dispatch layer.
Larger change but most robust marker.

---

### 4. SSE event-ID causality

**Idea.** Hue SSE event IDs are `creationtime:index` and monotonically
increase per connection. Before each PUT, snapshot `lastSeenEventId`.
The next 1–3 events on that light's resource ID after the snapshot are
causal echoes.

**Pros.** No content matching needed when no other resource is changing.

**Cons.** Breaks if *anything* else interleaves on the SSE stream —
a motion event, another bulb's state change, a sensor update.
Useful only as a *tiebreaker*, not standalone.

**Effort.** ~20 lines. Lowest-effort signal.

---

### 5. Change magnitude — user-confidence booster

**Idea.** Manual user changes in current Mira usage are nearly always
drastic: on↔off, brightness delta >20%, mirek delta >100. Mira's
30-second tracking ticks produce 2–8% brightness deltas and ±15–30
mirek shifts. A large delta is strong evidence of user origin.

**CRITICAL FRAMING — read before implementing.** Magnitude is **only a
positive signal for "user," never a negative signal for "not user."**
A small-delta event is *not* evidence of Mira origin. See the
[E17 Caveat](#the-e17-caveat) section below.

**Mira's own large deltas.** Mira *also* produces big deltas: WAKE
jumps off→on, WIND_DOWN steps can be 10–15% per beat, soft-pause resume
ramps from current to target in one shot. So magnitude must always be
evaluated *after* the pending-state set has had a chance to catch the
echo. Never use magnitude as a first-pass filter.

**Logic.**
```cpp
struct Delta {
    bool  onOffTransition;
    float briDelta;
    int   ctDelta;
};

bool isDrasticDelta(const Delta& d) {
    return d.onOffTransition
        || fabsf(d.briDelta) > 20.0f
        || abs(d.ctDelta)    > 100;
}
```

**Effort.** ~15 lines.

---

### 6. Weak-signal fusion — the integrator

No single signal is sufficient. The classifier combines them as an
ordered pipeline. The order matters: high-precision matchers fire
first, magnitude promotes user-confidence only after Mira-signatures
have all failed, and the default branch favors `User` to preserve E17.

```cpp
enum class EventOrigin { Mira, User };

EventOrigin classifyEvent(int lightIdx, const SSEEvent& evt) {
    // 1. Pending-state set (most precise — content + tolerance)
    if (consumeMatch(lightIdx, evt)) return EventOrigin::Mira;

    // 2. Scene-recall marker (categorical, unambiguous when present)
    if (evt.creationtime == lastMiraSceneCreationTime)
        return EventOrigin::Mira;

    // 3. Lattice watermark (stateless, probabilistic)
    if (isOnLattice(evt.briPercent)) return EventOrigin::Mira;

    // 4. Event-ID causality (structural tiebreaker)
    if (evt.eventId == expectedNextEventId(lightIdx))
        return EventOrigin::Mira;

    // 5. Magnitude — promotes user confidence only
    Delta d = computeDelta(lightCache[lightIdx], evt);
    if (isDrasticDelta(d)) return EventOrigin::User;

    // 6. Default. Small delta, no Mira signature.
    //    Conservative: classify as User to preserve E17 viability.
    //    If approaches 1–4 are robust, step 6 rarely fires for echoes.
    return EventOrigin::User;
}
```

**Why this order.**
- Step 1 is content-based and tolerance-aware — the strongest signal.
- Step 2 is categorical and unambiguous when scene-dispatch is in use.
- Step 3 is stateless backup for when state matching loses sync.
- Step 4 is fragile (interleaving breaks it) but cheap to include.
- Step 5 promotes user confidence but cannot demote it.
- Step 6 defaults to User so unattributed small-delta events don't get
  silently dropped — required for E17.

**Where current pain gets solved.** Today's echo slips come from the
double-buffer (signal-2 equivalent only, no tolerance, no set). Adding
signal 1 (pending-state set) alone eliminates the three known failure
modes. Signals 3 and 4 are belt-and-suspenders. Signal 5 protects
against signal-1 mis-pushes during Mira state transitions.

---

## The E17 Caveat

E17 in `SSE_BRAINSTORM.md` proposes replacing the 60-min soft-pause
timeout with "no user-initiated events for 10 min → resume." This
requires every legitimate user touch — including small adjustments
from a future Tap Dial or voice command — to be reliably classified
as `User` so the inactivity timer resets correctly.

**The trap.** A magnitude-based filter that classifies *small* deltas
as Mira echoes would silently starve E17's timer of legitimate user
activity. A user actively dialing brightness in 2% increments would
look like 10 minutes of inactivity, and Mira would resume tracking
mid-tweak.

**Therefore:** magnitude can only *promote* an event toward `User`
classification (step 5). It cannot *suppress* events from the default
`User` branch at step 6. The pipeline above is designed so adding E17
later requires no changes to echo discrimination — small user changes
naturally fall through to step 6 and classify as `User`.

Cross-reference this section if magnitude logic ever gets refactored.

---

## Implementation Phasing

In order of effort-to-impact ratio:

1. **Signal 2 (pending-state set).** Direct replacement for the
   current double-buffer. Fixes the three known failure modes
   (double-echo, split events, rounding) without other architectural
   change. ~80 lines. **Do first.**
2. **Signal 5 (magnitude).** Trivial addition once signal 2 is in.
   ~15 lines on top.
3. **Signal 6 (fusion).** Refactor `handleLightUpdate()` into the
   ordered pipeline above. ~30 lines of plumbing.
4. **Signal 3 (scene-mediated dispatch).** Biggest robustness win but
   largest rewrite. Defer until 1–3 are proven and any remaining echo
   slips justify it.
5. **Signal 1 (lattice watermark).** Optional. Adds another stateless
   signal. Useful if signal 3 stays deferred.
6. **Signal 4 (event-ID causality).** Lowest priority. Worth adding
   only if a specific case demands it.

---

## Verification / Open Questions

Before depending on any of these, confirm on the actual bridge
firmware. Add temporary `Serial.printf` to `handleLightUpdate()`
per the existing action item in `SSE_BRAINSTORM.md` §A and run a
controlled experiment.

- **Echo precision.** Does the bridge preserve fractional brightness
  in SSE echoes, or does it quantize to Zigbee levels? Test: PUT
  brightness 50.47, observe SSE echo. → affects signal 1.
- **Event splitting.** For a combined `on + brightness + mirek` PUT,
  does the bridge emit 1 SSE event or 3? → affects signal 2 push
  strategy.
- **Scene-recall ordering.** When recalling a scene, does the `scene`
  event always arrive *before* the resulting `light` events in the
  same SSE batch, and do they share `creationtime`? → affects
  signal 3.
- **Optimistic-vs-confirmation timing.** When does the second
  (Zigbee-confirmation) event arrive after the first (optimistic)
  event? Same `creationtime`? Different? → affects signal 2 TTL and
  signal 4.

Most of these answer in one afternoon of probing the bridge.

---

## Cross-References

- `SSE_BRAINSTORM.md` §A — owner-aware override (related but
  orthogonal: this doc is about Mira-vs-user, that one is about
  Hue-app vs accessory vs behavior_instance).
- `SSE_BRAINSTORM.md` §E17 — inactivity-based soft-pause resume
  (the future feature that constrains the magnitude framing).
- `SSE_BRAINSTORM.md` §G22, §G23 — SSE-driven simplifications that
  share `handleLightUpdate()` with this work; coordinate refactors.
