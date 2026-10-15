# Mira 3.0: first night on the board (Oct 14–15, 2026)

Flashed **Oct 14, 23:33 CT** from `mira-3` @ `164d320` (pushed to GitHub first).
This is step 5 of `MIRA_3.md` (the first-flash checklist). Findings go at the bottom.

## How it's being watched

- **Serial capture:** `tools/serial_soak_logger.py` writes to
  `logs/mira3_first_night_2026-10-14.log` (`logs/` is gitignored). It now takes the port
  as an argument; the board is on **COM4**, not COM5:
  `python -u tools/serial_soak_logger.py logs/<name>.log COM4`
- **Claude session:** a Monitor tails that log for the notable lines (boot, PUTs, holds,
  naps, asleep, errors, panics, port loss). There's an hourly check-in (:17) that
  re-arms the Monitor and adds findings here. Both end if the Claude session closes,
  but the logger keeps running on its own.
- **Kill the logger before reflashing** (it holds COM4): stop `python`, or
  `Get-Process python | Stop-Process`.
- If the laptop sleeps or Windows Update restarts it, the capture stops (that cut short
  three 2.x soaks). The dashboard log and telemetry on Railway keep running regardless.

## What tonight should show (from the checklist)

| When | Expected |
|------|----------|
| Boot (23:33) | Logger attached ~2 s late: `SSE: connected` / `SSE task started` were captured, but `Online (…) — Mira 3.0`, `Clock synced.` and `Boot — …` went by before it connected. **Check them in the dashboard log.** |
| Boot | If lights were on: one 15 s command per light, then quiet |
| :00 / :30 | Status line with the correct **Central** `[time]` |
| 01:00 | Lights-out → everything off, `Asleep` |
| Overnight | Silence: no holds, no PUTs, no resets |
| 06:00–09:00 | Wake (first light on after 6 AM, or 9 AM), then the 25→75% ramp over 15 min |
| 10:00 | Ceiling on, the 10:00–11:00 fade toward 100% (the first 60-min single command; unknown #1) |

A panic or watchdog restart should log `Boot — picked up where it left off` and switch
no lights (checklist item 10).

## Findings

_(Times are CT, from the serial log.)_

- **Oct 14 23:33**: flashed OK (DFU, 974 KB). Logger up, SSE connected.
