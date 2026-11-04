---
name: crash-investigation-sep12
description: ESP32 crashes ~daily (morning-clustered) + occasional total wedges; WiFi watchdog + reset-reason instrumentation flashed Sept 12 — next crash self-identifies in the Online log
metadata: 
  node_type: memory
  type: project
  originSessionId: 52aa70d0-24d9-421a-80af-7a396ba65619
  modified: 2026-09-13T14:40:30.704Z
---

**Symptoms (user + telemetry, Sept 8–12, 2026):** two failure flavors on the N1-Stage-5
firmware. (1) ~Daily panic → auto-reboot in ~40 s, clustered mornings around the wake
window (Sept 9 9:11a, Sept 10 9:05a — 13 s after `Stale revert → Manual override → soft
pause`, Sept 11 8:36a — ate the wake-start log). (2) Total wedge — telemetry/logs/lights
all silent until power cycle (Sept 8 ~3 h, Sept 12 98 min starting 7 min after a fresh
boot). Diagnosed from Railway alone: reboots show as stack high-water-mark **increases**
in `/api/telemetry/history` (watermarks only shrink during a run). Heap flat ~8.5 MB,
stack floors ~6.6k/12k — NOT memory. Wedge flavor attributed to STA WiFi loss with no
recovery path (`WiFi.begin` ran once in setup; bridge + Railway share the link).

**Shipped Sept 12 (flashed ~11:56 AM, verified live — "Online (sw-restart)" on Railway):**
- `wifiWatchdog()` in `loop()` — escalation: 30 s trust auto-reconnect → manual
  disconnect+begin kick every 30 s → `ESP.restart()` at 5 min (constants `WIFI_CHECK_MS`
  / `WIFI_RETRY_MS` / `WIFI_REBOOT_MS` in config.h). RTC-noinit `bootMarker` marks
  watchdog restarts.
- `captureBootReason()` + `resetReasonStr()` — Online log is now
  `Online (<reason>) — <time>`: PANIC / TASK-WDT / BROWNOUT / power-on / sw-restart /
  wifi-watchdog. **Next morning crash names itself.**

**Watchdog LIVE-VALIDATED Sept 13, 9:39 AM:** real STA drop; auto-reconnect failed for
the full 30 s grace; the first manual disconnect+begin kick recovered it in 2 s (32 s
total outage, SSE + PUTs back within 37 s). Confirms the stuck-STA theory — under old
firmware this exact drop would have been an indefinite wedge. Note the drop landed in
the same ~9 AM window as the historical crashes: morning AP/router turbulence may be a
common environmental trigger. First clean 25 h run (full wind-down → wake cycle, zero
reboots) also logged Sept 12→13.

**Open leads for the panic flavor:** Sept 10 crash fired seconds after the SSE
override→soft-pause path (stale revert + `dumpOverrideDiagnostic()` + SOFT_PAUSE entry);
Sept 11's consumed the wake trigger. `dataMux` locking looked disciplined on review.
Root cause NOT yet found — wait for a `(PANIC)`/`(TASK-WDT)`/`(BROWNOUT)` Online line,
then correlate with the serial capture if the laptop was tethered.

**Separate finding:** the user's 9:27 AM Sept 12 restart killed a healthy board — the
dashboard *looked* frozen while status POSTs flowed. N2 step 2 (Flask→browser SSE) was
never live-verified on Railway; verify/fix it and add a "last update Xs ago" staleness
indicator so a stale page is distinguishable from a dead device. Related:
[[n1-migration-in-progress]], [[Dashboard Credentials]].

**How to apply:** when the user reports the next crash, pull
`/api/log/recent` (owner login, see [[Dashboard Credentials]] — password updated Sept 12)
and read the latest `Online (<reason>)` line first; reason string decides the next step
(BROWNOUT → power supply, PANIC/WDT → need backtrace via serial, wifi-watchdog →
watchdog working as designed).
