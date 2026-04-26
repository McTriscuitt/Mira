import os
from datetime import datetime, timedelta
from flask import Flask, request, jsonify, session, redirect, render_template, url_for
from flask_sqlalchemy import SQLAlchemy
from sqlalchemy import text

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
    return session.get('logged_in')


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
    if not _dashboard_authed():
        return jsonify({'error': 'unauthorized'}), 401
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
        if request.form.get('password') == DASHBOARD_PASSWORD:
            session.permanent = True
            session['logged_in'] = True
            return redirect(url_for('index'))
        return render_template('login.html', error=True)
    return render_template('login.html', error=False)


@app.route('/logout')
def logout():
    session.clear()
    return redirect(url_for('login'))


# --- Dashboard API ---

@app.route('/')
def index():
    if not _dashboard_authed():
        return redirect(url_for('login'))
    return render_template('index.html')


@app.route('/api/status/latest')
def latest_status():
    if not _dashboard_authed():
        return jsonify({'error': 'unauthorized'}), 401
    snap = StatusSnapshot.query.order_by(StatusSnapshot.timestamp.desc()).first()
    if not snap:
        return jsonify(None)
    pending = Command.query.filter_by(status='pending').order_by(Command.created_at).first()
    return jsonify({
        'state': snap.state,
        'lux': snap.lux,
        'bri': snap.bri,
        'ct': snap.ct,
        'overhead_on': snap.overhead_on,
        'timestamp': snap.timestamp.isoformat() + 'Z',
        'wind_down_step': snap.wind_down_step,
        'wake_step': snap.wake_step,
        'wake_total': snap.wake_total,
        'soft_pause_remaining_s': snap.soft_pause_remaining_s,
        'pending_command_id': pending.id if pending else None,
    })


@app.route('/api/log/recent')
def recent_log():
    if not _dashboard_authed():
        return jsonify({'error': 'unauthorized'}), 401
    limit = request.args.get('limit', 10, type=int)
    entries = EventLog.query.order_by(EventLog.timestamp.desc()).limit(min(limit, 500)).all()
    return jsonify([{'timestamp': e.timestamp.isoformat() + 'Z', 'message': e.message} for e in entries])


@app.route('/logs')
def logs():
    if not _dashboard_authed():
        return redirect(url_for('login'))
    return render_template('logs.html')


@app.route('/api/command', methods=['POST'])
def send_command():
    if not _dashboard_authed():
        return jsonify({'error': 'unauthorized'}), 401
    data = request.json
    cmd = Command(action=data['action'], value=data.get('value'))
    db.session.add(cmd)
    db.session.commit()
    return jsonify({'ok': True, 'id': cmd.id})