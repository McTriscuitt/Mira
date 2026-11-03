import os
import re
from datetime import timedelta
from unittest import mock

import auth_api
from conftest import PASSWORD, admin_headers, current_code, login
from models import AccessLog, AdminSession, AuthState, LoginLock, Visit, db, utcnow


def test_login_works_and_sets_admin(client):
    assert client.get('/api/login').get_json()['available'] is True
    assert login(client).status_code == 200
    assert client.get('/api/login').get_json()['admin'] is True


def test_cookie_flags_come_from_config(app):
    assert app.config['SESSION_COOKIE_HTTPONLY'] is True
    assert app.config['SESSION_COOKIE_SAMESITE'] == 'Lax'


def test_wrong_password_and_wrong_code_look_the_same(client):
    wrong_password = login(client, password='nope', ip='1.1.1.1')
    wrong_code = login(client, code='000000', ip='2.2.2.2')
    assert wrong_password.status_code == wrong_code.status_code == 401
    assert wrong_password.get_json() == wrong_code.get_json() == {'error': 'incorrect'}


def test_failure_reason_recorded_but_never_the_password(client, app):
    login(client, password='my-secret-guess', ip='1.1.1.1')
    login(client, code='000000', ip='2.2.2.2')
    with app.app_context():
        rows = AccessLog.query.order_by(AccessLog.id).all()
        assert [r.detail for r in rows] == ['bad_password', 'bad_code']
        for row in rows:
            values = ' '.join(str(getattr(row, c.name)) for c in AccessLog.__table__.columns)
            assert 'my-secret-guess' not in values


def test_non_ascii_input_is_just_incorrect(client):
    response = login(client, password='pässwörd', code='١٢٣٤٥٦',
                     ip='3.3.3.3')
    assert response.status_code == 401


def test_login_needs_csrf_token(client):
    client.get('/api/login')
    response = client.post('/api/login', json={'password': PASSWORD, 'code': current_code()})
    assert response.status_code == 403


def test_cross_site_origin_refused(client):
    token = client.get('/api/login').get_json()['csrf']
    response = client.post('/api/login', json={'password': PASSWORD, 'code': current_code()},
                           headers={'X-CSRF-Token': token, 'Origin': 'https://evil.example'})
    assert response.status_code == 403


def test_three_failures_lock_the_address_even_with_the_right_password(client):
    for _ in range(3):
        last = login(client, password='nope', ip='1.2.3.4')
    assert last.status_code == 401 and last.get_json()['retry_s'] > 0
    blocked = login(client, ip='1.2.3.4')
    assert blocked.status_code == 429 and blocked.get_json()['error'] == 'locked'
    assert client.get('/api/login', headers={'X-Real-IP': '1.2.3.4'}).get_json()['locked_s'] > 0


def test_lock_is_per_address(client):
    for _ in range(3):
        login(client, password='nope', ip='1.2.3.4')
    assert login(client, ip='5.6.7.8').status_code == 200


def test_ipv6_lock_covers_the_whole_64(client):
    for suffix in ('1', '2', '3'):
        login(client, password='nope', ip=f'2001:db8:1:2::{suffix}')
    assert login(client, ip='2001:db8:1:2::99').status_code == 429


def test_lock_expires(client, app):
    for _ in range(3):
        login(client, password='nope', ip='1.2.3.4')
    with app.app_context():
        db.session.get(LoginLock, '1.2.3.4').locked_until = utcnow() - timedelta(seconds=1)
        db.session.commit()
    assert login(client, ip='1.2.3.4').status_code == 200


def test_old_failures_stop_counting(client, app):
    login(client, password='nope', ip='1.2.3.4')
    login(client, password='nope', ip='1.2.3.4')
    with app.app_context():
        db.session.get(LoginLock, '1.2.3.4').last_failure = utcnow() - timedelta(minutes=16)
        db.session.commit()
    login(client, password='nope', ip='1.2.3.4')
    with app.app_context():
        assert db.session.get(LoginLock, '1.2.3.4').locked_until is None


def test_success_clears_failures(client, app):
    login(client, password='nope', ip='1.2.3.4')
    assert login(client, ip='1.2.3.4').status_code == 200
    with app.app_context():
        assert db.session.get(LoginLock, '1.2.3.4') is None


def test_lockout_is_read_from_the_database(client, app):
    for _ in range(3):
        login(client, password='nope', ip='1.2.3.4')
    with app.app_context():
        db.session.remove()
        assert auth_api.lockout_remaining('1.2.3.4') > 0


def test_thirty_failures_block_every_address(client, app):
    with app.app_context():
        for i in range(29):
            db.session.add(AccessLog(role='visitor', ip=f'9.9.{i}.1', event='login_fail'))
        db.session.commit()
    assert login(client, password='nope', ip='1.1.1.1').status_code == 401
    assert login(client, ip='7.7.7.7').status_code == 429
    with app.app_context():
        assert db.session.get(AuthState, 1).global_blocked_until > utcnow()


def test_code_cannot_be_used_twice(app):
    first = app.test_client()
    second = app.test_client()
    code = current_code()
    assert login(first, code=code, ip='1.1.1.1').status_code == 200
    assert login(second, code=code, ip='2.2.2.2').status_code == 401
    with app.app_context():
        assert AccessLog.query.filter_by(detail='code_reused').count() == 1


def test_faked_forwarded_for_does_not_dodge_the_lock(client):
    for fake in ('8.8.8.1', '8.8.8.2', '8.8.8.3'):
        begin = client.get('/api/login').get_json()
        client.post('/api/login', json={'password': 'nope', 'code': '000000'},
                    headers={'X-CSRF-Token': begin['csrf'], 'X-Real-IP': '1.2.3.4',
                             'X-Forwarded-For': f'{fake}, 10.0.0.1'})
    begin = client.get('/api/login', headers={'X-Real-IP': '1.2.3.4',
                                              'X-Forwarded-For': '8.8.8.9'}).get_json()
    assert begin['locked_s'] > 0


def test_logout_revokes_a_copied_cookie(app):
    browser = app.test_client()
    login(browser)
    with browser.session_transaction() as session:
        copied = dict(session)
    thief = app.test_client()
    with thief.session_transaction() as session:
        session.update(copied)
    assert thief.get('/api/login').get_json()['admin'] is True

    assert browser.post('/api/logout', headers=admin_headers(browser)).status_code == 200
    assert 'admin' not in thief.get('/api/login').get_json()


def test_logout_all_ends_every_session(app):
    one, two = app.test_client(), app.test_client()
    login(one)
    with mock.patch('auth_api._claim_totp_step', return_value=True):
        login(two)
    one.post('/api/logout', headers=admin_headers(one), json={'all': True})
    assert 'admin' not in two.get('/api/login').get_json()
    with app.app_context():
        assert AdminSession.query.count() == 0


def test_expired_session_is_refused(client, app):
    login(client)
    with app.app_context():
        AdminSession.query.update({'expires_at': utcnow() - timedelta(seconds=1)})
        db.session.commit()
    assert 'admin' not in client.get('/api/login').get_json()


def test_unavailable_without_secrets(client, app):
    with mock.patch.dict(app.config, {'ADMIN_TOTP_SECRET': ''}):
        assert client.get('/api/login').get_json() == {'available': False}
        assert client.post('/api/login', json={}).status_code == 503


def test_access_is_admin_only_and_reports_the_address(client):
    assert client.get('/api/admin/access').status_code == 401
    login(client)
    body = client.get('/api/admin/access', headers={'X-Real-IP': '4.4.4.4'}).get_json()
    assert body['you']['ip'] == '4.4.4.4'
    assert body['entries'][0]['event'] == 'login_ok'


def test_retention_deletes_old_records(client, app):
    auth_api._last_purge = 0.0
    with app.app_context():
        db.session.add(Visit(kind='page', timestamp=utcnow() - timedelta(days=91)))
        db.session.add(Visit(kind='page', timestamp=utcnow() - timedelta(days=89)))
        db.session.commit()
    client.get('/')
    with app.app_context():
        assert Visit.query.filter(Visit.timestamp < utcnow() - timedelta(days=90)).count() == 0
        assert Visit.query.count() == 2  # the 89-day row and this page load


def test_only_known_commands_are_stored_word_for_word(client, app):
    for line, stored in [('nap 90', 'nap 90'), ('HOLD', 'hold'), ('hunter2', '[unknown]'),
                         ('nap hunter2', '[unknown]'), ('login', 'login'),
                         ('open secret', '[unknown]'), ('x' * 500, '[unknown]')]:
        client.post('/api/visit/command', json={'command': line, 'path': '/mira'})
        with app.app_context():
            assert Visit.query.order_by(Visit.id.desc()).first().command == stored


def test_server_and_browser_agree_on_what_is_loggable():
    path = os.path.join(os.path.dirname(__file__), '..', 'static', 'js', 'terminal.js')
    with open(path, encoding='utf-8') as source:
        match = re.search(r"const LOGGABLE = new RegExp\(\s*(.*?)\s*\);", source.read(), re.S)
    pieces = re.findall(r"'([^']*)'", match.group(1))
    assert ''.join(pieces).replace('\\\\', '\\') == auth_api.LOGGABLE
