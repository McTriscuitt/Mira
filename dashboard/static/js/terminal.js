// The terminal's engine: reads a line, finds the command for the current directory,
// prints the result. Content comes from SITE in portfolio-content.js. Admin commands
// are in terminal-admin.js and each project's commands are in their own file (mira.js).
//
// Hiding a command here is not security, since anyone can read this file. The server
// enforces every restriction; hiding just keeps the public interface tidy.
//
// All output goes through textContent, never innerHTML, so whatever a visitor types
// or the server returns is displayed as text and can't become markup.

const Terminal = (function () {
  'use strict';

  const LAUNCH_DELAY_MS = 450;
  const CHICAGO = 'America/Chicago';

  // What may be stored word for word in the visitor log. Anything else is sent as
  // [unknown], so a password typed at the normal prompt by mistake never leaves the
  // browser. Keep identical to LOGGABLE in auth_api.py; a test compares them.
  const LOGGABLE = new RegExp(
    '^(?:help|about|projects|contact|resume|clear|privacy|ls|whoami|pwd|login|logout(?: all)?' +
    '|access(?: logins| visits| commands)?|legacy|mira|\\./mira|open mira|cd(?: mira| \\.\\.| ~| /)?' +
    '|back|exit|status|schedule(?: check| update)?|log(?: \\d{1,3})?|normal|hold|wake|sleep|nap(?: \\d{1,4})?' +
    '|extend(?: \\d{1,4}[mh]?)?|simulate|(?:floor|dresser|chest|ceiling)(?: on| off)?)$'
  );

  const terminal = document.getElementById('terminal');
  const output = document.getElementById('output');
  const statusLine = document.getElementById('status-line');
  const form = document.getElementById('prompt-form');
  const input = document.getElementById('command-input');
  const promptLabel = document.getElementById('prompt-label');

  const reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  const touchScreen = window.matchMedia('(pointer: coarse)').matches;

  const directories = {};
  const listeners = { admin: [] };
  const history = [];
  let historyIndex = 0;   // history.length means "the line being typed"
  let draft = '';         // that line, kept while browsing history
  let launching = false;
  let busy = false;       // a command is finishing, so the prompt waits
  let reader = null;      // set while ask() is waiting for an answer
  let cwd = null;         // the directory we're in; null at the top
  let admin = terminal.dataset.admin === '1';
  let csrf = terminal.dataset.csrf || '';

  // --- Output ---

  // A segment is a string, { run: 'command' }, { href: 'url', label: 'text' } or
  // { text: 'words', className: 'muted' }.
  function segmentNode(segment) {
    if (typeof segment === 'string') return document.createTextNode(segment);

    if (segment.run) {
      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'command';
      button.textContent = segment.label || segment.run;
      button.addEventListener('click', function () { submit(segment.run); });
      return button;
    }

    if (segment.href) {
      const link = document.createElement('a');
      link.className = 'link';
      link.href = segment.href;
      link.textContent = segment.label || segment.href;
      if (/^https?:/.test(segment.href)) {
        link.target = '_blank';
        link.rel = 'noopener';
      }
      return link;
    }

    const span = document.createElement('span');
    span.className = segment.className || '';
    span.textContent = segment.text;
    return span;
  }

  function buildLine(segments, className) {
    const line = document.createElement('div');
    line.className = className ? 'line ' + className : 'line';
    [].concat(segments).forEach(function (segment) {
      line.appendChild(segmentNode(segment));
    });
    return line;
  }

  function print(segments, className) {
    const line = output.appendChild(buildLine(segments, className));
    scrollToPrompt();
    return line;
  }

  // rows: [[name, description], ...], where each side is a segment or a list of them.
  function printRows(rows) {
    const grid = document.createElement('div');
    grid.className = 'rows';
    rows.forEach(function (row) {
      const name = document.createElement('div');
      [].concat(row[0]).forEach(function (s) { name.appendChild(segmentNode(s)); });
      const description = document.createElement('div');
      description.className = 'description';
      [].concat(row[1]).forEach(function (s) { description.appendChild(segmentNode(s)); });
      grid.append(name, description);
    });
    output.appendChild(grid);
    scrollToPrompt();
  }

  function promptText() {
    return (admin ? 'admin ' : '') + (cwd ? '~/' + cwd.name : '~') + ' $';
  }

  function echo(text) {
    const line = buildLine([], 'echo');
    const prompt = document.createElement('span');
    prompt.className = 'prompt';
    prompt.textContent = promptText();
    line.append(prompt, document.createTextNode(text));
    output.appendChild(line);
  }

  function scrollToPrompt() {
    window.scrollTo(0, document.documentElement.scrollHeight);
  }

  function clearScreen() {
    output.replaceChildren();
  }

  function setStatusLine(text, className) {
    statusLine.hidden = !text;
    statusLine.textContent = text || '';
    statusLine.className = className ? 'status-line ' + className : 'status-line';
  }

  // --- Prompt ---

  function refreshPrompt() {
    promptLabel.textContent = promptText();
    input.placeholder = cwd ? 'type help' : 'type mira or help';
  }

  function showPrompt(visible) {
    form.hidden = !visible;
    if (visible) focusInput();
  }

  // Asks a question the answer to which goes nowhere but the caller: no echo, no
  // history, no visitor log. Resolves to the text, or null if cancelled.
  function ask(options) {
    return new Promise(function (resolve) {
      reader = { label: options.label, resolve: resolve, submitAt: options.submitAt };
      promptLabel.textContent = options.label;
      input.value = '';
      input.placeholder = '';
      input.type = options.secret ? 'password' : 'text';
      input.inputMode = options.numeric ? 'numeric' : 'text';
      input.autocomplete = options.autocomplete || 'off';
      showPrompt(true);
      input.focus({ preventScroll: true });
      scrollToPrompt();
    });
  }

  function finishAsk(answer) {
    const current = reader;
    reader = null;
    input.value = '';
    input.type = 'text';
    input.inputMode = 'text';
    input.autocomplete = 'off';
    refreshPrompt();
    const line = buildLine([], 'echo');
    const prompt = document.createElement('span');
    prompt.className = 'prompt';
    prompt.textContent = current.label;
    line.appendChild(prompt);
    if (answer === null) line.appendChild(document.createTextNode('^C'));
    output.appendChild(line);
    current.resolve(answer);
  }

  // --- Talking to the server ---

  // Never rejects. { ok, status, data }, where status 0 means the request didn't arrive.
  function api(path, options) {
    const opts = options || {};
    const headers = {};
    if (opts.method && opts.method !== 'GET') headers['X-CSRF-Token'] = opts.csrf || csrf;
    if (opts.body) headers['Content-Type'] = 'application/json';
    return fetch(path, {
      method: opts.method || 'GET',
      headers: headers,
      body: opts.body ? JSON.stringify(opts.body) : undefined,
      credentials: 'same-origin',
    }).then(function (response) {
      return response.json().catch(function () { return null; }).then(function (data) {
        return { ok: response.ok, status: response.status, data: data };
      });
    }).catch(function () {
      return { ok: false, status: 0, data: null };
    });
  }

  function logCommand(text) {
    const line = text.trim().toLowerCase();
    fetch('/api/visit/command', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        command: LOGGABLE.test(line) ? line : '[unknown]',
        path: cwd ? '/' + cwd.name : '/',
      }),
      keepalive: true,
    }).catch(function () {});
  }

  // --- Admin state ---

  function isAdmin() { return admin; }

  function setAdmin(value, token) {
    admin = value;
    csrf = value ? token : '';
    refreshPrompt();
    listeners.admin.forEach(function (listener) { listener(value); });
  }

  function onAdminChange(listener) { listeners.admin.push(listener); }

  // Call with any admin request's result. True if the login has ended, in which case
  // the prompt has been updated and a message printed.
  function sessionEnded(result) {
    if (result.status !== 401 || !admin) return false;
    setAdmin(false);
    print('your login has ended.', 'muted');
    return true;
  }

  // --- Directories ---

  function findProject(name) {
    return SITE.projects.find(function (project) {
      return project.name === (name || '').toLowerCase();
    });
  }

  function addDirectory(directory) {
    directories[directory.name] = directory;
  }

  function enter(name, options) {
    const opts = options || {};
    cwd = directories[name];
    refreshPrompt();
    if (!opts.fromHistory && !opts.initial) {
      window.history.pushState({ dir: name }, '', '/' + name);
    }
    if (cwd.onEnter) cwd.onEnter();
  }

  function leave(options) {
    const opts = options || {};
    if (!cwd) return;
    if (cwd.onLeave) cwd.onLeave();
    cwd = null;
    refreshPrompt();
    if (!opts.fromHistory) window.history.pushState({ dir: null }, '', '/');
  }

  function launch(project) {
    launching = true;
    form.hidden = true;
    print('launching ' + project.name + '...', 'muted');
    window.setTimeout(function () {
      window.location.href = project.url;
    }, reducedMotion ? 0 : LAUNCH_DELAY_MS);
  }

  function navigate(url, label) {
    launch({ name: label, url: url });
  }

  function openProject(project) {
    if (directories[project.name]) enter(project.name);
    else launch(project);
  }

  // Handles `open mira`, `cd mira` and `./mira` at the top level.
  function openTarget(name) {
    const project = findProject(name);
    if (project) {
      openProject(project);
    } else if (!name) {
      print(['open what? try ', { run: 'projects' }], 'muted');
    } else {
      print('no such project: ' + name, 'error');
      print(['see ', { run: 'projects' }], 'muted');
    }
  }

  // `cd` everywhere: `..`, `~`, `/` and nothing all head for the top.
  function changeDirectory(args) {
    const target = (args[0] || '').replace(/\/$/, '');
    const toTop = !target || target === '..' || target === '~' || target === '/';
    if (cwd) {
      if (toTop) leave();
      else if (target.toLowerCase() === cwd.name) print('already in ' + cwd.name, 'muted');
      else print(['no such directory: ' + target + '. ', { run: 'cd ..', label: 'cd ..' }, ' goes back.'], 'error');
    } else if (toTop) {
      print('already at the top.', 'muted');
    } else {
      openTarget(target);
    }
  }

  // --- Commands ---

  // `hidden`: left out of a visitor's help. `secret`: also never offered by completion or
  // "did you mean". Both are listed in an admin's help (printAdminHelp), where `tap: false`
  // keeps a row from being a button, for commands that need more words or end the login.
  // Commands marked `everywhere` also work inside a directory.
  const ROOT_COMMANDS = {
    help: {
      description: 'list what you can do here',
      everywhere: true,
      run: function () {
        if (cwd) {
          cwd.help();
        } else {
          printRows(
            SITE.projects.map(function (project) {
              return [{ run: project.name }, project.helpLine || 'open this project'];
            }).concat(visibleCommands().map(function (name) {
              return [{ run: name }, ROOT_COMMANDS[name].description];
            }))
          );
        }
        printAdminHelp();
        printKeyboardHint();
      },
    },
    about: {
      description: 'who I am',
      run: function () {
        print(SITE.name, 'title');
        SITE.about.forEach(function (text) { print(text); });
      },
    },
    projects: {
      description: 'what I have built',
      run: function () {
        printRows(SITE.projects.map(function (project) {
          return [{ run: project.name }, project.summary];
        }));
      },
    },
    contact: {
      description: 'how to reach me',
      available: function () {
        return SITE.contact.email || SITE.contact.linkedin || SITE.contact.github;
      },
      run: function () {
        const rows = [];
        const contact = SITE.contact;
        if (contact.email) {
          rows.push(['email', { href: 'mailto:' + contact.email, label: contact.email }]);
        }
        if (contact.linkedin) {
          rows.push(['linkedin', { href: contact.linkedin, label: stripScheme(contact.linkedin) }]);
        }
        if (contact.github) {
          rows.push(['github', { href: contact.github, label: stripScheme(contact.github) }]);
        }
        printRows(rows);
      },
    },
    resume: {
      description: 'my resume',
      available: function () { return SITE.resume; },
      run: function () {
        print([{ href: SITE.resume, label: 'open my resume' }]);
      },
    },
    privacy: {
      description: 'what this site records',
      run: function () {
        SITE.privacy.forEach(function (text) { print(text); });
      },
    },
    clear: {
      description: 'clear the screen',
      everywhere: true,
      run: clearScreen,
    },
    // Terminal habits. Not listed in help, but they do what people expect.
    ls: {
      hidden: true,
      description: 'same as projects',
      run: function () { ROOT_COMMANDS.projects.run(); },
    },
    whoami: {
      hidden: true,
      description: 'same as about',
      run: function () { ROOT_COMMANDS.about.run(); },
    },
    open: {
      hidden: true,
      tap: false,
      description: 'open a project: open mira',
      run: function (args) { openTarget(args[0]); },
    },
    pwd: {
      hidden: true,
      everywhere: true,
      description: 'where you are',
      run: function () { print(cwd ? '~/' + cwd.name : '~'); },
    },
    cd: {
      hidden: true,
      everywhere: true,
      tap: false,
      description: 'cd mira enters a project, cd .. goes back',
      run: changeDirectory,
    },
    back: {
      hidden: true,
      everywhere: true,
      description: 'back to the top',
      run: function () { changeDirectory([]); },
    },
    exit: {
      hidden: true,
      everywhere: true,
      description: 'back to the top',
      run: function () { changeDirectory([]); },
    },
  };

  function addCommands(commands) {
    Object.keys(commands).forEach(function (name) {
      ROOT_COMMANDS[name] = commands[name];
    });
  }

  // Logged in, help also lists the hidden and secret commands that work here. Inside a
  // directory that's the `everywhere` ones, minus any its own help already lists
  // (`helpCovers`). Every directory gets this without doing anything.
  function printAdminHelp() {
    if (!admin) return;
    const covered = (cwd && cwd.helpCovers) || [];
    const names = Object.keys(ROOT_COMMANDS).filter(function (name) {
      const command = ROOT_COMMANDS[name];
      return (command.hidden || command.secret) && command.description
        && !command.notForAdmin && isAvailable(name)
        && (!cwd || command.everywhere) && covered.indexOf(name) < 0;
    });
    if (names.length === 0) return;
    print('');
    print('also yours, logged in:', 'muted');
    printRows(names.map(function (name) {
      const command = ROOT_COMMANDS[name];
      return [command.tap === false ? name : { run: name }, command.description];
    }));
  }

  function printKeyboardHint() {
    if (touchScreen) return;
    print('');
    print('tab completes, up and down recall earlier commands.', 'muted');
  }

  function stripScheme(url) {
    return url.replace(/^https?:\/\/(www\.)?/, '');
  }

  function has(table, name) {
    return Object.prototype.hasOwnProperty.call(table, name);
  }

  function isAvailable(name) {
    const command = ROOT_COMMANDS[name];
    return Boolean(command) && (!command.available || Boolean(command.available()));
  }

  function visibleCommands() {
    return Object.keys(ROOT_COMMANDS).filter(function (name) {
      const command = ROOT_COMMANDS[name];
      return !command.hidden && !command.secret && isAvailable(name);
    });
  }

  // Everything offered by tab completion and "did you mean". Secret commands never are.
  function knownWords() {
    if (cwd) {
      return Object.keys(cwd.commands).concat(Object.keys(ROOT_COMMANDS).filter(function (name) {
        return ROOT_COMMANDS[name].everywhere && !ROOT_COMMANDS[name].secret;
      }));
    }
    return SITE.projects.map(function (project) { return project.name; })
      .concat(Object.keys(ROOT_COMMANDS).filter(function (name) {
        return !ROOT_COMMANDS[name].secret && isAvailable(name);
      }));
  }

  function editDistance(a, b) {
    let previous = Array.from({ length: b.length + 1 }, function (_, i) { return i; });
    for (let i = 1; i <= a.length; i++) {
      const current = [i];
      for (let j = 1; j <= b.length; j++) {
        current[j] = Math.min(
          previous[j] + 1,
          current[j - 1] + 1,
          previous[j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1)
        );
      }
      previous = current;
    }
    return previous[b.length];
  }

  function closestWord(word) {
    let best = null;
    let bestDistance = 3;   // only suggest within two edits
    knownWords().forEach(function (candidate) {
      const distance = editDistance(word, candidate);
      if (distance < bestDistance) {
        best = candidate;
        bestDistance = distance;
      }
    });
    return best;
  }

  function notFound(word) {
    const suggestion = closestWord(word.toLowerCase());
    print('command not found: ' + word, 'error');
    print(suggestion
      ? ['did you mean ', { run: suggestion }, '?']
      : ['try ', { run: 'help' }], 'muted');
  }

  // A project's own command typed at the top level, such as `hold`.
  function ownerDirectory(word) {
    return Object.keys(directories).find(function (name) {
      return has(directories[name].commands, word);
    });
  }

  // Returns a promise if the command is still finishing.
  function execute(line) {
    const words = line.trim().split(/\s+/);
    const first = words[0].toLowerCase();
    const args = words.slice(1);

    if (cwd) return executeInside(first, args, words[0]);

    const project = findProject(first.replace(/^\.\//, ''));
    if (project) {
      const subcommand = args[0];
      if (subcommand && directories[project.name]) {
        print('commands run from inside ' + project.name + '.', 'error');
        print(['enter ', { run: project.name }, ' first, then type ' + subcommand + '.'], 'muted');
        return null;
      }
      openProject(project);
      return null;
    }
    if (has(ROOT_COMMANDS, first) && isAvailable(first)) {
      return ROOT_COMMANDS[first].run(args);
    }
    const owner = ownerDirectory(first);
    if (owner) {
      print(first + ' is a ' + owner + ' command.', 'error');
      print(['enter ', { run: owner }, ' first.'], 'muted');
      return null;
    }
    notFound(words[0]);
    return null;
  }

  function executeInside(first, args, typed) {
    if (has(cwd.commands, first)) return cwd.commands[first].run(args);
    if (first === 'ls') {
      cwd.help();
      return null;
    }
    if (has(ROOT_COMMANDS, first) && isAvailable(first)) {
      const command = ROOT_COMMANDS[first];
      if (command.everywhere) return command.run(args);
      if (!command.secret) {
        print(first + ' only works at the top level.', 'error');
        print([{ run: 'cd ..' }, ' goes back.'], 'muted');
        return null;
      }
    }
    if (first === cwd.name || first === './' + cwd.name) {
      print('already in ' + cwd.name, 'muted');
      return null;
    }
    notFound(typed);
    return null;
  }

  // Runs a line as if it had been typed, so clicks and typing behave the same.
  function submit(line) {
    if (launching || busy || reader) return;

    const text = line.trim();
    input.value = '';
    draft = '';

    if (text) {
      if (history[history.length - 1] !== text) history.push(text);
      historyIndex = history.length;
      echo(text);
      logCommand(text);
      const finishing = execute(text);
      if (finishing && finishing.then) {
        busy = true;
        form.hidden = true;
        finishing.finally(function () {
          busy = false;
          if (!launching) showPrompt(true);
          scrollToPrompt();
        });
        return;
      }
    }

    scrollToPrompt();
    focusInput();
  }

  // --- Input ---

  function focusInput() {
    // On a phone, focusing raises the keyboard and hides the page, so wait until
    // the visitor taps the prompt themselves.
    if (!touchScreen) input.focus({ preventScroll: true });
  }

  function recall(step) {
    const next = historyIndex + step;
    if (next < 0 || next > history.length) return;
    if (historyIndex === history.length) draft = input.value;
    historyIndex = next;
    input.value = next === history.length ? draft : history[next];
    input.setSelectionRange(input.value.length, input.value.length);
  }

  function complete() {
    const typed = input.value.trimStart().toLowerCase();
    if (!typed || /\s/.test(typed)) return;

    const matches = knownWords().filter(function (word) {
      return word.startsWith(typed);
    });

    if (matches.length === 1) {
      input.value = matches[0];
    } else if (matches.length > 1) {
      echo(input.value);
      print(matches.join('  '), 'muted');
      scrollToPrompt();
    }
  }

  input.addEventListener('keydown', function (event) {
    if (reader) {
      const cancel = event.key === 'Escape'
        || (event.ctrlKey && event.key.toLowerCase() === 'c'
            && input.selectionStart === input.selectionEnd);
      if (cancel) {
        event.preventDefault();
        finishAsk(null);
      } else if (event.key === 'Tab' || event.key === 'ArrowUp' || event.key === 'ArrowDown') {
        event.preventDefault();
      }
      return;
    }

    if (event.key === 'ArrowUp') {
      event.preventDefault();
      recall(-1);
    } else if (event.key === 'ArrowDown') {
      event.preventDefault();
      recall(1);
    } else if (event.key === 'Tab') {
      if (input.value.trim()) {
        event.preventDefault();
        complete();
      }
      // With an empty prompt, Tab moves focus as usual so keyboard users can
      // reach the links in the output.
    } else if (event.ctrlKey && event.key.toLowerCase() === 'l') {
      event.preventDefault();
      clearScreen();
    } else if (event.ctrlKey && event.key.toLowerCase() === 'c'
               && input.selectionStart === input.selectionEnd) {
      // With text selected, Ctrl+C still copies.
      event.preventDefault();
      echo(input.value + '^C');
      input.value = '';
      scrollToPrompt();
    }
  });

  // A phone's number pad may have no Enter key, so a fixed-length answer sends itself.
  // Spaces are dropped because authenticator apps copy codes as "123 456".
  input.addEventListener('input', function () {
    if (!reader || !reader.submitAt) return;
    const digits = input.value.replace(/[^0-9]/g, '');
    if (digits.length === reader.submitAt) finishAsk(digits);
  });

  form.addEventListener('submit', function (event) {
    event.preventDefault();
    if (reader) finishAsk(input.value);
    else submit(input.value);
  });

  // Clicking empty space returns to the prompt, unless the visitor is selecting
  // text to copy or clicked something that takes the click itself.
  terminal.addEventListener('click', function (event) {
    if (event.target.closest('a, button')) return;
    if (String(window.getSelection())) return;
    input.focus({ preventScroll: true });
  });

  // On a phone, a tap anywhere but a link or button raises the keyboard at the prompt.
  // The tap is handled at touchend rather than left to the click: a phone only raises
  // the keyboard for a focus() made during a tap, and the mouse events it sends after
  // one can blur the input again. Cancelling touchend stops those events, so the focus
  // made here is the last word. Scrolls, long presses and pinches aren't taps.
  let touchStart = null;

  terminal.addEventListener('touchstart', function (event) {
    const touch = event.touches[0];
    touchStart = event.touches.length === 1
      ? { x: touch.clientX, y: touch.clientY, at: event.timeStamp }
      : null;
  }, { passive: true });

  terminal.addEventListener('touchend', function (event) {
    const start = touchStart;
    touchStart = null;
    const touch = event.changedTouches[0];
    if (!start || !touch || !event.cancelable) return;
    if (event.timeStamp - start.at > 500) return;
    if (Math.abs(touch.clientX - start.x) > 10 || Math.abs(touch.clientY - start.y) > 10) return;

    if (form.hidden || event.target === input) return;   // the input handles its own taps
    if (event.target.closest('a, button')) return;
    if (String(window.getSelection())) return;            // this tap clears a selection
    event.preventDefault();
    raiseKeyboard();
  });

  // focus() does nothing to an input that already has focus, and a phone can leave it
  // focused with the keyboard down: Android's back button does, and so does a focus()
  // made outside a tap, like ask()'s after a request. Blurring first makes the focus new.
  // No preventScroll, so the browser keeps the prompt above the keyboard.
  function raiseKeyboard() {
    if (document.activeElement === input && !keyboardUp()) input.blur();
    input.focus();
  }

  // The keyboard shrinks the visible area without resizing the page. A best guess:
  // a wrong "up" leaves the keyboard down until the input itself is tapped, and a wrong
  // "down" only makes the keyboard blink.
  function keyboardUp() {
    const view = window.visualViewport;
    return !!view && document.documentElement.clientHeight - view.height * view.scale > 150;
  }

  // Coming back with the browser's back button can restore this page exactly as
  // it was left, mid-launch with the prompt hidden. Put the prompt back.
  window.addEventListener('pageshow', function () {
    if (!launching) return;
    launching = false;
    form.hidden = false;
    focusInput();
  });

  // The back and forward buttons move between the top and a project.
  window.addEventListener('popstate', function () {
    const name = window.location.pathname.replace(/^\/|\/$/g, '');
    if (directories[name]) {
      if (cwd && cwd.name === name) return;
      if (cwd) leave({ fromHistory: true });
      enter(name, { fromHistory: true });
    } else {
      leave({ fromHistory: true });
    }
  });

  // --- Small shared formatting ---

  function formatDateTime(iso) {
    const date = new Date(iso);
    return date.toLocaleDateString('en-US', { month: 'short', day: 'numeric', timeZone: CHICAGO })
      + ' ' + date.toLocaleTimeString('en-US', { hour: 'numeric', minute: '2-digit', timeZone: CHICAGO });
  }

  function formatAge(seconds) {
    if (seconds < 90) return seconds + ' s ago';
    if (seconds < 5400) return Math.round(seconds / 60) + ' min ago';
    if (seconds < 129600) return Math.round(seconds / 3600) + ' h ago';
    return Math.round(seconds / 86400) + ' days ago';
  }

  // --- Start ---

  function intro(startDir) {
    const lines = [
      print(SITE.name, 'title'),
      print(SITE.tagline, 'muted'),
      print(''),
    ];
    if (!startDir) {
      lines.push(print(['Type ', { run: 'mira' }, ' to see my lighting project, or ', { run: 'help' }, ' for more.']));
    }

    if (!reducedMotion) {
      lines.forEach(function (line, i) {
        line.classList.add('reveal');
        line.style.setProperty('--i', i);
      });
    }
  }

  document.addEventListener('DOMContentLoaded', function () {
    const startDir = directories[terminal.dataset.startDir] ? terminal.dataset.startDir : '';
    intro(startDir);
    refreshPrompt();
    window.history.replaceState({ dir: startDir || null }, '', startDir ? '/' + startDir : '/');
    if (startDir) enter(startDir, { initial: true });
    form.hidden = false;
    focusInput();
    window.scrollTo(0, 0);
  });

  return {
    addCommands: addCommands,
    addDirectory: addDirectory,
    api: api,
    ask: ask,
    clearScreen: clearScreen,
    formatAge: formatAge,
    formatDateTime: formatDateTime,
    currentDirectory: function () { return cwd ? cwd.name : null; },
    isAdmin: isAdmin,
    navigate: navigate,
    notFound: notFound,
    onAdminChange: onAdminChange,
    print: print,
    printKeyboardHint: printKeyboardHint,
    printRows: printRows,
    sessionEnded: sessionEnded,
    setAdmin: setAdmin,
    setStatusLine: setStatusLine,
  };
})();
