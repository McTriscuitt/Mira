# May 13 — Mid-Batch Abort Patch

**Status:** Verified working. See `May 13 Post Patch Error.md` for the post-flash serial log.

**One-line:** SSE override detection was correctly firing, but the in-flight `setLight()` chain inside `tickNormal` (or `tickWakeRamp` / `tickWindDown`) didn't notice — so PUTs #N+1..#K kept going out *after* the override flipped state to `SOFT_PAUSE`, driving the un-PUT bulbs to the lux-curve target the user had just manually overridden.

---

## The bug

The trigger was a real user override, captured in an earlier serial log dump (paraphrased):

```
[3:54:55PM] Wednesday [NORMAL]
Lux: 18.8 → bri=62.2%, ct=336
setLight(616a67e4…) → HTTP 200   ← bedside PUT goes out
setLight(2064523e…) → HTTP 200   ← desk PUT goes out
SSE override — soft pause.       ← user changed brightness via Hue app
Override fire idx=0
  evt: bri=67.98
  cacheBefore: on=1 bri=82.61 ct=295
  ...
setLight(6cccebb8…) → HTTP 200   ← ceil_1 PUT still goes out
setLight(14bd82eb…) → HTTP 200   ← ceil_2 PUT still goes out
Bulbs updated.
[3:55:25PM] Wednesday [SOFT_PAUSE]   ← only NOW the loop notices the state flip
```

User report: *"I change the brightness from 90 to 50, and if the lights are in the middle of updating, some of them finish updating to an undesired brightness level."*

Behavior we wanted: *"if the lights are in the middle of updating, and an override is detected, the lights do not finish updating, but instead skip to the new state."*

## Root cause

Structural, not a tolerance bug. The override detector itself was working correctly — it caught the manual change on PUT #2's echo and flipped `state = SOFT_PAUSE`. But:

1. Each `setLight()` and `setLightColor()` ends with a tail call to `sseTick()` to drain pending bridge echoes off the SSE socket. This is what gives the override its low latency.
2. `sseTick()` is synchronous — when it processes an Override event, `handleLightUpdate()` sets `state = State::SOFT_PAUSE` *before returning to the caller*.
3. The caller (`tickNormal`'s 4-bulb chain at `main.cpp:1154-1158`, plus the analogous chains in `tickWakeRamp` / `tickWindDown` / the pause-resume ramp / the overhead-off pair) does not re-check `state` between PUTs.

So the override can fire mid-chain, but the chain doesn't yield to the state machine — it keeps PUTting against a curve target the user has just overruled.

This is "cooperative preemption" — SSE can preempt at the boundary between PUTs, but the chained caller never explicitly yields control.

## The fix

A one-line guard at the top of both PUT helpers, *before* `noteRecentPut()` and the HTTP request:

```cpp
void setLight(const char* uuid, bool on, float bri, int ct, int durationMs) {
    if (state == State::SOFT_PAUSE || state == State::HARD_OFF) return;
    // ... rest unchanged
}

void setLightColor(const char* uuid, bool on, float bri, int hueV1, int satV1, int durationMs) {
    if (state == State::SOFT_PAUSE || state == State::HARD_OFF) return;
    // ... rest unchanged
}
```

Two important ordering choices:

- **Before `noteRecentPut()`.** If we recorded a trajectory slot but then skipped the actual PUT, an unrelated future event for the same bulb could match against the orphan slot during its `durationMs + RECENT_PUT_GRACE_MS` lifetime, silently misclassifying a real override as `EchoMatch`. Bailing before the record keeps the ring state clean.
- **Both `SOFT_PAUSE` and `HARD_OFF`.** The state-machine invariant being enforced is "no automated bulb control in these states." A new caller that adds a chained PUT site in the future gets the same protection for free without remembering to add explicit checks.

Why a centralized guard rather than inline `if (state != ...) return;` checks between every PUT in the tick functions: the rule ("no auto-control PUTs in SOFT_PAUSE / HARD_OFF") is already a global invariant of the state machine. The right place to enforce a global invariant is the helper that would otherwise violate it.

## Verification

`May 13 Post Patch Error.md` is the serial log after flashing. Across roughly 13 minutes of operation:

- **Override 1 — user-caused** (1:34:28 PM, user changed brightness via Hue app). System paused correctly; no stray PUTs after the override line. ✓
- **Override 2 — false positive after dashboard `NORMAL`** (1:35:27 PM). Resumed from `SOFT_PAUSE` via dashboard, the lux-driven PUTs went out, and the bridge's echo of bedside (`bri=96.84`) didn't match any live trajectory slot or the pre-event cache (`bri=63.64`, leftover from the user override in Override 1). This is a *different* bug — the resume path's first PUT after `forceState(NORMAL)` doesn't seed the trajectory ring against the pre-resume cache value — not in scope for this patch.
- **Override 3 — same false-positive shape** (1:38:28 PM, user-induced lux swing via finger over sensor → lux returns to ambient → trajectory ring slot expired before echo arrived). Same secondary issue, same out-of-scope.

For Overrides 2 and 3, the patch still did its job — once the override fired, *no further* `setLight()` lines appear before the next state's tick. The chain was correctly aborted. The false positives themselves are a separate concern (trajectory ring expiry vs. late-arriving echoes during forced resumes, and seeding the cache during state transitions).

## Side effects

Cosmetic only: `tickNormal`'s post-batch lines still run after the abort —

```
sentTarget = target;
saveLastState(target.bri, target.ct);
Serial.println("Bulbs updated.");
sendLog(...);
```

— so the serial log prints `"Bulbs updated."` even when only some bulbs were PUT. The immediately following `"Manual override — soft pause"` line explains what happened, so the sequence is still readable. Cleaning up the cosmetic gap (e.g., return early from `tickNormal` post-batch on state change) is a low-priority follow-up.

`sentTarget` is written with the un-PUT target, but it doesn't matter — the system is now in `SOFT_PAUSE` and won't read `sentTarget` until the resume ramp 60 min later, by which point lux has fully evolved anyway.

## Files changed

- `src/main.cpp` — guard added to `setLight()` (around line 538) and `setLightColor()` (around line 494)
- `CLAUDE.md` — Hue API Patterns + Implemented + Post-Phase-3 refinements updated to document the abort as layer 4 of echo discrimination
- `Markdowns/SSE.md` — new `### Mid-Batch Abort` subsection under `## Override Detection Flow`

## See also

- `May 13 Post Patch Error.md` — raw serial log proving the patch works (plus exhibits of the unrelated trajectory-ring-expiry false positive)
- `Markdowns/SSE.md` — full SSE subsystem reference, including the new abort section
- `Markdowns/SSE v1.0 Awkward Structure.md` — original design critique that motivated the recentPuts ring + no-op filter
- `Markdowns/ECHO_DISCRIMINATION.md` — brainstorm doc that explored the layered approach
