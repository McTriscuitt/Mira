"""Who is asking, and whether they're allowed: client address, admin sessions,
CSRF, rate limits and response headers."""

import hashlib
import hmac
import ipaddress
import secrets
import threading
import time
from collections import defaultdict, deque
from datetime import timedelta
from functools import wraps

from flask import g, jsonify, request, session

from models import AdminSession, db, utcnow

SESSION_DAYS = 90

CONTENT_SECURITY_POLICY = '; '.join([
    "default-src 'self'",
    "script-src 'self'",
    "style-src 'self'",
    "img-src 'self' data:",
    "connect-src 'self'",
    "font-src 'self'",
    "object-src 'none'",
    "base-uri 'none'",
    "form-action 'self'",
    "frame-ancestors 'none'",
])


# --- Client address ---

def _valid_ip(value):
    try:
        ipaddress.ip_address(value)
        return True
    except ValueError:
        return False


def client_ip():
    """The visitor's address as Railway's edge reports it.

    The edge overwrites X-Real-IP on every public request. The first X-Forwarded-For
    value is whatever the caller sent, so it's never used; only the last one, which
    the nearest proxy appended, is a fallback.
    """
    real_ip = request.headers.get('X-Real-IP', '').strip()
    if _valid_ip(real_ip):
        return real_ip
    forwarded = request.headers.get('X-Forwarded-For', '').split(',')[-1].strip()
    if _valid_ip(forwarded):
        return forwarded
    return request.remote_addr or 'unknown'


def lock_key(ip):
    """What a lockout attaches to. One IPv6 customer usually holds a whole /64, so
    locking a single address in it would be easy to step around."""
    try:
        parsed = ipaddress.ip_address(ip)
    except ValueError:
        return ip
    if parsed.version == 6:
        return str(ipaddress.ip_network(f'{ip}/64', strict=False).network_address)
    return ip


def user_agent():
    return (request.headers.get('User-Agent') or '')[:256]


# --- Admin sessions ---

def _hash_token(token):
    return hashlib.sha256(token.encode()).hexdigest()


def start_admin_session(ip):
    """Replaces whatever session the browser had, so a value planted before login
    can't carry over."""
    token = secrets.token_urlsafe(32)
    session.clear()
    session.permanent = True
    session['sid'] = token
    session['csrf'] = secrets.token_urlsafe(32)
    db.session.add(AdminSession(
        token_hash=_hash_token(token),
        expires_at=utcnow() + timedelta(days=SESSION_DAYS),
        ip=ip,
    ))


def session_row(token_hash):
    row = AdminSession.query.filter_by(token_hash=token_hash).first()
    if row is None:
        return None
    if row.expires_at <= utcnow():
        db.session.delete(row)
        db.session.commit()
        return None
    return row


def is_admin():
    if 'mira_admin' not in g:
        token = session.get('sid')
        g.mira_admin = bool(token) and session_row(_hash_token(token)) is not None
    return g.mira_admin


def current_token_hash():
    token = session.get('sid')
    return _hash_token(token) if token else None


def end_admin_session(everywhere=False):
    if everywhere:
        AdminSession.query.delete()
    else:
        token_hash = current_token_hash()
        if token_hash:
            AdminSession.query.filter_by(token_hash=token_hash).delete()
    session.clear()
    g.mira_admin = False


# --- CSRF ---

def csrf_token():
    if 'csrf' not in session:
        session['csrf'] = secrets.token_urlsafe(32)
    return session['csrf']


def csrf_ok():
    expected = session.get('csrf')
    sent = request.headers.get('X-CSRF-Token', '')
    return (bool(expected) and hmac.compare_digest(expected.encode(), sent.encode())
            and same_origin())


def same_origin():
    """A browser always sends Origin on a cross-site POST, and a page can't forge it.
    Requests with no Origin aren't cross-site browser requests."""
    origin = request.headers.get('Origin')
    if not origin:
        return True
    host = origin.split('://', 1)[-1]
    return host in {request.headers.get('Host'), request.headers.get('X-Forwarded-Host')}


def admin_required(view):
    """For JSON routes. Anything that changes state also needs the CSRF token."""
    @wraps(view)
    def wrapped(*args, **kwargs):
        if not is_admin():
            return jsonify({'error': 'unauthorized'}), 401
        if request.method not in ('GET', 'HEAD') and not csrf_ok():
            return jsonify({'error': 'forbidden'}), 403
        return view(*args, **kwargs)
    return wrapped


# --- Rate limits ---

class RateLimiter:
    """Sliding window per address, held in memory: a redeploy resetting these is
    harmless. Login attempts use the Postgres lockout instead."""

    MAX_TRACKED = 5000

    def __init__(self):
        self._lock = threading.Lock()
        self._hits = defaultdict(deque)

    def allow(self, bucket, limit, per_seconds, ip=None):
        now = time.monotonic()
        key = (bucket, ip or client_ip())
        with self._lock:
            if len(self._hits) > self.MAX_TRACKED:
                self._hits.clear()
            hits = self._hits[key]
            while hits and hits[0] <= now - per_seconds:
                hits.popleft()
            if len(hits) >= limit:
                return False
            hits.append(now)
            return True

    def reset(self):
        with self._lock:
            self._hits.clear()


rate_limiter = RateLimiter()


def rate_limited(bucket, limit, per_seconds=60):
    """Refuses the request with a 429 once this address passes the limit."""
    def decorate(view):
        @wraps(view)
        def wrapped(*args, **kwargs):
            if not rate_limiter.allow(bucket, limit, per_seconds):
                return jsonify({'error': 'too many requests'}), 429
            return view(*args, **kwargs)
        return wrapped
    return decorate


# --- Response headers ---

def add_security_headers(response, secure):
    headers = response.headers
    headers.setdefault('Content-Security-Policy', CONTENT_SECURITY_POLICY)
    headers.setdefault('X-Content-Type-Options', 'nosniff')
    headers.setdefault('X-Frame-Options', 'DENY')
    headers.setdefault('Referrer-Policy', 'same-origin')
    headers.setdefault('Cross-Origin-Opener-Policy', 'same-origin')
    headers.setdefault('Permissions-Policy', 'camera=(), microphone=(), geolocation=()')
    if secure:
        headers.setdefault('Strict-Transport-Security', 'max-age=31536000')
    if not request.path.startswith('/static/'):
        headers['Cache-Control'] = 'no-store'
    return response
