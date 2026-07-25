import json
import os
import re
import threading
from datetime import datetime, timedelta
from flask import (Flask, Response, request, jsonify, session, redirect,
                   render_template, url_for, stream_with_context)
from flask_sqlalchemy import SQLAlchemy
from sqlalchemy import text

_TIME_RE = re.compile(
    r'\d{1,2}:\d{2}(?:AM|PM) (?:Sunday|Monday|Tuesday|Wednesday|Thursday|Friday|Saturday)'
)

def _redact_message(msg):
    msg = _TIME_RE.sub('', msg)
    msg = re.sub(r'\s*—\s*$', '', msg)
    msg = re.sub(r'^\s*\|\s*', '', msg)
    return msg.strip()

app = Flask(__name__)
app.secret_key = os.environ['SECRET_KEY']

_db_url = os.environ['DATABASE_URL']
if _db_url.startswith('postgres://'):
    _db_url = _db_url.replace('postgres://', 'postgresql://', 1)
app.config['SQLALCHEMY_DATABASE_URI'] = _db_url
app.config['SQLALCHEMY_TRACK_MODIFICATIONS'] = False
app.config['PERMANENT_SESSION_LIFETIME'] = timedelta(days=90)

db = SQLAlchemy(app)

ESP32_API_KEY = os.environ['ESP32_API_KEY']
DASHBOARD_PASSWORD = os.environ['DASHBOARD_PASSWORD']
DEMO_PASSWORD = os.environ.get('DEMO_PASSWORD', '')


# --- Models ---

class StatusSnapshot(db.Model):
    __tablename__ = 'status_snapshots'
    id = db.Column(db.Integer, primary_key=True)
    timestamp = db.Column(db.DateTime, default=datetime.utcnow)
    state = db.Column(db.String(20), nullable=False)
    lux = db.Column(db.Float)
    bri = db.Column(db.Float)
    ct = db.Column(db.Integer)
    overhead_on = db.Column(db.Boolean)
    wind_down_step = db.Column(db.Integer)
    wake_step = db.Column(db.Integer)
    wake_total = db.Column(db.Integer)
    soft_pause_remaining_s = db.Column(db.Integer)
    # Per-light cycle exclusion (dashboard "lights in cycle" chips)
    excl_floor = db.Column(db.Boolean)
    excl_chest = db.Column(db.Boolean)
    excl_dresser = db.Column(db.Boolean)
    excl_ceiling = db.Column(db.Boolean)
    stable_lux_count = db.Column(db.Integer)
    # N1 Stage 5 — firmware soak telemetry (free heap + task stack watermarks,
    # bytes). Lets long soaks be read from the dashboard instead of a serial log.
    heap_free = db.Column(db.BigInteger)
    sse_stack_free = db.Column(db.Integer)
    net_stack_free = db.Column(db.Integer)


class Command(db.Model):
    __tablename__ = 'commands'
    id = db.Column(db.Integer, primary_key=True)
    created_at = db.Column(db.DateTime, default=datetime.utcnow)
    action = db.Column(db.String(50), nullable=False)
    value = db.Column(db.Integer, nullable=True)
    executed_at = db.Column(db.DateTime, nullable=True)
    status = db.Column(db.String(20), default='pending')


class EventLog(db.Model):
    __tablename__ = 'event_log'
    id = db.Column(db.Integer, primary_key=True)
    timestamp = db.Column(db.DateTime, default=datetime.utcnow)
    message = db.Column(db.Text, nullable=False)


class AccessLog(db.Model):
    __tablename__ = 'access_log'
    id = db.Column(db.Integer, primary_key=True)
    timestamp = db.Column(db.DateTime, default=datetime.utcnow)
    role = db.Column(db.String(10), nullable=False)
    ip = db.Column(db.String(45))


with app.app_context():
    db.create_all()
    with db.engine.connect() as conn:
        for table, col, coltype in [
            ('status_snapshots', 'wind_down_step',        'INTEGER'),
            ('status_snapshots', 'wake_step',             'INTEGER'),
            ('status_snapshots', 'wake_total',            'INTEGER'),
            ('status_snapshots', 'soft_pause_remaining_s','INTEGER'),
            ('status_snapshots', 'excl_floor',            'BOOLEAN'),
            ('status_snapshots', 'excl_chest',            'BOOLEAN'),
            ('status_snapshots', 'excl_dresser',          'BOOLEAN'),
            ('status_snapshots', 'excl_ceiling',          'BOOLEAN'),
            ('status_snapshots', 'stable_lux_count',     'INTEGER'),
            ('status_snapshots', 'heap_free',             'BIGINT'),
            ('status_snapshots', 'sse_stack_free',        'INTEGER'),
            ('status_snapshots', 'net_stack_free',        'INTEGER'),
            ('commands',         'value',                 'INTEGER'),
        ]:
            conn.execute(text(
                f'ALTER TABLE {table} ADD COLUMN IF NOT EXISTS {col} {coltype}'
            ))
        conn.commit()

        # Migrate bri from v1 integer (0–254) to v2 float percent (0.0–100.0)
        row = conn.execute(text(
            "SELECT data_type FROM information_schema.columns "
            "WHERE table_name='status_snapshots' AND column_name='bri'"
        )).fetchone()
        if row and row[0] == 'integer':
            conn.execute(text(
                'ALTER TABLE status_snapshots ALTER COLUMN bri TYPE FLOAT USING bri::float'
            ))
            conn.execute(text(
                'UPDATE status_snapshots SET bri = ROUND(CAST(bri AS NUMERIC) / 2.54, 1) WHERE bri > 100'
            ))
            conn.commit()


# --- SSE fan-out (N2 step 2) ---

class _Broadcast:
    """In-process change notifier for /api/status/stream.

    Two monotonic version counters (status snapshots, log lines) under one
    Condition; stream generators wait on it and re-query the DB when a counter
    moves. Process-local by design — the Procfile must keep gunicorn at
    `-w 1` (threads are fine) or browsers on other workers would never wake.
    """

    def __init__(self):
        self._cond = threading.Condition()
        self._status_v = 0
        self._log_v = 0

    def bump_status(self):
        with self._cond:
            self._status_v += 1
            self._cond.notify_all()

    def bump_log(self):
        with self._cond:
            self._log_v += 1
            self._cond.notify_all()

    def versions(self):
        with self._cond:
            return self._status_v, self._log_v

    def wait(self, status_v, log_v, timeout):
        with self._cond:
            if (self._status_v, self._log_v) == (status_v, log_v):
                self._cond.wait(timeout)
            return self._status_v, self._log_v


_broadcast = _Broadcast()


# --- Auth helpers ---

def _esp32_authed():
    return request.headers.get('Authorization') == f'Bearer {ESP32_API_KEY}'


def _dashboard_authed():
    return session.get('role') in ('owner', 'demo')


def _owner_authed():
    return session.get('role') == 'owner'


# --- ESP32 endpoints ---

@app.route('/api/status', methods=['POST'])
def ingest_status():
    if not _esp32_authed():
        return jsonify({'error': 'unauthorized'}), 401
    d = request.json
    excl = d.get('excluded') or {}
    db.session.add(StatusSnapshot(
        state=d['state'],
        lux=d.get('lux'),
        bri=d.get('bri'),
        ct=d.get('ct'),
        overhead_on=d.get('overhead_on', False),
        wind_down_step=d.get('wind_down_step'),
        wake_step=d.get('wake_step'),
        wake_total=d.get('wake_total'),
        soft_pause_remaining_s=d.get('soft_pause_remaining_s'),
        excl_floor=excl.get('floor', False),
        excl_chest=excl.get('chest', False),
        excl_dresser=excl.get('dresser', False),
        excl_ceiling=excl.get('ceiling', False),
        stable_lux_count=d.get('stable_lux_count'),
        heap_free=d.get('heap_free'),
        sse_stack_free=d.get('sse_stack_free'),
        net_stack_free=d.get('net_stack_free'),
    ))
    db.session.commit()
    _broadcast.bump_status()
    return jsonify({'ok': True})


@app.route('/api/log', methods=['POST'])
def ingest_log():
    if not _esp32_authed():
        return jsonify({'error': 'unauthorized'}), 401
    db.session.add(EventLog(message=request.json.get('message', '')))
    db.session.commit()
    _broadcast.bump_log()
    return jsonify({'ok': True})


@app.route('/api/command', methods=['GET'])
def poll_command():
    if not _esp32_authed():
        return jsonify({'error': 'unauthorized'}), 401
    cmd = Command.query.filter_by(status='pending').order_by(Command.created_at).first()
    if not cmd:
        return jsonify({'command': None})
    return jsonify({'command': cmd.action, 'id': cmd.id, 'value': cmd.value})


@app.route('/api/command/<int:cmd_id>/cancel', methods=['POST'])
def cancel_command(cmd_id):
    if not _owner_authed():
        return jsonify({'error': 'unauthorized'}), 403
    cmd = db.session.get(Command, cmd_id)
    if not cmd or cmd.status != 'pending':
        return jsonify({'error': 'not found or already executed'}), 404
    cmd.status = 'cancelled'
    db.session.commit()
    _broadcast.bump_status()
    return jsonify({'ok': True})


@app.route('/api/command/<int:cmd_id>/ack', methods=['POST'])
def ack_command(cmd_id):
    if not _esp32_authed():
        return jsonify({'error': 'unauthorized'}), 401
    cmd = db.session.get(Command, cmd_id)
    if not cmd:
        return jsonify({'error': 'not found'}), 404
    cmd.status = 'executed'
    cmd.executed_at = datetime.utcnow()
    db.session.commit()
    _broadcast.bump_status()
    return jsonify({'ok': True})


# --- Dashboard auth ---

@app.route('/login', methods=['GET', 'POST'])
def login():
    if request.method == 'POST':
        pw = request.form.get('password')
        ip = request.headers.get('X-Forwarded-For', request.remote_addr)
        if pw == DASHBOARD_PASSWORD:
            session.permanent = True
            session['role'] = 'owner'
            db.session.add(AccessLog(role='owner', ip=ip))
            db.session.commit()
            return redirect(url_for('index'))
        elif DEMO_PASSWORD and pw == DEMO_PASSWORD:
            session['role'] = 'demo'
            db.session.add(AccessLog(role='demo', ip=ip))
            db.session.commit()
            return redirect(url_for('index'))
        return render_template('login.html', error=True)
    return render_template('login.html', error=False)


@app.route('/admin/access-log')
def admin_access_log():
    if not _owner_authed():
        return redirect(url_for('login'))
    entries = AccessLog.query.order_by(AccessLog.timestamp.desc()).limit(200).all()
    return render_template('access_log.html', entries=entries)


@app.route('/logout')
def logout():
    session.clear()
    return redirect(url_for('login'))


# --- Dashboard API ---

@app.route('/')
def index():
    if not _dashboard_authed():
        return redirect(url_for('login'))
    return render_template('index.html', role=session.get('role', ''))


def _status_payload(include_private):
    """Latest snapshot + pending command as a dict; None if no snapshot yet.
    Shared by /api/status/latest and /api/status/stream — keep redaction here
    so the two views can never drift."""
    snap = StatusSnapshot.query.order_by(StatusSnapshot.timestamp.desc()).first()
    if not snap:
        return None
    pending = Command.query.filter_by(status='pending').order_by(Command.created_at).first()
    result = {
        'state': snap.state,
        'lux': snap.lux,
        'bri': snap.bri,
        'ct': snap.ct,
        'overhead_on': snap.overhead_on,
        'wind_down_step': snap.wind_down_step,
        'wake_step': snap.wake_step,
        'wake_total': snap.wake_total,
        'soft_pause_remaining_s': snap.soft_pause_remaining_s,
        'pending_command_id': pending.id if pending else None,
        'excluded': {
            'floor':   bool(snap.excl_floor),
            'chest':   bool(snap.excl_chest),
            'dresser': bool(snap.excl_dresser),
            'ceiling': bool(snap.excl_ceiling),
        },
        'stable_lux_count': snap.stable_lux_count,
    }
    if include_private:
        result['timestamp'] = snap.timestamp.isoformat() + 'Z'
        # N1 Stage 5 — firmware soak telemetry (owner-only, internal detail)
        result['telemetry'] = {
            'heap_free': snap.heap_free,
            'sse_stack_free': snap.sse_stack_free,
            'net_stack_free': snap.net_stack_free,
        }
    return result


@app.route('/api/status/latest')
def latest_status():
    if not _dashboard_authed():
        return jsonify({'error': 'unauthorized'}), 401
    return jsonify(_status_payload(session.get('role') != 'demo'))


@app.route('/api/status/stream')
def status_stream():
    if not _dashboard_authed():
        return jsonify({'error': 'unauthorized'}), 401
    # Session is only readable while the request context is fresh — capture the
    # role now, before the generator starts yielding.
    include_private = session.get('role') != 'demo'

    def gen():
        status_v, log_v = _broadcast.versions()
        # Immediate first frame so a (re)connecting client paints without
        # waiting for the firmware's next POST.
        yield 'event: status\ndata: ' + json.dumps(_status_payload(include_private)) + '\n\n'
        db.session.remove()
        while True:
            new_sv, new_lv = _broadcast.wait(status_v, log_v, timeout=25.0)
            if (new_sv, new_lv) == (status_v, log_v):
                # Keepalive comment — invisible to EventSource, defeats
                # idle-connection reaping by proxies along the way.
                yield ': ping\n\n'
                continue
            if new_sv != status_v:
                yield 'event: status\ndata: ' + json.dumps(_status_payload(include_private)) + '\n\n'
                # Release the pooled DB connection between events — a handful
                # of open streams would otherwise sit idle-in-transaction and
                # exhaust the default pool.
                db.session.remove()
            if new_lv != log_v:
                # Log lines carry role-dependent redaction; just poke the
                # client to refetch /api/log/recent instead of pushing content.
                yield 'event: log\ndata: {}\n\n'
            status_v, log_v = new_sv, new_lv

    resp = Response(stream_with_context(gen()), mimetype='text/event-stream')
    resp.headers['Cache-Control'] = 'no-cache'
    resp.headers['X-Accel-Buffering'] = 'no'
    return resp


@app.route('/api/log/recent')
def recent_log():
    if not _dashboard_authed():
        return jsonify({'error': 'unauthorized'}), 401
    limit = request.args.get('limit', 10, type=int)
    search = request.args.get('search', '', type=str).strip()
    exclude = request.args.get('exclude', '', type=str).strip()
    q = EventLog.query
    if search:
        q = q.filter(EventLog.message.ilike(f'%{search}%'))
    for term in (t.strip() for t in exclude.split(',') if t.strip()):
        q = q.filter(~EventLog.message.ilike(f'%{term}%'))
    entries = q.order_by(EventLog.timestamp.desc()).limit(min(limit, 500)).all()
    if session.get('role') == 'demo':
        return jsonify([{'message': _redact_message(e.message)} for e in entries])
    return jsonify([{'timestamp': e.timestamp.isoformat() + 'Z', 'message': e.message} for e in entries])


@app.route('/logs')
def logs():
    if not _dashboard_authed():
        return redirect(url_for('login'))
    return render_template('logs.html')


@app.route('/api/command', methods=['POST'])
def send_command():
    if not _owner_authed():
        return jsonify({'error': 'unauthorized'}), 403
    data = request.json
    cmd = Command(action=data['action'], value=data.get('value'))
    db.session.add(cmd)
    db.session.commit()
    _broadcast.bump_status()
    return jsonify({'ok': True, 'id': cmd.id})


@app.route('/lux')
def lux_curve_page():
    if not _owner_authed():
        return redirect(url_for('login'))
    return render_template('lux_curve.html', role=session.get('role', ''))


@app.route('/api/lux/history')
def lux_history():
    if not _owner_authed():
        return jsonify({'error': 'unauthorized'}), 403
    start_str = request.args.get('start')
    end_str   = request.args.get('end')
    if start_str and end_str:
        try:
            start = datetime.fromisoformat(start_str.rstrip('Z'))
            end   = datetime.fromisoformat(end_str.rstrip('Z'))
        except ValueError:
            return jsonify({'error': 'invalid timestamp'}), 400
    else:
        date_str = request.args.get('date')
        if date_str:
            try:
                d = datetime.strptime(date_str, '%Y-%m-%d').date()
            except ValueError:
                return jsonify({'error': 'invalid date'}), 400
        else:
            d = datetime.utcnow().date()
        start = datetime(d.year, d.month, d.day)
        end   = start + timedelta(days=1)
    snaps = (StatusSnapshot.query
             .filter(StatusSnapshot.timestamp >= start,
                     StatusSnapshot.timestamp < end,
                     StatusSnapshot.lux.isnot(None))
             .order_by(StatusSnapshot.timestamp.asc())
             .all())
    return jsonify([{'t': s.timestamp.isoformat() + 'Z', 'lux': s.lux, 'state': s.state} for s in snaps])


@app.route('/api/telemetry/history')
def telemetry_history():
    # N1 Stage 5 — heap/stack soak telemetry (owner-only). Default window 48 h
    # to match the Stage 5 heap-stability verification check; ?hours=N widens
    # it (capped at 14 days).
    if not _owner_authed():
        return jsonify({'error': 'unauthorized'}), 403
    hours = request.args.get('hours', 48, type=int)
    start = datetime.utcnow() - timedelta(hours=max(1, min(hours, 24 * 14)))
    snaps = (StatusSnapshot.query
             .filter(StatusSnapshot.timestamp >= start,
                     StatusSnapshot.heap_free.isnot(None))
             .order_by(StatusSnapshot.timestamp.asc())
             .all())
    return jsonify([{
        't': s.timestamp.isoformat() + 'Z',
        'heap_free': s.heap_free,
        'sse_stack_free': s.sse_stack_free,
        'net_stack_free': s.net_stack_free,
    } for s in snaps])