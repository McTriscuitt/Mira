# TERMINAL_MIRA_PLAN.md: next phase of the portfolio rework

Written Nov 2, 2026. Updated Nov 3 as the handoff for the dashboard session.

> **The user has asked for no liberties.** The "Decisions so far" and "What the user wants"
> sections are settled. Anything they don't cover gets asked before it's built, even when a
> default seems obvious. "Starting points for the conversation" further down is the original
> question list; where it conflicts with the decisions, the decisions win.

---

## Start here: the dashboard session

This session does all the site work: Flask, the terminal and the old dashboard.

**Already done (Nov 3):**
- `NAP` and `EXTEND` are flashed, and the board booted and appears on the dashboard. No reflash
  should be needed for the site work.
- Railway has `SECRET_KEY`, `ADMIN_PASSWORD_HASH` and `ADMIN_TOTP_SECRET`, made with
  `tools/make_admin_secrets.py`. The password hash is Werkzeug scrypt, and the code secret is
  standard TOTP. Railway and GitHub both have two-factor login on.
- `DASHBOARD_PASSWORD` and `DEMO_PASSWORD` are still set. The current `app.py` crashes on boot
  without `DASHBOARD_PASSWORD`. Tell the user when they can be deleted, after the new login is
  deployed.

**Check first:**
- **Railway Watch Paths are set to `/dashboard`.** Setting `/dashboard/**` was followed by a crash
  and a failed deploy, and the cause wasn't investigated. Don't assume `/dashboard` filters
  correctly: confirm that a firmware-only push doesn't redeploy and a dashboard change does.
- **`NAP` and `EXTEND` haven't been run on the real lights.** Test them through the new terminal
  once admin commands work. `NAP` is refused from 9 PM on, so test it earlier in the day.
- The Nov 2 reflash checklist in `MIRA_3.md` still has its wall switch, power loss and false hold
  checks open.

**Code findings to build around:**
- `ingest_status` drops the `mode` and `lights_on` the firmware sends, so the server can't tell a
  hold from a nap. It needs new columns and ingest code.
- A command's ack means queued, not applied, and rejected commands are acked too. Report success
  by watching `mode` change, or the "ignored" log line.
- Pending commands never expire today; the decision is 5 minutes.
- `/api/command` accepts any action. Allow only the known list.
- Each SSE stream holds one of 16 threads, the same ones the board's posts and polls use. Only
  admins inside `mira` get a stream; visitors fetch once per typed `status`.
- `knownWords()` in `terminal.js` includes hidden commands, so `login`, `access`, `legacy` and
  `logout` must be excluded from completion and "did you mean".
- The first `X-Forwarded-For` value can be faked. Work out Railway's trustworthy client IP before
  relying on it for the lockout or access log.
- Keep the lockout counters in Postgres so a redeploy doesn't reset them.
- New requirement: `pyotp` goes in `dashboard/requirements.txt`.
- `app.py` won't boot on SQLite as written; see the test harness notes below.

**Not decided yet, so ask:**
- The build order. One option: login and security, then the command tree, Mira's commands, the
  admin status line, visitor logging, and the old dashboard move and rework.
- The name of the simulation command, its WIP text and the `privacy` note's wording.
- Whether `app.py` splits into blueprints, and whether Mira's commands go in their own JS file.
- The `about` lines and tagline, which are still drafts.
- Merging `mira-3` into `main`, which was agreed only for after the firmware is verified.

---

## Finished: phase 1, the landing page

Live on Railway from branch `mira-3` (commits `5a6cffa`, `9562c77`). The user has called this
part of the redesign finished.

- `/` is the portfolio terminal. `/mira` is the old dashboard, still a separate page.
- HTML, CSS and JS are in separate files for the new page:
  - `dashboard/templates/landing.html`
  - `dashboard/static/css/landing.css`
  - `dashboard/static/js/terminal.js`: behavior, with a `COMMANDS` table
  - `dashboard/static/js/portfolio-content.js`: the text and the project list
- Railway settings: branch `mira-3`, root directory `/dashboard`.
- `SQLAlchemy==2.0.55` is pinned because 2.1 defaults `postgresql://` to `psycopg`, which
  isn't installed. A fresh build without the pin crashes on boot with a 502.
- Still open from phase 1:
  - The `about` lines and tagline in `portfolio-content.js` are drafts, publicly visible,
    waiting for the user's wording.
  - Email, LinkedIn and resume are empty, so they're hidden.
  - After the last deploy, routes were only checked by requesting them. Typing in the live
    terminal and "last seen" updating weren't confirmed in that session.
- Testing without Postgres: `app.py` won't boot on SQLite as written. A throwaway harness
  worked by rewriting its Postgres-only migration SQL to a no-op with a SQLAlchemy
  `before_cursor_execute` listener, then driving the real app with Playwright. The harness
  was in that session's scratchpad, so it needs recreating. `/admin/access-log` returns 500 on
  Windows because `%-d` in its template is Linux-only; that's a test-environment issue.

---

## Decisions so far (user's answers, Nov 2)

The user asked for no liberties: anything not listed here gets clarified before it's built.

- **Purpose:** the terminal is mainly a showcase. Day to day, the user controls lights from the
  Hue app.
- **Roles:** `demo` and `recruiter` are removed. Default is public view-only; admin login holds
  everything restricted.
- **Command trees:** each project has its own tree. Mira's commands work only after entering
  `mira`. At root, `hold` errors with a hint to enter `mira` first, and `mira hold` also errors.
  `help`, `clear`, `login` and `logout` work everywhere; other root commands error inside `mira`.
- **Mira commands:** `status`, `log`, `normal`, `hold`, `wake`, `sleep`, `nap`, `extend x`, plus
  `cd ..`, `back` and `exit`. Control commands appear in Mira's `help` marked admin-only. A
  visitor's attempt points to a simulation command, which shows a WIP notice for now.
- **Firmware:** `nap` now, with `extend`. The user authorized flashing; the board is plugged in.
- **Public data:** live data is public, including log times. Board memory stats stay admin-only.
  Visitors may lag by a minute or more.
- **Login:** never mentioned in `help`, completion or suggestions. Session stays 90 days. Limit of
  3 attempts.
- **Pages:** `/login` is removed. The old dashboard moves to `/legacy`, reached by an admin-only
  `legacy` command. The access log moves to a login-locked terminal command.
- **Logging:** record every visitor, every failed and successful login, and failed passwords if
  possible.
- **Security:** take all available measures, reworking setup or deployment if needed.
- **Simulation:** its own later phase, a 2D/3D space driven by commands, running the real C++ core
  compiled to WebAssembly.

Added Nov 3:

- **`nap`:** 60 min by default, `nap 90` sets minutes, 15 s fade off, replaces an active hold,
  refused at or after 9 PM with a pointer to `sleep`.
- **`extend`:** `extend 30`, `30m` or `1h`; error when no hold or nap is active. A nap still becomes
  asleep at 9 PM, 1 AM lights-out still ends a hold, and a hold extended past a fade stays frozen
  and then catches up.
- **`/logs`:** stays as an admin-only page at `/legacy/logs`. Lux readings keep being recorded.
- **Old dashboard:** reworked once, for this migration, to match the current commands. It may fall
  out of step later because it's legacy. Logged-out visitors go to `/`.
- **Admin:** `access` shows the access log; `access` and `legacy` stay out of `help`. Prompt becomes
  `admin ~ $` when logged in.
- **Status:** visitors get one result per typed `status`; admins get a live stream.
- **Logging:** record login successes and failures, but never failed passwords. Visitor data is the
  normal request data plus typed commands, with no location or fingerprinting, kept 90 days, with
  a short privacy note.
- **Lockout:** an IP locks for 15 min after 3 failures; 30 failures across all IPs block login for
  an hour.
- **Two-factor login:** yes, six-digit codes in Microsoft Authenticator. Ask for the password, then
  the code, then show one `incorrect` message. That whole try counts as one attempt.
- **Discoverability:** the user accepts that `login` can be found in the public JS and repo.
- **Deployment:** new secrets from `tools/make_admin_secrets.py` (`SECRET_KEY`,
  `ADMIN_PASSWORD_HASH`, `ADMIN_TOTP_SECRET`), Watch Paths (user), merge to `main` after firmware is
  verified, pending commands expire after 5 min. Keep `DASHBOARD_PASSWORD` set until the new login
  is deployed; the current `app.py` crashes on boot without it. Remove it and `DEMO_PASSWORD` after.
- **`help`:** visitors see control commands marked as not usable by them. `access`, `legacy` and
  `logout` are never listed. Logged in, control commands are listed normally.
- **Admin live stream:** one status line above the prompt while inside `mira`, updating instantly.
- **Old dashboard rework:** Normal, Hold, Wake, Sleep and Nap buttons. Wind down and Hard off go.
  The slider becomes time remaining with `+10 min` (via `extend`) and `finish`. Light chips become
  display-only. The lux widget becomes a link to `/legacy/lux`.
- **Typed command logging:** known commands word for word, anything else as `[unknown]`.
- **Until secrets are set:** `login` reports unavailable and admin features stay off.
- **Lux page:** moves to `/legacy/lux`.
- **Order:** secrets first, then firmware, then the site.

---

## What the user wants (their ideas, from Nov 2)

1. **Mira built into the terminal**, rather than a separate site that looks similar. Their
   reasoning: subcommands would "demonstrate more than just the dashboard". They aren't sure
   how difficult it is, but like the idea and think it's probably the best course.
2. **Launching `mira` enters a directory.** Everything typed after that goes to Mira, not the
   main terminal.
3. **Subcommands replace button presses**, such as `normal` or `hold`.
4. **A way back to the root directory**, the main commands. The real-terminal equivalent is
   `cd ..`.
5. **Mira restricted without a password.**
6. **A `login` command in the terminal.** Just a password. It gives admin access to the whole
   terminal: Mira and future projects that connect to the portfolio. They'd also accept a
   login page, but prefer the command.

---

## Starting points for the conversation

These came from reading the code at the end of phase 1. They're prompts for the questions the
user asked for, not conclusions, and the next session should look for more.

### What's already in the code

- The API should mostly carry over: `/api/status/latest`, `/api/status/stream`,
  `/api/log/recent` and `/api/command` are all JSON or SSE, separate from the HTML. A terminal
  can call the same routes, so this may need no reflash. Confirm that rather than assuming it.
- The server already enforces roles. Status, stream and logs accept `owner` or `demo`.
  Sending and cancelling commands are owner-only, and demo log lines have times redacted.
- `terminal.js` has one flat `COMMANDS` table and a fixed `~ $` prompt. Directories mean a
  nested command tree and a prompt that changes.
- `/api/command` accepts any `action` string; it doesn't check it against a known list.
- `/login` has no rate limiting. There's no CSRF protection, and the session cookie settings
  are Flask's defaults (no `SESSION_COOKIE_*` config).
- The owner session lasts 90 days. The demo session doesn't set `session.permanent`.
- Gunicorn runs `-w 1 --threads 16`, and each open SSE stream holds a thread.
- The status `state` and the command names are still the interim 2.x mapping (`MIRA_3.md` §8).
  The 3.0 names are in `mode`. This rework is the session that section was waiting for.

### Access

- **What does a visitor without a password see?** "Mira restricted" conflicts with recruiters
  being able to see Mira. CLAUDE.md's notes also ask for a read-only `recruiter` role, between
  demo and owner, with no hidden owner IP button. A recruiter who can't see the main project
  loses much of the point. Options include a public read-only mode, a shared recruiter
  password, or a canned demo with no live data. Is `demo` still needed once this exists?
- **What's private?** Live light status and logs reveal when someone is home and asleep. That
  matters more if any of it becomes public. Which fields and log lines are safe to show?
- **What should visitors be able to try?** A command like `hold` is the most impressive thing
  on the page and the one thing a stranger shouldn't run for real. Does a visitor get
  "permission denied", or a simulated result?
- **"Admin over the whole terminal"**: what does that mean concretely today, with one project?
  Is it one role across everything? What would future projects need from it?

### Login security

- A public terminal with a `login` command is an obvious target, and a successful guess
  controls lights in a real bedroom. Rate limiting and lockout come into scope, along with
  cookie flags (`Secure`, `HttpOnly`, `SameSite`) and CSRF.
- The password must be masked while typing, and kept out of the echoed output, command history
  and any shareable URL. Running `terminal.js` in the browser also means hiding a command in
  JS is never security; only the server can enforce it.
- Should there be a `logout`, and how long should a login last on someone else's computer?
- The access log records role and IP. What should it record for terminal logins and failures?

### Daily use

- **The dashboard is how the user actually controls their lights.** Typing `hold` on a phone in
  bed is slower than tapping a button. Should commands be clickable, as the portfolio's are?
  Is there a shortcut for one-tap control, or is that an acceptable trade?
- **Is the old dashboard retired, or kept during the move?** Keeping `/mira` until the terminal
  has been used for real avoids losing control if the new version has a bug.

### Live data in a scrolling terminal

- A terminal scrolls, while the dashboard shows values that update in place. What's the live
  status view: a fixed status line, a `status` command, or something like `watch`?
- When a command is sent, the board picks it up within ~5 s, so feedback has stages: sent,
  picked up, applied. What does that look like as output? What if the board is offline?
- How many streams can stay open? If visitors can reach Mira without logging in, each one
  could hold a thread out of 16. Open the stream only while inside `mira`?

### What doesn't fit a terminal

- What happens to the wake/hold progress slider, per-light chips, `/logs`, `/lux` and
  `/admin/access-log`? Canvas charts aren't text. The `/lux` page and curve JS are 2.x
  leftovers, so some may simply go away. Which commands would replace the rest?
- Which subcommands should exist? 3.0's modes are active, hold, nap, asleep and wake, and the
  nap button is on the user's list. Should command names follow 3.0 or what the UI says today?

### Terminal behavior and navigation

- Should the address change? Does `/mira` open the terminal already inside Mira, so a link or
  refresh lands there? What should the browser's back button do?
- Which ways back are accepted: `cd ..`, `cd`, `back`, `exit`? `back` is friendlier for
  recruiters, and `cd ..` is what a terminal user will try.
- Do root commands (`help`, `contact`, `clear`) work inside `mira`, and does `help` show both?
- Is a file-listing metaphor (`ls`, `pwd`) worth carrying through, or just the prompt?

### Fit with the recruiter's first impression

- A recruiter may spend 30 seconds here. Does the terminal still show what Mira is if all they
  see is a directory and a command list? Is there a `demo` or guided path? Are there photos,
  video or an explanation of the system, since the real thing is physical lights nobody
  visiting can see?
- Mobile: does this hold up on a phone, for both the recruiter and the user?

### Sequencing and the code

- Possible order: access and login first, then the command tree, then Mira's subcommands, then
  live status, then retiring the old dashboard. Is that the right order?
- How are future projects plugged in, and does Mira's code live in its own file
  (`static/js/mira.js` or similar) from the start?
- `app.py` is one file at 485+ lines. Is it time for Flask blueprints?
- With Railway watching `mira-3`, any commit redeploys the site, including firmware-only ones.
  **Watch Paths** set to `/dashboard/**` would stop that. `main` is still 2.x, so at some point
  the branch question needs settling too.
