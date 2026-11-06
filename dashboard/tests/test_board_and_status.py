import os
from datetime import timedelta
from unittest import mock

from sqlalchemy.exc import SQLAlchemyError

import app as app_module
import mira_api
from conftest import BOARD, admin_headers, login, post_status
from models import Command, EventLog, StatusSnapshot, db, utcnow


def send(client, action, value=None):
    body = {'action': action}
    if value is not None:
        body['value'] = value
    return client.post('/api/command', json=body, headers=admin_headers(client))


# --- The firmware's contract ---

def test_board_routes_need_the_key(client):
    assert client.post('/api/status', json={'state': 'NORMAL'}).status_code == 401
    assert client.post('/api/log', json={'message': 'x'}).status_code == 401
    assert client.get('/api/command').status_code == 401
    assert client.post('/api/command/1/ack').status_code == 401


def test_board_routes_need_no_cookie_or_csrf(client):
    assert post_status(client).status_code == 200
    assert client.post('/api/log', headers=BOARD, json={'message': 'hi'}).status_code == 200
    assert client.get('/api/command', headers=BOARD).get_json() == {'command': None}


def test_mode_and_lights_on_are_stored(client, app):
    post_status(client, mode='NAP', state='SOFT_PAUSE')
    with app.app_context():
        snap = StatusSnapshot.query.first()
        assert snap.mode == 'NAP'
        assert snap.on_floor is True and snap.on_ceiling is False


def test_poll_and_ack_round_trip(client):
    login(client)
    command_id = send(client, 'NAP', 90).get_json()['id']
    polled = client.get('/api/command', headers=BOARD).get_json()
    assert polled == {'command': 'NAP', 'id': command_id, 'value': 90}
    assert client.post(f'/api/command/{command_id}/ack', headers=BOARD).status_code == 200
    assert client.get('/api/command', headers=BOARD).get_json() == {'command': None}


def test_stale_commands_expire_instead_of_firing(client, app):
    with app.app_context():
        db.session.add(Command(action='HOLD', created_at=utcnow() - timedelta(minutes=6)))
        db.session.commit()
    assert client.get('/api/command', headers=BOARD).get_json() == {'command': None}
    with app.app_context():
        assert Command.query.first().status == 'expired'


# --- Status: admin only ---

def test_visitors_get_no_status(client):
    post_status(client)
    assert client.get('/api/status/latest').status_code == 401


def test_status_with_no_data_yet_is_null(client):
    login(client)
    assert client.get('/api/status/latest').get_json() is None


def test_admin_status_is_live_with_memory_stats(client):
    login(client)
    post_status(client, bri=10.0)
    assert client.get('/api/status/latest').get_json()['bri'] == 10.0
    post_status(client, bri=20.0)
    body = client.get('/api/status/latest').get_json()
    assert body['bri'] == 20.0 and body['telemetry']['heap_free'] == 123456
    assert body['mode'] == 'ACTIVE' and body['online'] is True and body['timestamp']


def test_old_board_snapshot_reads_offline(client, app):
    login(client)
    post_status(client)
    with app.app_context():
        StatusSnapshot.query.update({'timestamp': utcnow() - timedelta(minutes=5)})
        db.session.commit()
    assert client.get('/api/status/latest').get_json()['online'] is False


def test_snapshot_from_before_this_change_maps_its_mode(client, app):
    login(client)
    with app.app_context():
        db.session.add(StatusSnapshot(state='LOCKED_OUT'))
        db.session.commit()
    assert client.get('/api/status/latest').get_json()['mode'] == 'ASLEEP'


def test_visitor_log_has_times_but_a_capped_size_and_no_search(client, app):
    with app.app_context():
        for i in range(60):
            db.session.add(EventLog(message=f'line {i}'))
        db.session.commit()
    body = client.get('/api/log/recent?limit=500&search=line 5').get_json()
    assert len(body) == mira_api.PUBLIC_LOG_MAX
    assert body[0]['timestamp']


def test_public_reads_are_rate_limited(client):
    codes = [client.get('/api/log/recent').status_code for _ in range(61)]
    assert codes[0] == 200 and codes[-1] == 429


# --- Admin-only ---

def test_visitors_cannot_control_anything(client):
    assert client.post('/api/command', json={'action': 'HOLD'}).status_code == 401
    assert client.post('/api/command/1/cancel').status_code == 401
    assert client.get('/api/command/1').status_code == 401
    assert client.get('/api/status/stream').status_code == 401
    assert client.get('/api/lux/history').status_code == 401
    assert client.get('/api/telemetry/history').status_code == 401


def test_commands_need_csrf_even_when_logged_in(client):
    login(client)
    assert client.post('/api/command', json={'action': 'HOLD'}).status_code == 403


def test_command_allowlist_and_bounds(client):
    login(client)
    for action, value in [('HOLD', None), ('RESUME', None), ('WAKE', None), ('SLEEP', None),
                          ('NAP', None), ('NAP', 90), ('EXTEND', 30),
                          ('LIGHT_ON', 0), ('LIGHT_OFF', 3)]:
        assert send(client, action, value).status_code == 200, action
    for action, value in [('HARD_OFF', None), ('WIND_DOWN', None), ('EVIL', None),
                          ('NAP', 0), ('NAP', 721), ('NAP', 'x'), ('EXTEND', None),
                          ('EXTEND', True), ('HOLD', 5), ('NAP_REMAINING', 0), (None, None),
                          ('LIGHT_ON', None), ('LIGHT_OFF', 4), ('LIGHT_OFF', -1),
                          ('LIGHT_ON', True), ('LIGHT_ON', 'chest'), ('EXCLUDE_LIGHT', 1)]:
        assert send(client, action, value).status_code == 400, (action, value)


def test_stream_stops_being_offered_after_logout(client):
    login(client)
    client.post('/api/logout', headers=admin_headers(client))
    assert client.get('/api/status/stream').status_code == 401


# --- Reporting what a command did ---

def outcome_of(app, command_id, after_ack_s=None):
    with app.app_context():
        command = db.session.get(Command, command_id)
        now = None
        if after_ack_s is not None:
            now = command.executed_at + timedelta(seconds=after_ack_s)
        return mira_api.command_outcome(command, now)


def ack(client, command_id):
    client.post(f'/api/command/{command_id}/ack', headers=BOARD)


def test_outcome_waiting_then_applied(client, app):
    login(client)
    command_id = send(client, 'HOLD').get_json()['id']
    assert outcome_of(app, command_id)[0] == 'waiting'
    ack(client, command_id)
    assert outcome_of(app, command_id)[0] == 'picked_up'
    post_status(client, mode='HOLD', state='SOFT_PAUSE')
    assert outcome_of(app, command_id, after_ack_s=3)[0] == 'applied'


def test_outcome_ignored_quotes_the_board(client, app):
    login(client)
    command_id = send(client, 'NAP', 30).get_json()['id']
    ack(client, command_id)
    client.post('/api/log', headers=BOARD,
                json={'message': "Nap ignored — it's past 9:00 PM (Sleep turns in for the night)"})
    outcome, note = outcome_of(app, command_id)
    assert outcome == 'ignored' and 'past 9:00 PM' in note


def test_ack_alone_is_not_success(client, app):
    login(client)
    post_status(client, mode='ACTIVE')
    command_id = send(client, 'HOLD').get_json()['id']
    ack(client, command_id)
    assert outcome_of(app, command_id, after_ack_s=5)[0] == 'picked_up'
    assert outcome_of(app, command_id, after_ack_s=31)[0] == 'unconfirmed'


def test_extend_is_confirmed_by_its_log_line(client, app):
    login(client)
    command_id = send(client, 'EXTEND', 30).get_json()['id']
    ack(client, command_id)
    client.post('/api/log', headers=BOARD,
                json={'message': 'Hold extended 30 min — until 7:30 PM'})
    assert outcome_of(app, command_id)[0] == 'applied'


def test_old_ignored_line_does_not_count(client, app):
    login(client)
    client.post('/api/log', headers=BOARD, json={'message': 'Hold ignored — asleep'})
    with app.app_context():
        EventLog.query.update({'timestamp': utcnow() - timedelta(minutes=1)})
        db.session.commit()
    command_id = send(client, 'HOLD').get_json()['id']
    ack(client, command_id)
    assert outcome_of(app, command_id)[0] != 'ignored'


def board_log(client, message):
    client.post('/api/log', headers=BOARD, json={'message': message + ' — 3:00 PM Thursday'})


def test_light_command_is_confirmed_by_its_own_line(client, app):
    login(client)
    off_id = send(client, 'LIGHT_OFF', 1).get_json()['id']
    on_id = send(client, 'LIGHT_ON', 2).get_json()['id']
    ack(client, off_id)
    ack(client, on_id)
    board_log(client, 'Chest off for today — dashboard')  # another light, the other way
    board_log(client, 'Dresser off — out for today')       # the Hue app's wording, not ours
    assert outcome_of(app, off_id)[0] == 'picked_up'
    assert outcome_of(app, on_id)[0] == 'picked_up'
    board_log(client, 'Dresser off for today — dashboard')
    board_log(client, 'Chest on — dashboard')
    outcome, note = outcome_of(app, off_id)
    assert outcome == 'applied' and note.startswith('Dresser off for today')
    assert outcome_of(app, on_id)[0] == 'applied'


def test_light_off_past_its_scheduled_off_is_confirmed(client, app):
    login(client)
    command_id = send(client, 'LIGHT_OFF', 2).get_json()['id']
    ack(client, command_id)
    board_log(client, 'Chest off — off on the schedule now')  # the Hue app's wording, not ours
    assert outcome_of(app, command_id)[0] == 'picked_up'
    board_log(client, 'Chest off — dashboard (off on the schedule now)')
    outcome, note = outcome_of(app, command_id)
    assert outcome == 'applied' and 'for today' not in note


def test_light_command_refused_by_the_board(client, app):
    login(client)
    command_id = send(client, 'LIGHT_ON', 2).get_json()['id']
    ack(client, command_id)
    board_log(client, 'Chest on ignored — asleep (Wake starts the day)')
    outcome, note = outcome_of(app, command_id)
    assert outcome == 'ignored' and 'asleep' in note


def test_command_unknown_to_older_firmware_reads_as_refused(client, app):
    login(client)
    command_id = send(client, 'LIGHT_OFF', 0).get_json()['id']
    ack(client, command_id)
    client.post('/api/log', headers=BOARD,
                json={'message': 'Dashboard command LIGHT_OFF is unknown in 3.0 — ignored'})
    outcome, note = outcome_of(app, command_id)
    assert outcome == 'ignored' and 'unknown' in note


def test_outcome_expired(app):
    with app.app_context():
        command = Command(action='HOLD', created_at=utcnow() - timedelta(minutes=6))
        db.session.add(command)
        db.session.commit()
        assert mira_api.command_outcome(command)[0] == 'expired'


# --- Pages and headers ---

def test_old_routes_are_gone(client):
    for path in ('/login', '/logs', '/lux', '/admin/access-log', '/logout'):
        assert client.get(path).status_code == 404, path


def test_legacy_sends_visitors_home(client):
    for path in ('/legacy', '/legacy/logs', '/legacy/lux'):
        response = client.get(path)
        assert response.status_code == 302 and response.location.endswith('/')


def test_headers_on_every_page(client):
    for path in ('/', '/mira', '/api/schedule'):
        response = client.get(path)
        assert response.status_code == 200, path
        policy = response.headers['Content-Security-Policy']
        assert "script-src 'self'" in policy and 'unsafe-inline' not in policy
        assert response.headers['X-Frame-Options'] == 'DENY'
        assert response.headers['Cache-Control'] == 'no-store'


def test_home_screen_icon_files_exist(client):
    for path in ('/', '/mira'):
        page = client.get(path).get_data(as_text=True)
        assert 'rel="apple-touch-icon" href="/static/img/apple-touch-icon.png"' in page
        assert 'rel="manifest" href="/static/manifest.webmanifest"' in page
    assert client.get('/static/img/apple-touch-icon.png').status_code == 200
    manifest = client.get('/static/manifest.webmanifest').get_json(force=True)
    assert manifest['start_url'] == '/' and manifest['display'] == 'standalone'
    for icon in manifest['icons']:
        assert client.get(icon['src']).status_code == 200, icon['src']


def test_visitors_get_no_cookie(client):
    assert 'Set-Cookie' not in client.get('/').headers


def test_database_outage_leaves_the_landing_page_up(client, app):
    def broken(*_args, **_kwargs):
        raise SQLAlchemyError('database is down')
    with mock.patch('auth_api.is_admin', broken), mock.patch('pages.is_admin', broken):
        assert client.get('/').status_code == 200
        assert client.get('/mira').status_code == 200


def test_production_cookie_and_headers():
    """Everything else runs in the local-test mode. This is what Railway gets."""
    with mock.patch.dict(os.environ):
        del os.environ['MIRA_INSECURE_DEV']
        production = app_module.create_app()
    secure = production.test_client()
    begin = secure.get('/api/login', base_url='https://mira.example')
    cookie = begin.headers['Set-Cookie']
    assert cookie.startswith('__Host-mira=')
    assert 'Secure' in cookie and 'HttpOnly' in cookie and 'SameSite=Lax' in cookie
    assert 'Domain' not in cookie and 'Path=/' in cookie
    assert 'max-age' in begin.headers['Strict-Transport-Security']
