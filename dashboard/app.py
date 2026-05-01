import os
import re
from datetime import datetime, timedelta
from flask import Flask, request, jsonify, session, redirect, render_template, url_for
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
    bri = db.Column(db.Integer)
    ct = db.Column(db.Integer)
    overhead_on = db.Column(db.Boolean)
    wind_down_step = db.Column(db.Integer)
    wake_step = db.Column(db.Integer)
    wake_total = db.Column(db.Integer)
    soft_pause_remaining_s = db.Column(db.Integer)


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
            ('commands',         'value',                 'INTEGER'),
        ]:
            conn.execute(text(
                f'ALTER TABLE {table} ADD COLUMN IF NOT EXISTS {col} {coltype}'
            ))
        conn.commit()


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
    ))
    db.session.commit()
    return jsonify({'ok': True})


@app.route('/api/log', methods=['POST'])
def ingest_log():
    if not _esp32_authed():
        return jsonify({'error': 'unauthorized'}), 401
    db.session.add(EventLog(message=request.json.get('message', '')))
    db.session.commit()
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


@app.route('/api/status/latest')
def latest_status():
    if not _dashboard_authed():
        return jsonify({'error': 'unauthorized'}), 401
    snap = StatusSnapshot.query.order_by(StatusSnapshot.timestamp.desc()).first()
    if not snap:
        return jsonify(None)
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
    }
    if session.get('role') != 'demo':
        result['timestamp'] = snap.timestamp.isoformat() + 'Z'
    return jsonify(result)


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
    date_str = request.args.get('date')
    if date_str:
        try:
            d = datetime.strptime(date_str, '%Y-%m-%d').date()
        except ValueError:
            return jsonify({'error': 'invalid date'}), 400
    else:
        d = datetime.utcnow().date()
    start = datetime(d.year, d.month, d.day)
    end = start + timedelta(days=1)
    snaps = (StatusSnapshot.query
             .filter(StatusSnapshot.timestamp >= start,
                     StatusSnapshot.timestamp < end,
                     StatusSnapshot.lux.isnot(None))
             .order_by(StatusSnapshot.timestamp.asc())
             .all())
    return jsonify([{'t': s.timestamp.isoformat() + 'Z', 'lux': s.lux, 'state': s.state} for s in snaps])