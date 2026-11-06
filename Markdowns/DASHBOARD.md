# DASHBOARD.md: the site

> **Merged into `mira-3` and pushed Nov 3. Login works; the other live checks are still open.** See the checklist in
> `TERMINAL_MIRA_BUILD_LOG.md`.

Flask + plain HTML/CSS/JS on Railway (Hobby plan, PostgreSQL). `/` is a portfolio terminal, and Mira
is a directory inside it. The old dashboard lives on at `/legacy` for the admin. It was reworked once
for this move and may fall behind; the terminal is the maintained interface.

Gunicorn runs `-w 1 --threads 16`. **`-w 1` is load-bearing:** the stream's change notifier is
in-process. Each open stream holds a thread, the same threads the board's posts and polls use, so
streams are admin-only and capped at 4.

## Code map (`dashboard/`)

| File | What it holds |
|---|---|
| `app.py` | Config and startup. Gunicorn loads `app:app` |
| `models.py` | Tables and the additive schema updates that run at boot |
| `security.py` | Client IP, admin sessions, CSRF, rate limits, response headers |
| `broadcast.py` | Wakes open streams when the board posts |
| `board_api.py` | The firmware's routes. Changing these means a reflash |
| `auth_api.py` | Login, lockout, visitor logging, the `access` data |
| `mira_api.py` | Status, schedule, log, stream, commands, and working out what a command did |
| `schedule.py` | The site's copy of the schedule, and the check on the copy the board posts. A test compares it with `src/config.h` |
| `pages.py` | HTML pages |
| `static/js/terminal.js` | The terminal's engine and root commands |
| `static/js/terminal-admin.js` | `login`, `logout`, `access`, `legacy` |
| `static/js/mira.js` | Mira's directory |
| `static/js/portfolio-content.js` | The site's words and project list. **Edit this for content** |
| `static/js/legacy-*.js`, `static/css/legacy-*.css` | The old pages |
| `static/manifest.webmanifest`, `static/img/apple-touch-icon.png`, `icon-192.png`, `icon-512.png` | Home-screen install: name, full-screen mode, icons. The PNGs are renders of `static/img/app-icon.svg`: square, opaque, artwork inside the middle 80% so Android's masks can't clip it |
| `tests/` | `python -m pytest tests`; `python tests/e2e_terminal.py` drives a real browser |

`app.py` boots on SQLite now, so the tests need no Postgres. Set `MIRA_TEST_DATABASE_URL` to run them
on a local Postgres; they drop every table, so `conftest.py` refuses any database not on this computer.

## Environment variables

| Variable | Purpose |
|---|---|
| `SECRET_KEY`, `ADMIN_PASSWORD_HASH`, `ADMIN_TOTP_SECRET` | From `tools/make_admin_secrets.py`. Without the last two, `login` reports unavailable |
| `ESP32_API_KEY` | The board's bearer key |
| `DATABASE_URL` | Postgres |
| `MIRA_INSECURE_DEV=1` | Local tests only: drops the `Secure` cookie requirement. Never set it on Railway |

## Commands

**Everywhere:** `help`, `clear`, `cd`, `back`, `exit`, `pwd`, and the hidden `login`, `logout [all]`,
`access [logins|visits|commands]`, `legacy`.

**Admin help lists everything.** Logged in, `help` adds an "also yours, logged in" section with
every hidden and secret command that works where you are (`login` excepted). The engine
(`terminal.js` `printAdminHelp`) adds it after any directory's own help, so a new project gets it
for free; a directory lists the engine commands its own help already covers in `helpCovers`.

**At the top:** `mira`, `about`, `projects`, `contact`, `resume`, `privacy`. Also `ls`, `whoami`, `open`.

**Inside `mira`:**

What a light command did is read from the board's own line about that light
(`Chest off for today — dashboard`, `Chest on ignored — asleep …`). For any command, a
board line `Dashboard command X is unknown` (firmware older than the command) counts as
a refusal.

| Command | Sends | Who |
|---|---|---|
| `schedule` | The day the lights follow, with `◀ now` on the current stretch. Fetches `/api/schedule` | Anyone |
| `schedule check` | Where the site's copy came from (the board, and when, or the site's own) | Admin |
| `schedule update` | `SEND_SCHEDULE`: the board posts its schedule again | Admin |
| `status` | Fetches once: mode (with the end time of a hold or nap), the schedule's level, each light on / off / out for today and whether the schedule agrees. The room-light row shows only if the board sends lux, which it stopped doing in 3.1 | Admin |
| `log [n]` | Fetches once, 50 lines at most for visitors | Anyone |
| `normal` | `RESUME` | Admin |
| `hold` | `HOLD` | Admin |
| `wake` | `WAKE` | Admin |
| `sleep` | `SLEEP` | Admin |
| `nap [min]` | `NAP`, default 60 | Admin |
| `extend 30\|30m\|1h` | `EXTEND` | Admin |
| `floor`, `dresser`, `chest`, `ceiling` + `on`\|`off` | `LIGHT_ON` / `LIGHT_OFF`, value = roster index (0-3, `src/lights.h` order). Off is out for today; on brings it back in line. One help row, `<light> on\|off`. Needs the Nov 5 firmware | Admin |
| `simulate` | Work-in-progress notice for now | Anyone |

Hidden commands are a convenience only. Anyone can read the JS; the server enforces every restriction.

## Login and security

- **Check:** password, then a six-digit code, then one `incorrect`. A whole try counts as one attempt.
  The code sends itself at the sixth digit, because an iPhone's number pad has no return key.
- **Lockout:** 3 failures lock an address for 15 min (a whole /64 on IPv6). 30 failures across all
  addresses block login for an hour. Stored in Postgres.
- **Codes** work once: `auth_state.last_totp_step` refuses a repeat.
- **Sessions** last 90 days from login. The cookie holds a random token and `admin_sessions` holds its
  hash, so logout really ends it. Cookie: `__Host-mira`, `Secure`, `HttpOnly`, `SameSite=Lax`.
- **CSRF:** every state-changing browser request needs `X-CSRF-Token` and a matching `Origin`.
- **Client IP:** `X-Real-IP`, which Railway's edge overwrites, then the last `X-Forwarded-For` value.
  Never the first, which the caller controls.
- **Headers:** a strict CSP with no inline scripts or styles on any page, HSTS, frame denial, no-store.
  New pages must keep their CSS and JS in `static/`.
- **Logged:** login events in `access_log` and visits in `visits`, deleted after 90 days. Typed
  commands are stored word for word only if they match `LOGGABLE`, otherwise as `[unknown]`. That
  pattern exists in both `auth_api.py` and `terminal.js`, and a test compares them.
- **`bad_code` in `access`** means someone had the right password. Change it.

## API

**Firmware**, with `Authorization: Bearer <ESP32_API_KEY>`. These paths are compiled into the board.

| Method | Path | Purpose |
|---|---|---|
| POST | `/api/status` | Status snapshot, every 30 s and right after any command |
| POST | `/api/log` | A log line |
| GET | `/api/command` | Oldest pending command. Expires anything over 5 min old first |
| POST | `/api/command/<id>/ack` | Means queued on the board, not applied |
| POST | `/api/schedule` | The board's schedule, at boot and on `SEND_SCHEDULE`. Replaces the site's copy |

**Public**, rate limited.

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/schedule` | The board's schedule if it has posted one, else the site's (`source`: `board` or `site`) |
| GET | `/api/log/recent` | `?limit=N`. Admin also gets `search` and `exclude` |
| POST | `/api/visit/command` | Records a typed command |
| GET, POST | `/api/login` | GET: is login open, plus a CSRF token. POST: try it |

**Admin session.** POST routes also need the CSRF token.

| Method | Path | Purpose |
|---|---|---|
| POST | `/api/logout` | Body `{"all": true}` ends every session |
| GET | `/api/status/latest` | Live status with memory stats |
| POST | `/api/command` | Queue `RESUME`, `HOLD`, `WAKE`, `SLEEP`, `NAP`, `EXTEND`, `LIGHT_ON`, `LIGHT_OFF` or `SEND_SCHEDULE` |
| GET | `/api/command/<id>` | `outcome`: waiting, picked_up, applied, ignored, unconfirmed, expired, cancelled |
| POST | `/api/command/<id>/cancel` | Cancel before pickup |
| GET | `/api/status/stream` | SSE: `status`, `log` and `ended` events, 25 s keepalive |
| GET | `/api/admin/access` | `?view=logins\|visits\|commands` |
| GET | `/api/lux/history`, `/api/telemetry/history` | For the old lux page, and memory history |

## Status fields

`mode` is the 3.0 name: `ACTIVE`, `WAKE`, `HOLD`, `NAP` or `ASLEEP`. `state` still carries the 2.x name
for the old lux page. `bri` and `ct` are the schedule's in-line level, not a measurement of the bulbs,
so they can differ from the lights during a hold. `lights_on` is what's physically on, and `excluded`
is what's out for today.

## Pages

| Path | Visitor | Admin |
|---|---|---|
| `/` | Terminal | Terminal, `admin ~ $` |
| `/mira` | Terminal, inside Mira | Same, with the live status line |
| `/legacy`, `/legacy/logs`, `/legacy/lux` | Redirects to `/` | Old dashboard |

## Later

- **Simulation:** a 2D/3D room driven by commands, running the real C++ core compiled to WebAssembly.
- **Webhooks:** see CLAUDE.md. Nothing decided.
