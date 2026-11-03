# TERMINAL_MIRA_BUILD_LOG.md: overnight build, Nov 3-4, 2026

**Status: built and tested locally, not deployed.** The work is on branch `terminal-mira`. Railway
only watches `mira-3`, so the live site hasn't changed. The firmware wasn't touched (no changes in
`src/`) and no command was sent to the real board.

---

## Morning checklist

Do these in order. Steps 2-6 need you, because they use your password, your authenticator or the
real lights.

1. **Deploy.** This makes it live:
   ```
   git checkout mira-3
   git merge terminal-mira
   git push
   ```
   Your local `mira-3` was already 3 commits ahead of GitHub when this started (`4c4c508`,
   `d331040`, `042c012`: notes, the `NAP`/`EXTEND` firmware and the secrets script). This push sends
   them too. That's expected, and it doesn't reflash anything.
2. **Check the public side.** Open the site and type `mira`, then `status`, then `log`.
3. **Log in.** Type `login`, then your password, then the six-digit code.
   - Your old dashboard login stops working at this deploy, so this is the only way in.
   - If you immediately see `that took too long`, the origin check is refusing you. Railway's logs
     will show a `refused origin` line. Tell me what it says.
4. **Check the IP.** Type `access`. The last line starts `you:`.
   - The address should be your real public IP, and should match `x-real-ip`.
   - **If it isn't your IP, tell me before relying on the lockout.** All visitors could be sharing
     one lockout and rate limit.
5. **Try the lights, before 9 PM.** This is also the first real test of `NAP` and `EXTEND`. Inside
   `mira`: `hold`, `extend 10`, `normal`, `nap 1`, then wait for the lights to come back.
   - `nap` after 9 PM should say the board refused it, which is also worth seeing once.
6. **Try the old dashboard.** Type `legacy` and press each button.
7. **Delete `DASHBOARD_PASSWORD` and `DEMO_PASSWORD` from Railway**, once login has worked.
8. **Test Watch Paths.** This was still open from the plan: confirm a firmware-only push doesn't
   redeploy and a dashboard change does.
9. **Finish the Nov 2 checklist** in `MIRA_3.md`: wall switch, power loss and false hold.

**To undo the deploy:** use Railway's redeploy on the previous deployment, or run
`git revert -m 1 <merge commit>` on `mira-3` and push. The database changes only add things, and
the old code ran fine on the upgraded schema in testing, so rolling back is safe.

---

## What was built

- **Login:** `login` asks for a password, then a code, then shows a single `incorrect`. The password
  is masked and stays out of the screen, history and logs.
- **Lockout:** 3 failures lock an address for 15 minutes, and 30 failures across all addresses block
  login for an hour. Both are stored in Postgres, so a redeploy can't reset them.
- **Directories:** `mira` enters a directory, and `cd ..`, `back` and `exit` leave it. The address
  bar and the browser's back button follow along.
- **Mira's commands:** `status`, `log`, `normal`, `hold`, `wake`, `sleep`, `nap [min]`,
  `extend 30|30m|1h`, `simulate`.
- **Command feedback:** a command shows sent, then picked up, then applied, or quotes the board's
  refusal. It's judged by the `mode` change or an `ignored` log line, because an ack only means
  queued.
- **Public data:** status and logs are public, with times. Board memory stats are admin-only.
- **Live status:** admins get one status line above the prompt inside `mira`. Visitors get one
  result per typed `status`.
- **Admin commands:** `access`, `legacy` and `logout`, none listed anywhere. The prompt becomes
  `admin ~ $`.
- **Logging:** visits, typed commands (`[unknown]` for anything unrecognized), login successes and
  failures. Never passwords. Kept 90 days, with a `privacy` command describing it.
- **Old dashboard:** moved to `/legacy`, `/legacy/logs` and `/legacy/lux`, with the agreed rework.
  `/login`, `/logs`, `/lux` and `/admin/access-log` are gone.
- **Server fixes from the plan:** `mode` and `lights_on` are stored, pending commands expire after 5
  minutes, and `/api/command` only accepts known actions.

---

## Liberties taken

Each of these was my call, and any can be changed.

### Wording, all publicly visible

- The command is named **`simulate`**. Its text says it's a work in progress.
- I wrote the **`privacy`** note and Mira's **intro**. Both are marked DRAFT in
  `static/js/portfolio-content.js`. The `about` lines and tagline are still your earlier drafts.
- Control commands are tagged **`(admin only)`** in `help`. Trying one as a visitor says it "changes
  real lights in my room, so it is not open to visitors".

### Behavior

- **`/mira` opens the terminal already inside Mira**, so a resume link or a refresh lands there.
- **Control commands in `help` can't be clicked when you're logged in.** A stray tap on `sleep`
  would turn off your real lights. Visitors can click them, since they only get the notice.
- **`access`, `legacy`, `logout` and `login` work inside `mira` too**, and act like unknown commands
  for visitors, apart from `login`.
- **The time slider on `/legacy` is display-only.** The firmware can only seek a nap, not a hold, so
  dragging would have worked for one and not the other. `+10 min` and `finish` do the work.
- **Sleep on `/legacy` takes two taps**, the way Hard off did.
- **`Nap` on `/legacy` is always the 60 minute default.**
- **The `/legacy/logs` filter buttons** are now all, wake, hold, nap, asleep and boot.

### Added beyond the plan

- **Codes work once.** A six-digit code can't be reused, even within its 30 seconds. Two logins in
  the same 30 seconds means waiting for the next code.
- **`logout` really ends the session.** A server-side record backs each login, so a copied cookie
  dies at logout too. `logout all` ends every session, for a forgotten computer.
- **`access` has three views:** `access logins`, `access visits` and `access commands`.
- **Failed logins record which part failed.** **`bad_code` in `access` means someone had the right
  password, so change it.**
- **An IPv6 lockout covers the whole /64**, since one customer usually holds all of it.
- **Rate limits per address:** 60 per minute for status and logs, 120 for command logging, 30 for
  login requests. A visitor's `status` is cached for 30 seconds and `log` is capped at 50 lines.
- **At most 4 live streams at once**, so they can't use up the threads the board needs. A stream
  also ends itself when its login does.
- **Strict security headers on every page**, including a Content Security Policy that forbids inline
  scripts and styles. That's why the old pages' code moved into `static/`.
- **Logout is a POST with a CSRF token**, so a link on another site can't log you out.
- **Database indexes** on the tables that grow every few seconds, because "latest status" is now a
  public read.
- **The board counts as offline after 90 seconds** of silence.
- **The landing page stays up if the database is down.**
- `pwd` works, as a terminal habit.

### Structure and data

- **`app.py` split into modules:** `models`, `security`, `broadcast`, `board_api`, `auth_api`,
  `mira_api`, `pages`. Mira's commands are in `static/js/mira.js`, admin commands in
  `terminal-admin.js`.
- **Login records are also deleted after 90 days**, and old rows in `access_log` go with them. The
  plan only set 90 days for visitor data. Tell me if you'd rather keep security records longer.
- **Your own visits are logged too**, flagged `(admin)`.
- **Tests added:** `dashboard/tests/`. Not used by the running site.
- **`Markdowns/MIRA_3_BUILD_REPORT.md`** had an uncommitted table reformat from before this session.
  It's committed on this branch, per your commit-everything preference.

### Not done, on purpose

- **`mira-3` isn't merged into `main`.** That waits for the firmware to be verified.
- **Nothing was pushed to `mira-3`,** as you chose.
- **The `/legacy/lux` page is unchanged apart from its move.** It still has the 2.x curve math, since
  you said legacy may fall out of step.

---

## Testing

| What | Result |
|---|---|
| Unit tests, SQLite | 54 passed |
| Unit tests, Postgres 18 | 54 passed |
| Browser test, Chromium, on Postgres 18 | 62 of 62 checks passed |
| Upgrade: live code's schema, then new code, then live code again | Passed |

The browser test uses a fake board that I wrote from `src/dashboard.cpp`. To rerun:
`python -m pytest tests` and `python tests/e2e_terminal.py`, from `dashboard/`.

### What this testing can't tell you

- **Real board:** the fake board imitates its HTTP calls, but the real one wasn't involved. The
  firmware's routes and key are unchanged.
- **Railway's proxy:** the client IP header and the origin check depend on it. Checklist steps 3 and
  4 cover these. The IP header choice comes from Railway staff forum answers, and those have
  conflicted over time:
  [newest staff answer](https://station.railway.com/questions/need-authoritative-client-ip-proxy-tru-090eac86),
  [earlier conflicting advice](https://station.railway.com/questions/which-header-should-i-rely-on-for-real-c-d78a6f96).
- **Gunicorn:** it doesn't run on Windows, so tests used Flask's threaded server. `Procfile` is
  unchanged. Live streams through gunicorn are only proven once deployed.
- **The Postgres driver:** tests used `pg8000` because `psycopg2` has no build for this computer's
  Python. Railway uses `psycopg2`, and Railway's Postgres version may not be 18.
- **Your real secrets and authenticator:** tests generated their own password and codes with the
  same libraries `tools/make_admin_secrets.py` uses.
- **Real phones:** phone checks used Chromium's phone emulation.
