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
  but the logger keeps running on its own. The Monitor's filter (status lines dropped
  first, since `ASLEEP`/`ACTIVE` would match "asleep"/"wake" every 30 s):
  ```
  tail -n 0 -F logs/mira3_first_night_2026-10-14.log \
    | grep -E -v --line-buffered '\] \[[0-9]{1,2}:[0-9]{2}[AP]M |SSE: stale|SSE: connected' \
    | grep -E -i --line-buffered "online|boot|clock|PUT|you set|hold|nap|asleep|out for today|lights-out|wake|bridge said|fail|error|panic|guru|abort|backtrace|watchdog|reset|unreachable|port lost|giving up|stopped|disconnect|reconnect|SSE:"
  ```
  (Routine 10-min `SSE: stale`/`connected` pairs are dropped after 00:15, once they
  were explained; any other `SSE:` line still comes through. Check-ins count the
  reconnects in the file.)
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
- **23:33:53 takeover**: `PUT Floor → 25.0% 392mk over 15.0s : 200`, then quiet.
  Correct: the 23:00–23:30 fade to 25% is over and the dresser is already out, so
  only the floor stays on (`on=F--- want=F---`). Checklist item 3 ✓.
- **Status line ✓**: `[11:34PM Wednesday]` is correct Central time and lands on
  :02/:32. Heap is steady at ~8.52–8.57 MB, SSE stack free is 10,112.
- **23:44:14 `SSE: stale — reconnecting`**, back at 23:44:15, about 10 min after
  the first connect. Recovered with no stray edges or commands. Worth tracking how
  often this happens overnight. A steady ~10-min rhythm would point to a bridge-side
  idle timeout, not a network problem.
- **23:50 check, nothing unusual**: 35 status lines, all `ACTIVE in-line 25.0%
  on=F---`. One command total, no holds, no errors. Heap 8,515,075–8,568,319 (no
  drift). SSE stack free fell 10,112 → 7,008 at the 23:44 reconnect and has stayed
  there. That's the low-water mark (the lowest it has ever been, so it can only go
  down). The extra use comes from the reconnect's TLS handshake plus the resync GET,
  which the first connect hadn't done yet. Peak use is ~5.3 KB of the 12,288-byte
  stack (≈57% free). Fine, but if it keeps stepping down on later reconnects, that's
  a leak or recursion, not a one-time peak.
- **23:55:22 `Asleep — everything off`**: the floor (the last light on) was
  switched off by hand. No Mira PUT came before it, so the classifier correctly read
  the off as yours, not an echo. Everything off after 21:00 → asleep, with no commands
  sent. Checklist item 9 (all off after 21:00) ✓. The 01:00 lights-out should now be a
  no-op: already asleep, nothing to switch.
- **00:05:26 second `SSE: stale`**, reconnected in 1 s. The gaps between drops so far
  are ~10.5 min (first connect → 1st drop) and ~21 min (1st → 2nd), so there's no
  fixed rhythm yet. SSE stack free stayed at 7,008: no further step down, so the
  23:44 drop was a one-time peak, not a leak.
- **00:15:29 third `SSE: stale`: explained, by design.** `SSE_STALE_TIMEOUT_MS =
  600000` (bridge.cpp:596, "Hue v2 sends no keepalive"). With nothing changing, the
  bridge sends nothing at all, so Mira reconnects after every 10 min of silence. The
  21-min gap fits too: the floor-off event at 23:55:22 reset the timer → 00:05:2x.
  Expect ~6 reconnects/hour all night while asleep, each with a resync GET. That's
  harmless, and it's how a silently dead connection gets caught within 10 min.
  Possible later tweak: none needed. A longer timeout would only delay catching a
  truly dead stream.
- **SSE stack low-water 7,008 → 6,928** (−80 B) at the 00:15 reconnect. That's a
  small extra depth on one path, not a step per reconnect (00:05 added nothing).
  Keep an eye on it: a leak would keep stepping down at each reconnect.
- **00:34 check-in, quiet ✓**: 36 status lines since 00:15, all `ASLEEP 0.0%`. No
  commands, holds or errors. One reconnect (00:30:42, 15 min after the last; the
  bridge must have sent something unrelated to lights around 00:20 that reset the
  10-min timer). SSE stack held at 6,928 through it. Heap stays in its 8.514–8.568 MB
  band. Logger alive.
