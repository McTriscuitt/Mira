// Mira's directory: everything typed after entering `mira` lands here.

(function () {
  'use strict';

  const print = Terminal.print;
  const api = Terminal.api;

  const MAX_MINUTES = 720;
  const FAST_POLL_MS = 1500;
  const SLOW_POLL_MS = 5000;
  const FAST_POLL_FOR_MS = 60000;
  const STREAM_RETRY_MS = 30000;
  const DAY_S = 86400;
  // In src/lights.h order: a light command sends its light's index here.
  const LIGHTS = ['floor', 'dresser', 'chest', 'ceiling'];

  const MODE_LABELS = {
    ACTIVE: 'following the schedule',
    WAKE: 'waking up',
    HOLD: 'on hold',
    NAP: 'napping',
    ASLEEP: 'asleep',
  };

  // --- Reading status ---

  function minutesLeft(status) {
    const minutes = Math.ceil((status.remaining_s || 0) / 60);
    return minutes > 0 ? minutes + ' min left' : '';
  }

  function modeText(status) {
    const label = MODE_LABELS[status.mode] || String(status.mode).toLowerCase();
    const left = minutesLeft(status);
    return left ? label + ', ' + left : label;
  }

  function namesWhere(flags) {
    return LIGHTS.filter(function (name) { return flags && flags[name]; });
  }

  function kelvin(mirek) {
    return Math.round(1000000 / mirek / 10) * 10;
  }

  // --- Clock and schedule helpers ---

  // Local seconds since midnight, as times in a schedule are.
  function chicagoSeconds() {
    const parts = new Intl.DateTimeFormat('en-US', {
      timeZone: 'America/Chicago', hour: 'numeric', minute: 'numeric', second: 'numeric', hourCycle: 'h23',
    }).formatToParts(new Date());
    function part(type) {
      return parseInt(parts.find(function (p) { return p.type === type; }).value, 10);
    }
    return (part('hour') % 24) * 3600 + part('minute') * 60 + part('second');
  }

  function clockParts(sec) {
    const h = Math.floor(sec / 3600) % 24;
    const m = Math.floor((sec % 3600) / 60);
    return { text: (h % 12 || 12) + ':' + String(m).padStart(2, '0'), meridiem: h < 12 ? 'AM' : 'PM' };
  }

  function clock(sec) {
    const p = clockParts(((sec % DAY_S) + DAY_S) % DAY_S);
    return p.text + ' ' + p.meridiem;
  }

  function span(start, end) {
    const a = clockParts(((start % DAY_S) + DAY_S) % DAY_S);
    const b = clockParts(((end % DAY_S) + DAY_S) % DAY_S);
    return a.meridiem === b.meridiem ? a.text + '–' + b.text + ' ' + b.meridiem : clock(start) + '–' + clock(end);
  }

  function findEvent(sched, kind) {
    return sched.events.find(function (e) { return e.kind === kind; });
  }

  // The day runs from the wake window past midnight to lights-out, so times are
  // compared as seconds since the wake window (the firmware's "Mira-day seconds").
  function dayClock(sched) {
    const open = findEvent(sched, 'wake_window');
    const origin = open ? open.at : sched.periods[0].start;
    return function (sec) { return ((sec - origin) % DAY_S + DAY_S) % DAY_S; };
  }

  // The firmware's brightness-to-color-temperature map (ctForBri in src/timeline.cpp).
  function ctForBri(sched, bri) {
    const c = sched.ct;
    const t = (bri - c.warm_bri) / (100 - c.warm_bri);
    return Math.round(Math.min(c.warm, Math.max(c.cool, c.warm - (c.warm - c.cool) * t)));
  }

  function lightEvents(sched, md) {
    return sched.events
      .filter(function (e) { return e.kind === 'light_on' || e.kind === 'light_off'; })
      .map(function (e) { return { at: md(e.at), light: e.light, on: e.kind === 'light_on' }; })
      .sort(function (a, b) { return a.at - b.at; });
  }

  // The lights the schedule has on at a point in the day, after the wake.
  function lightsAt(sched, md, at) {
    const lightsOut = findEvent(sched, 'lights_out');
    if (lightsOut && at >= md(lightsOut.at)) return [];
    const on = { floor: true };
    Object.keys(sched.wake.joins).forEach(function (name) { on[name] = true; });
    lightEvents(sched, md).forEach(function (e) {
      if (e.at <= at) on[e.light] = e.on;
    });
    return LIGHTS.filter(function (name) { return on[name]; });
  }

  async function fetchSchedule() {
    const result = await api('/api/schedule');
    if (result.status === 429) {
      print('too many requests. try again in a minute.', 'error');
    } else if (!result.ok || !result.data) {
      print('could not reach the server.', 'error');
    } else {
      return result.data;
    }
    return null;
  }

  // --- Status, admin only ---

  function lightText(status, name, scheduledOn) {
    const on = Boolean(status.lights_on && status.lights_on[name]);
    const out = Boolean(status.excluded && status.excluded[name]);
    let text = on ? 'on' : (out ? 'out for today' : 'off');
    if (scheduledOn && !out && on !== Boolean(scheduledOn[name])) {
      text += scheduledOn[name] ? ' (the schedule has it on)' : ' (the schedule has it off)';
    }
    return text;
  }

  function printStatus(status, sched) {
    let mode = modeText(status);
    if (status.remaining_s > 0 && (status.mode === 'HOLD' || status.mode === 'NAP')) {
      mode += ', until ' + clock(chicagoSeconds() + status.remaining_s);
    }
    const rows = [['mode', mode]];
    if (status.ct) {
      rows.push(['schedule level', status.bri.toFixed(0) + '%, ' + status.ct + ' mirek (' + kelvin(status.ct) + ' K)'
        + (status.mode === 'HOLD' ? '; the held lights may differ' : '')]);
    }
    // Compared with the schedule only while Mira follows it: asleep, napping or
    // mid-wake, the lights are meant to differ.
    let scheduledOn = null;
    if (sched && (status.mode === 'ACTIVE' || status.mode === 'HOLD')) {
      const md = dayClock(sched);
      scheduledOn = {};
      lightsAt(sched, md, md(chicagoSeconds())).forEach(function (name) { scheduledOn[name] = true; });
    }
    LIGHTS.forEach(function (name) { rows.push([name, lightText(status, name, scheduledOn)]); });
    if (status.lux !== null && status.lux !== undefined) {
      rows.push(['room light', status.lux.toFixed(1) + ' lux']);
    }
    rows.push(['board', (status.online ? 'online, last heard ' : 'offline, last heard ')
      + Terminal.formatAge(status.age_s)]);
    if (status.telemetry && status.telemetry.heap_free) {
      const t = status.telemetry;
      rows.push(['memory', Math.round(t.heap_free / 1024) + ' KB free; stack margin '
        + t.sse_stack_free + ' sse, ' + t.net_stack_free + ' net']);
    }
    if (status.pending_command_id) rows.push(['queued', 'a command is waiting for the board']);
    Terminal.printRows(rows);
  }

  async function statusCommand() {
    if (!Terminal.isAdmin()) {
      print('status shows the board\'s live details, so it is admin only.', 'error');
      print([{ run: 'schedule' }, ' shows the day the lights follow.'], 'muted');
      return;
    }
    const results = await Promise.all([api('/api/status/latest'), api('/api/schedule')]);
    const result = results[0];
    if (Terminal.sessionEnded(result)) return;
    if (result.status === 429) {
      print('too many requests. try again in a minute.', 'error');
    } else if (!result.ok) {
      print('could not reach the server.', 'error');
    } else if (!result.data) {
      print('the board has not reported anything yet.', 'muted');
    } else {
      printStatus(result.data, results[1].ok ? results[1].data : null);
    }
  }

  // --- Schedule, public ---

  function levelText(sched, from, to) {
    if (Math.round(from) === Math.round(to)) {
      return 'steady ' + from.toFixed(0) + '%, ' + kelvin(ctForBri(sched, from)) + ' K';
    }
    return 'fade ' + from.toFixed(0) + '→' + to.toFixed(0) + '%, '
      + kelvin(ctForBri(sched, from)) + '→' + kelvin(ctForBri(sched, to)) + ' K';
  }

  function wakeText(sched) {
    const w = sched.wake;
    const open = findEvent(sched, 'wake_window');
    const auto = findEvent(sched, 'auto_wake');
    const joins = Object.keys(w.joins).sort(function (a, b) { return w.joins[a] - w.joins[b]; })
      .map(function (name) { return name + ' at +' + Math.round(w.joins[name] / 60) + ' min'; });
    let text = 'wake: ';
    if (open && auto) text += 'the first light on after ' + clock(open.at) + ', or ' + clock(auto.at) + ' at the latest. ';
    text += 'floor ' + w.from.toFixed(0) + '→' + w.to.toFixed(0) + '% over ' + Math.round(w.fade_s / 60) + ' min';
    return joins.length ? text + '; ' + joins.join(', ') : text;
  }

  // One row per stretch of the day. A period is split where a light turns on or off
  // inside it, so each row's list of lights is right.
  function scheduleRows(sched, now) {
    const md = dayClock(sched);
    const changes = lightEvents(sched, md);
    const open = findEvent(sched, 'wake_window');
    const auto = findEvent(sched, 'auto_wake');
    const rows = [[open && auto ? span(open.at, auto.at) : 'morning', wakeText(sched)]];

    sched.periods.forEach(function (p, index) {
      const start = md(p.start);
      const end = md(p.end) || DAY_S;
      const cuts = [start];
      changes.forEach(function (c) {
        if (c.at > start && c.at < end && cuts.indexOf(c.at) < 0) cuts.push(c.at);
      });
      cuts.push(end);
      function levelAt(t) { return p.from + (p.to - p.from) * (t - start) / (end - start); }
      for (let i = 0; i + 1 < cuts.length; i++) {
        const a = cuts[i];
        const b = cuts[i + 1];
        const on = lightsAt(sched, md, a);
        let text = (index === 0 && i === 0 ? 'after the wake, ' : '') + levelText(sched, levelAt(a), levelAt(b))
          + '. ' + (on.length ? on.join(', ') : 'nothing on');
        const turned = changes.filter(function (c) { return c.at === a && a !== 0; })
          .map(function (c) { return c.light + (c.on ? ' turns on' : ' turns off'); });
        if (turned.length) text += ' (' + turned.join(', ') + ')';
        const description = [text];
        if (now >= a && now < b) description.push(' ', { text: '◀ now', className: 'now' });
        rows.push([span(p.start + (a - start), p.start + (b - start)), description]);
      }
    });

    const lightsOut = findEvent(sched, 'lights_out');
    if (lightsOut) {
      const description = ['lights-out: anything still on fades off. asleep until the wake'];
      if (now >= md(lightsOut.at)) description.push(' ', { text: '◀ now', className: 'now' });
      rows.push([clock(lightsOut.at), description]);
    }
    return rows;
  }

  function printSchedule(sched) {
    const md = dayClock(sched);
    Terminal.printRows(scheduleRows(sched, md(chicagoSeconds())));
    print('');
    const cutoff = findEvent(sched, 'sleep_cutoff');
    if (cutoff) {
      print('every light off before ' + clock(cutoff.at) + ' is a ' + Math.round(sched.nap_s / 60)
        + ' min nap; from ' + clock(cutoff.at) + ' on, it means asleep.', 'muted');
    }
    print('one light turned off stays off for the rest of the day. a brightness change holds '
      + 'every light where it is until the next fade.', 'muted');
  }

  function printScheduleSource(sched) {
    if (sched.source === 'board') {
      print('the board sent this copy ' + Terminal.formatDateTime(sched.received_at) + '.');
    } else {
      print('this is the site\'s own copy; the board has not sent one yet.');
    }
    print(['schedule update', ' asks the board for its schedule now.'], 'muted');
  }

  async function scheduleCommand(args) {
    const word = (args[0] || '').toLowerCase();
    if (args.length > 1 || (word && word !== 'check' && word !== 'update')) {
      print(['usage: ', { run: 'schedule' }, ', schedule check or schedule update'], 'error');
      return null;
    }
    if (word && !Terminal.isAdmin()) {
      print('schedule ' + word + ' is admin only.', 'error');
      print([{ run: 'schedule' }, ' shows the day the lights follow.'], 'muted');
      return null;
    }
    if (word === 'update') return sendControl('schedule update', 'SEND_SCHEDULE');
    const sched = await fetchSchedule();
    if (!sched) return null;
    if (word === 'check') printScheduleSource(sched);
    else printSchedule(sched);
    return null;
  }

  // The board ends each line with its own clock and day, which the date column replaces.
  function trimBoardClock(message) {
    return message.replace(/\s+—\s+\d{1,2}:\d{2}\s?(?:AM|PM)\s+\w+day$/, '');
  }

  async function logCommand(args) {
    const count = args[0] === undefined ? 10 : parseInt(args[0], 10);
    if (!/^\d*$/.test(args[0] || '') || !(count >= 1)) {
      print(['usage: ', { run: 'log' }, ' or log 25'], 'error');
      return;
    }
    const result = await api('/api/log/recent?limit=' + count);
    if (result.status === 429) {
      print('too many requests. try again in a minute.', 'error');
    } else if (!result.ok) {
      print('could not reach the server.', 'error');
    } else if (result.data.length === 0) {
      print('nothing logged yet.', 'muted');
    } else {
      // Oldest first, so the newest line sits next to the prompt.
      result.data.slice().reverse().forEach(function (entry) {
        print([
          { text: Terminal.formatDateTime(entry.timestamp).padEnd(17) + '  ', className: 'muted' },
          trimBoardClock(entry.message),
        ]);
      });
    }
  }

  // --- Controlling the lights ---

  const DONE_TEXT = {
    RESUME: 'back on the schedule.',
    HOLD: 'on hold.',
    WAKE: 'waking up.',
    SLEEP: 'asleep.',
    NAP: 'napping.',
    EXTEND: 'extended.',
    LIGHT_ON: 'on.',
    LIGHT_OFF: 'off for today.',
    SEND_SCHEDULE: 'the board sent its schedule.',
  };

  // A light turned off after its scheduled off is just off, and the board's own line
  // says which: "Chest off for today — dashboard" or "Chest off — dashboard (...)".
  function doneText(action, note) {
    if (action === 'LIGHT_OFF' && !/off for today/.test(note || '')) return 'off.';
    return DONE_TEXT[action] || 'done.';
  }

  function wait(ms) {
    return new Promise(function (resolve) { window.setTimeout(resolve, ms); });
  }

  // Runs in the background so the prompt stays usable. An ack only means the board
  // queued the command, so the server works out whether it actually took effect.
  async function followCommand(id, action, line) {
    const started = Date.now();
    for (;;) {
      await wait(Date.now() - started < FAST_POLL_FOR_MS ? FAST_POLL_MS : SLOW_POLL_MS);
      const result = await api('/api/command/' + id);
      if (Terminal.sessionEnded(result)) return;
      if (!result.ok) continue;

      const outcome = result.data.outcome;
      if (outcome === 'waiting') continue;
      if (outcome === 'picked_up') {
        line.textContent = 'the board picked it up...';
      } else if (outcome === 'applied') {
        line.textContent = doneText(action, result.data.note);
        line.className = 'line';
        return;
      } else if (outcome === 'ignored') {
        line.textContent = 'the board refused: ' + trimBoardClock(result.data.note || '');
        line.className = 'line error';
        return;
      } else if (outcome === 'unconfirmed') {
        line.textContent = 'the board took it, but no change showed up. check status.';
        return;
      } else if (outcome === 'expired') {
        line.textContent = 'the board never picked this up, so it was dropped.';
        line.className = 'line error';
        return;
      } else {
        return;
      }
    }
  }

  async function sendControl(name, action, value) {
    if (!Terminal.isAdmin()) {
      print(name + ' changes real lights in my room, so it is not open to visitors.', 'error');
      print([{ run: 'simulate' }, ' is where you will be able to try it.'], 'muted');
      return;
    }
    const body = { action: action };
    if (value !== undefined) body.value = value;
    const result = await api('/api/command', { method: 'POST', body: body });
    if (Terminal.sessionEnded(result)) return;
    if (!result.ok) {
      print(result.status === 0 ? 'could not reach the server.' : 'the server refused that.', 'error');
      return;
    }
    if (!result.data.board_online) {
      print('the board looks offline. this runs if it comes back within 5 minutes.', 'muted');
    }
    const line = print('sent, waiting for the board...', 'muted');
    followCommand(result.data.id, action, line);
  }

  function control(name, action) {
    return function () { return sendControl(name, action); };
  }

  function runNap(args) {
    if (args.length === 0 || !Terminal.isAdmin()) return sendControl('nap', 'NAP');
    const minutes = /^\d+$/.test(args[0]) ? parseInt(args[0], 10) : 0;
    if (args.length > 1 || minutes < 1 || minutes > MAX_MINUTES) {
      print('usage: nap, or nap 90 for 90 minutes (up to ' + MAX_MINUTES + ')', 'error');
      return null;
    }
    return sendControl('nap', 'NAP', minutes);
  }

  // Accepts 30, 30m or 1h.
  function parseDuration(text) {
    const match = /^(\d+)([mh]?)$/.exec(text || '');
    if (!match) return 0;
    return parseInt(match[1], 10) * (match[2] === 'h' ? 60 : 1);
  }

  function runExtend(args) {
    if (!Terminal.isAdmin()) return sendControl('extend', 'EXTEND');
    const minutes = parseDuration((args[0] || '').toLowerCase());
    if (args.length !== 1 || minutes < 1 || minutes > MAX_MINUTES) {
      print('usage: extend 30, extend 30m or extend 1h (up to ' + MAX_MINUTES + ' minutes)', 'error');
      return null;
    }
    return sendControl('extend', 'EXTEND', minutes);
  }

  // `dresser off` keeps that light off for the rest of today; `dresser on` turns it back
  // on, in line with the schedule.
  function lightCommand(light) {
    return function (args) {
      const word = (args[0] || '').toLowerCase();
      const typed = word ? light + ' ' + word : light;
      if (!Terminal.isAdmin()) return sendControl(typed, 'LIGHT_ON');
      if (args.length !== 1 || (word !== 'on' && word !== 'off')) {
        print('usage: ' + light + ' on, or ' + light + ' off for the rest of today', 'error');
        return null;
      }
      return sendControl(typed, word === 'on' ? 'LIGHT_ON' : 'LIGHT_OFF', LIGHTS.indexOf(light));
    };
  }

  function runSimulate() {
    print('simulation: work in progress', 'title');
    print('A room you can run these commands against, driven by Mira’s real C++ core compiled to WebAssembly. Not built yet.');
  }

  // --- Live status line, admin only ---

  let stream = null;
  let lastStatus = null;
  let receivedAt = 0;
  let ageTimer = null;
  let retryTimer = null;

  function renderStatusLine() {
    if (!lastStatus) {
      Terminal.setStatusLine('waiting for the board...', 'muted');
      return;
    }
    const age = lastStatus.age_s + Math.round((Date.now() - receivedAt) / 1000);
    const on = namesWhere(lastStatus.lights_on);
    const parts = [modeText(lastStatus)];
    if (lastStatus.ct) parts.push(lastStatus.bri.toFixed(0) + '%');
    parts.push(on.length ? on.join(' ') : 'all off');
    if (age > 90) {
      Terminal.setStatusLine('board offline, last heard ' + Terminal.formatAge(age) + ' · ' + parts.join(' · '), 'error');
    } else {
      Terminal.setStatusLine(parts.join(' · ') + ' · ' + Terminal.formatAge(age), 'muted');
    }
  }

  function closeStream() {
    if (stream) stream.close();
    stream = null;
    window.clearInterval(ageTimer);
    window.clearTimeout(retryTimer);
    ageTimer = null;
    retryTimer = null;
    Terminal.setStatusLine(null);
  }

  function openStream() {
    closeStream();
    if (!window.EventSource || !Terminal.isAdmin()) return;
    renderStatusLine();
    stream = new EventSource('/api/status/stream');
    stream.addEventListener('status', function (event) {
      lastStatus = JSON.parse(event.data);
      receivedAt = Date.now();
      renderStatusLine();
    });
    stream.addEventListener('ended', function () {
      closeStream();
      Terminal.setAdmin(false);
      print('your login has ended.', 'muted');
    });
    stream.addEventListener('error', function () {
      // EventSource retries dropped connections itself, but gives up on an error
      // response such as "too many streams".
      if (stream && stream.readyState === EventSource.CLOSED) {
        stream = null;
        retryTimer = window.setTimeout(openStream, STREAM_RETRY_MS);
      }
    });
    ageTimer = window.setInterval(renderStatusLine, 1000);
  }

  // --- The directory ---

  const COMMANDS = {
    schedule: {
      description: 'the day the lights follow',
      run: scheduleCommand,
    },
    status: {
      description: 'the lights right now',
      adminOnly: true,
      run: statusCommand,
    },
    log: {
      description: 'what the lights have been doing; log 25 shows more',
      run: logCommand,
    },
    normal: {
      description: 'end a hold or nap and go back to the schedule',
      control: true,
      run: control('normal', 'RESUME'),
    },
    hold: {
      description: 'freeze the lights where they are',
      control: true,
      run: control('hold', 'HOLD'),
    },
    wake: {
      description: 'start the morning fade-in',
      control: true,
      run: control('wake', 'WAKE'),
    },
    sleep: {
      description: 'everything off for the night',
      control: true,
      run: control('sleep', 'SLEEP'),
    },
    nap: {
      description: 'lights off for 60 min, or nap 90 for 90',
      control: true,
      run: runNap,
    },
    extend: {
      description: 'add time to a hold or nap: extend 30, 30m or 1h',
      control: true,
      run: runExtend,
    },
    simulate: {
      description: 'try these commands in a simulated room',
      run: runSimulate,
    },
  };
  // One help row covers all four (see lightHelpRow).
  LIGHTS.forEach(function (light) {
    COMMANDS[light] = { light: true, control: true, run: lightCommand(light) };
  });

  function adminTag(description) {
    return [description + ' ', { text: '(admin only)', className: 'tag' }];
  }

  function helpRow(name) {
    const command = COMMANDS[name];
    if (!command.control && !command.adminOnly) return [{ run: name }, command.description];
    if (!Terminal.isAdmin()) return [{ run: name }, adminTag(command.description)];
    // Logged in, these change real lights, so a stray tap shouldn't be able to.
    return [command.control ? name : { run: name }, command.description];
  }

  function scheduleHelpRows() {
    if (!Terminal.isAdmin()) {
      return [['schedule check|update', adminTag('where the site\'s copy came from, or ask the board for it')]];
    }
    return [
      [{ run: 'schedule check' }, 'where the site\'s copy of the schedule came from, and when'],
      ['schedule update', 'ask the board to send its schedule now'],
    ];
  }

  function lightHelpRow() {
    const description = 'one light on or off for today: ' + LIGHTS.slice(0, -1).join(', ')
      + ' or ' + LIGHTS[LIGHTS.length - 1];
    if (Terminal.isAdmin()) return ['<light> on|off', description];
    return ['<light> on|off', [description + ' ', { text: '(admin only)', className: 'tag' }]];
  }

  function help() {
    const names = Object.keys(COMMANDS).filter(function (name) { return !COMMANDS[name].light; });
    const rows = names.map(helpRow);
    rows.splice(names.indexOf('simulate'), 0, lightHelpRow());
    rows.splice.apply(rows, [names.indexOf('schedule') + 1, 0].concat(scheduleHelpRows()));
    Terminal.printRows(rows.concat([
      [{ run: 'cd ..' }, 'back to the top; back and exit work too'],
      [{ run: 'clear' }, 'clear the screen'],
    ]));
  }

  function project() {
    return SITE.projects.find(function (p) { return p.name === 'mira'; });
  }

  Terminal.addDirectory({
    name: 'mira',
    commands: COMMANDS,
    help: help,
    helpCovers: ['cd', 'back', 'exit', 'clear'],  // the engine's admin help skips these
    onEnter: function () {
      project().intro.forEach(function (text) { print(text); });
      print(['Try ', { run: 'schedule' }, ', ', { run: 'log' }, ' or ', { run: 'help' }, '.'], 'muted');
      openStream();
    },
    onLeave: closeStream,
  });

  Terminal.onAdminChange(function (admin) {
    if (!admin) closeStream();
    else if (Terminal.currentDirectory() === 'mira') openStream();
  });

  window.addEventListener('pagehide', closeStream);
})();
