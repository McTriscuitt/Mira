# DASHBOARD.md: the site

> **On branch `terminal-mira` until it's merged and verified.** Until then, `mira-3` and the live
> site still run the older design. See `TERMINAL_MIRA_BUILD_LOG.md`.

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
| `mira_api.py` | Status, log, stream, commands, and working out what a command did |
| `pages.py` | HTML pages |
| `static/js/terminal.js` | The terminal's engine and root commands |
| `static/js/terminal-admin.js` | `login`, `logout`, `access`, `legacy` |
| `static/js/mira.js` | Mira's directory |
| `static/js/portfolio-content.js` | The site's words and project list. **Edit this for content** |
| `static/js/legacy-*.js`, `static/css/legacy-*.css` | The old pages |
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

**At the top:** `mira`, `about`, `projects`, `contact`, `resume`, `privacy`. Also `ls`, `whoami`, `open`.

**Inside `mira`:**

| Command | Sends | Who |
|---|---|---|
| `status` | Fetches once | Anyone |
| `log [n]` | Fetches once, 50 lines at most for visitors | Anyone |
| `normal` | `RESUME` | Admin |
| `hold` | `HOLD` | Admin |
| `wake` | `WAKE` | Admin |
| `sleep` | `SLEEP` | Admin |
| `nap [min]` | `NAP`, default 60 | Admin |
| `extend 30\|30m\|1h` | `EXTEND` | Admin |
| `simulate` | Work-in-progress notice for now | Anyone |

Hidden commands are a convenience only. Anyone can read the JS; the server enforces every restriction.

## Login and security

- **Check:** password, then a six-digit code, then one `incorrect`. A whole try counts as one attempt.
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

**Public**, rate limited.

| Method | Path | Purpose |
|---|---|---|
| GET | `/api/status/latest` | Visitors: cached 30 s, no memory stats. Admin: live, with memory stats |
| GET | `/api/log/recent` | `?limit=N`. Admin also gets `search` and `exclude` |
| POST | `/api/visit/command` | Records a typed command |
| GET, POST | `/api/login` | GET: is login open, plus a CSRF token. POST: try it |

**Admin session.** POST routes also need the CSRF token.

| Method | Path | Purpose |
|---|---|---|
| POST | `/api/logout` | Body `{"all": true}` ends every session |
| POST | `/api/command` | Queue `RESUME`, `HOLD`, `WAKE`, `SLEEP`, `NAP` or `EXTEND` |
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
