# Mira — Hue API v1 → v2 Migration Planning

We've been discussing migrating Mira's Hue integration from API v1 (local HTTP) to API v2 (local HTTPS). Nothing has been implemented yet — this is a planning conversation. Here's what was decided.

---
## User Notes

- I am sure there are other ways to utilize the SSE. We will need to brainstorm other ways to use it

---

## Why Migrate

- v2 supports Server-Sent Events (SSE) from the bridge, enabling instant override detection instead of polling
- Currently `checkOverride()` fires up to 4 GET requests per tick, which combined with `setLight()` PUTs is causing ~45s actual tick intervals instead of the intended 30s
- SSE eliminates all GET polling, replacing it with a persistent connection the bridge pushes events to

---

## HTTPS / Certificate Handling

Do not use `setInsecure()` for normal operation. The target architecture is fully automatic cert management via NVS:

- On startup, load the stored cert + expiry timestamp from NVS (via `Preferences`)
- If no cert exists, or it is expired or within 30 days of expiry → re-fetch from the bridge using `setInsecure()` for that one call only, then store the new cert + parsed `notAfter` timestamp to NVS
- All normal Hue API connections use the NVS-stored cert via `client.setCACert()`
- Expiry check uses NTP time — already available in the firmware
- This makes cert management fully hands-off after initial deployment — no reflashing ever needed

The single insecure fetch is acceptable risk since it only occurs on the local LAN and only when no valid cert is stored.

---

## Decisions Made

- Refactor `LightTarget.bri` from `uint8_t` (0–254) to `float` (0.0–100.0 percent) natively throughout the entire codebase — `luxToTarget()`, ramp math, tolerance comparisons, everything. No conversion shim at the PUT boundary; all math works in percentage units end to end
- `dynamics.duration` replaces `transitiontime` (units change from 100ms to ms)
- `ct`/`mirek` stays the same unit — no change needed
- Light UUIDs: do a one-time manual discovery call (`GET https://192.168.1.186/clip/v2/resource/light`) to retrieve the four UUIDs, then hardcode them in `config.h` alongside existing light defines. UUIDs are stable — tied to Zigbee identity, not network state. They only change if a bulb is factory reset and re-paired
- SSE stream opened at `GET https://192.168.1.186/eventstream/clip/v2` with `hue-application-key` header — read in the existing non-blocking wait loop alongside button polling
- The double-buffer (`sentTarget`/`prevSentTarget`) becomes unnecessary once SSE is in place — override detection triggers on the event payload itself, not a polled comparison
- Fix tick timing: subtract processing time from the 30s wait so intervals are consistently 30s regardless of HTTP call duration:

```cpp
unsigned long tickStart = millis();
// ... all tick processing ...
unsigned long elapsed = millis() - tickStart;
unsigned long waitTime = (elapsed < 30000UL) ? (30000UL - elapsed) : 0;
// wait waitTime instead of flat 30000
```

---

## millis() Rollover Audit

`millis()` rolls over to zero after ~49.7 days. Subtraction-based elapsed time is safe across rollover due to unsigned integer wrap behavior. Audit the entire codebase for unsafe patterns and replace them:

```cpp
// UNSAFE — do not use
if (millis() > startTime + interval)
if (millis() >= someAbsoluteTimestamp)

// SAFE — always use this form
if (millis() - startTime >= interval)
```

Flag every `millis()` comparison in `main.cpp` and verify it uses the subtraction form.

---

## Migration Phases

The migration is split into three independently testable phases. Flash and verify on hardware at the end of each phase before starting the next.

### Phase 1 — Float bri refactor + config prep ✅ complete
*Pure internal refactor. No behavior change — lights will act identically.*

- `lightcurve.h` — scale all bri constants to 0.0–100.0%, change `LightTarget.bri` from `uint8_t` to `float`
- `main.cpp` — update all bri math to work in percent throughout (ramp math, sentTarget, windDown, wakeRamp, resume ramp, shouldUpdate, isManualOverride); add temporary v1-boundary shim in `setLight()` (`bri * 2.54f` → int) and `getLightState()` (`bri / 2.54f` → float) so existing v1 API calls keep working
- `config.h` — split `STATE_TOLERANCE` into `STATE_TOLERANCE_BRI` (float, percent) and `STATE_TOLERANCE_CT` (int, mirek); add v2 URL/key/UUID placeholder defines for Phase 2 setup
- millis() rollover audit — all existing comparisons already use safe subtraction form; no changes needed

### Phase 2 — NVS cert storage + v2 API calls ✅ complete
*Switches the actual Hue API calls to HTTPS v2.*

- `config.h` — v2 base URL + `HUE_API_KEY` activated; UUID defines filled in from discovery (Bedside: `6cccebb8`, Desk: `14bd82eb`, Ceil_1: `616a67e4`, Ceil_2: `2064523e`)
- `main.cpp` — `ensureBridgeCert()` loads/fetches bridge TLS cert from NVS; `setLight()` and `setLightColor()` rewritten to v2 HTTPS JSON (`on.on`, `dimming.brightness`, `color_temperature.mirek`, `dynamics.duration` in ms); Phase 1 shims removed; `_hsbToXY()` added for color mode; `getLightState()` stays v1 HTTP (removed in Phase 3)
- Dashboard — `bri` column migrated to `FLOAT`; startup migration SQL converts historical rows; JS `calc()` and `by()` updated to v2 percent constants in both templates; bri displays show `%` suffix

### Phase 3 — SSE stream + remove polling
*The payoff: instant override detection, consistent 30s tick intervals.*
- Use Opus

- `main.cpp` — open SSE stream at `GET https://192.168.1.186/eventstream/clip/v2`; parse events in the non-blocking wait loop; replace `checkOverride()` with SSE event handler; remove `getLightState()` entirely; remove double-buffer (`sentTarget`/`prevSentTarget`); apply tick timing correction (subtract processing time from the 30s wait)

---

## Scope of Changes

| Area | Change | Phase |
|---|---|---|
| `config.h` | Split `STATE_TOLERANCE`; add v2 URL/key/UUID placeholders | ✅ 1 |
| `lightcurve.h` | All curve constants and `luxToTarget()` output in 0.0–100.0 range | ✅ 1 |
| `LightTarget` struct | `bri` becomes `float` | ✅ 1 |
| `setLight()` / `setLightColor()` | Full v2 JSON + HTTPS, UUID-based endpoints, `_hsbToXY()` | ✅ 2 |
| `getLightState()` | bri scaled to percent (v1 HTTP stays until Phase 3) | ✅ 1 → 3 |
| NVS cert storage | `ensureBridgeCert()`, `_fetchAndStoreBridgeCert()`, `_derToPem()` | ✅ 2 |
| `config.h` | v2 base URL + API key active; UUIDs hardcoded | ✅ 2 |
| Dashboard | `bri` column → FLOAT; JS curve constants + `by()` + display | ✅ 2 |
| `checkOverride()` | Replaced by SSE event handler | 3 |
| Non-blocking wait loop | SSE stream reader + tick timing correction | 3 |
| millis() rollover audit | Already clean — no changes needed | ✅ done |

---

## Dashboard Changes for the bri Float Refactor

The firmware's `LightTarget.bri` is changing from `uint8_t` (0–254) to `float` (0.0–100.0). This ripples into the dashboard in several places.

### `app.py` — Database column type

```python
# Before
bri = db.Column(db.Integer)

# After
bri = db.Column(db.Float)
```

A Postgres `INTEGER` column will silently truncate a float. Change the column type and run a one-time migration on existing rows to convert historical bri values into the new unit:

```sql
ALTER TABLE status_snapshots ALTER COLUMN bri TYPE FLOAT;
UPDATE status_snapshots SET bri = ROUND(CAST(bri AS NUMERIC) / 2.54, 1);
```

The `/ 2.54` is exact: Hue v1 max bri is 254, which maps to 100.0%.

### `index.html` — bri display

```js
// Before — bri was 0–254, percent was derived
document.getElementById('val-bri').textContent =
    sentinel ? '—' : `${data.bri} · ${Math.round(data.bri / 254 * 100)}%`;

// After — bri IS the percent
document.getElementById('val-bri').textContent =
    sentinel ? '—' : `${data.bri.toFixed(1)}%`;
```

### `index.html` and `lux_curve.html` — JS `calc()` function

Both templates contain a client-side duplicate of the firmware lux curve. **This JS must be kept in sync with `lightcurve.h` manually — it is not derived from the firmware at runtime.** After the float refactor the constants need updating:

```js
// Before (v1 bri units, 0–254)
if      (lux <= 10)  { bri = 50  + 100 * this._log(lux/10, 20); }
else if (lux <= 200) { const t=(lux-10)/190;   bri = 150 + 50  * this._crawl(t, 0.5); }
else if (lux <= 500) { const t=(lux-200)/300;  bri = 160 + 60  * t**5; }
else                 { const t=(lux-500)/2500; bri = 220 + 34  * this._log(t, 4.5); }
const n = (bri - 50) / 204;

// After (v2 bri percent, 0.0–100.0 — mirrors updated lightcurve.h constants)
if      (lux <= 10)  { bri = 19.7 + 39.4 * this._log(lux/10, 20); }
else if (lux <= 200) { const t=(lux-10)/190;   bri = 59.1 + 19.6 * this._crawl(t, 0.5); }
else if (lux <= 500) { const t=(lux-200)/300;  bri = 63.0 + 23.6 * t**5; }
else                 { const t=(lux-500)/2500; bri = 86.6 + 13.4 * this._log(t, 4.5); }
const n = (bri - 19.7) / 80.3;
```

### `lux_curve.html` — Y-axis and canvas mapping

The `by()` function maps bri to canvas Y coordinates. The grid lines, axis labels, and discontinuity connector are all hardcoded in v1 units:

```js
// Before
function by(b) { return PT + ch - (b - 50) / 204 * ch; }
[50, 100, 150, 200, 254].forEach(b => { /* grid lines */ });
[50, 100, 150, 200, 254].forEach(b => ctx.fillText(b, PL - 6, by(b)));
ctx.moveTo(lx(200), by(200)); ctx.lineTo(lx(200), by(160)); // discontinuity connector
ctx.fillText('overheads', lx(200) + 7, by(180));            // label midpoint

// After
function by(b) { return PT + ch - (b - 19.7) / 80.3 * ch; }
[20, 40, 60, 80, 100].forEach(b => { /* grid lines */ });
[20, 40, 60, 80, 100].forEach(b => ctx.fillText(b + '%', PL - 6, by(b)));
ctx.moveTo(lx(200), by(78.7)); ctx.lineTo(lx(200), by(63.0)); // S3_BRI_HI → S2_BRI_LO
ctx.fillText('overheads', lx(200) + 7, by(70.8));             // midpoint of 78.7 and 63.0
```

The crosshair label that prints the bri value at the cursor should also add a `%` suffix after this change.

### Firmware log messages

Any `sendLog()` call that currently computes `target.bri * 100 / 254` to show a percent should be updated — bri is already a percent:

```cpp
// Before
sendLog("... bri=" + String(target.bri) + " (" + String(target.bri * 100 / 254) + "%) ...");

// After
sendLog("... bri=" + String(target.bri, 1) + "% ...");
```

---

## Railway / GitHub Deploy Configuration

Disconnect Railway's built-in git auto-deploy. Replace with a GitHub Actions workflow that triggers Railway deploys only when dashboard-relevant files change (`app.py`, `requirements.txt`, dashboard HTML/CSS/JS). Firmware changes (`src/`, `platformio.ini`, etc.) should never trigger a Railway deploy. Requires storing a Railway API token as a GitHub Actions secret. Approximately 20 lines of YAML.

---

## Future Brainstorming — Webhooks / IFTTT / External Triggers

We've been discussing ways to integrate external automation into Mira. No decisions made yet — this needs more brainstorming. Some directions worth exploring:

- Inbound webhooks to the Flask dashboard (`/webhook/external`) so external services can trigger state changes — Google Calendar sleep event → `WIND_DOWN`, Siri Shortcut → any state, IFTTT Webhooks service → arbitrary triggers
- The existing `ESP32_API_KEY` bearer token pattern should extend to any inbound webhook endpoints for security
- This connects to the planned Google Calendar integration already noted in `DASHBOARD.md`
- No implementation decisions made — treat this as an open design area to revisit
