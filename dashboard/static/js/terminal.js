// The landing page terminal: reads a line, runs a command, prints the result.
// Content comes from SITE in portfolio-content.js.
//
// All output goes through textContent, never innerHTML, so whatever a visitor
// types is displayed as text and can't become markup.

(function () {
  'use strict';

  const LAUNCH_DELAY_MS = 450;

  const terminal = document.getElementById('terminal');
  const output = document.getElementById('output');
  const form = document.getElementById('prompt-form');
  const input = document.getElementById('command-input');

  const reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  const touchScreen = window.matchMedia('(pointer: coarse)').matches;

  const history = [];
  let historyIndex = 0;   // history.length means "the line being typed"
  let draft = '';         // that line, kept while browsing history
  let launching = false;

  // --- Output ---

  // A segment is a string, { run: 'command' } or { href: 'url', label: 'text' }.
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

  function buildLine(segments, className) {
    const line = document.createElement('div');
    line.className = className ? 'line ' + className : 'line';
    [].concat(segments).forEach(function (segment) {
      line.appendChild(segmentNode(segment));
    });
    return line;
  }

  function print(segments, className) {
    return output.appendChild(buildLine(segments, className));
  }

  // rows: [[segment, segment], ...]
  function printRows(rows) {
    const grid = document.createElement('div');
    grid.className = 'rows';
    rows.forEach(function (row) {
      const name = document.createElement('div');
      name.appendChild(segmentNode(row[0]));
      const description = document.createElement('div');
      description.className = 'description';
      description.appendChild(segmentNode(row[1]));
      grid.append(name, description);
    });
    output.appendChild(grid);
  }

  function echo(text) {
    const line = buildLine([], 'echo');
    const prompt = document.createElement('span');
    prompt.className = 'prompt';
    prompt.textContent = '~ $';
    line.append(prompt, document.createTextNode(text));
    output.appendChild(line);
  }

  function scrollToPrompt() {
    window.scrollTo(0, document.documentElement.scrollHeight);
  }

  // --- Commands ---

  function findProject(name) {
    return SITE.projects.find(function (project) {
      return project.name === (name || '').toLowerCase();
    });
  }

  function launch(project) {
    launching = true;
    form.hidden = true;
    print('launching ' + project.name + '...', 'muted');
    window.setTimeout(function () {
      window.location.href = project.url;
    }, reducedMotion ? 0 : LAUNCH_DELAY_MS);
  }

  // Handles `open mira`, `cd mira` and `./mira`, which all mean the same thing.
  function openTarget(name) {
    const project = findProject(name);
    if (project) {
      launch(project);
    } else if (!name) {
      print(['open what? try ', { run: 'projects' }], 'muted');
    } else {
      print('no such project: ' + name, 'error');
      print(['see ', { run: 'projects' }], 'muted');
    }
  }

  const COMMANDS = {
    help: {
      description: 'list what you can do here',
      run: function () {
        printRows(
          SITE.projects.map(function (project) {
            return [{ run: project.name }, 'launch this project'];
          }).concat(visibleCommands().map(function (name) {
            return [{ run: name }, COMMANDS[name].description];
          }))
        );
        if (!touchScreen) {
          print('');
          print('tab completes, up and down recall earlier commands.', 'muted');
        }
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
    clear: {
      description: 'clear the screen',
      run: clearScreen,
    },
    // Terminal habits. Not listed in help, but they do what people expect.
    ls: { hidden: true, run: function () { COMMANDS.projects.run(); } },
    whoami: { hidden: true, run: function () { COMMANDS.about.run(); } },
    open: { hidden: true, run: function (args) { openTarget(args[0]); } },
    cd: { hidden: true, run: function (args) { openTarget(args[0]); } },
  };

  function stripScheme(url) {
    return url.replace(/^https?:\/\/(www\.)?/, '');
  }

  function isAvailable(name) {
    const command = COMMANDS[name];
    return Boolean(command) && (!command.available || Boolean(command.available()));
  }

  function visibleCommands() {
    return Object.keys(COMMANDS).filter(function (name) {
      return !COMMANDS[name].hidden && isAvailable(name);
    });
  }

  // Everything that can be typed first: used by tab completion and "did you mean".
  function knownWords() {
    return SITE.projects.map(function (project) { return project.name; })
      .concat(Object.keys(COMMANDS).filter(isAvailable));
  }

  function clearScreen() {
    output.replaceChildren();
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

  function execute(line) {
    const words = line.trim().split(/\s+/);
    const first = words[0].toLowerCase();
    const args = words.slice(1);

    const project = findProject(first.replace(/^\.\//, ''));
    if (project) {
      launch(project);
    } else if (Object.prototype.hasOwnProperty.call(COMMANDS, first) && isAvailable(first)) {
      COMMANDS[first].run(args);
    } else {
      const suggestion = closestWord(first);
      print('command not found: ' + words[0], 'error');
      print(suggestion
        ? ['did you mean ', { run: suggestion }, '?']
        : ['try ', { run: 'help' }], 'muted');
    }
  }

  // Runs a line as if it had been typed, so clicks and typing behave the same.
  function submit(line) {
    if (launching) return;

    const text = line.trim();
    input.value = '';
    draft = '';

    if (text) {
      if (history[history.length - 1] !== text) history.push(text);
      historyIndex = history.length;
      echo(text);
      execute(text);
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

  form.addEventListener('submit', function (event) {
    event.preventDefault();
    submit(input.value);
  });

  // Clicking empty space returns to the prompt, unless the visitor is selecting
  // text to copy or clicked something that takes the click itself.
  terminal.addEventListener('click', function (event) {
    if (event.target.closest('a, button')) return;
    if (String(window.getSelection())) return;
    input.focus({ preventScroll: true });
  });

  // Coming back with the browser's back button can restore this page exactly as
  // it was left, mid-launch with the prompt hidden. Put the prompt back.
  window.addEventListener('pageshow', function () {
    if (!launching) return;
    launching = false;
    form.hidden = false;
    focusInput();
  });

  // --- Start ---

  function intro() {
    const lines = [
      print(SITE.name, 'title'),
      print(SITE.tagline, 'muted'),
      print(''),
      print(['Type ', { run: 'mira' }, ' to launch my lighting project, or ', { run: 'help' }, ' for more.']),
    ];

    if (!reducedMotion) {
      lines.forEach(function (line, i) {
        line.classList.add('reveal');
        line.style.setProperty('--i', i);
      });
    }
  }

  intro();
  form.hidden = false;
  focusInput();
})();
