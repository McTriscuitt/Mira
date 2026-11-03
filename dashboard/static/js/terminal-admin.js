// Commands that exist for the site's owner. For a visitor, each one behaves exactly
// like a command that doesn't exist, apart from `login` itself.

(function () {
  'use strict';

  const print = Terminal.print;
  const api = Terminal.api;

  function minutesFrom(seconds) {
    const minutes = Math.ceil(seconds / 60);
    return minutes === 1 ? '1 min' : minutes + ' min';
  }

  function printLocked(seconds) {
    print(seconds
      ? 'too many attempts. try again in ' + minutesFrom(seconds) + '.'
      : 'too many attempts. try again later.', 'error');
  }

  async function login() {
    if (Terminal.isAdmin()) {
      print('already logged in.', 'muted');
      return;
    }
    const begin = await api('/api/login');
    if (!begin.ok) {
      print(begin.status === 429 ? 'too many requests. try again in a minute.' : 'could not reach the server.', 'error');
      return;
    }
    if (begin.data.admin) {
      Terminal.setAdmin(true, begin.data.csrf);  // logged in from another tab
      print('already logged in.', 'muted');
      return;
    }
    if (!begin.data.available) {
      print('login is unavailable.', 'muted');
      return;
    }
    if (begin.data.locked_s) {
      printLocked(begin.data.locked_s);
      return;
    }

    const password = await Terminal.ask({
      label: 'password:',
      secret: true,
      autocomplete: 'current-password',
    });
    if (!password) {
      print('cancelled.', 'muted');
      return;
    }
    const code = await Terminal.ask({
      label: 'code:',
      numeric: true,
      autocomplete: 'one-time-code',
    });
    if (code === null) {
      print('cancelled.', 'muted');
      return;
    }

    const result = await api('/api/login', {
      method: 'POST',
      csrf: begin.data.csrf,
      body: { password: password, code: code },
    });
    if (result.ok) {
      Terminal.setAdmin(true, result.data.csrf);
      print('logged in.');
    } else if (result.status === 401) {
      print('incorrect', 'error');
      if (result.data && result.data.retry_s) printLocked(result.data.retry_s);
    } else if (result.status === 429) {
      printLocked(result.data && result.data.retry_s);
    } else if (result.status === 403) {
      print('that took too long. try again.', 'error');
    } else {
      print('could not reach the server.', 'error');
    }
  }

  async function logout(args) {
    if (!Terminal.isAdmin()) {
      Terminal.notFound('logout');
      return;
    }
    const everywhere = args[0] === 'all';
    const result = await api('/api/logout', { method: 'POST', body: everywhere ? { all: true } : {} });
    if (result.ok || result.status === 401) {
      Terminal.setAdmin(false);
      print(everywhere ? 'logged out everywhere.' : 'logged out.');
    } else {
      print('could not log out. the server said ' + result.status + '.', 'error');
    }
  }

  function column(text, width) {
    return String(text === null || text === undefined ? '-' : text).padEnd(width) + '  ';
  }

  const EVENT_LABELS = {
    login_ok: 'login',
    login_fail: 'failed',
    login_blocked: 'blocked',
    logout: 'logout',
  };

  function printAccessRow(view, row) {
    const when = column(Terminal.formatDateTime(row.t), 17);
    if (view === 'logins') {
      print(when + column(EVENT_LABELS[row.event] || row.event, 8) + column(row.ip, 15) + (row.detail || ''));
    } else if (view === 'visits') {
      const source = row.referrer ? ' from ' + row.referrer : '';
      print(when + column(row.ip, 15) + (row.path || '') + source + (row.admin ? ' (admin)' : ''));
    } else {
      print(when + column(row.ip, 15) + (row.command || '') + (row.admin ? ' (admin)' : ''));
    }
  }

  async function access(args) {
    if (!Terminal.isAdmin()) {
      Terminal.notFound('access');
      return;
    }
    const view = args[0] || 'logins';
    if (['logins', 'visits', 'commands'].indexOf(view) < 0) {
      print('access shows logins, visits or commands.', 'error');
      return;
    }
    const result = await api('/api/admin/access?view=' + view);
    if (Terminal.sessionEnded(result)) return;
    if (!result.ok) {
      print('could not load the access log.', 'error');
      return;
    }
    if (result.data.entries.length === 0) {
      print('nothing recorded yet.', 'muted');
    } else {
      result.data.entries.slice().reverse().forEach(function (row) {
        printAccessRow(view, row);
      });
    }
    print('');
    print([
      'also: ',
      { run: 'access logins' }, '  ',
      { run: 'access visits' }, '  ',
      { run: 'access commands' },
    ], 'muted');
    const you = result.data.you;
    print('you: ' + you.ip + ' (x-real-ip ' + you.x_real_ip + ', x-forwarded-for '
      + you.x_forwarded_for + ', socket ' + you.remote_addr + ')', 'muted');
  }

  function legacy() {
    if (!Terminal.isAdmin()) {
      Terminal.notFound('legacy');
      return;
    }
    Terminal.navigate('/legacy', 'old dashboard');
  }

  Terminal.addCommands({
    login: { secret: true, everywhere: true, run: login },
    logout: { secret: true, everywhere: true, run: logout },
    access: { secret: true, everywhere: true, run: access },
    legacy: { secret: true, everywhere: true, run: legacy },
  });
})();
