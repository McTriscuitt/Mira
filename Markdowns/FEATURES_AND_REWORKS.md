# FEATURES_AND_REWORKS.md — Mira Feature & Rework Brainstorm

Catalogued June 9, 2026, from a full read of `main.cpp`, `config.h`, `lightcurve.h`,
`CLAUDE.md`, `DASHBOARD.md`, `SSE.md`, `SSE_BRAINSTORM.md`, and
`SSE v1.0 Awkward Structure.md`.

Companion to `SSE_BRAINSTORM.md`, which owns the SSE-specific idea space (Tap Dial
B5, real-time dashboard D13–15, adaptive learning E16, G22/G23, owner-aware policy
A). This file extends those ideas and covers everything else. Cross-references use
that file's IDs.

**ID scheme** — stable handles so items can be referenced unambiguously across
Claude Code sessions:

| Prefix | Meaning |
|---|---|
| `N#` | Direct answers to the open notes in `CLAUDE.md` |
| `R#` | Reworks — existing code made cleaner / faster / more correct |
| `F#` | New behavior features |
| `P#` | Dashboard, portfolio, ops, and tooling |

When an item ships: move a summary line to `CLAUDE.md` → "Recently completed" and
mark it **SHIPPED** here (don't delete — the IDs stay referenceable).

---

## User Notes / Decisions

- June 11, 2026: we can remove the physical buttons, i dont see them as necessary. 

---

## Doc Drift Found While Auditing (fix opportunistically)

1. **UTC offset mismatch.** `config.h` has `UTC_OFFSET_SEC (-18000)` (UTC-5 / CDT —
   correct for San Antonio in June). `CLAUDE.md` says "currently -14400 (UTC-4 /
   Eastern Daylight)". Both are wrong twice a year regardless of which is current —
   see R8 for the real fix.
2. **Pinned cert never used on live paths.** `CLAUDE.md` ("Both use WiFiClientSecure
   with the NVS-stored bridge TLS cert") and `SSE.md` ("`setCACert()` not
   `setInsecure()`") describe the *intended* design. The code as written calls
   `setInsecure()` in `setLight()`, `setLightColor()`, `bootstrapLightStates()`,
   **and** `sseConnect()`. `_bridgeCertPem` is fetched, stored in NVS, verified at
   boot — and then unused by every live connection. See R3.

---

# N. Answers to the Open CLAUDE.md Notes

## N1 — Event-driven reactive core *(the architecture question)*

> **SHIPPED — all 5 stages flashed** (Stage 1 June 11, Stage 2 July 14, Stage 3
> July 15, Stage 4 July 16 live-verified, Stage 5 July 16 flashed +
> boot-verified — netTask owns all Railway HTTP, command latency ~5 s, heap/
> stack telemetry on /api/status + /api/telemetry/history). Remaining: Stage 5
> final checks + the cross-stage regression checklist. Status/specs:
> `N1_MIGRATION.md`.

> CLAUDE.md / SSE_BRAINSTORM note: "how can we turn this into an sse/event
> driven/reactive system, so that the 30 second tick interval is solely for
> updating brightness?"

**Shape.** A small event queue + per-state dispatch, with SSE drained on the second
core. Subsumes G22 (instant lockout re-arm), G23 (drop `lastFloorOn` accumulator),
and awkward-structure item (a) (async SSE task).

```cpp
enum class EvType : uint8_t {
    LuxTick,        // 30 s timer — the only thing the "tick" remains
    SseLight,       // classified light event from the SSE task
    SseAccessory,   // future: button / relative_rotary / zigbee_connectivity
    ButtonPress,    // debounced GPIO press (short/long encoded in payload)
    DashboardCmd,   // command pulled by the network task
    TimerFire       // soft-timer expiry (ramp step, pause expiry, etc.)
};
struct Event {
    EvType   type;
    uint8_t  a;     // idx / button id / command enum / timer id
    int32_t  i;     // value (cmdValue, steps, …)
    float    f;     // lux / bri
    uint32_t tMs;
};
QueueHandle_t evQueue = xQueueCreate(32, sizeof(Event)); // FreeRTOS queues are thread-safe
```

`loop()` collapses to *consume → dispatch*:

```cpp
void loop() {
    Event ev;
    if (xQueueReceive(evQueue, &ev, pdMS_TO_TICKS(50)) == pdTRUE) dispatch(ev);
    softTimers.tick();  // enqueues TimerFire for due deadlines
}
```

**Producers.** SSE task pinned to Core 1 (`xTaskCreatePinnedToCore`) → `SseLight` /
`SseAccessory`; a 30 s soft timer → `LuxTick`; button polling (can stay in a small
task or the consumer's 50 ms idle) → `ButtonPress`; the network task (see N2) →
`DashboardCmd`.

**Concurrency discipline (small surface, but get it right):**

- `lightCache[]` becomes **single-writer**: only the SSE task writes it; the main
  task reads via `cachedLight()`. Wrap the 3-field copy in a
  `portMUX_TYPE` critical section to be rigorous about torn reads.
- `state` is mutated **only by the dispatcher**. The SSE task never sets
  `state = SOFT_PAUSE` directly — it enqueues an Override-classified `SseLight`
  event and the consumer decides.
- **Mid-batch abort needs an equivalent in the new model.** Today, `setLight()`
  tail-calls `sseTick()` so an Override flips state *between* PUTs in a
  `tickNormal` chain and the guard short-circuits the rest. Under the queue model
  the Override event would sit in the queue until the chain finishes — reintroducing
  the bug the guard fixed. Fix: SSE task sets a `volatile bool overridePending`
  the moment it classifies an Override; `setLight()`/`setLightColor()` keep their
  top-of-function guard but check `overridePending || state == SOFT_PAUSE || …`.
  The dispatcher consumes the flag when it processes the event.

**Ramps.** Convert `wakeStep` / `windDownStep` / `pauseResumeStep` advancement from
tick-coupled increments to `TimerFire`-driven steps — but **keep the step counters
as the state representation**, because the dashboard sliders seek on them.

**Migration order (each step independently shippable):**

1. Move `sseTick()` into a pinned Core-1 task that writes the cache and enqueues
   events (cache is already read-mostly post-(d), so this is now low-risk).
2. Convert `loop()` to the consumer; `LuxTick` handler is today's tick body.
3. Fold in G22/G23 — floor-lamp edge + lockout re-arm move into the `SseLight`
   dispatch path; delete `checkFloorState()` and all seven `lastFloorOn` syncs.
4. Convert ramps to soft timers.
5. Move dashboard networking to its own task (unblocks N2's long-poll).

**Payoff:** wake trigger, lockout re-arm, overrides, button presses, and dashboard
commands all land in ~100 ms instead of ≤30 s. The 30 s tick's only job is the
curve. Effort: the largest item in this file — plan ~2–4 sessions, staged.

---

## N2 — Near-instant dashboard, both directions

> **IN PROGRESS.** N1 Stage 5 (the prerequisite) shipped July 16; user
> confirmed the residual lag live the same night: commands apply in ~5 s but
> the display waits on the browser's 30 s status poll. Agreed plan, in order:
> **(1) burst polling — SHIPPED July 24**: after any command (`sendCommand`
> and the fire-and-forget light chips) `startStatusBurst()` in `index.html`
> polls `/api/status/latest` every 2 s until the snapshot confirms the
> command (`_pendingCommand` cleared), with an 8 s floor (covers the
> firmware's 5 s command pickup + snapshot push for chip toggles, which never
> set `_pendingCommand`) and a 20 s cap; refreshes the log once on burst end;
> no reflash, Railway deploy only. **(2) Flask → browser SSE**
> (`/api/status/stream`, the D13–15 fan-out; mind gunicorn worker config
> for held connections). **(3) firmware long-poll** on `/api/command` from
> netTask, killing the last ~5 s. Firmware already pushes a status snapshot
> ~100 ms after applying any command (Stage 5 deviation), so each piece
> compounds: all three ≈ press → lights → dashboard confirm in ~1 s.

> CLAUDE.md note: "is there a way to do sse-like updates (near-instant) with the
> dashboard?"

Two directions, and they're different problems:

**Firmware → browser** is D13–15 in `SSE_BRAINSTORM.md` (firmware POSTs events,
Flask fans out to browsers via server-side SSE / `EventSource`). Already specced
there; nothing to add.

**Browser → firmware** is the missing half: a dashboard button press currently
waits up to 30 s for the next `pollDashboardCommand()`. Options, cheapest first:

1. **Long-poll** — `GET /api/command` holds the request open up to ~25 s and
   returns the instant a command queues. Firmware-side this is nearly free *after*
   N1 step 5 (a dedicated network task can block harmlessly; today it would stall
   the whole loop). Flask-side: one held connection per device — fine for one
   ESP32; note gunicorn sync workers each hold one slot, so bump workers or use
   gevent. Interim before N1: R11 (drain whole queue per tick) reduces the pain.
2. **ESP32 subscribes to a Railway SSE stream** — `GET /api/command/stream` with
   the same `EventSource`-style protocol the firmware already speaks to the Hue
   bridge. Reuses the existing SSE client machinery (second `WiFiClientSecure`,
   second buffer). Use Postgres `LISTEN/NOTIFY` as the Flask fan-out — the same
   plumbing D14 needs, so build it once.

Recommendation: long-poll first (smaller), upgrade to the shared SSE fan-out when
D13–15 lands. Skip MQTT — it solves this too but adds a broker you don't need.

---

## N3 — Per-state override policy *(wind-down / wake catching manual changes)*

> CLAUDE.md note: "should the wind down and wake ramps catch soft pause-esque
> changes?"

**Yes — the machinery already supports it.** Ramp PUTs call `noteRecentPut()` with
30 s `dynamics`, so trajectory slots stay live through WAKE/WIND_DOWN and echoes
match fine. The *only* reason those states are blind is the `SkipState` early-out
in `handleLightUpdate()`. The real design question is the **response**, which
should differ per state. Replace the binary skip with a policy lookup:

```cpp
enum class OverridePolicy : uint8_t { Ignore, SoftPause, CancelToNormal };

static OverridePolicy policyFor(State s) {
    switch (s) {
        case State::NORMAL:    return OverridePolicy::SoftPause;       // today's behavior
        case State::WIND_DOWN: return OverridePolicy::SoftPause;       // see note below
        case State::WAKE:      return OverridePolicy::SoftPause;
        default:               return OverridePolicy::Ignore;          // LOCKED_OUT, SOFT_PAUSE, HARD_OFF
    }
}
```

**The WIND_DOWN design tension, honestly:**

- **SoftPause** — user keeps their exact chosen level for an hour. Most respectful
  of the manual change ("I brightened because I'm not going to bed yet"). Reset
  `stableLuxCount` on entry from WIND_DOWN so the counter rebuilds a full ~30 min
  before wind-down can re-trigger after resume.
- **CancelToNormal** — resume curve-following immediately. Feels right ("not
  bedtime → normal mode"), *but* `tickNormal` will PUT the curve target on the next
  tick, overwriting the user's exact level within 30 s — the system fights the
  hand that just adjusted it. Only acceptable if the curve target ≈ what the user
  wanted anyway.

Recommendation: **SoftPause for all three driving states** (consistent, never
fights the user) + the `stableLuxCount` reset. Keep `CancelToNormal` in the enum
as a tuning option. `SkipPauseResume` behavior is unchanged. If a `CancelToNormal`
path is ever used, it must do the full NORMAL re-entry sync (`overheadsOn` /
`chestOn` / `lastFloorOn` from cache, `sentTarget` sentinel).

Effort: small — one enum, one function, one changed branch in `handleLightUpdate()`.

---

## N4 — Serial-like console page *(without melting the system)*

> CLAUDE.md note: "create section/page for serial-like output? would that be too
> intense and slow the whole system down substantially?"

Streaming every `Serial.print` over HTTPS would, yes. Three bounded designs:

1. **`LOG_VERBOSE` timed mirror** *(do this now — ~20 lines)*. A dashboard command
   arms a flag for N minutes; while armed, lines that today go only to Serial are
   also buffered and shipped via the existing log path (batched per tick, not
   per-line — pairs with R1's log buffer). Auto-disarms. Cost is bounded and
   opt-in.
2. **RAM ring + armed delta shipping** *(do with D13)*. Keep a ~200-line ring of
   fixed `char[96]` buffers in firmware. While the console page is open (armed via
   command flag), each 30 s status POST carries only the new lines since last
   ship. Dashboard renders a terminal-style pane. Zero cost when the page is
   closed.
3. **Local-only WebSerial** — `ESPAsyncWebServer` + WebSocket serving a live
   serial page on the LAN. True real-time, zero Railway load, but only works at
   home and adds a library. Optional; nice during heavy debugging.

---

## N5 — Ticks aligned to :00 / :30

> **SHIPPED (July 14, 2026, `4b10a9a` — rode along N1 Stage 2.)** `ALIGNED_TICKS`
> in config.h, commentable as specced. One deviation from the sketch below: a
> computed slot under 5 s means the just-fired tick was the boundary tick, so
> the scheduler skips to the *following* boundary — the naive remainder-slot
> version double-fired the same boundary (duplicate-tick bug, found+fixed on
> first flash; see N1_MIGRATION.md Stage 2 hardware finding).

> CLAUDE.md note: "change it so that polls happen on :00 and :30, instead of
> whenever the system reflashes. make it commentable so that it can be ignored
> for testing."

Trivial with NTP in hand. Compute the wait target instead of a fixed 30 000 ms:

```cpp
#define ALIGNED_TICKS 1   // comment out for testing (reverts to flash-relative 30 s)

// at the bottom of loop(), replacing the fixed-interval wait:
#if ALIGNED_TICKS
    unsigned long secsIntoSlot = timeClient.getEpochTime() % 30UL;
    unsigned long waitMs = (30UL - secsIntoSlot) * 1000UL;   // ±1 s — NTPClient is second-granular
#else
    unsigned long waitMs = 30000UL;
#endif
    while (millis() - tickStart < waitMs) { /* existing wait-loop body */ }
```

Notes: NTPClient only gives whole seconds, so alignment is ±1 s — fine. After R8
(SNTP), `gettimeofday()` gives microseconds and alignment becomes exact. One
gotcha to log-and-ignore: an NTP resync can produce a single short/long tick.
Free side benefits: timeline points land on clean boundaries, and any future
multi-device setup (F21 in `SSE_BRAINSTORM.md`) gets phase alignment for free.

---

## N6 — Recruiter role + magic links

> CLAUDE.md note: "add a 'recruiter' role … in between demo and owner permissions,
> no hidden owner IP button allowed for recruiter role"

Straightforward third role, with one upgrade: **tokenized magic links** instead of
a shared recruiter password.

- **Schema:** `recruiter_tokens(token TEXT PK, label TEXT, created TIMESTAMPTZ,
  expires TIMESTAMPTZ, views INT DEFAULT 0)`.
- **Route:** `GET /r/<token>` → validate + not expired → set session role
  `recruiter`, increment `views`, redirect to `/`.
- **Permissions matrix:** GET everything including `/lux` and
  `/api/lux/history` (the read-only view of the most impressive page is the
  point); **no** POST endpoints (`/api/command`, settings); **no** hidden owner
  IP button; no token management UI.
- **Owner UI:** a small token panel — create (label: "Acme recruiter"), expiry
  picker, per-token view count ("opened 3 times"), revoke.
- **Banner:** recruiter sessions get a slim banner explaining what they're looking
  at, with links to the GitHub repo and resume/portfolio.

Pairs with P1 (public landing-page widget) for the full portfolio funnel:
public widget → magic link → full read-only dashboard.

---

# R. Architecture & Implementation Reworks

## R1 — One Railway round trip per tick

**Today:** each tick does `GET /api/command` + `POST /api/status` + zero-or-more
`POST /api/log` — three or more full TLS handshakes to Railway every 30 s, all
serialized inside the tick budget.

**Rework:** a single `POST /api/tick`:

- Request body: `{ "status": { …current status payload… }, "logs": [ "...", … ] }`
- Response body: `{ "commands": [ { "id": 7, "command": "WAKE", "value": 12 }, … ] }`

Firmware-side, `sendLog()` keeps its signature but appends to a small buffer
(fixed `char[N][120]` ring per R9) flushed with the tick. Urgent lines (override
fire) can set a flush-now flag if sub-tick delivery ever matters.

**ACK tradeoff:** commands delivered in the response are implicitly "picked up."
Either mark them delivered at response time (at-most-once — fine for state
commands, which the next status POST confirms anyway) or keep the explicit
`/ack` for them. Recommend dropping `/ack` and treating the next status POST's
`state` field as the confirmation — the dashboard already tracks pending
commands against status.

Effort: ~1 session across firmware + Flask. Cuts 2+ handshakes/tick and shrinks
tick processing time materially.

## R2 — Persistent / reused TLS connection to the bridge

Every `setLight()` constructs a fresh `WiFiClientSecure` → a full TLS handshake
per PUT. A 4-bulb transition tick spends easily 1 s+ on handshakes alone.

- Promote one `WiFiClientSecure bridgeClient` + one `HTTPClient bridgeHttp` to
  globals; `bridgeHttp.setReuse(true)`; `begin(bridgeClient, url)` per call.
- Handle the bridge closing idle keep-alive connections: on send failure,
  reconnect once and retry the PUT.
- Side effect (good): faster PUTs shrink the window during which echoes pile up
  at the SSE socket, making the `postedAtMs` refresh + tail `sseTick()` drain
  even more comfortable.

Effort: small. Measure before/after with a `millis()` delta log around the PUT.

## R3 — Actually use the pinned cert *(correctness fix — see Doc Drift #2)*

> **SHIPPED — revised (July 15, 2026, `6b478b6`).** The `setCACert()` approach
> below is unworkable on this stack: core 2.0.0 always enforces mbedtls
> hostname verification when a CA is set, we dial by IP, and the bridge cert's
> CN is the bridge ID → CN mismatch on every handshake (confirmed live).
> The same flaw had `ensureBridgeCert()`'s boot probe silently re-fetching the
> cert every boot. Shipped instead: `peerMatchesPinned()` byte-compares the
> peer cert to the NVS PEM post-handshake on the boot probe + SSE stream
> (nothing sent before the check); HTTPClient paths stay insecure-mode TLS
> (they transmit on connect) and inherit the boot-time identity check.
> Details: `N1_MIGRATION.md` Decisions Log.

`ensureBridgeCert()` builds the whole NVS cert lifecycle and then every live
connection ignores it. Fix:

- In `setLight()`, `setLightColor()`, `bootstrapLightStates()`, `sseConnect()`:
  replace `client.setInsecure()` with
  `if (_bridgeCertPem.length()) client.setCACert(_bridgeCertPem.c_str()); else client.setInsecure();`
  (the fallback covers a failed first fetch).
- `setInsecure()` remains only inside `_fetchAndStoreBridgeCert()` (bootstrap
  trust on first use — by design).
- Update the `CLAUDE.md` and `SSE.md` lines so the docs match reality again.

Effort: ~15 minutes + a regression pass (one full tick, one SSE event, one boot).

## R4 — `grouped_light` PUTs (Zigbee groupcast)

When all active bulbs share a target — `tickNormal`'s common path and every ramp
tick — one PUT to a Hue zone's `grouped_light` replaces four per-light PUTs.
Beyond the traffic cut, the bridge issues a Zigbee **groupcast**, so bulbs
transition visually simultaneously instead of staggered.

- Create zones in the Hue app matching the bands (e.g., `Mira-core` =
  floor+dresser, `Mira-mid` = +chest, `Mira-all` = +overheads); record
  `grouped_light` UUIDs in `secrets.h`.
- `setGroup(groupUuid, on, bri, ct, durationMs)` used when every participating
  light shares the target; fall back to per-light PUTs on divergence ticks
  (edges, exclusions).
- **Echo discrimination survives:** call `noteRecentPut()` for *each member*
  of the group before the group PUT — individual light echoes still arrive and
  land on their known trajectories. The `grouped_light` event itself has an
  unknown UUID → `UnknownUuid`, harmless.

Effort: medium. Best after R5 (the role table knows group membership).

## R5 — `LightRole` table: kill the flag/comment sprawl

`CEIL_2` lives as a dozen commented-out lines; `chestOn`/`overheadsOn` are
parallel hand-rolled edge flags; `expectedOn` in `handleLightUpdate()` re-derives
band membership in a switch; the "PUT floor; if chestOn PUT chest; PUT dresser;
if overheadsOn PUT ceiling" block is triplicated (normal update, resume ramp,
wake ramp).

```cpp
struct LightRole {
    uint8_t     idx;
    const char* uuid;
    const char* name;
    float       joinLux;       // 0 = always-on core; 250 = chest band; 500 = overhead band
    bool        nightAnchor;   // floor lamp: survives wind-down at S4_FLOOR_BRI
    bool        participating; // CEIL_2 = false — no more commented-out call sites
    bool        intentOn;      // replaces chestOn / overheadsOn (firmware intent, edge accumulator)
};
LightRole roles[LIGHT_COUNT] = { … };
```

- `tickNormal`'s edge detection becomes one loop over rows with `joinLux > 0`.
- `expectedOn` = `roles[idx].joinLux == 0 || roles[idx].intentOn || excludedLight[idx]`.
- Extract `driveActiveLights(float bri, int ct, int durationMs)` — one loop,
  three call sites deleted. (Also becomes the single chokepoint F7 wants.)
- Wind-down/wake ordering derives from `joinLux` descending/ascending.
- Adding/removing/renaming a bulb = one table row.

Effort: ~1 session. Pure structure; no behavior change intended — diff the
Serial output of a simulated day (P6) or a live evening against pre-refactor.

## R6 — NVS-backed runtime config

Generalize the cert-in-NVS pattern: a versioned `RuntimeConfig` struct with
compiled defaults, NVS overrides, and dashboard editing.

- Members: curve constants (`S4_*`, `S3_*`, `S2_*`, `S1_*`, `CT_COOL/WARM`),
  `STATE_TOLERANCE_*`, `SOFT_PAUSE_MS`, `WAKE_RAMP_TICKS`, `PAUSE_RESUME_TICKS`,
  `LOCKOUT_RESET_MIN_OF_DAY`, `WIND_DOWN_GATE_HOUR`, stable-lux threshold/window, plus a `version` field
  (bump → fall back to compiled defaults on mismatch).
- `Preferences` namespace `"miracfg"`; load-or-default in `setup()`.
- Dashboard: owner-only settings page; `SET_CONFIG` command (bulk JSON);
  "reset to compiled defaults" button.
- **This is the landing pad** for E16's "apply suggested curve" button and the
  already-planned season-mode selector. Reflashing becomes a logic-change-only
  event.

Effort: ~1–2 sessions including the settings page.

## R7 — Single source of truth for the curve constants

The `lightcurve.h` constants are hand-duplicated in `index.html` and
`lux_curve.html` (a documented footgun: "keep the JS duplicates in sync").

- Preferred: firmware reports its live constants — `GET /api/curve` (served by
  the dashboard from the latest values firmware shipped at boot / on R6 config
  change). Dashboard JS keeps the segment *math* but pulls every constant live.
- Interim alternative: a tiny GitHub Actions step that code-gens `curve.js`
  from `lightcurve.h` on push.

Effort: small once R6 exists; the codegen variant is small standalone.

## R8 — Real timezone handling (SNTP + POSIX TZ)

A fixed `UTC_OFFSET_SEC` silently shifts `WIND_DOWN_GATE_HOUR` (wind-down eligibility)
and `LOCKOUT_RESET_MIN_OF_DAY` by an hour at every DST transition — twice a year, the
evening behavior moves. Plus the offset is currently documented inconsistently
(Doc Drift #1).

- Replace NTPClient + WiFiUDP with the ESP32 built-in:
  `configTzTime("CST6CDT,M3.2.0,M11.1.0", "pool.ntp.org");`
- Reads: `time(nullptr)` for epoch (cert expiry math unchanged),
  `localtime_r()` → `tm` for hour/day; rewrite `getTimeString()` /
  `printStatus()` on `tm` fields.
- Delete the NTPClient library dependency and `UTC_OFFSET_SEC` entirely.

Effort: small. The zero-maintenance goal demands it — this is the one bug class
that fires on a calendar schedule.

## R9 — String / heap hygiene in the hot paths

ESP32 + Arduino `String` churn is the classic slow killer of "zero-maintenance"
devices around day 30–60.

- `sseBuf += c` per byte is realloc churn on every event → fixed `char
  sseBuf[4096]` + `size_t sseLen`. The current silent `sseBuf = ""` reset at
  4096 also **drops a partial event with no log** — log it
  (`"SSE line >4096 dropped"`).
- Outgoing JSON bodies built by `String` concatenation in `setLight*()` →
  `snprintf` into a stack `char[256]`.
- `sendLog()` buffering (R1) uses fixed `char[N][120]` slots, not `String`s.
- Ship heap health in status (see P3): `esp_get_free_heap_size()`,
  `esp_get_minimum_free_heap_size()` — a slowly sinking min-free line is the
  early warning.

Effort: small-medium, mechanical.

## R10 — Soft-pause resume seeds from stale state *(likely visible bug)*

> **SHIPPED (July 15, 2026, `69aa744`)** — seeded from the floor-lamp cache
> exactly as specced below (floor as room proxy; sentTarget fallback when the
> floor is off/unpopulated).

`tickSoftPause()` sets `pauseResumeStartTarget = sentTarget` — the **pre-pause**
target. If the user dimmed to 20 % during the pause, the first resume tick
interpolates from ~the old level: the lights **snap toward pre-pause brightness,
then "fade."** The intended behavior (and what the docs describe) is a fade
*from where the user left the lights*.

Fix — seed from the cache:

```cpp
LightState f = cachedLight(LIGHT_FLOOR);   // floor as room proxy (per-light later via R5)
pauseResumeStartTarget = {
    (f.on && f.bri > 0.0f) ? f.bri : sentTarget.bri,
    (uint16_t)((f.ct > 0)  ? f.ct  : sentTarget.ct)
};
```

Apply the same seeding to any other resume path that interpolates from
`sentTarget`. Effort: minutes. Verify by pausing, dimming hard in the Hue app,
and letting the pause expire.

## R11 — Drain the whole command queue per tick *(interim until N2)*

> **SHIPPED (July 15, 2026, `69aa744`)** — bounded loop of 8 as specced; bails
> without re-polling when a command arrives with no id (un-ackable → would
> refetch forever).

`pollDashboardCommand()` processes exactly one command, so three quick dashboard
actions take 90 s to apply. Wrap the body in a bounded loop:

```cpp
for (int n = 0; n < 8; n++) {
    // existing GET → if "command" null: break → apply → ack
}
```

Effort: minutes. Superseded by N2 long-poll but worth doing today.

---

# F. New Behavior Features

## F1 — Lux oversampling + variance → media detection

The VEML7700 is read once per 30 s tick — a single point-in-time sample that
clouds, shadows, or a passing person can corrupt, and that a flickering TV fools
completely.

- **Oversample:** read lux every ~1–2 s inside the wait loop into a small
  `float ring[32]`; per tick use the **median** (kills transients) and compute
  stdev / coefficient of variation. Caveat to verify: `VEML_LUX_AUTO` re-ranges
  and a read can take ~100 ms+ — sample at 2 s spacing if 1 s is heavy.
- **Movie-night fix (real lived-in bug):** rapidly fluctuating *low* lux at
  night is screen content. Today that *feeds* `stableLuxCount` and Mira dims the
  floor lamp 30 minutes into a film. Gate the counter:
  `if (cv > MOVIE_CV_THRESHOLD) skip the increment` (and optionally skip curve
  PUTs that tick so the lights don't chase the screen).
- Ship `lux_cv` in the status payload — the timeline page can shade "media
  detected" spans.

Effort: small. Constants → R6.

## F2 — Solar-aware evening gates *(closes the seasonal blind spot)*

Mira's stated design goal is seasonal adaptivity, but `WIND_DOWN_GATE_HOUR 19`
(wind-down eligibility in `tickNormal`; was a hard-coded 21 until July 15, 2026)
and `LOCKOUT_RESET_MIN_OF_DAY 1230` (was `LOCKOUT_RESET_HOUR 21`) are the
hard-coded exceptions. In December, civil dusk in San Antonio is ~5:40 PM — the room sits
in the dark band for 3+ hours before wind-down is even *eligible*; in June, dusk
is ~8:40 PM and 21:00 is about right.

- A ~30-line NOAA-style solar position calculation (lat/long in `secrets.h` +
  the clock; pure math, offline) computed once daily after NTP sync →
  `duskMinutesLocal`.
- Eligibility becomes
  `nowMinutes >= max(duskMinutes + DUSK_OFFSET_MIN, WINDDOWN_FLOOR_MINUTES)` —
  the configurable floor preserves "never before X" if wanted. Same treatment
  for the lockout re-arm hour.
- Dashboard context line for free: "dusk 8:43 PM".

Effort: small. Pairs naturally with R8 (do the time rework first).

## F3 — First-class "nudge layer" *(UX + the best E16 dataset)*

Several ideas — Tap Dial rotary offset (B5), a reading boost, a movie preset —
are all the same mechanism: a **post-curve adjustment with a TTL**.

- Globals: `float nudgeBri` (signed %), `int nudgeCt` (signed mirek),
  `unsigned long nudgeExpiresMs`.
- Applied in exactly one place, after `luxToTarget()` (the R5
  `driveActiveLights()` chokepoint): `target.bri = constrain(target.bri +
  nudgeBri, 1.0f, 100.0f);` etc.
- Cleared on expiry (default ~90 min), on leaving NORMAL, and optionally when
  lux moves far enough that the context changed.
- Sources: dashboard slider (`SET_NUDGE` command), Tap Dial rotary later,
  button combos later.
- **E16 alignment:** every applied nudge logs
  `(lux, hour, curve_bri, curve_bri + nudge, source)`. This is *richer*
  preference data than overrides — the user expresses "a bit dimmer here"
  without tripping SOFT_PAUSE and detaching from the curve — and by
  construction it lives inside the protected 3–30 % band the echo architecture
  was designed to preserve.

Effort: small firmware + a dashboard slider. High daily-feel payoff.

## F4 — Self-light calibration routine *(experimental; writeup gold)*

The sensor reads ambient **plus Mira's own bulbs** — the feedback loop the curve
currently absorbs implicitly. Characterize it explicitly:

- Dashboard `CALIBRATE` command, guarded: only at night with `lux < ~5`
  baseline, blinds closed. Record baseline; for each band config (core /
  +chest / +all) sweep bri 0→100 % in 10 % steps; PUT, wait ~5 s to settle,
  record median lux; restore previous state.
- Result: `selfLux[band][briStep]` table → NVS. Runtime option:
  `ambientEstimate = measuredLux − interp(selfLux[currentBand][currentBri])`,
  making the curve *daylight*-driven instead of total-light-driven.
- Abort if the baseline shifts mid-run (> ~2 lux — car headlights, someone
  opening the door).
- Even if the runtime subtraction never ships, the measured optical-feedback
  characterization is an exceptional ECEN 310 / portfolio artifact.

Effort: medium. Mark experimental.

## F5 — `WAKE_SUNRISE`: simulated dawn from the phone alarm

The planned webhook work enables it: phone alarm → iOS Shortcuts automation →
`POST /webhook/wake` → command queue → ramp starts **in full darkness**, before
real light exists.

- New variant alongside the existing lux-driven wake: `triggerWakeSunrise()`
  sets `wakeStartTarget = {S4_FLOOR_BRI, CT_WARM}` and
  `wakeEndTarget = {SUNRISE_TARGET_BRI, SUNRISE_TARGET_CT}` (config, e.g.
  75 % / 300 mirek) — *not* `luxToTarget(ambientLux)`, which would target the
  night floor at lux≈0.
- Fires from LOCKED_OUT directly (no floor-lamp edge required).
- **Open design question:** on completion, handing to NORMAL in a still-dark
  room lets the curve immediately dim back down. Likely wants a short
  "sunrise hold" (hold target until lux > X or T minutes elapse) before NORMAL
  takes over.

Effort: small firmware + the webhook endpoint (extends the `ESP32_API_KEY`
bearer pattern per the existing open-design note).

## F6 — Exploit the Signe's gradient

The floor lamp is a *gradient* lamp being driven as a single white point.

- **Gradient night anchor:** wind-down's final state sets `gradient.points`
  (Signe takes 3–5 points) — deep warm at the bottom fading upward. Note the
  v2 gradient API takes *colors* per point, not per-point brightness; "dark at
  top" is approximated with a very deep low-luminance xy. Overall `dimming`
  stays global at `S4_FLOOR_BRI`.
- **Sunrise sweep:** during early WAKE ticks, animate the gradient bottom→top
  like a rising sun before the other lamps join.
- **Echo safety:** gradient-only SSE events carry no `on`/`dimming`/
  `color_temperature` fields → they already land in `NoFields` and are
  harmless. Our gradient PUTs include `dimming`, which the existing trajectory
  covers.
- Worth 20 minutes first: check whether these bulbs expose the native
  `timed_effects: sunrise` — probably stay with firmware-owned ramps (the
  dashboard slider seek depends on them), but know what the bridge offers.

Effort: small-medium. Pure delight feature; visually the best demo in the house.

## F7 — Circadian CT clamp

CT derives purely from brightness, so a bright lamp at 10 PM can still sit
coolish. Add an hour-based mirek floor at the single post-curve chokepoint
(R5's `driveActiveLights()`):

```cpp
int h = localHour();
if (h >= 20 || h < 6) target.ct = max((int)target.ct, 350);  // never cooler than ~2850K at night
```

Constants → R6. Three lines; f.lux for the room, independent of the brightness
curve.

## F8 — AWAY / hold mode

A mode distinct from HARD_OFF: lights fully manual, but status/logging stay
live — for travel or a houseguest who shouldn't be ambushed by a 20-minute
sunrise because they touched the floor lamp.

- Recommend a **flag, not a 7th state**: `bool holdMode`, persisted in NVS so
  it survives reboots. Effects: `checkFloorState()` skips the wake trigger;
  `stableLuxCount` frozen; driving PUTs suppressed (equivalent to an indefinite
  soft pause with no resume ramp).
- A 7th state would touch the cycle button's `% 6`, `forceState()`, and every
  state list — the flag is the smaller honest design. Revisit if it grows
  behaviors.
- Dashboard: toggle + status pill; cleared manually only.

Effort: small.

---

# P. Dashboard, Portfolio, Ops & Tooling

## P1 — Portfolio restructure + public live-status widget *(the CLAUDE.md PRIORITY)*

> CLAUDE.md note: "rework files to accommodate dashboard being integrated into
> main 'portfolio' page — railway will become the landing page, with /mira and
> /tempproject coming from that"

- **Flask blueprints:** `landing` at `/`, `mira` blueprint under
  `url_prefix="/mira"`, future `/envmon` for the Environmental Monitoring
  rework. Shared base template/theme; auth scoped per blueprint. Templates and
  static files move under per-blueprint folders.
- **Public widget:** unauthenticated `GET /api/public/status` →
  `{state, lux_coarse, updated_ago_s}` — server-side cached ~60 s,
  rate-limited, lux rounded (e.g., nearest 50) and optionally delayed ~5 min.
  *Privacy note, honestly:* fine-grained live lux is an occupancy signal for
  anyone watching; coarse + delayed keeps the demo value without broadcasting
  presence.
- Embed a small live card on the landing page in the MIRA aesthetic — a
  recruiter lands on the portfolio and sees a real embedded system **running
  right now**. Funnel: public widget → N6 magic link → full read-only dashboard.

Effort: the restructure is ~1 session; the widget is small on top.

## P2 — Push notifications via ntfy.sh

The concrete easy path for `SSE_BRAINSTORM.md` H27: free, no account —
`requests.post(f"https://ntfy.sh/{NTFY_TOPIC}", data=msg)` from Flask.

- Add a `kind` field to `/api/log` POSTs (`"override"`, `"winddown_complete"`,
  `"boot"`, …) so Flask triggers on structure, not string matching.
- Notify on: override fires, device offline > 5 min (a Railway cron checks
  last-status age), wind-down complete, boot-after-panic (P3).
- `NTFY_TOPIC` as a Railway env var; topic name is the only secret — pick an
  unguessable one.

Effort: tiny.

## P3 — Reliability telemetry *(prove the zero-maintenance claim)*

- Status payload additions: `rssi: WiFi.RSSI()`, `heap_free`, `heap_min`
  (`esp_get_minimum_free_heap_size()`), `sse_reconnects` (counter since boot),
  `uptime_s`.
- First POST after boot includes `reset_reason` mapped from
  `esp_reset_reason()` → "power-on" vs "panic" vs "task WDT" vs "brownout".
- Enable the task watchdog on the loop task (`esp_task_wdt_init` + add) so a
  wedged HTTP call reboots instead of hanging forever.
- Dashboard: a health panel — last seen, uptime, reset reason pill, RSSI and
  heap sparklines (two new snapshot columns + migration). A slowly sinking
  `heap_min` line is the day-40 fragmentation early warning R9 guards against;
  an RSSI sparkline turns "the bridge connection hiccuped" into a graph.

Effort: small firmware, ~1 dashboard session.

## P4 — OTA updates from the CI pipeline

- `FIRMWARE_VERSION` constant reported in status.
- GitHub Actions: on tag → `pio run` → attach the `.bin` to a GitHub release.
- Dashboard compares the latest release tag vs the reported version → "Update
  available" → queues an `OTA` command carrying the URL.
- Firmware: `esp_https_ota` (or `Update.h` streaming) into the inactive OTA
  slot; mark-valid-or-rollback via `esp_ota_mark_app_valid_cancel_rollback()`
  after the first successful status POST on the new image.
- **Verify first:** the Nano ESP32 partition scheme includes `ota_0/ota_1`
  (16 MB flash — if the default app-only table is active, set
  `board_build.partitions` in `platformio.ini`). GitHub asset URLs 302-redirect
  — follow redirects or mirror the `.bin` on Railway.

Effort: medium. CI/CD to a microcontroller — convenience *and* an exceptional
portfolio line.

## P5 — Postgres retention rollup + CSV export

2,880 snapshot rows/day grows unbounded and will eventually make `/lux` crawl.

- Nightly job (Railway cron service or an APScheduler thread in Flask): rows
  older than 30 days → aggregate into `status_5min` (avg/min/max lux + bri,
  modal ct), then delete the raws. Timeline queries pick the table by range.
- `GET /api/export.csv?start&end` (owner-only, streamed) — also the data feed
  for P6's replay.

Effort: small-medium.

## P6 — Host-native simulator + lux-day replay *(rigor evidence for ECEN 310)*

- PlatformIO `[env:native]`. `lightcurve.h` is already pure ✓; the work is
  extracting the state machine into `core.{h,cpp}` operating on thin injected
  seams: `IClock`, `ILuxSource`, `IBridge` (put), `ILogger`. `main.cpp` keeps
  trivial adapters wrapping the real hardware.
- Tests: a scripted fake day runs in milliseconds with assertions on the
  transition sequence (wake → normal → wind-down → locked-out), edge cases
  (movie-night counter, override policies, lockout re-arm).
- **The killer version:** replay *actual recorded lux days* (P5 CSV export)
  through candidate curve constants before flashing — "what would the new
  curve have done on May 3rd" as a tuner overlay.
- Also the cleanest possible answer to "sufficient rigor": a unit-tested
  embedded state machine.

Effort: the seam extraction is ~1–2 sessions; tests grow incrementally.

## P7 — `DUMP_DIAG` on-demand diagnostics command

`dumpOverrideDiagnostic()` only fires on Override. Factor its formatter out and
add a `DUMP_DIAG` dashboard command that ships the full `sseEventRing`, outcome
histogram, active trajectory slots, heap, RSSI, and uptime to the event log on
demand — remote debugging without waiting for a misfire. Effort: tiny.

## P8 — Energy analytics

Pure dashboard work from data already stored: a per-fixture watts model
(`standby + (max − standby) × bri/100`; look up real max draw — Hue E26
White & Color ≈ 9 W, Signe gradient floor is substantially higher — verify from
the spec sheets) integrated over 30 s snapshots → Wh/day chart, lights-on
hours, and a CT histogram ("your evenings average ~2700 K"). Effort: small.

---

# ROI Ranking & Suggested Sequencing

1. **N1 + N2** *(with R11 today as the interim)* — the responsiveness unlock;
   answers the stated architecture question and subsumes G22/G23 + async-SSE.
2. **Network batch: R1 + R2 + R3** — the biggest per-tick latency cut for
   roughly an afternoon, and R3 is a correctness fix.
3. **R10** — likely user-visible bug; minutes to fix.
4. **Time batch: R8 + F2** *(N5 rides along)* — kills the DST bug class and the
   last seasonal blind spot.
5. **F3 (nudge layer)** — daily-feel improvement *and* manufactures the best
   E16 dataset.

Then: the **structure cluster R5 → R6 → R7** (role table, runtime config,
curve single-source); **F1** (movie-night fix); **N3** (per-state override
policy — small once decided); **P3 + P2** (telemetry + notifications);
**portfolio batch P1 + N6**; **tooling P4 + P6**. Opportunistic: F4–F8, R4,
P5, P7, P8.

## Pairings / Dependency Map

| Item | Pairs with / depends on |
|---|---|
| N1 | subsumes G22, G23, awkward-structure (a); enables N2 long-poll |
| N2 | R11 interim; D13–15 share the Flask SSE fan-out |
| R1 | evolves into N2's long-poll endpoint; uses R9's log buffer |
| R5 | enables R4 group membership, F7 chokepoint, N3 `expectedOn` lookup |
| R6 | landing pad for E16 apply-path + season selector; feeds R7 |
| F2 | do R8 first |
| F3 | feeds E16 (protected 3–30 % band); Tap Dial B5 rotary is a source |
| P5 | data source for P6 replay |
| P3 | event source for P2 notifications |
| P1 | funnel into N6 magic links |
