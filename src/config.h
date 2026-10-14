#pragma once
//
// config.h — Mira 3.0 behavior and tuning constants (tracked, public).
//
// 3.0 change: this file no longer #includes secrets.h. The pure core
// (timeline.cpp / director.cpp) includes it and must also build on a host
// machine for the native tests, where no secrets exist. Files that need
// credentials (bridge.cpp, dashboard.cpp, main.cpp) include secrets.h directly.
//
// Behavior reference: Markdowns/MIRA_3.md.
//
#include <stdint.h>

// ── Clock ───────────────────────────────────────────────────────────────────
// US Central with automatic DST (2nd Sunday of March → 1st Sunday of November),
// POSIX TZ format. Replaces 2.x's fixed UTC_OFFSET_SEC, which drifted an hour
// at every DST change — fatal for a schedule-driven system.
#define MIRA_TZ             "CST6CDT,M3.2.0,M11.1.0"
#define NTP_SERVER_1        "pool.ntp.org"
#define NTP_SERVER_2        "time.nist.gov"
#define NTP_SYNC_TIMEOUT_MS 120000UL   // no first sync by then → restart and try again

// ── The day (local time) ────────────────────────────────────────────────────
// Seconds since local midnight. The awake day runs from the wake window
// (06:00) to lights-out (01:00 the next morning). MIRA_3.md §2 has the table.
#define HHMM(h, m) ((int32_t)(h) * 3600L + (int32_t)(m) * 60L)

#define WAKE_WINDOW_OPEN     HHMM(6, 0)    // a light on (or turned on) from here starts the wake
#define AUTO_WAKE            HHMM(9, 0)    // ...or this, whichever comes first
#define MIDDAY_FADE_START    HHMM(10, 0)   // ceiling on; 75 → 100%
#define MIDDAY_FADE_END      HHMM(11, 0)
#define AFTERNOON_FADE_START HHMM(17, 0)   // 100 → 75%
#define AFTERNOON_FADE_END   HHMM(18, 0)
#define CEILING_OFF_AT       HHMM(19, 0)
#define EVENING_FADE_START   HHMM(20, 0)   // 75 → 40%
#define EVENING_FADE_END     HHMM(21, 0)   // chest off
#define SLEEP_CUTOFF         HHMM(21, 0)   // everything off from here = asleep (before it = nap)
#define LATE_FADE_START      HHMM(23, 0)   // 40 → 25%
#define LATE_FADE_END        HHMM(23, 30)  // dresser off
#define LIGHTS_OUT_AT        HHMM(1, 0)    // next morning: whatever's still on goes off → asleep

#define CEILING_ON_AT   MIDDAY_FADE_START
#define CHEST_OFF_AT    EVENING_FADE_END
#define DRESSER_OFF_AT  LATE_FADE_END

// Brightness levels (percent — Hue v2 native units)
#define WAKE_START_BRI  25.0f    // where the wake starts — "the new night level", to be tested
#define MORNING_BRI     75.0f    // the wake's "immediate brightness cap", held until 10:00
#define DAY_BRI        100.0f
#define EVENING_BRI     75.0f
#define NIGHT_BRI       40.0f
#define LATE_BRI        25.0f

// Wake sequence (seconds after the trigger)
#define WAKE_FADE_S          900   // 15 min, WAKE_START_BRI → MORNING_BRI
#define WAKE_JOIN_DRESSER_S  300   // +5 min
#define WAKE_JOIN_CHEST_S    600   // +10 min (the ceiling joins here too on a late wake after 10:00)

// ── Color temperature ───────────────────────────────────────────────────────
// The 2.x brightness→CT map, unchanged: linear from CT_WARM at CT_WARM_BRI up to
// CT_COOL at 100%, clamped. 400 mirek = 2500 K, 280 mirek ≈ 3570 K. One command
// moves brightness and CT together, so CT tracks the map through every fade.
#define CT_COOL      280
#define CT_WARM      400
#define CT_WARM_BRI  19.7f

// ── Transitions ─────────────────────────────────────────────────────────────
#define FADE_MS       15000UL   // every light Mira turns on or off; steady-period catch-ups
#define JOIN_SNAP_MS    300UL   // a light you turn on is brought in line near-instantly
#define FREEZE_MS       400UL   // a hold stops in-progress fades where they are
#define MIN_FADE_MS    1000UL   // floor for any fade command's duration
// Longest single transition sent to a bulb. Hue's limit is believed to be ~109 min
// (hardware unknown #1); every scheduled fade is ≤ 60 min, so this never splits
// one today. If the bridge rejects long durations, lower this: longer fades then
// go out in legs, each landing before the next is sent.
#ifndef MAX_FADE_MS                 // (overridable — env:native_legs tests the legs path)
#define MAX_FADE_MS 3600000UL
#endif

// ── Your changes (MIRA_3.md §5–§6) ──────────────────────────────────────────
#define NAP_MS            3600000UL  // all off before SLEEP_CUTOFF → 60 min nap
#define OFF_GROUP_MS         3000UL  // offs this close together are one action (Hue-app room toggle)
#define ON_EDGE_GRACE_MS     3000UL  // brightness reports this soon after a turn-on belong to it
#define CLOBBER_WINDOW_MS    3000UL  // a Mira command this recent may land on top of your change

// ── Command bookkeeping ─────────────────────────────────────────────────────
#define ALIGN_BRI_TOL       0.5f     // a sent command already matches the plan within this...
#define ALIGN_END_TOL_MS    5000L    // ...and ends within this of the period's end
#define OFF_CONFIRM_MS     20000UL   // on/off not echoed by the bridge by then → re-send
#define RETRY_BASE_MS       5000UL   // failed-command backoff: 5 s, 10 s, 20 s, ... capped
#define RETRY_MAX_MS       60000UL
#define RESTORE_MAX_AGE_MS 600000L   // RTC state older than this at boot is ignored (inferred instead)

// ── Echo recognition (bridge.cpp) ───────────────────────────────────────────
// Every command Mira sends comes back over the bridge's event stream looking
// exactly like a change you made; these size the window that tells them apart.
#define TRAJECTORY_TOLERANCE_BRI 5.0f    // Zigbee/bulb quantization slop, percent
#define RECENT_PUT_GRACE_MS      5000UL  // post-duration slack for late echoes
// 3.0's fades run up to an hour, so "anywhere between start and end" would hide
// your changes for the whole fade. Only right after a command (the bridge echoes
// its target at once) is the whole range accepted; after that, only values near
// where the fade should be by now — the bridge's mid-fade progress reports, if it
// sends any (hardware unknown #2). Wider than the quantization slop in case the
// bulb's fade isn't perfectly linear in the reported percent.
#define ECHO_EARLY_MS            10000UL
#define ECHO_PROGRESS_TOL_BRI    8.0f
#define RECENT_PUT_RING_SIZE     3
#define STALE_REVERT_LOOKBACK_MS 90000UL // the bridge walks back an unapplied PUT ~35 s out
#define SSE_EVENT_RING_SIZE      16
// #define ECHO_TRACE 1                  // per-event Serial trace of the classifier

// Experimental — hardware unknown #4 in MIRA_3.md. Also send each fade to lights
// that are off (without turning them on), so a light switched on later comes up
// already in line — if the bulbs honor dimming while "soft off". Off until tested.
// #define PRESET_OFF_LIGHTS 1

// ── Telemetry + dashboard ───────────────────────────────────────────────────
#define TELEMETRY_TICK_S  30        // lux read + status snapshot, aligned to :00/:30
#define NET_CMD_POLL_MS   5000UL    // dashboard command poll (netTask)

// ── WiFi watchdog (Sept 12, 2026) ───────────────────────────────────────────
// The STA link occasionally drops and the core's auto-reconnect can wedge
// against some APs. Bridge PUTs and Railway both ride this one link, so a dead
// STA froze the whole system silently (observed Sept 8, 3 h; Sept 12, 98 min).
// Escalation ladder, checked every WIFI_CHECK_MS on the main loop:
//   0–WIFI_RETRY_MS down  → trust the core's auto-reconnect (brief blips self-heal)
//   past WIFI_RETRY_MS    → manual disconnect+begin kick, repeated each WIFI_RETRY_MS
//   past WIFI_REBOOT_MS   → ESP.restart() (RTC-marked so the boot log says "wifi-watchdog")
#define WIFI_CHECK_MS    2000UL
#define WIFI_RETRY_MS   30000UL
#define WIFI_REBOOT_MS 300000UL  // 5 min
