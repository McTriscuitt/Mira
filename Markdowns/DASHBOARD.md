# DASHBOARD.md — Mira Web Dashboard

Flask + HTML/CSS/JS frontend, live on Railway (Hobby plan, PostgreSQL). URL in `secrets.h` as `DASHBOARD_BASE_URL`. The ESP32's Core-0 `netTask` polls `/api/command` every 5 s and POSTs a status snapshot every 30 s tick plus ~100 ms after applying any command (N1 Stage 5). Browsers get server-pushed updates over `/api/status/stream` SSE with a 30 s poll as fallback (N2 step 2). Served by gunicorn `-w 1 --threads 16` — single process (the SSE fan-out is in-process state), threaded so held stream connections don't block other requests.

---

## User Notes
---

## Tech Stack

| Layer | Choice | Notes |
|-------|--------|-------|
| Backend | Flask (Python) | Lightweight, Railway deploy |
| Frontend | HTML / CSS / JS | No framework — keep it lean |
| Database | PostgreSQL (Railway service) | StatusSnapshots, Commands, EventLog tables |
| Hosting | Railway (Hobby plan) | |
| ESP32 comms | HTTP REST, firmware-polled | `netTask` POSTs status (every tick + on command apply) + polls commands every 5 s; dashboard is passive server toward the ESP32 |
| Browser comms | SSE push + poll fallback | `/api/status/stream` (`EventSource`) pushes status frames + log pokes; 30 s polls remain as fallback; post-command burst polling (2 s, 8–20 s window) covers stream-down gaps |

---

## Design Philosophy

- **Clean and minimalistic** — no clutter, no unnecessary chrome
- **Dark mode first** — dark backgrounds, light text, subtle accents
- **Hidden scrollbars** — scroll works but no visible gutter
- **System fonts** — `system-ui, sans-serif`
- **Apple-level polish** — restrained color, smooth transitions, nothing busy or decorative
- Reference `mira_lux_curve_tuner_v5.html` for spacing, panel styling, and color palette

---

## Implemented

| Feature | Notes |
|---------|-------|
| User auth / login | Session-based, 90-day cookie. Two roles: `owner` (full access, `DASHBOARD_PASSWORD`) and `demo` (read-only, `DEMO_PASSWORD` env var — optional). `/lux` and `/api/lux/history` are owner-only (demo cannot access). |
| Live status display | Lux, bri (+ %), ct, overhead on/off, last seen — pushed near-instantly via `/api/status/stream` SSE; `/api/status/latest` polled every 30 s as fallback (N2 step 2) |
| Burst polling (N2 step 1) | After any command, `startStatusBurst()` polls status every 2 s until the snapshot confirms (`_pendingCommand` clears), 8 s floor (covers fire-and-forget chip toggles) / 20 s cap; refetches the log once on burst end |
| State control buttons | Normal, Soft Pause, Wind Down, Wake, Hard Off — send pending commands; active button stays highlighted until server confirms command gone; button text/border darkens dynamically as ambient lux increases past 1000 to maintain contrast against the light cone; Hard Off requires two-click confirm (first click arms "confirm?" for 3 s, second click sends) |
| Pending command tracking | `pending_command_id` in status response lets client preserve pending state across polls without false clears |
| Progress block | Shows current state name + detail; slider for Wake/Wind-down with seek; finish button jumps ramp to end |
| Live slider preview | Dragging the slider updates text live before committing |
| Event log (index) | Last 10 entries shown inline; "Event log" label is a clickable link to `/logs`; refetched on SSE `log` pokes, plus every 30 s as fallback |
| Event log page | Full paginated log at `/logs`; date separators; "load more" hides when DB is exhausted; filter buttons: all / normal / wake / wind down / soft pause / hard off — normal uses exclusion filter (strips state-transition events, shows routine operation logs) |
| Command queue | `commands` table; firmware GETs oldest pending, ACKs after execution; dashboard can cancel before pickup |
| Mobile hover fix | All `:hover` rules wrapped in `@media (hover: hover)` — no sticky-tap on touch devices |
| Lux curve widget (index) | 50%-wide canvas centered below the MIRA logo; hidden from demo mode; "Lux curve" label links to `/lux`; draws the bri/ct curve with a dot at the current lux reading; dot is hollow when state is LOCKED_OUT or HARD_OFF |
| Lux curve page (`/lux`) | Owner-only. Two views toggled by buttons: **Curve** — full bri/ct vs. lux chart with axis labels, grid, segment discontinuity marker at 200 lux (overhead threshold), and interactive dot showing current bri/ct/lux in the stats panel. **Timeline** — full local-day lux history (midnight-to-midnight, fixed via UTC-boundary query so no 8pm-to-8pm bleed); date navigation; slider + canvas drag-scrub (click or drag anywhere on canvas to scrub); dynamic y-axis cap rounds to nearest 100 above day max if data exceeds 3000 lux; stats panel shows lux/bri/ct for selected point; "live" badge on the latest point when viewing today; auto-polls every 30 s on today's date |

---

## Planned / Not Yet Built

| Feature | Notes |
|---------|-------|
| Season mode selector | Pick season or specific light curve; brighter curves for short-day seasons |
| Google Calendar integration | Read end time from sleep calendar; schedule wake ramp start — OAuth via Flask backend |

---

## Control Parity

Physical button actions mirror dashboard controls:

| Action | Button | Dashboard |
|--------|--------|-----------|
| NORMAL / pause toggle | BTN_MODE short press | Normal / Soft Pause buttons |
| Hard off | BTN_MODE long press | Hard Off button |
| Cycle states | BTN_CYCLE short press | Individual state buttons |

---

## ESP32 API Endpoints

All ESP32 calls use `Authorization: Bearer <ESP32_API_KEY>`.

| Method | Path | Purpose |
|--------|------|---------|
| POST | `/api/status` | Ingest status snapshot each tick |
| POST | `/api/log` | Ingest log message |
| GET | `/api/command` | Poll oldest pending command |
| POST | `/api/command/<id>/ack` | Mark command executed |

Dashboard-only (session auth):

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/api/status/latest` | Latest snapshot + `pending_command_id` |
| GET | `/api/status/stream` | SSE: `status` events (same payload/redaction as `latest`, pushed on snapshot ingest and command queue/cancel/ack) + `log` poke events (client refetches `/api/log/recent`); 25 s keepalive pings |
| GET | `/api/log/recent` | Log entries — `?limit=N&search=<term>&exclude=<term1,term2>` |
| POST | `/api/command` | Queue new command |
| POST | `/api/command/<id>/cancel` | Cancel pending command |

Owner-only:

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/lux` | Lux curve page (redirects to login if not owner) |
| GET | `/api/lux/history` | Lux history snapshots — preferred params: `start=<ISO>&end=<ISO>` (local-day UTC boundaries); legacy fallback: `date=YYYY-MM-DD` (UTC date, may bleed across local midnight) |

