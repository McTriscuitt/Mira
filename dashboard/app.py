"""Mira's site: the portfolio terminal, the firmware's API and the old dashboard.

Gunicorn loads `app:app` (see Procfile). `-w 1` is load-bearing: see broadcast.py.
"""

import os
from datetime import timedelta

from flask import Flask

import auth_api
import board_api
import mira_api
import pages
from models import db, ensure_schema
from security import SESSION_DAYS, add_security_headers


def create_app():
    app = Flask(__name__)
    app.secret_key = os.environ['SECRET_KEY']

    # Only a local test run turns this on. Anything else gets the strict cookie.
    secure = os.environ.get('MIRA_INSECURE_DEV') != '1'

    database_url = os.environ['DATABASE_URL']
    if database_url.startswith('postgres://'):
        database_url = database_url.replace('postgres://', 'postgresql://', 1)

    app.config.update(
        SQLALCHEMY_DATABASE_URI=database_url,
        SQLALCHEMY_TRACK_MODIFICATIONS=False,
        # Railway can drop idle connections, and the board shouldn't see that as a 500.
        SQLALCHEMY_ENGINE_OPTIONS={'pool_pre_ping': True},
        MAX_CONTENT_LENGTH=64 * 1024,
        PERMANENT_SESSION_LIFETIME=timedelta(days=SESSION_DAYS),
        # The 90 days run from login, and the database row enforces it.
        SESSION_REFRESH_EACH_REQUEST=False,
        SESSION_COOKIE_NAME='__Host-mira' if secure else 'mira',
        SESSION_COOKIE_SECURE=secure,
        SESSION_COOKIE_HTTPONLY=True,
        SESSION_COOKIE_SAMESITE='Lax',
        ESP32_API_KEY=os.environ['ESP32_API_KEY'],
        ADMIN_PASSWORD_HASH=os.environ.get('ADMIN_PASSWORD_HASH', ''),
        ADMIN_TOTP_SECRET=os.environ.get('ADMIN_TOTP_SECRET', ''),
    )

    db.init_app(app)
    with app.app_context():
        ensure_schema()

    for module in (board_api, auth_api, mira_api, pages):
        app.register_blueprint(module.bp)

    app.after_request(lambda response: add_security_headers(response, secure))
    return app


app = create_app()
