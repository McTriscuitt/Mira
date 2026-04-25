# DASHBOARD.md — Mira Web Dashboard

Flask + HTML/CSS/JS frontend, hosted on Railway. Replaces Discord webhook for monitoring and adds remote control. To be built after firmware is stable.

---

## User Notes

- Splash screen could just be "Mira"
- Button toggles for dashboard. One button in at a time
- user authentication/login

---

## Tech Stack

| Layer | Choice | Notes |
|-------|--------|-------|
| Backend | Flask (Python) | Lightweight, easy Railway deploy |
| Frontend | HTML / CSS / JS | No framework — keep it lean |
| Database | PostgreSQL (Railway service) | Keeps Flask↔DB link clean; easier data manipulation than Discord/ThingSpeak/SQLite |
| Hosting | Railway (Hobby plan) | |
| ESP32 comms | HTTP REST | ESP32 will expose a lightweight local server endpoint (TBD) |

---

## Design Philosophy

- **Clean and minimalistic** — no clutter, no unnecessary chrome
- **Dark mode first** — dark backgrounds, light text, subtle accents
- **Hidden scrollbars** — scroll works but no visible gutter (see `mira_lux_curve_tuner_v5.html` as style baseline)
- **System fonts** — `system-ui, sans-serif`
- **Apple-level polish** — restrained color, smooth transitions, nothing busy or decorative
- Reference the lux curve tuner for spacing, panel styling, and color palette

---

## Opening / Splash Screen

- Project name: **Mira**
- Ideas (to be decided):
  - Giant **M** across the screen, Netflix-logo style
  - Giant **M** as background with white fill in the negative space
- Goal: striking, premium feel — not a typical dashboard header

---

## Planned Features

| Feature | Notes |
|---------|-------|
| Live status display | Current state, lux reading, bri, ct — updated each poll |
| Lux curve graph | Integrate `mira_lux_curve_tuner_v5.html`; plot current poll position on the curve |
| Force NORMAL | Remote equivalent of BTN_MODE short press |
| Soft pause toggle | Remote trigger / cancel |
| Force wind-down | Manually start wind-down sequence |
| Force wake | Manually trigger wake ramp |
| Hard off toggle | Remote equivalent of BTN_MODE long press |
| DAY_TYPES config | Per-weekday WORK / RELAXED setting — replaces abandoned button-based config screen |
| Event log | Replace Discord webhook; show startup, bulb updates, state transitions |
| Season mode selector | Pick current season or specific light curve; slightly brighter curves for short-day seasons |
| Google Calendar integration *(potential)* | Read end time from a dedicated sleep calendar; schedule wake ramp to start before the event ends — e.g. ends 7:00 AM → ramp starts 6:40 AM (WORK) or 6:00 AM (RELAXED). Calendar OAuth handled by Flask backend. |

---

## Season Modes

- Different light curves or time restrictions per season
- Facilitates brighter room during seasons where daylight ends earlier
- Two implementation paths (not mutually exclusive):
  1. **Manual** — dashboard picker lets user select season or a specific light curve
  2. **Automatic** — use NTP month to infer season and switch curve/time restrictions automatically

---

## Control Parity

Physical button actions must mirror dashboard controls and vice versa:

| Action | Button | Dashboard |
|--------|--------|-----------|
| NORMAL / pause toggle | BTN_MODE short press | Soft pause toggle button |
| Hard off | BTN_MODE long press | Hard off toggle |
| Cycle states | BTN_CYCLE short press | Individual state force buttons |

---

## Notes

- DAY_TYPES changes from the dashboard do **not** need to persist to ESP32 flash — the dashboard is the source of truth and pushes config on reconnect
- Dashboard is the authoritative source for per-weekday WORK / RELAXED config
- ESP32 local server endpoint design is TBD (lightweight HTTP, no auth needed on LAN)
