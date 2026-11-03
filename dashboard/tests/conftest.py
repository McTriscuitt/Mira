"""Runs the real app against a throwaway SQLite file. No Postgres or secrets needed.

    cd dashboard
    python -m pytest tests

Set MIRA_TEST_DATABASE_URL to run against a real Postgres instead.
"""

import os
import sys
import tempfile

import pyotp
import pytest
from werkzeug.security import generate_password_hash

PASSWORD = 'correct horse battery staple'
TOTP_SECRET = pyotp.random_base32()
BOARD_KEY = 'test-board-key'

_db_dir = tempfile.mkdtemp(prefix='mira-tests-')
os.environ.update(
    SECRET_KEY='test-secret',
    DATABASE_URL=os.environ.get('MIRA_TEST_DATABASE_URL')
    or 'sqlite:///' + os.path.join(_db_dir, 'test.db').replace('\\', '/'),
    ESP32_API_KEY=BOARD_KEY,
    ADMIN_PASSWORD_HASH=generate_password_hash(PASSWORD, method='scrypt'),
    ADMIN_TOTP_SECRET=TOTP_SECRET,
    MIRA_INSECURE_DEV='1',
)
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import app as app_module  # noqa: E402
import mira_api  # noqa: E402
from models import AuthState, db  # noqa: E402
from security import rate_limiter  # noqa: E402

BOARD = {'Authorization': f'Bearer {BOARD_KEY}'}


@pytest.fixture
def app():
    flask_app = app_module.app
    with flask_app.app_context():
        db.session.remove()
        db.drop_all()
        db.create_all()
        db.session.add(AuthState(id=1, last_totp_step=0))
        db.session.commit()
    rate_limiter.reset()
    mira_api.reset_caches()
    yield flask_app


@pytest.fixture
def client(app):
    return app.test_client()


def current_code():
    return pyotp.TOTP(TOTP_SECRET).now()


def login(client, password=PASSWORD, code=None, ip=None):
    headers = {'X-Real-IP': ip} if ip else {}
    begin = client.get('/api/login', headers=headers).get_json()
    headers['X-CSRF-Token'] = begin.get('csrf', '')
    return client.post('/api/login', headers=headers,
                       json={'password': password, 'code': code or current_code()})


def admin_headers(client):
    with client.session_transaction() as session:
        return {'X-CSRF-Token': session['csrf']}


def post_status(client, **overrides):
    body = {
        'state': 'NORMAL', 'mode': 'ACTIVE', 'lux': 12.5, 'bri': 75.0, 'ct': 330,
        'overhead_on': False, 'soft_pause_remaining_s': 0,
        'excluded': {'chest': True},
        'lights_on': {'floor': True, 'dresser': True},
        'heap_free': 123456, 'sse_stack_free': 2000, 'net_stack_free': 3000,
    }
    body.update(overrides)
    return client.post('/api/status', headers=BOARD, json=body)
