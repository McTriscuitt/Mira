"""The routes the firmware calls. These paths and the bearer key are compiled into
the board, so changing them means a reflash. Browsers have no business here, which
is why no cookie or CSRF logic applies."""

import hmac
from datetime import timedelta

from flask import Blueprint, current_app, jsonify, request

from broadcast import broadcast
from models import Command, EventLog, StatusSnapshot, db, utcnow

bp = Blueprint('board', __name__)

COMMAND_LIFETIME = timedelta(minutes=5)


def _board_authed():
    sent = request.headers.get('Authorization', '')
    expected = f"Bearer {current_app.config['ESP32_API_KEY']}"
    return hmac.compare_digest(sent.encode(), expected.encode())


def expire_old_commands():
    """A command queued while the board was away shouldn't fire when it comes back
    hours later."""
    cutoff = utcnow() - COMMAND_LIFETIME
    expired = (Command.query
               .filter(Command.status == 'pending', Command.created_at < cutoff)
               .update({'status': 'expired'}, synchronize_session=False))
    if expired:
        db.session.commit()


@bp.route('/api/status', methods=['POST'])
def ingest_status():
    if not _board_authed():
        return jsonify({'error': 'unauthorized'}), 401
    d = request.get_json(silent=True)
    if not isinstance(d, dict) or not d.get('state'):
        return jsonify({'error': 'bad request'}), 400
    excluded = d.get('excluded') or {}
    lights_on = d.get('lights_on') or {}
    db.session.add(StatusSnapshot(
        state=d['state'],
        mode=d.get('mode'),
        lux=d.get('lux'),
        bri=d.get('bri'),
        ct=d.get('ct'),
        overhead_on=d.get('overhead_on', False),
        wind_down_step=d.get('wind_down_step'),
        wake_step=d.get('wake_step'),
        wake_total=d.get('wake_total'),
        soft_pause_remaining_s=d.get('soft_pause_remaining_s'),
        excl_floor=excluded.get('floor', False),
        excl_chest=excluded.get('chest', False),
        excl_dresser=excluded.get('dresser', False),
        excl_ceiling=excluded.get('ceiling', False),
        on_floor=lights_on.get('floor', False),
        on_chest=lights_on.get('chest', False),
        on_dresser=lights_on.get('dresser', False),
        on_ceiling=lights_on.get('ceiling', False),
        stable_lux_count=d.get('stable_lux_count'),
        heap_free=d.get('heap_free'),
        sse_stack_free=d.get('sse_stack_free'),
        net_stack_free=d.get('net_stack_free'),
    ))
    db.session.commit()
    broadcast.bump_status()
    return jsonify({'ok': True})


@bp.route('/api/log', methods=['POST'])
def ingest_log():
    if not _board_authed():
        return jsonify({'error': 'unauthorized'}), 401
    d = request.get_json(silent=True) or {}
    db.session.add(EventLog(message=str(d.get('message', ''))))
    db.session.commit()
    broadcast.bump_log()
    return jsonify({'ok': True})


@bp.route('/api/command', methods=['GET'])
def poll_command():
    if not _board_authed():
        return jsonify({'error': 'unauthorized'}), 401
    expire_old_commands()
    command = Command.query.filter_by(status='pending').order_by(Command.created_at).first()
    if not command:
        return jsonify({'command': None})
    return jsonify({'command': command.action, 'id': command.id, 'value': command.value})


@bp.route('/api/command/<int:command_id>/ack', methods=['POST'])
def ack_command(command_id):
    if not _board_authed():
        return jsonify({'error': 'unauthorized'}), 401
    command = db.session.get(Command, command_id)
    if not command:
        return jsonify({'error': 'not found'}), 404
    command.status = 'executed'
    command.executed_at = utcnow()
    db.session.commit()
    broadcast.bump_status()
    return jsonify({'ok': True})
