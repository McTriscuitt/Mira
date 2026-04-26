# DASHBOARD.md — Mira Web Dashboard

Flask + HTML/CSS/JS frontend, live on Railway (Hobby plan, PostgreSQL). URL in `config.h` as `DASHBOARD_BASE_URL`. The ESP32 polls the dashboard for commands and posts status on every 30 s tick.

---

## User Notes

- Splash screen could just be "Mira"

---

## Tech Stack

| Layer | Choice | Notes |
|-------|--------|-------|
| Backend | Flask (Python) | Lightweight, Railway deploy |
| Frontend | HTML / CSS / JS | No framework — keep it lean |
| Database | PostgreSQL (Railway service) | StatusSnapshots, Commands, EventLog tables |
| Hosting | Railway (Hobby plan) | |
| ESP32 comms | HTTP REST, firmware-polled | ESP32 POSTs status + polls commands each tick; dashboard is passive server |

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
| User auth / login | Session-based, 90-day cookie, password from env |
| Live status display | Lux, bri (+ %), ct, overhead on/off, last seen — polls `/api/status/latest` every 30 s |
| State control buttons | Normal, Soft Pause, Wind Down, Wake, Hard Off — send pending commands; active button stays highlighted until server confirms command gone; button text/border darkens dynamically as ambient lux increases past 1000 to maintain contrast against the light cone |
| Pending command tracking | `pending_command_id` in status response lets client preserve pending state across polls without false clears |
| Progress block | Shows current state name + detail; slider for Wake/Wind-down with seek; finish button jumps ramp to end |
| Live slider preview | Dragging the slider updates text live before committing |
| Event log (index) | Last 10 entries shown inline; "Event log" label is a clickable link to `/logs`; refreshed every 30 s |
| Event log page | Full paginated log at `/logs`; date separators; "load more" hides when DB is exhausted; filter buttons: all / normal / wake / wind down / soft pause / hard off — normal uses exclusion filter (strips state-transition events, shows routine operation logs) |
| Command queue | `commands` table; firmware GETs oldest pending, ACKs after execution; dashboard can cancel before pickup |
| Mobile hover fix | All `:hover` rules wrapped in `@media (hover: hover)` — no sticky-tap on touch devices |

---

## Planned / Not Yet Built

| Feature | Notes |
|---------|-------|
| Lux curve graph | Integrate `mira_lux_curve_tuner_v5.html`; plot current poll position on the curve |
| DAY_TYPES config | Per-weekday WORK / RELAXED setting — replaces abandoned button-based config screen |
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
| GET | `/api/log/recent` | Log entries — `?limit=N&search=<term>&exclude=<term1,term2>` |
| POST | `/api/command` | Queue new command |
| POST | `/api/command/<id>/cancel` | Cancel pending command |

---

## Notes

- DAY_TYPES changes from the dashboard do **not** need to persist to ESP32 flash — dashboard is source of truth, pushes config on reconnect
- Dashboard is authoritative for per-weekday WORK / RELAXED config
