"""The terminal's login, plus what it records about logins and visitors."""

import hmac
import re
import threading
import time
from datetime import timedelta

import pyotp
from flask import Blueprint, current_app, jsonify, request
from sqlalchemy import update
from werkzeug.security import check_password_hash

from models import AccessLog, AdminSession, AuthState, LoginLock, Visit, db, iso, utcnow
from security import (admin_required, client_ip, csrf_ok, csrf_token, end_admin_session,
                      is_admin, lock_key, rate_limited, same_origin, start_admin_session,
                      user_agent)

bp = Blueprint('auth', __name__)

IP_MAX_FAILURES = 3
IP_LOCK = timedelta(minutes=15)
GLOBAL_MAX_FAILURES = 30
GLOBAL_WINDOW = timedelta(hours=1)
GLOBAL_LOCK = timedelta(hours=1)
RETENTION = timedelta(days=90)
MAX_PASSWORD_LENGTH = 1024  # scrypt's cost shouldn't be something a caller can inflate
TOTP_STEP_S = 30

# One login at a time. Without this, a burst of parallel attempts could all pass the
# lockout check before any of them recorded a failure.
_login_mutex = threading.Lock()

# What may be stored word for word. Anything else is stored as [unknown], so a password
# typed at the normal prompt by mistake never reaches the log. Keep this identical to
# LOGGABLE in static/js/terminal.js; a test compares them.
LOGGABLE = (
    r'^(?:help|about|projects|contact|resume|clear|privacy|ls|whoami|pwd|login|logout(?: all)?'
    r'|access(?: logins| visits| commands)?|legacy|mira|\./mira|open mira|cd(?: mira| \.\.| ~| /)?'
    r'|back|exit|status|log(?: \d{1,3})?|normal|hold|wake|sleep|nap(?: \d{1,4})?'
    r'|extend(?: \d{1,4}[mh]?)?|simulate)$'
)
_LOGGABLE_RE = re.compile(LOGGABLE)

_last_purge = 0.0
_purge_mutex = threading.Lock()


def admin_configured():
    config = current_app.config
    return bool(config.get('ADMIN_PASSWORD_HASH') and config.get('ADMIN_TOTP_SECRET'))


# --- Lockout ---

def _seconds_until(moment, now):
    return max(1, int((moment - now).total_seconds()) + 1)


def lockout_remaining(ip, now=None):
    """Seconds until this address may try again; 0 means it may try now."""
    now = now or utcnow()
    state = db.session.get(AuthState, 1)
    if state and state.global_blocked_until and state.global_blocked_until > now:
        return _seconds_until(state.global_blocked_until, now)
    lock = db.session.get(LoginLock, lock_key(ip))
    if lock and lock.locked_until and lock.locked_until > now:
        return _seconds_until(lock.locked_until, now)
    return 0


def _record_failure(ip, detail):
    now = utcnow()
    key = lock_key(ip)
    lock = db.session.get(LoginLock, key)
    if lock is None:
        lock = LoginLock(ip=key, failures=0)
        db.session.add(lock)
    if lock.last_failure is None or now - lock.last_failure >= IP_LOCK:
        lock.failures = 0
    lock.failures += 1
    lock.last_failure = now
    if lock.failures >= IP_MAX_FAILURES:
        lock.locked_until = now + IP_LOCK
        lock.failures = 0

    db.session.add(AccessLog(role='visitor', ip=ip, event='login_fail', detail=detail,
                             user_agent=user_agent()))
    db.session.flush()

    recent = AccessLog.query.filter(AccessLog.event == 'login_fail',
                                    AccessLog.timestamp > now - GLOBAL_WINDOW).count()
    if recent >= GLOBAL_MAX_FAILURES:
        db.session.get(AuthState, 1).global_blocked_until = now + GLOBAL_LOCK
    db.session.commit()


# --- Checking a login ---

def _password_ok(password):
    try:
        return check_password_hash(current_app.config['ADMIN_PASSWORD_HASH'], password)
    except (ValueError, TypeError):
        current_app.logger.error('ADMIN_PASSWORD_HASH could not be read')
        return False


def _matching_totp_step(code, now_s=None):
    """The time step this code belongs to, allowing one step of clock drift, or None."""
    totp = pyotp.TOTP(current_app.config['ADMIN_TOTP_SECRET'])
    step = int(now_s if now_s is not None else time.time()) // TOTP_STEP_S
    for candidate in (step - 1, step, step + 1):
        if hmac.compare_digest(totp.at(candidate * TOTP_STEP_S), code):
            return candidate
    return None


def _claim_totp_step(step):
    """Each code works once, so one read over a shoulder or from a log can't be replayed
    inside its 30 seconds. True if this step hadn't been used."""
    result = db.session.execute(
        update(AuthState)
        .where(AuthState.id == 1, AuthState.last_totp_step < step)
        .values(last_totp_step=step)
    )
    return result.rowcount == 1


def _check_credentials(password, code):
    """Returns None on success, or why it failed. The caller only ever shows `incorrect`."""
    # Both are always evaluated, so timing doesn't reveal which one was wrong.
    password_right = _password_ok(password)
    step = _matching_totp_step(code)
    if not password_right:
        return 'bad_password'
    if step is None:
        return 'bad_code'
    if not _claim_totp_step(step):
        return 'code_reused'
    return None


# --- Routes ---

@bp.route('/api/login', methods=['GET'])
@rate_limited('login-begin', 30)
def login_begin():
    """What the terminal asks when `login` is typed, before it prompts for anything."""
    if is_admin():
        return jsonify({'admin': True, 'csrf': csrf_token()})
    if not admin_configured():
        return jsonify({'available': False})
    return jsonify({'available': True, 'csrf': csrf_token(),
                    'locked_s': lockout_remaining(client_ip())})


@bp.route('/api/login', methods=['POST'])
@rate_limited('login-submit', 30)
def login_submit():
    if not admin_configured():
        return jsonify({'error': 'unavailable'}), 503
    if not csrf_ok():
        return jsonify({'error': 'expired'}), 403

    data = request.get_json(silent=True)
    data = data if isinstance(data, dict) else {}
    password = data.get('password')
    code = data.get('code')
    if not isinstance(password, str) or len(password) > MAX_PASSWORD_LENGTH:
        password = ''
    code = code.strip().replace(' ', '') if isinstance(code, str) else ''
    if not re.fullmatch(r'[0-9]{6}', code):
        code = '000000x'  # can't match, but still goes through the same checks

    ip = client_ip()
    with _login_mutex:
        locked_s = lockout_remaining(ip)
        if locked_s:
            db.session.add(AccessLog(role='visitor', ip=ip, event='login_blocked',
                                     user_agent=user_agent()))
            db.session.commit()
            return jsonify({'error': 'locked', 'retry_s': locked_s}), 429

        failure = _check_credentials(password, code)
        if failure:
            db.session.rollback()
            _record_failure(ip, failure)
            locked_s = lockout_remaining(ip)
            body = {'error': 'incorrect'}
            if locked_s:
                body['retry_s'] = locked_s
            return jsonify(body), 401

        LoginLock.query.filter_by(ip=lock_key(ip)).delete()
        start_admin_session(ip)
        db.session.add(AccessLog(role='admin', ip=ip, event='login_ok', user_agent=user_agent()))
        db.session.commit()
    return jsonify({'ok': True, 'csrf': csrf_token()})


@bp.route('/api/logout', methods=['POST'])
@admin_required
def logout():
    data = request.get_json(silent=True)
    everywhere = isinstance(data, dict) and data.get('all') is True
    ip = client_ip()
    end_admin_session(everywhere)
    db.session.add(AccessLog(role='admin', ip=ip, event='logout', user_agent=user_agent(),
                             detail='everywhere' if everywhere else None))
    db.session.commit()
    return jsonify({'ok': True})


# --- Visitor log ---

def _purge_old_records():
    """Runs at most hourly, from whichever request gets there first."""
    global _last_purge
    now = time.monotonic()
    with _purge_mutex:
        if _last_purge and now - _last_purge < 3600:
            return
        _last_purge = now
    cutoff = utcnow() - RETENTION
    Visit.query.filter(Visit.timestamp < cutoff).delete(synchronize_session=False)
    AccessLog.query.filter(AccessLog.timestamp < cutoff).delete(synchronize_session=False)
    LoginLock.query.filter(LoginLock.last_failure < cutoff).delete(synchronize_session=False)
    AdminSession.query.filter(AdminSession.expires_at < utcnow()).delete(synchronize_session=False)


def record_visit(kind, path, command=None):
    """Logging must never be the reason a page fails."""
    try:
        admin = is_admin()
        db.session.add(Visit(
            kind=kind,
            ip=client_ip(),
            path=(path or '')[:128],
            command=command,
            user_agent=user_agent(),
            referrer=(request.referrer or '')[:256] or None,
            admin=admin,
        ))
        _purge_old_records()
        db.session.commit()
    except Exception:  # pylint: disable=broad-except
        db.session.rollback()
        current_app.logger.exception('could not record a visit')


def loggable_command(line):
    if not isinstance(line, str):
        return '[unknown]'
    line = line.strip().lower()
    return line if len(line) <= 64 and _LOGGABLE_RE.fullmatch(line) else '[unknown]'


@bp.route('/api/visit/command', methods=['POST'])
@rate_limited('visit-command', 120)
def visit_command():
    """Sets no state anyone acts on and carries no credentials, so it needs no token,
    only the origin check and the rate limit."""
    if not same_origin():
        return jsonify({'error': 'forbidden'}), 403
    data = request.get_json(silent=True)
    data = data if isinstance(data, dict) else {}
    path = data.get('path') if data.get('path') in ('/', '/mira') else '/'
    record_visit('command', path, loggable_command(data.get('command')))
    return jsonify({'ok': True})


# --- The `access` command ---

@bp.route('/api/admin/access')
@admin_required
def access():
    view = request.args.get('view', 'logins')
    limit = max(1, min(request.args.get('limit', 25, type=int), 200))

    if view == 'logins':
        rows = AccessLog.query.order_by(AccessLog.timestamp.desc()).limit(limit).all()
        entries = [{'t': iso(r.timestamp), 'event': r.event or 'login_ok', 'detail': r.detail,
                    'role': r.role, 'ip': r.ip, 'agent': r.user_agent} for r in rows]
    elif view in ('visits', 'commands'):
        kind = 'page' if view == 'visits' else 'command'
        rows = (Visit.query.filter_by(kind=kind).order_by(Visit.timestamp.desc())
                .limit(limit).all())
        entries = [{'t': iso(r.timestamp), 'path': r.path, 'command': r.command, 'ip': r.ip,
                    'agent': r.user_agent, 'referrer': r.referrer, 'admin': bool(r.admin)}
                   for r in rows]
    else:
        return jsonify({'error': 'bad view'}), 400

    return jsonify({
        'view': view,
        'entries': entries,
        # Lets the morning check confirm which header Railway really fills in.
        'you': {
            'ip': client_ip(),
            'x_real_ip': request.headers.get('X-Real-IP'),
            'x_forwarded_for': request.headers.get('X-Forwarded-For'),
            'remote_addr': request.remote_addr,
        },
    })
