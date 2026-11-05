"""Drives the real site in a real browser, with a fake board. Not part of `pytest tests`:
it needs Playwright and starts its own server.

    python -m pip install playwright pyotp && python -m playwright install chromium
    cd dashboard
    python tests/e2e_terminal.py [screenshot-folder]

Nothing here can reach the real lights. It runs on a throwaway SQLite file.
"""

import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

import pyotp
from playwright.sync_api import expect, sync_playwright
from werkzeug.security import generate_password_hash

HERE = os.path.dirname(os.path.abspath(__file__))
SHOTS = sys.argv[1] if len(sys.argv) > 1 else None
PASSWORD = 'correct horse battery staple'
SECRET = pyotp.random_base32()
BOARD_KEY = 'e2e-board-key'
PORT = 5077
BASE = f'http://127.0.0.1:{PORT}'

results = []
LIGHT_NAMES = ('Floor', 'Dresser', 'Chest', 'Ceiling')  # src/lights.h order and wording


def check(name, condition, detail=''):
    results.append((name, bool(condition)))
    print(('ok   ' if condition else 'FAIL ') + name + (f'  ({detail})' if detail and not condition else ''))


class FakeBoard:
    """Speaks the firmware's HTTP contract: posts status and logs, polls commands, acks."""

    def __init__(self):
        self.mode = 'ACTIVE'
        self.remaining = 0
        self.received = []
        self.stop = threading.Event()
        self.refuse = {}

    def _call(self, path, body=None):
        data = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(
            BASE + path, data=data, method='GET' if body is None and 'ack' not in path else 'POST',
            headers={'Authorization': f'Bearer {BOARD_KEY}', 'Content-Type': 'application/json'})
        with urllib.request.urlopen(request, timeout=5) as response:
            return json.loads(response.read())

    def post_status(self):
        legacy = {'ACTIVE': 'NORMAL', 'WAKE': 'WAKE', 'HOLD': 'SOFT_PAUSE', 'NAP': 'SOFT_PAUSE',
                  'ASLEEP': 'LOCKED_OUT'}[self.mode]
        on = self.mode in ('ACTIVE', 'WAKE', 'HOLD')
        self._call('/api/status', {
            'state': legacy, 'mode': self.mode, 'lux': 42.5, 'bri': 75.0 if on else 0.0,
            'ct': 330 if on else 0, 'overhead_on': False, 'wake_step': 3, 'wake_total': 30,
            'soft_pause_remaining_s': self.remaining,
            'excluded': {'chest': True, 'floor': False, 'dresser': False, 'ceiling': False},
            'lights_on': {'floor': on, 'dresser': on, 'chest': False, 'ceiling': False},
            'heap_free': 180000, 'sse_stack_free': 2100, 'net_stack_free': 3300,
        })

    def log(self, message):
        self._call('/api/log', {'message': message + ' — 7:15 PM Tuesday'})

    def apply(self, action, value):
        self.received.append((action, value))
        if action in self.refuse:
            self.log(self.refuse[action])
        elif action == 'HOLD':
            self.mode, self.remaining = 'HOLD', 3600
            self.log('Hold — dashboard — until 8:15 PM')
        elif action == 'RESUME':
            self.mode, self.remaining = 'ACTIVE', 0
            self.log('Hold over — dashboard')
        elif action == 'NAP':
            self.mode, self.remaining = 'NAP', (value or 60) * 60
            self.log('Nap — everything off; back at 8:15 PM')
        elif action == 'EXTEND':
            self.remaining += value * 60
            self.log(f'Hold extended {value} min — until 9:00 PM')
        elif action == 'SLEEP':
            self.mode, self.remaining = 'ASLEEP', 0
            self.log('Asleep — dashboard')
        elif action == 'WAKE':
            self.mode = 'WAKE'
            self.log('Wake — dashboard (25% → 75% over 15 min)')
        elif action == 'LIGHT_OFF':
            self.log(f'{LIGHT_NAMES[value]} off for today — dashboard')
        elif action == 'LIGHT_ON':
            self.log(f'{LIGHT_NAMES[value]} on — dashboard')

    def run(self):
        while not self.stop.is_set():
            try:
                polled = self._call('/api/command')
                if polled.get('command'):
                    self._call(f"/api/command/{polled['id']}/ack")
                    self.apply(polled['command'], polled.get('value'))
                    self.post_status()
            except Exception as error:  # pylint: disable=broad-except
                print('board error', error)
            self.stop.wait(0.5)


def wait_for_server(process):
    for _ in range(100):
        if process.poll() is not None:
            raise SystemExit('server exited: ' + process.stdout.read())
        try:
            with socket.create_connection(('127.0.0.1', PORT), timeout=0.2):
                return
        except OSError:
            time.sleep(0.1)
    raise SystemExit('server did not start')


def start_server():
    db_path = os.path.join(tempfile.mkdtemp(prefix='mira-e2e-'), 'e2e.db').replace('\\', '/')
    database_url = os.environ.get('MIRA_TEST_DATABASE_URL') or 'sqlite:///' + db_path
    env = dict(os.environ,
               SECRET_KEY='e2e', DATABASE_URL=database_url, ESP32_API_KEY=BOARD_KEY,
               ADMIN_PASSWORD_HASH=generate_password_hash(PASSWORD, method='scrypt'),
               ADMIN_TOTP_SECRET=SECRET, MIRA_INSECURE_DEV='1', PYTHONWARNINGS='ignore')
    code = ('from app import app; '
            f"app.run(port={PORT}, threaded=True, use_reloader=False)")
    process = subprocess.Popen([sys.executable, '-c', code], cwd=os.path.dirname(HERE), env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    threading.Thread(target=process.stdout.read, daemon=True).start()
    wait_for_server(process)
    return process


class Term:
    def __init__(self, page):
        self.page = page

    def type(self, line):
        box = self.page.locator('#command-input')
        box.fill(line)
        box.press('Enter')

    def text(self):
        return self.page.locator('#output').inner_text()

    def prompt(self):
        return self.page.locator('#prompt-label').inner_text()

    def expect_text(self, text, timeout=8000):
        expect(self.page.locator('#output')).to_contain_text(text, timeout=timeout)

    def last_lines(self, count=4):
        return '\n'.join(self.text().strip().split('\n')[-count:])

    def shot(self, name):
        if SHOTS:
            self.page.screenshot(path=os.path.join(SHOTS, name + '.png'))


def visitor_checks(term, board, page, csp_errors):
    page.goto(BASE + '/')
    term.expect_text('Tristan DeOrnellis')
    check('prompt starts at ~ $', term.prompt() == '~ $', term.prompt())
    term.shot('01-landing')

    term.type('help')
    term.expect_text('what this site records')
    help_text = term.text()
    for secret in ('login', 'logout', 'access', 'legacy'):
        check(f'root help hides {secret}', secret not in help_text)
    term.type('clear')

    term.type('logi')
    term.expect_text('command not found: logi')
    check('no suggestion of login', 'login' not in term.text(), term.text())
    term.type('clear')

    box = page.locator('#command-input')
    box.fill('lo')
    box.press('Tab')
    check('tab does not complete login', box.input_value() == 'lo', box.input_value())
    box.fill('')
    term.type('clear')

    term.type('access')
    term.expect_text('command not found: access')
    term.type('legacy')
    term.expect_text('command not found: legacy')
    term.type('logout')
    term.expect_text('command not found: logout')
    term.type('clear')

    term.type('hold')
    term.expect_text('hold is a mira command')
    term.type('mira hold')
    term.expect_text('enter mira first')
    term.type('dresser off')
    term.expect_text('dresser is a mira command')
    term.type('clear')

    term.type('mira')
    term.expect_text('Mira runs the lights')
    check('prompt shows the directory', term.prompt() == '~/mira $', term.prompt())
    check('address follows', page.url.endswith('/mira'), page.url)
    check('visitor has no status line', not page.locator('#status-line').is_visible())
    term.shot('02-mira-visitor')

    term.type('about')
    term.expect_text('only works at the top level')
    term.type('clear')

    term.type('help')
    term.expect_text('admin only')
    help_text = term.text()
    for name in ('status', 'log', 'normal', 'hold', 'wake', 'sleep', 'nap', 'extend', 'simulate',
                 '<light> on|off', 'floor, dresser, chest or ceiling'):
        check(f'mira help lists {name}', name in help_text)
    for secret in ('login', 'logout', 'access', 'legacy'):
        check(f'mira help hides {secret}', secret not in help_text)
    term.shot('03-mira-help-visitor')
    term.type('clear')

    received_before = len(board.received)
    for attempt in ('hold', 'nap 90', 'extend 30', 'sleep', 'wake', 'normal', 'dresser off',
                    'chest on', 'ceiling'):
        term.type(attempt)
        term.expect_text('not open to visitors')
        term.type('clear')
    term.type('simulate')
    term.expect_text('work in progress')
    term.type('clear')
    check('visitor commands reached nothing', len(board.received) == received_before)

    term.type('status')
    term.expect_text('following the schedule')
    term.expect_text('floor, dresser')
    term.expect_text('out for today')
    check('visitor sees no memory stats', 'memory' not in term.text())
    term.shot('04-status-visitor')
    term.type('clear')

    term.type('log')
    term.expect_text('Boot')
    check('board clock is trimmed from log lines', 'Tuesday' not in term.text(), term.text())
    check('log is oldest first', term.text().find('Boot') < term.text().find('Fading'))
    term.type('clear')

    term.type('cd ..')
    expect(page.locator('#prompt-label')).to_have_text('~ $')
    check('address returns to /', page.url.rstrip('/') == BASE, page.url)
    term.type('mira')
    term.type('back')
    expect(page.locator('#prompt-label')).to_have_text('~ $')
    term.type('mira')
    term.type('exit')
    expect(page.locator('#prompt-label')).to_have_text('~ $')

    term.type('mira')
    expect(page.locator('#prompt-label')).to_have_text('~/mira $')
    page.go_back()
    expect(page.locator('#prompt-label')).to_have_text('~ $')
    page.go_forward()
    expect(page.locator('#prompt-label')).to_have_text('~/mira $')

    page.goto(BASE + '/mira')
    expect(page.locator('#prompt-label')).to_have_text('~/mira $')
    check('/mira lands inside mira', 'Mira runs the lights' in term.text())

    page.goto(BASE + '/legacy')
    check('/legacy sends a visitor home', page.url.rstrip('/') == BASE, page.url)

    term.type('privacy')
    term.expect_text('deleted after 90 days')
    check('no cookies for a visitor', page.context.cookies() == [], page.context.cookies())
    check('no CSP violations so far', not csp_errors, csp_errors)


def login_checks(term, page):
    page.goto(BASE + '/')
    term.expect_text('Tristan')
    box = page.locator('#command-input')

    term.type('login')
    expect(page.locator('#prompt-label')).to_have_text('password:')
    check('password input is masked', box.get_attribute('type') == 'password')
    term.shot('05-login-password')
    box.fill('wrong password here')
    box.press('Enter')
    expect(page.locator('#prompt-label')).to_have_text('code:')
    check('input is plain again for the code', box.get_attribute('type') == 'text')
    box.press_sequentially('00000')
    check('five digits wait for the sixth', page.locator('#prompt-label').text_content() == 'code:')
    box.press_sequentially('0')
    term.expect_text('incorrect')  # sent without Enter: a phone's number pad may not have one
    check('password never shown', 'wrong password here' not in term.text())

    box.fill('')
    box.press('ArrowUp')
    check('password not in history', box.input_value() == 'login', box.input_value())
    box.fill('')

    term.type('login')
    expect(page.locator('#prompt-label')).to_have_text('password:')
    box.press('Escape')
    term.expect_text('cancelled')
    expect(page.locator('#prompt-label')).to_have_text('~ $')
    check('cancel leaves a normal input', box.get_attribute('type') == 'text')

    term.type('login')
    expect(page.locator('#prompt-label')).to_have_text('password:')
    box.fill(PASSWORD)
    box.press('Enter')
    expect(page.locator('#prompt-label')).to_have_text('code:')
    now = pyotp.TOTP(SECRET).now()
    box.fill(now[:3] + ' ' + now[3:])  # the way an authenticator app copies it
    term.expect_text('logged in.')
    expect(page.locator('#prompt-label')).to_have_text('admin ~ $')
    cookies = page.context.cookies()
    check('session cookie is HttpOnly', cookies and all(c['httpOnly'] for c in cookies), cookies)
    check('password not in output', PASSWORD not in term.text())
    term.shot('06-logged-in')

    page.reload()
    expect(page.locator('#prompt-label')).to_have_text('admin ~ $')
    check('login survives a reload', True)


def admin_checks(term, board, page):
    term.type('help')
    term.expect_text('what this site records')
    check('admin help still hides logout', 'logout' not in term.text())
    term.type('clear')

    term.type('access')
    term.expect_text('failed')
    term.expect_text('bad_password')
    term.expect_text('you: ')
    term.shot('07-access-logins')
    term.type('clear')
    term.type('hunter2-typed-by-mistake')
    term.expect_text('command not found')
    term.type('clear')
    term.type('access commands')
    term.expect_text('[unknown]')
    check('mistyped text never stored', 'hunter2' not in term.text())
    check('failed password absent from access', 'wrong password here' not in term.text())
    term.type('clear')

    term.type('mira')
    expect(page.locator('#prompt-label')).to_have_text('admin ~/mira $')
    expect(page.locator('#status-line')).to_contain_text('following the schedule')
    term.shot('08-status-line')

    term.type('help')
    term.expect_text('freeze the lights')
    check('control rows not clickable when logged in',
          page.locator('.rows .command', has_text='sleep').count() == 0)
    check('admin help has no admin-only tag', 'admin only' not in term.text())
    term.type('clear')

    term.type('hold')
    term.expect_text('on hold.', timeout=15000)
    expect(page.locator('#status-line')).to_contain_text('on hold, 60 min left')
    check('board received HOLD', board.received[-1] == ('HOLD', None), board.received[-1:])
    term.shot('09-hold-applied')

    term.type('extend 1h')
    term.expect_text('extended.', timeout=15000)
    check('1h became 60 minutes', board.received[-1] == ('EXTEND', 60), board.received[-1:])

    term.type('normal')
    term.expect_text('back on the schedule.', timeout=15000)
    term.type('clear')

    term.type('nap')
    term.expect_text('napping.', timeout=15000)
    check('nap default sends no value', board.received[-1] == ('NAP', None), board.received[-1:])
    term.type('clear')
    term.type('normal')
    term.expect_text('back on the schedule.', timeout=15000)
    term.type('clear')

    term.type('nap 90')
    term.expect_text('napping.', timeout=15000)
    check('nap 90 sends 90', board.received[-1] == ('NAP', 90), board.received[-1:])
    term.type('clear')
    term.type('normal')
    term.expect_text('back on the schedule.', timeout=15000)
    term.type('clear')

    board.refuse['NAP'] = "Nap ignored — it's past 9:00 PM (Sleep turns in for the night)"
    term.type('nap')
    term.expect_text('the board refused', timeout=15000)
    term.expect_text('past 9:00 PM')
    board.refuse.clear()
    term.shot('10-nap-refused')
    term.type('clear')

    term.type('dresser off')
    term.expect_text('off for today.', timeout=15000)
    check('dresser off sends LIGHT_OFF 1', board.received[-1] == ('LIGHT_OFF', 1), board.received[-1:])
    term.type('clear')
    term.type('Chest ON')
    expect(page.locator('#output .line').last).to_have_text('on.', timeout=15000)
    check('chest on sends LIGHT_ON 2', board.received[-1] == ('LIGHT_ON', 2), board.received[-1:])
    term.shot('10b-light-commands')
    term.type('clear')

    board.refuse['LIGHT_ON'] = 'Ceiling on ignored — asleep (Wake starts the day)'
    term.type('ceiling on')
    term.expect_text('the board refused', timeout=15000)
    term.expect_text('Ceiling on ignored — asleep')
    board.refuse['LIGHT_OFF'] = 'Dashboard command LIGHT_OFF is unknown in 3.0 — ignored'
    term.type('floor off')
    term.expect_text('LIGHT_OFF is unknown', timeout=15000)
    board.refuse.clear()
    term.type('clear')

    received_before = len(board.received)
    term.type('dresser')
    term.expect_text('usage: dresser on')
    term.type('chest maybe')
    term.expect_text('usage: chest on')
    check('a usage error sends nothing', len(board.received) == received_before, board.received[-1:])
    term.type('clear')

    term.type('nap banana')
    term.expect_text('usage: nap')
    term.type('extend')
    term.expect_text('usage: extend')
    term.type('clear')

    term.type('status')
    term.expect_text('memory')
    term.expect_text('KB free')
    term.shot('11-status-admin')

    term.type('legacy')
    page.wait_for_url('**/legacy')


def legacy_checks(page, board, csp_errors):
    expect(page.locator('#progress-state-name')).to_have_text('Normal', timeout=8000)
    check('lux shows', page.locator('#val-lux').inner_text() == '43', page.locator('#val-lux').inner_text())
    labels = page.locator('.btn').all_inner_texts()
    check('five buttons', [l.lower() for l in labels] == ['normal', 'hold', 'wake', 'sleep', 'nap'], labels)
    check('floor chip lit', 'in-cycle' in page.locator('[data-light=floor]').get_attribute('class'))
    check('chest chip dimmed', 'excluded' in page.locator('[data-light=chest]').get_attribute('class'))
    check('old lux widget gone', page.locator('#lux-widget-canvas').count() == 0)
    if SHOTS:
        page.screenshot(path=os.path.join(SHOTS, '12-legacy-normal.png'), full_page=True)

    page.locator('.btn[data-action=HOLD]').click()
    expect(page.locator('#progress-state-name')).to_have_text('On hold', timeout=10000)
    expect(page.locator('#progress-text')).to_contain_text('min remaining')
    check('slider is display-only', page.locator('#progress-slider').is_disabled())
    if SHOTS:
        page.screenshot(path=os.path.join(SHOTS, '13-legacy-hold.png'), full_page=True)

    page.locator('#extend-btn').click()
    expect(page.locator('#progress-text')).to_contain_text('70 min remaining', timeout=10000)
    check('+10 uses EXTEND', board.received[-1] == ('EXTEND', 10), board.received[-1:])

    page.locator('#finish-btn').click()
    expect(page.locator('#progress-state-name')).to_have_text('Normal', timeout=10000)

    page.locator('.btn[data-action=SLEEP]').click()
    expect(page.locator('#sleep-btn')).to_have_text('confirm?')
    page.locator('.btn[data-action=SLEEP]').click()
    expect(page.locator('#progress-state-name')).to_have_text('Asleep', timeout=10000)
    page.locator('.btn[data-action=WAKE]').click()
    expect(page.locator('#progress-state-name')).to_have_text('Waking up', timeout=10000)
    board.mode = 'ACTIVE'
    board.post_status()

    page.goto(BASE + '/legacy/logs')
    expect(page.locator('#log-list')).to_contain_text('Hold', timeout=8000)
    page.locator('.filter-btn', has_text='nap').click()
    expect(page.locator('#log-list')).to_contain_text('Nap', timeout=8000)
    if SHOTS:
        page.screenshot(path=os.path.join(SHOTS, '14-legacy-logs.png'))

    page.goto(BASE + '/legacy/lux')
    expect(page.locator('#tl-info')).to_contain_text('lux', timeout=8000)
    if SHOTS:
        page.screenshot(path=os.path.join(SHOTS, '15-legacy-lux.png'))
    check('no CSP violations on the old pages', not csp_errors, csp_errors)

    page.goto(BASE + '/legacy')
    page.locator('[data-logout]').click()
    page.wait_for_url(BASE + '/')
    expect(page.locator('#prompt-label')).to_have_text('~ $')
    page.goto(BASE + '/legacy')
    check('logout ended legacy access', page.url.rstrip('/') == BASE, page.url)


def stream_ends_on_logout(browser):
    """A stream in one tab has to notice a logout in another."""
    context = browser.new_context()
    page = context.new_page()
    term = Term(page)
    page.goto(BASE + '/mira')
    # A fresh code step, since the earlier login used the current one.
    time.sleep(31)
    term.type('login')
    expect(page.locator('#prompt-label')).to_have_text('password:')
    box = page.locator('#command-input')
    box.fill(PASSWORD)
    box.press('Enter')
    expect(page.locator('#prompt-label')).to_have_text('code:')
    box.fill(pyotp.TOTP(SECRET).now())
    expect(page.locator('#prompt-label')).to_have_text('admin ~/mira $')
    expect(page.locator('#status-line')).to_contain_text('schedule')
    check('status line appears on login inside mira', True)

    other = context.new_page()
    other_term = Term(other)
    other.goto(BASE + '/')
    expect(other.locator('#prompt-label')).to_have_text('admin ~ $')
    other_term.type('logout')
    other_term.expect_text('logged out.')
    expect(other.locator('#prompt-label')).to_have_text('~ $')

    board_wake = FakeBoard()
    board_wake.post_status()
    expect(page.locator('#prompt-label')).to_have_text('~/mira $', timeout=30000)
    term.expect_text('your login has ended')
    check('other tab noticed the logout', True)
    check('status line gone', not page.locator('#status-line').is_visible())

    box.fill('hold')
    box.press('Enter')
    term.expect_text('not open to visitors')
    context.close()


def phone_checks(browser):
    context = browser.new_context(viewport={'width': 390, 'height': 760}, has_touch=True,
                                  is_mobile=True, device_scale_factor=2)
    page = context.new_page()
    term = Term(page)
    page.goto(BASE + '/mira')
    expect(page.locator('#prompt-label')).to_have_text('~/mira $')
    page.locator('.command', has_text='status').first.tap()
    term.expect_text('following the schedule')
    term.shot('16-phone-status')
    page.locator('#command-input').fill('help')
    page.locator('#command-input').press('Enter')
    term.expect_text('admin only')
    term.shot('17-phone-help')
    width = page.evaluate('document.documentElement.scrollWidth')
    check('no sideways scroll on a phone', width <= 390, width)
    context.close()


def main():
    server = start_server()
    board = FakeBoard()
    board.log('Boot — lights on; taking over')
    board.log('Fading to 75% by 6:00 PM')
    board.post_status()
    runner = threading.Thread(target=board.run, daemon=True)
    runner.start()

    try:
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch()
            context = browser.new_context(viewport={'width': 1100, 'height': 760})
            page = context.new_page()
            csp_errors = []
            page.on('console', lambda m: csp_errors.append(m.text)
                    if 'Content Security Policy' in m.text else None)
            page.on('pageerror', lambda e: csp_errors.append(str(e)))
            term = Term(page)

            visitor_checks(term, board, page, csp_errors)
            login_checks(term, page)
            admin_checks(term, board, page)
            legacy_checks(page, board, csp_errors)
            context.close()
            stream_ends_on_logout(browser)
            phone_checks(browser)
            browser.close()
    finally:
        board.stop.set()
        server.terminate()

    failed = [name for name, passed in results if not passed]
    print(f'\n{len(results) - len(failed)} of {len(results)} checks passed')
    for name in failed:
        print('FAILED:', name)
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
