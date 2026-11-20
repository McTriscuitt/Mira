# Idea: the site moves to the Pi too, under its own domain

*Saved Nov 20, 2026, for when the Pi transition starts. Nothing is bought, built or
decided. This is the hosting half of the Pi move; the lights half is
[`IDEA_OWN_ZIGBEE_HUB.md`](IDEA_OWN_ZIGBEE_HUB.md). That doc's Hosting section says
"no need to leave Railway", and this doc is the alternative, to be weighed against it.
Prices are from memory, not current listings.*

## Where things stand

- The site runs on Railway (Hobby plan, PostgreSQL). `/` is the McTerminal portfolio
  terminal, and Mira is a directory inside it at `/mira`.
- The plan is to rename the site **McTerminal** (after McTriscuit) at **`mcterminal.app`**.
  **The domain hasn't been bought yet.** Railway's "add a CNAME and a TXT record to
  mcterminal.app" screen assumes you already own the name; Railway can't register it.
- There are no hostnames hardcoded in the repo (checked for `railway.app`,
  `SERVER_NAME`, cookie domains), so the site works on any name it's reached by.

## Step 1 (any time, independent of the Pi): buy the domain

A domain is a name you rent and can point anywhere, so buying it now isn't tied to
where the site runs. Anything shared from now on (resume, GitHub profile) keeps
working through a later move to the Pi.

1. **Buy `mcterminal.app` at Cloudflare Registrar** (sold at cost, roughly $12–20/yr).
   Cloudflare matters because Cloudflare Tunnel (step 3) needs the domain's DNS on
   Cloudflare, so buying there avoids moving the DNS later.
2. **Point it at Railway.** In Railway, add the custom domain. Then in Cloudflare's DNS
   panel add the two records Railway shows:
   - `CNAME`: name `@` (the bare domain) or `www`, value `xxxx.up.railway.app`.
     Cloudflare allows a CNAME on the bare domain through CNAME flattening.
   - `TXT`: Railway's verification name and string.
   - Set both to **DNS only (grey cloud), not Proxied**, or Railway can't issue its
     certificate.
   - Enter only the part before `.mcterminal.app` in the Name field; Cloudflare adds the rest.
3. Wait for Railway's panel to turn green (minutes, up to a few hours).
   **`.app` is HTTPS-only** (HSTS preload), so the domain shows an error until
   Railway's certificate is issued. It never falls back to plain HTTP.

**The board needs no reflash for this.** `DASHBOARD_BASE_URL` in `secrets.h` keeps
pointing at the `*.up.railway.app` address, which keeps working alongside the custom
domain.

## How outsiders would reach a site on the Pi

Every device at home shares the router's one public IP. The Pi's `192.168.1.x` address
only means something inside the house, and by default the router answers nothing from
outside. There are two ways through:

| | Port forwarding + dynamic DNS | **Cloudflare Tunnel (recommended)** |
|---|---|---|
| How | Router sends ports 80/443 to the Pi; domain points at the home IP | Pi dials *out* to Cloudflare and keeps it open; visitors reach Cloudflare, which passes requests down the tunnel |
| Home IP | Public, findable by looking up the domain | Hidden |
| Router changes | Yes | None, nothing inbound opens |
| ISP blocks / CGNAT | Often breaks it | Works anyway |
| HTTPS | Yours to run (Let's Encrypt + renewals) | Cloudflare's |
| Cost | Free | Free (needs DNS on Cloudflare) |

The tunnel keeps the rule the hub doc already follows: **nothing connects inbound to
the house.**

## How this fits the hub idea

If the hub happens, the brain (`mirad`), the broker and the site could all run on the
same Pi:

- **The cloud broker could go away.** The hub doc's plan is hub → cloud broker → Flask
  site. With the site on the hub, the hub's own broker serves the site directly, and
  open question 6 there ("a second Railway service or a managed one") goes away.
- **Mira's controls keep working when the internet is down** if the dial and phone page
  talk to the hub on the home network. Only outside visitors need the tunnel.
- **Without the hub** (3.1 board still running), the board could post to the Pi at its
  `192.168.1.x` address instead of across the internet. That needs a `DASHBOARD_BASE_URL`
  change and a reflash, so it's a planned step of its own.

## Costs and risks

- **Uptime is your house's.** A power or internet cut takes the portfolio down for
  everyone, possibly while a recruiter is clicking through. Railway doesn't have this
  problem.
- **The database move is the real work, not the domain.** Railway's managed Postgres
  has to be dumped and restored on the Pi, then backed up from there on. `app.py` won't
  boot on SQLite (2.x note), so Postgres runs on the Pi.
- **You're the sysadmin:** OS updates, services that restart after reboot (systemd),
  backups, and storage that wears out (use a USB SSD, not an SD card). One box would
  hold the brain, Zigbee2MQTT's pairing data and the site's database, so its backups
  matter even more.
- **Deploys change.** Railway deploys on push to `main`. On the Pi it's a `git pull` +
  service restart, by hand or from a GitHub Actions job. This affects the planned
  selective-deploy workflow in CLAUDE.md.
- **Upside:** no monthly bill, full control, and good portfolio material on its own.

## Order, each step reversible

1. Buy the domain and point it at Railway (above). This is useful whatever else happens.
2. Stand up the site on the Pi behind a tunnel on a test name (e.g. `pi.mcterminal.app`)
   with a copy of the database, while Railway keeps serving `mcterminal.app`.
3. Cut over: point `mcterminal.app` at the tunnel. **Keep Railway running** until the
   Pi has proven itself, so switching back is one DNS edit.
4. Optionally move the board (or the hub) to talk to the site on the home network.

## Open questions (yours to decide)

1. **Railway or the Pi for the site?** That is, the hub doc's "stay on Railway" against
   this doc. A middle path: the Pi runs Mira, Railway keeps the public portfolio.
2. **Is the portfolio going down with home power or internet acceptable?**
3. **`mcterminal.app` vs. other endings** (`.dev` is also HTTPS-only; `.com` isn't).
4. **Rollback window:** how long Railway stays paid for after cutover.
5. **Does the board move to the home-network address**, or stay on the public URL until
   the hub replaces it?
