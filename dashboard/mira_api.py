"""Mira's data for browsers. The schedule and the log are public; status, the stream and
everything that touches the lights need an admin session."""

import json
import threading
from datetime import datetime, timedelta

from flask import (Blueprint, Response, jsonify, request, stream_with_context)

from board_api import COMMAND_LIFETIME
from broadcast import broadcast
import schedule
from models import BoardSchedule, Command, EventLog, StatusSnapshot, db, iso, utcnow
from security import admin_required, current_token_hash, is_admin, rate_limited, session_row

bp = Blueprint('mira_api', __name__)

BOARD_OFFLINE_AFTER_S = 90    # the board posts status every 30 s
PUBLIC_LOG_MAX = 50
ADMIN_LOG_MAX = 500
MAX_MINUTES = 720

# The most streams open at once. Each holds one of gunicorn's 16 threads, the same
# threads the board's posts and polls need.
MAX_STREAMS = 4
_stream_slots = threading.BoundedSemaphore(MAX_STREAMS)

# The board's lights in src/lights.h order. A light command's value is the index, and
# these are the firmware's lightName()s, which its log lines about that light start with.
LIGHT_NAMES = ('Floor', 'Dresser', 'Chest', 'Ceiling')

# Action -> (lowest value, highest value), or None for no value. NAP's value is optional.
ALLOWED_COMMANDS = {
    'RESUME': None,
    'HOLD': None,
    'WAKE': None,
    'SLEEP': None,
    'NAP': (1, MAX_MINUTES),
    'EXTEND': (1, MAX_MINUTES),
    'LIGHT_ON': (0, len(LIGHT_NAMES) - 1),
    'LIGHT_OFF': (0, len(LIGHT_NAMES) - 1),
    'SEND_SCHEDULE': None,
}
VALUE_OPTIONAL = {'NAP'}

_LEGACY_MODES = {'LOCKED_OUT': 'ASLEEP', 'WAKE': 'WAKE', 'SOFT_PAUSE': 'HOLD', 'NORMAL': 'ACTIVE'}

# How to tell what a command did. An ack only means the board queued it: rejected
# commands are acked too. Firmware wording lives in src/director.cpp.
_EXPECTED_MODES = {
    'RESUME': ('ACTIVE', 'WAKE'),
    'HOLD': ('HOLD',),
    'WAKE': ('WAKE',),
    'SLEEP': ('ASLEEP',),
    'NAP': ('NAP',),
}
_IGNORED_PREFIX = {
    'RESUME': 'Resume ignored',
    'HOLD': 'Hold ignored',
    'WAKE': 'Wake ignored',
    'NAP': 'Nap ignored',
    'EXTEND': 'Extend ignored',
    'SEND_SCHEDULE': 'Schedule not sent',
}
_APPLIED_LOG = {'EXTEND': 'extended', 'SEND_SCHEDULE': 'Schedule sent'}
_LIGHT_WORDS = {'LIGHT_ON': 'on', 'LIGHT_OFF': 'off'}
_SETTLE = timedelta(seconds=2)    # the board acks slightly before it posts the new status
_GIVE_UP = timedelta(seconds=30)


# --- Status ---

def _latest_snapshot():
    return StatusSnapshot.query.order_by(StatusSnapshot.timestamp.desc()).first()


def _pending_command():
    cutoff = utcnow() - COMMAND_LIFETIME
    return (Command.query
            .filter(Command.status == 'pending', Command.created_at >= cutoff)
            .order_by(Command.created_at).first())


def _status_payload(snap):
    return {
        'state': snap.state,
        'mode': snap.mode or _LEGACY_MODES.get(snap.state, snap.state),
        'lux': snap.lux,
        'bri': snap.bri,
        'ct': snap.ct,
        'remaining_s': snap.soft_pause_remaining_s or 0,
        'wake_step': snap.wake_step,
        'wake_total': snap.wake_total,
        'lights_on': {
            'floor': bool(snap.on_floor),
            'dresser': bool(snap.on_dresser),
            'chest': bool(snap.on_chest),
            'ceiling': bool(snap.on_ceiling),
        },
        'excluded': {
            'floor': bool(snap.excl_floor),
            'dresser': bool(snap.excl_dresser),
            'chest': bool(snap.excl_chest),
            'ceiling': bool(snap.excl_ceiling),
        },
        'timestamp': iso(snap.timestamp),
    }


def _with_age(payload):
    """Age is worked out when serving, so a cached copy still reports it correctly and
    a visitor's wrong clock doesn't matter."""
    moment = datetime.fromisoformat(payload['timestamp'].rstrip('Z'))
    age_s = max(0, int((utcnow() - moment).total_seconds()))
    return {**payload, 'age_s': age_s, 'online': age_s <= BOARD_OFFLINE_AFTER_S}


def admin_status_payload():
    snap = _latest_snapshot()
    if not snap:
        return None
    pending = _pending_command()
    payload = _with_age(_status_payload(snap))
    payload['pending_command_id'] = pending.id if pending else None
    payload['telemetry'] = {
        'heap_free': snap.heap_free,
        'sse_stack_free': snap.sse_stack_free,
        'net_stack_free': snap.net_stack_free,
    }
    return payload


@bp.route('/api/status/latest')
@admin_required
@rate_limited('status', 60)
def latest_status():
    return jsonify(admin_status_payload())


# --- Schedule ---

@bp.route('/api/schedule')
@rate_limited('schedule', 60)
def latest_schedule():
    """The board's own copy when it has posted one, otherwise the site's."""
    row = db.session.get(BoardSchedule, 1)
    if row is None:
        return jsonify({**schedule.DEFAULT, 'source': 'site', 'received_at': None})
    return jsonify({**json.loads(row.body), 'source': 'board', 'received_at': iso(row.received_at)})


@bp.route('/api/status/stream')
@admin_required
def status_stream():
    if not _stream_slots.acquire(blocking=False):
        return jsonify({'error': 'too many streams'}), 503

    # The session is only readable while the request is fresh, so capture it now.
    token_hash = current_token_hash()

    def gen():
        status_v, log_v = broadcast.versions()
        # An immediate first frame, so a client paints without waiting for the board.
        yield 'event: status\ndata: ' + json.dumps(admin_status_payload()) + '\n\n'
        db.session.remove()
        while True:
            new_status, new_log = broadcast.wait(status_v, log_v, timeout=25.0)
            # Logging out elsewhere has to end this stream too.
            alive = session_row(token_hash) is not None
            db.session.remove()
            if not alive:
                yield 'event: ended\ndata: {}\n\n'
                return
            if (new_status, new_log) == (status_v, log_v):
                # A comment: invisible to EventSource, keeps proxies from reaping it.
                yield ': ping\n\n'
                continue
            if new_status != status_v:
                yield 'event: status\ndata: ' + json.dumps(admin_status_payload()) + '\n\n'
                # Release the pooled connection between events, or a few open streams
                # would sit idle-in-transaction and exhaust the pool.
                db.session.remove()
            if new_log != log_v:
                yield 'event: log\ndata: {}\n\n'
            status_v, log_v = new_status, new_log

    response = Response(stream_with_context(gen()), mimetype='text/event-stream')
    response.call_on_close(_stream_slots.release)
    response.headers['X-Accel-Buffering'] = 'no'
    return response


# --- Log ---

@bp.route('/api/log/recent')
@rate_limited('log', 60)
def recent_log():
    admin = is_admin()
    limit = max(1, min(request.args.get('limit', 10, type=int),
                       ADMIN_LOG_MAX if admin else PUBLIC_LOG_MAX))
    query = EventLog.query
    if admin:  # search scans the whole table, so it isn't offered publicly
        search = request.args.get('search', '', type=str).strip()
        exclude = request.args.get('exclude', '', type=str).strip()
        if search:
            query = query.filter(EventLog.message.ilike(f'%{search}%'))
        for term in (t.strip() for t in exclude.split(',') if t.strip()):
            query = query.filter(~EventLog.message.ilike(f'%{term}%'))
    entries = query.order_by(EventLog.timestamp.desc()).limit(limit).all()
    return jsonify([{'timestamp': iso(e.timestamp), 'message': e.message} for e in entries])


# --- Commands ---

def _valid_command(action, value):
    if action not in ALLOWED_COMMANDS:
        return False
    bounds = ALLOWED_COMMANDS[action]
    if bounds is None:
        return value is None
    if value is None:
        return action in VALUE_OPTIONAL
    if isinstance(value, bool) or not isinstance(value, int):
        return False
    return bounds[0] <= value <= bounds[1]


@bp.route('/api/command', methods=['POST'])
@admin_required
def send_command():
    data = request.get_json(silent=True)
    data = data if isinstance(data, dict) else {}
    action, value = data.get('action'), data.get('value')
    if not _valid_command(action, value):
        return jsonify({'error': 'unknown command'}), 400
    command = Command(action=action, value=value)
    db.session.add(command)
    db.session.commit()
    broadcast.bump_status()
    snap = _latest_snapshot()
    online = bool(snap) and _with_age(_status_payload(snap))['online']
    return jsonify({'ok': True, 'id': command.id, 'board_online': online})


@bp.route('/api/command/<int:command_id>/cancel', methods=['POST'])
@admin_required
def cancel_command(command_id):
    command = db.session.get(Command, command_id)
    if not command or command.status != 'pending':
        return jsonify({'error': 'not found or already executed'}), 404
    command.status = 'cancelled'
    db.session.commit()
    broadcast.bump_status()
    return jsonify({'ok': True})


def _ignored_patterns(command):
    """LIKE patterns for the board's refusal of this command. Firmware too old to know a
    command refuses it in a line of its own (src/dashboard.cpp)."""
    patterns = [f'Dashboard command {command.action} is %']
    prefix = _IGNORED_PREFIX.get(command.action)
    if prefix:
        patterns.append(f'%{prefix}%')
    if command.action in _LIGHT_WORDS and command.value in range(len(LIGHT_NAMES)):
        # "Chest on ignored — asleep (Wake starts the day)"
        patterns.append(f'{LIGHT_NAMES[command.value]} {_LIGHT_WORDS[command.action]} ignored%')
    return patterns


def _applied_pattern(command):
    marker = _APPLIED_LOG.get(command.action)
    if marker:
        return f'%{marker}%'
    if command.action in _LIGHT_WORDS and command.value in range(len(LIGHT_NAMES)):
        # "Chest on — dashboard", "Chest off for today — dashboard"
        return f'{LIGHT_NAMES[command.value]} {_LIGHT_WORDS[command.action]}%— dashboard%'
    return None


def _latest_log_like(since, pattern):
    return (EventLog.query
            .filter(EventLog.timestamp >= since, EventLog.message.like(pattern))
            .order_by(EventLog.timestamp.desc()).first())


def command_outcome(command, now=None):
    """Returns (outcome, note). Outcome is one of waiting, picked_up, applied, ignored,
    unconfirmed, expired or cancelled."""
    now = now or utcnow()
    if command.status == 'cancelled':
        return 'cancelled', None
    if command.status == 'expired' or (command.status == 'pending'
                                       and now - command.created_at >= COMMAND_LIFETIME):
        return 'expired', None
    if command.status == 'pending':
        return 'waiting', None

    since = command.created_at
    for pattern in _ignored_patterns(command):
        ignored = _latest_log_like(since, pattern)
        if ignored:
            return 'ignored', ignored.message

    pattern = _applied_pattern(command)
    if pattern:
        applied = _latest_log_like(since, pattern)
        if applied:
            return 'applied', applied.message

    settled = now - command.executed_at >= _SETTLE
    expected = _EXPECTED_MODES.get(command.action)
    if expected and settled:
        snap = (StatusSnapshot.query
                .filter(StatusSnapshot.timestamp >= since)
                .order_by(StatusSnapshot.timestamp.desc()).first())
        if snap and (snap.mode or _LEGACY_MODES.get(snap.state)) in expected:
            return 'applied', None

    if now - command.executed_at >= _GIVE_UP:
        return 'unconfirmed', None
    return 'picked_up', None


@bp.route('/api/command/<int:command_id>')
@admin_required
def command_status(command_id):
    command = db.session.get(Command, command_id)
    if not command:
        return jsonify({'error': 'not found'}), 404
    outcome, note = command_outcome(command)
    return jsonify({'id': command.id, 'action': command.action, 'outcome': outcome, 'note': note})


# --- Lux and memory history, for the old dashboard pages ---

@bp.route('/api/lux/history')
@admin_required
def lux_history():
    start_str = request.args.get('start')
    end_str = request.args.get('end')
    if start_str and end_str:
        try:
            start = datetime.fromisoformat(start_str.rstrip('Z'))
            end = datetime.fromisoformat(end_str.rstrip('Z'))
        except ValueError:
            return jsonify({'error': 'invalid timestamp'}), 400
    else:
        date_str = request.args.get('date')
        if date_str:
            try:
                day = datetime.strptime(date_str, '%Y-%m-%d').date()
            except ValueError:
                return jsonify({'error': 'invalid date'}), 400
        else:
            day = utcnow().date()
        start = datetime(day.year, day.month, day.day)
        end = start + timedelta(days=1)
    snaps = (StatusSnapshot.query
             .filter(StatusSnapshot.timestamp >= start,
                     StatusSnapshot.timestamp < end,
                     StatusSnapshot.lux.isnot(None))
             .order_by(StatusSnapshot.timestamp.asc())
             .all())
    return jsonify([{'t': iso(s.timestamp), 'lux': s.lux, 'state': s.state} for s in snaps])


@bp.route('/api/telemetry/history')
@admin_required
def telemetry_history():
    hours = request.args.get('hours', 48, type=int)
    start = utcnow() - timedelta(hours=max(1, min(hours, 24 * 14)))
    snaps = (StatusSnapshot.query
             .filter(StatusSnapshot.timestamp >= start,
                     StatusSnapshot.heap_free.isnot(None))
             .order_by(StatusSnapshot.timestamp.asc())
             .all())
    return jsonify([{
        't': iso(s.timestamp),
        'heap_free': s.heap_free,
        'sse_stack_free': s.sse_stack_free,
        'net_stack_free': s.net_stack_free,
    } for s in snaps])
