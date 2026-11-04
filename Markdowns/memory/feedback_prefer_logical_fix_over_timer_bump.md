---
name: Prefer logical fixes over widening time windows
description: For race/timing bugs, exhaust logical fixes before bumping timeouts or mute windows — preserve the responsiveness properties that motivated the architecture.
type: feedback
originSessionId: 76ea4ce4-22a3-4340-88fd-132d97e9485c
---
When diagnosing a timing-related bug (e.g., SSE override false positives, mute window races), prefer fixes that address the logical cause over fixes that simply widen a timer. Specifically: rewriting the check to use the right data (e.g., per-field SSE event values instead of cache-merged state) is preferable to making a mute window 10× longer.

**Why:** During the May 10 2026 soft-pause debugging, the user pushed back on raising `muteOverride` slack from 2 s to 15 s: "making the gap larger removes the benefits of the instantaneous-ness of the sse. is there a different check we could do, or is this the best thing weve got?" The system was designed around SSE specifically *because* it gives instant override detection, and inflating mute windows undermines that. The actual fix turned out to be evaluating override only against fields present in each event, not against cache-merged state.

**How to apply:** Before proposing "just make the timeout/window bigger" as the fix for a race, look for a cleaner logical answer — is there stale data being consulted that shouldn't be? Is there a redundant check? A more precise comparison? Bring the long-timer fix forward only if a logical answer doesn't exist or is disproportionately complex. State the trade-off explicitly so the user can choose.
