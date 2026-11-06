import copy
import os
import re

import pytest

import mira_api
import schedule
from conftest import BOARD, admin_headers, login
from models import Command, EventLog, db, utcnow

CONFIG_H = os.path.join(os.path.dirname(__file__), '..', '..', 'src', 'config.h')


def board_copy(**changes):
    body = copy.deepcopy(schedule.DEFAULT)
    body.update(changes)
    return body


# --- What visitors read ---

def test_site_copy_until_the_board_posts(client):
    body = client.get('/api/schedule').get_json()
    assert body['source'] == 'site' and body['received_at'] is None
    assert body['periods'] == schedule.DEFAULT['periods']


def test_board_copy_wins(client):
    periods = copy.deepcopy(schedule.DEFAULT['periods'])
    periods[1]['to'] = 90.0
    assert client.post('/api/schedule', headers=BOARD, json=board_copy(periods=periods)).status_code == 200
    body = client.get('/api/schedule').get_json()
    assert body['source'] == 'board' and body['received_at']
    assert body['periods'][1]['to'] == 90.0


def test_a_later_post_replaces_the_earlier(client):
    client.post('/api/schedule', headers=BOARD, json=board_copy(nap_s=1800))
    client.post('/api/schedule', headers=BOARD, json=board_copy(nap_s=2400))
    assert client.get('/api/schedule').get_json()['nap_s'] == 2400


def test_schedule_is_public_and_rate_limited(client):
    codes = [client.get('/api/schedule').status_code for _ in range(61)]
    assert codes[0] == 200 and codes[-1] == 429


# --- What the board may post ---

def test_posting_needs_the_board_key(client):
    assert client.post('/api/schedule', json=board_copy()).status_code == 401
    login(client)
    assert client.post('/api/schedule', json=board_copy(), headers=admin_headers(client)).status_code == 401


@pytest.mark.parametrize('bad', [
    None,
    [],
    {'periods': []},
    board_copy(periods=[{'start': 0, 'end': 3600, 'from': 50, 'to': 150}]),
    board_copy(periods=[{'start': -1, 'end': 3600, 'from': 50, 'to': 50}]),
    board_copy(events=[{'at': 0, 'kind': 'explode'}]),
    board_copy(events=[{'at': 0, 'kind': 'light_on', 'light': 'lamp'}]),
    board_copy(wake={'from': 25, 'to': 75, 'fade_s': 900, 'joins': {'lamp': 5}}),
    board_copy(ct={'cool': 'warm', 'warm': 400, 'warm_bri': 19.7}),
    board_copy(nap_s=True),
])
def test_malformed_schedules_are_refused(client, bad):
    assert client.post('/api/schedule', headers=BOARD, json=bad).status_code == 400
    assert client.get('/api/schedule').get_json()['source'] == 'site'


def test_unknown_fields_are_dropped(client):
    client.post('/api/schedule', headers=BOARD, json=board_copy(extra='<script>'))
    assert 'extra' not in client.get('/api/schedule').get_json()


# --- schedule update ---

def test_send_schedule_is_admin_only_and_takes_no_value(client):
    assert client.post('/api/command', json={'action': 'SEND_SCHEDULE'}).status_code == 401
    login(client)
    headers = admin_headers(client)
    assert client.post('/api/command', json={'action': 'SEND_SCHEDULE', 'value': 1},
                       headers=headers).status_code == 400
    assert client.post('/api/command', json={'action': 'SEND_SCHEDULE'},
                       headers=headers).status_code == 200


def test_send_schedule_is_confirmed_by_its_log_line(client, app):
    with app.app_context():
        command = Command(action='SEND_SCHEDULE', status='executed', executed_at=utcnow())
        db.session.add(command)
        db.session.commit()
        assert mira_api.command_outcome(command)[0] == 'picked_up'
        db.session.add(EventLog(message='Schedule sent — dashboard'))
        db.session.commit()
        assert mira_api.command_outcome(command)[0] == 'applied'


def test_send_schedule_failure_reads_as_refused(client, app):
    with app.app_context():
        command = Command(action='SEND_SCHEDULE', status='executed', executed_at=utcnow())
        db.session.add(command)
        db.session.add(EventLog(message='Schedule not sent — HTTP 500'))
        db.session.commit()
        assert mira_api.command_outcome(command) == ('ignored', 'Schedule not sent — HTTP 500')


# --- The site's copy matches the firmware's config ---

def _config():
    with open(CONFIG_H, encoding='utf-8') as source:
        text = source.read()
    values = {}
    for name, h, m in re.findall(r'#define\s+(\w+)\s+HHMM\((\d+),\s*(\d+)\)', text):
        values[name] = int(h) * 3600 + int(m) * 60
    for name, number in re.findall(r'#define\s+(\w+)\s+(-?\d+(?:\.\d+)?)(?:f|UL|L)?\b', text):
        values.setdefault(name, float(number))
    for name, alias in re.findall(r'#define\s+(\w+)\s+([A-Z_]+)\s*(?://|$)', text, re.M):
        if alias in values:
            values.setdefault(name, values[alias])
    return values


@pytest.mark.skipif(not os.path.exists(CONFIG_H), reason='firmware source not present')
def test_site_copy_matches_config_h():
    c = _config()
    # Same order as SEGS in src/timeline.cpp.
    bounds = [
        ('WAKE_WINDOW_OPEN', 'MIDDAY_FADE_START', 'MORNING_BRI', 'MORNING_BRI'),
        ('MIDDAY_FADE_START', 'MIDDAY_FADE_END', 'MORNING_BRI', 'DAY_BRI'),
        ('MIDDAY_FADE_END', 'AFTERNOON_FADE_START', 'DAY_BRI', 'DAY_BRI'),
        ('AFTERNOON_FADE_START', 'AFTERNOON_FADE_END', 'DAY_BRI', 'EVENING_BRI'),
        ('AFTERNOON_FADE_END', 'EVENING_FADE_START', 'EVENING_BRI', 'EVENING_BRI'),
        ('EVENING_FADE_START', 'EVENING_FADE_END', 'EVENING_BRI', 'NIGHT_BRI'),
        ('EVENING_FADE_END', 'LATE_FADE_START', 'NIGHT_BRI', 'NIGHT_BRI'),
        ('LATE_FADE_START', 'LATE_FADE_END', 'NIGHT_BRI', 'LATE_BRI'),
        ('LATE_FADE_END', 'LIGHTS_OUT_AT', 'LATE_BRI', 'LATE_BRI'),
    ]
    assert schedule.DEFAULT['periods'] == [
        {'start': c[a], 'end': c[b], 'from': c[f], 'to': c[t]} for a, b, f, t in bounds
    ]
    # Same order as TIMELINE_EVENTS in src/timeline.cpp.
    assert schedule.DEFAULT['events'] == [
        {'at': c['WAKE_WINDOW_OPEN'], 'kind': 'wake_window'},
        {'at': c['AUTO_WAKE'], 'kind': 'auto_wake'},
        {'at': c['CEILING_ON_AT'], 'kind': 'light_on', 'light': 'ceiling'},
        {'at': c['CEILING_OFF_AT'], 'kind': 'light_off', 'light': 'ceiling'},
        {'at': c['CHEST_OFF_AT'], 'kind': 'light_off', 'light': 'chest'},
        {'at': c['SLEEP_CUTOFF'], 'kind': 'sleep_cutoff'},
        {'at': c['DRESSER_OFF_AT'], 'kind': 'light_off', 'light': 'dresser'},
        {'at': c['LIGHTS_OUT_AT'], 'kind': 'lights_out'},
    ]
    assert schedule.DEFAULT['wake'] == {
        'from': c['WAKE_START_BRI'], 'to': c['MORNING_BRI'], 'fade_s': c['WAKE_FADE_S'],
        'joins': {'dresser': c['WAKE_JOIN_DRESSER_S'], 'chest': c['WAKE_JOIN_CHEST_S']},
    }
    assert schedule.DEFAULT['ct'] == {'cool': c['CT_COOL'], 'warm': c['CT_WARM'], 'warm_bri': c['CT_WARM_BRI']}
    assert schedule.DEFAULT['nap_s'] * 1000 == c['NAP_MS']
