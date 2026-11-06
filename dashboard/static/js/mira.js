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

  function printStatus(status) {
    const out = namesWhere(status.excluded);
    const on = namesWhere(status.lights_on);
    const rows = [
      ['mode', modeText(status)],
      ['lights on', on.length ? on.join(', ') : 'none'],
    ];
    if (out.length) rows.push(['out for today', out.join(', ')]);
    if (status.ct) {
      rows.push(['schedule level', status.bri.toFixed(0) + '%, ' + status.ct + ' mirek (' + kelvin(status.ct) + ' K)']);
    }
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
    const result = await api('/api/status/latest');
    if (result.status === 429) {
      print('too many requests. try again in a minute.', 'error');
    } else if (!result.ok) {
      print('could not reach the server.', 'error');
    } else if (!result.data) {
      print('the board has not reported anything yet.', 'muted');
    } else {
      printStatus(result.data);
    }
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
    status: {
      description: 'the lights right now',
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

  function helpRow(name) {
    const command = COMMANDS[name];
    if (!command.control) return [{ run: name }, command.description];
    // Logged in, these change real lights, so a stray tap shouldn't be able to.
    if (Terminal.isAdmin()) return [name, command.description];
    return [{ run: name }, [command.description + ' ', { text: '(admin only)', className: 'tag' }]];
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
    Terminal.printRows(rows.concat([
      [{ run: 'cd ..' }, 'back to the top; back and exit work too'],
      [{ run: 'clear' }, 'clear the screen'],
    ]));
    Terminal.printKeyboardHint();
  }

  function project() {
    return SITE.projects.find(function (p) { return p.name === 'mira'; });
  }

  Terminal.addDirectory({
    name: 'mira',
    commands: COMMANDS,
    help: help,
    onEnter: function () {
      project().intro.forEach(function (text) { print(text); });
      print(['Try ', { run: 'status' }, ', ', { run: 'log' }, ' or ', { run: 'help' }, '.'], 'muted');
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
