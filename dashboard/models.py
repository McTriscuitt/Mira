"""Database tables and the additive schema updates that run at boot."""

from datetime import datetime, timezone

from flask_sqlalchemy import SQLAlchemy
from sqlalchemy import inspect, text

db = SQLAlchemy()


def utcnow():
    """Naive UTC, matching the timestamps already stored in the database."""
    return datetime.now(timezone.utc).replace(tzinfo=None)


def iso(moment):
    return moment.isoformat() + 'Z' if moment else None


class StatusSnapshot(db.Model):
    __tablename__ = 'status_snapshots'
    id = db.Column(db.Integer, primary_key=True)
    timestamp = db.Column(db.DateTime, default=utcnow)
    state = db.Column(db.String(20), nullable=False)  # 2.x name; `mode` is the 3.0 name
    mode = db.Column(db.String(10))
    lux = db.Column(db.Float)
    bri = db.Column(db.Float)
    ct = db.Column(db.Integer)
    overhead_on = db.Column(db.Boolean)
    wind_down_step = db.Column(db.Integer)
    wake_step = db.Column(db.Integer)
    wake_total = db.Column(db.Integer)
    soft_pause_remaining_s = db.Column(db.Integer)
    # Out for today
    excl_floor = db.Column(db.Boolean)
    excl_chest = db.Column(db.Boolean)
    excl_dresser = db.Column(db.Boolean)
    excl_ceiling = db.Column(db.Boolean)
    # Physically on
    on_floor = db.Column(db.Boolean)
    on_chest = db.Column(db.Boolean)
    on_dresser = db.Column(db.Boolean)
    on_ceiling = db.Column(db.Boolean)
    stable_lux_count = db.Column(db.Integer)
    # Board memory, admin-only
    heap_free = db.Column(db.BigInteger)
    sse_stack_free = db.Column(db.Integer)
    net_stack_free = db.Column(db.Integer)


class Command(db.Model):
    __tablename__ = 'commands'
    id = db.Column(db.Integer, primary_key=True)
    created_at = db.Column(db.DateTime, default=utcnow)
    action = db.Column(db.String(50), nullable=False)
    value = db.Column(db.Integer, nullable=True)
    executed_at = db.Column(db.DateTime, nullable=True)
    status = db.Column(db.String(20), default='pending')  # pending, executed, cancelled, expired


class BoardSchedule(db.Model):
    """A single row (id 1): the schedule the board last posted, as JSON. The board
    posts it at boot and when asked (`schedule check`)."""
    __tablename__ = 'board_schedule'
    id = db.Column(db.Integer, primary_key=True)
    body = db.Column(db.Text, nullable=False)
    received_at = db.Column(db.DateTime, default=utcnow, nullable=False)


class EventLog(db.Model):
    __tablename__ = 'event_log'
    id = db.Column(db.Integer, primary_key=True)
    timestamp = db.Column(db.DateTime, default=utcnow)
    message = db.Column(db.Text, nullable=False)


class AccessLog(db.Model):
    """Login activity. Passwords and codes are never stored, right or wrong."""
    __tablename__ = 'access_log'
    id = db.Column(db.Integer, primary_key=True)
    timestamp = db.Column(db.DateTime, default=utcnow)
    role = db.Column(db.String(10), nullable=False)
    ip = db.Column(db.String(45))
    # login_ok, login_fail, login_blocked, logout. Empty on rows from before the terminal.
    event = db.Column(db.String(20))
    # Why a login failed: bad_password, bad_code or code_reused.
    detail = db.Column(db.String(40))
    user_agent = db.Column(db.String(256))


class Visit(db.Model):
    """One row per page load or typed command. Deleted after 90 days."""
    __tablename__ = 'visits'
    id = db.Column(db.Integer, primary_key=True)
    timestamp = db.Column(db.DateTime, default=utcnow, index=True)
    kind = db.Column(db.String(10), nullable=False)  # page or command
    ip = db.Column(db.String(45))
    path = db.Column(db.String(128))
    command = db.Column(db.String(64))
    user_agent = db.Column(db.String(256))
    referrer = db.Column(db.String(256))
    admin = db.Column(db.Boolean, default=False)


class AdminSession(db.Model):
    """A logged-in browser. The cookie holds a random token and this holds its hash,
    so deleting the row ends the session even if the cookie was copied."""
    __tablename__ = 'admin_sessions'
    id = db.Column(db.Integer, primary_key=True)
    token_hash = db.Column(db.String(64), unique=True, nullable=False)
    created_at = db.Column(db.DateTime, default=utcnow)
    expires_at = db.Column(db.DateTime, nullable=False)
    ip = db.Column(db.String(45))


class LoginLock(db.Model):
    """Failure count for one address. In Postgres so a redeploy can't reset it."""
    __tablename__ = 'login_locks'
    ip = db.Column(db.String(45), primary_key=True)
    failures = db.Column(db.Integer, default=0, nullable=False)
    last_failure = db.Column(db.DateTime)
    locked_until = db.Column(db.DateTime)


class AuthState(db.Model):
    """A single row (id 1) for login state shared by all addresses."""
    __tablename__ = 'auth_state'
    id = db.Column(db.Integer, primary_key=True)
    last_totp_step = db.Column(db.BigInteger, default=0, nullable=False)
    global_blocked_until = db.Column(db.DateTime)


# Columns added after a table first shipped. `create_all` doesn't alter existing tables.
_ADDED_COLUMNS = [
    ('status_snapshots', 'wind_down_step', 'INTEGER'),
    ('status_snapshots', 'wake_step', 'INTEGER'),
    ('status_snapshots', 'wake_total', 'INTEGER'),
    ('status_snapshots', 'soft_pause_remaining_s', 'INTEGER'),
    ('status_snapshots', 'excl_floor', 'BOOLEAN'),
    ('status_snapshots', 'excl_chest', 'BOOLEAN'),
    ('status_snapshots', 'excl_dresser', 'BOOLEAN'),
    ('status_snapshots', 'excl_ceiling', 'BOOLEAN'),
    ('status_snapshots', 'stable_lux_count', 'INTEGER'),
    ('status_snapshots', 'heap_free', 'BIGINT'),
    ('status_snapshots', 'sse_stack_free', 'INTEGER'),
    ('status_snapshots', 'net_stack_free', 'INTEGER'),
    ('status_snapshots', 'mode', 'VARCHAR(10)'),
    ('status_snapshots', 'on_floor', 'BOOLEAN'),
    ('status_snapshots', 'on_chest', 'BOOLEAN'),
    ('status_snapshots', 'on_dresser', 'BOOLEAN'),
    ('status_snapshots', 'on_ceiling', 'BOOLEAN'),
    ('commands', 'value', 'INTEGER'),
    ('access_log', 'event', 'VARCHAR(20)'),
    ('access_log', 'detail', 'VARCHAR(40)'),
    ('access_log', 'user_agent', 'VARCHAR(256)'),
]

# These tables gain a row every few seconds, and "latest" is now a public read.
_INDEXES = [
    ('ix_status_snapshots_timestamp', 'status_snapshots', 'timestamp'),
    ('ix_event_log_timestamp', 'event_log', 'timestamp'),
    ('ix_commands_status_created', 'commands', 'status, created_at'),
    ('ix_access_log_timestamp', 'access_log', 'timestamp'),
]


def ensure_schema():
    """Bring the database up to date. Every change is additive, so the previous
    deploy keeps working while it overlaps with this one."""
    db.create_all()
    engine = db.engine
    with engine.connect() as conn:
        inspector = inspect(conn)
        existing = {}
        for table, column, column_type in _ADDED_COLUMNS:
            if table not in existing:
                existing[table] = {c['name'] for c in inspector.get_columns(table)}
            if column not in existing[table]:
                conn.execute(text(f'ALTER TABLE {table} ADD COLUMN {column} {column_type}'))
        for name, table, columns in _INDEXES:
            conn.execute(text(f'CREATE INDEX IF NOT EXISTS {name} ON {table} ({columns})'))
        conn.commit()

        # Brightness moved from v1 integers (0-254) to v2 float percent.
        if engine.dialect.name == 'postgresql':
            row = conn.execute(text(
                "SELECT data_type FROM information_schema.columns "
                "WHERE table_name='status_snapshots' AND column_name='bri'"
            )).fetchone()
            if row and row[0] == 'integer':
                conn.execute(text(
                    'ALTER TABLE status_snapshots ALTER COLUMN bri TYPE FLOAT USING bri::float'
                ))
                conn.execute(text(
                    'UPDATE status_snapshots SET bri = ROUND(CAST(bri AS NUMERIC) / 2.54, 1) '
                    'WHERE bri > 100'
                ))
                conn.commit()

    if db.session.get(AuthState, 1) is None:
        db.session.add(AuthState(id=1, last_totp_step=0))
        db.session.commit()
