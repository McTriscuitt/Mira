Layer 9: What I think is awkward, and where there's room to redesign

A few structural choices feel load-bearing in ways that probably aren't necessary. In rough order of "biggest    
leverage if changed":

(a) sseTick() only runs in the wait loop. This is the single biggest source of latency variance. When the main   
core is in a Railway HTTP call, app-level SSE drain is paused. You could move SSE handling to a FreeRTOS task    
pinned to the second core — then SSE events get processed in true real time, regardless of what loop() is doing.
The ESP32-S3 supports this directly; xTaskCreatePinnedToCore would do it. The async task would need to use a     
mutex when touching state, sentTarget, lightCache, etc., but the surface area is small.

(b) Time-based mute as the echo-discriminator. Echoes and overrides are mechanically distinguishable if you keep
a record of what you just told the bridge. You could maintain a small "recent PUTs" ring buffer keyed by light   
UUID + target values. When an SSE event arrives, look up the most recent PUT for that UUID; if the event's fields
match the PUT within tolerance, it's an echo, ignore it. If they don't match, it's external, react. No mute     
window needed at all — and no false negatives during the mute window either (real overrides during the first 3 s
after a PUT would be detected, which they currently aren't).

(c) sentTarget is a single tuple, not per-light. Right now sentTarget represents what you asked all four lights  
to be at — but during overhead-edge transitions, the overheads are off while bedside/desk are on. There's a      
subtle assumption that all "expectedOn" lights share the same target, which works today but is structurally      
brittle. A LightTarget sentTarget[4] indexed by light index would make the override check unambiguous per-light.
This is a small refactor with no behavioral risk.

(d) The cache exists for two distinct purposes. It's both (i) a snapshot of current bulb state used by
cachedLight() callers (e.g., overheadsOn = cachedLight(LIGHT_CEIL_1).on after state transitions) and (ii) an     
input to override detection (no longer, after today's rewrite). With (ii) removed, the cache's contract
simplifies — it's just "best-known last-reported state." That makes (a) easier too, because the async task can   
update the cache freely without coordinating with override semantics.

(e) SSE bootstrap (bootstrapLightStates()) and SSE event handling diverge. The bootstrap reads full state for all
four lights at startup. Subsequent SSE events update piecemeal. You could collapse these: instead of a one-time
HTTP GET, take the first few seconds of SSE events as initial state, or do a full re-bootstrap on every
reconnect. The current split exists mostly because SSE doesn't naturally send a snapshot — but you could request
a snapshot via the regular v2 endpoint anytime the cache feels stale (e.g., after a SOFT_PAUSE expires).

★ Insight ─────────────────────────────────────
- The deepest design tension here is that SSE as a protocol is push-only, but your application has a
  request/response mental model (you PUT, you expect a result). Trying to derive "did my PUT succeed?" from a push
  channel makes the code work against the grain. Most of the awkwardness — mute windows, cache merging, echo       
  discrimination — is downstream of that fundamental mismatch.
- (b) — keeping a recent-PUT history and matching events to it — is the cleanest reframing because it
  acknowledges that mismatch explicitly: SSE is the channel for external changes, and self-PUT echoes are something
  we can filter out by knowing what we just asked for.
- (a) — async SSE task — is the most impactful for responsiveness but doesn't change the discrimination logic; it
  just makes the existing logic run sooner.
  ─────────────────────────────────────────────────