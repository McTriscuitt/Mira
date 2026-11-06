"""The day the lights follow. The board posts its own copy when it boots and when an
admin types `schedule check`, and that copy always wins. DEFAULT is the site's copy,
shown until the board has posted one. A test compares it with src/config.h.

All times are local seconds since midnight (US Central). The driven day starts at the
wake window (06:00) and runs past midnight to lights-out (01:00), so a period's end can
be smaller than its start.
"""

import json

LIGHTS = ('floor', 'dresser', 'chest', 'ceiling')
EVENT_KINDS = ('wake_window', 'auto_wake', 'light_on', 'light_off', 'sleep_cutoff', 'lights_out')
MAX_BODY_BYTES = 8192
_DAY_S = 86400


def _hhmm(h, m):
    return h * 3600 + m * 60


# Mirrors src/config.h and the SEGS / TIMELINE_EVENTS tables in src/timeline.cpp.
DEFAULT = {
    'periods': [
        {'start': _hhmm(6, 0), 'end': _hhmm(10, 0), 'from': 75.0, 'to': 75.0},
        {'start': _hhmm(10, 0), 'end': _hhmm(11, 0), 'from': 75.0, 'to': 100.0},
        {'start': _hhmm(11, 0), 'end': _hhmm(17, 0), 'from': 100.0, 'to': 100.0},
        {'start': _hhmm(17, 0), 'end': _hhmm(18, 0), 'from': 100.0, 'to': 75.0},
        {'start': _hhmm(18, 0), 'end': _hhmm(19, 0), 'from': 75.0, 'to': 75.0},
        {'start': _hhmm(19, 0), 'end': _hhmm(20, 0), 'from': 75.0, 'to': 40.0},
        {'start': _hhmm(20, 0), 'end': _hhmm(20, 45), 'from': 40.0, 'to': 40.0},
        {'start': _hhmm(20, 45), 'end': _hhmm(21, 0), 'from': 40.0, 'to': 25.0},
        {'start': _hhmm(21, 0), 'end': _hhmm(1, 0), 'from': 25.0, 'to': 25.0},
    ],
    'events': [
        {'at': _hhmm(6, 0), 'kind': 'wake_window'},
        {'at': _hhmm(9, 0), 'kind': 'auto_wake'},
        {'at': _hhmm(10, 0), 'kind': 'light_on', 'light': 'ceiling'},
        {'at': _hhmm(19, 0), 'kind': 'light_off', 'light': 'ceiling'},
        {'at': _hhmm(20, 0), 'kind': 'light_off', 'light': 'chest'},
        {'at': _hhmm(21, 0), 'kind': 'sleep_cutoff'},
        {'at': _hhmm(22, 30), 'kind': 'light_off', 'light': 'dresser'},
        {'at': _hhmm(1, 0), 'kind': 'lights_out'},
    ],
    'wake': {'from': 25.0, 'to': 75.0, 'fade_s': 900, 'joins': {'dresser': 300, 'chest': 600}},
    'ct': {'cool': 280, 'warm': 400, 'warm_bri': 19.7},
    'nap_s': 3600,
}


def _is_int(value, low, high):
    return isinstance(value, int) and not isinstance(value, bool) and low <= value <= high


def _is_level(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and 0 <= value <= 100


def clean(data):
    """The board's schedule with only the fields the site uses, or None if anything is
    missing or out of range."""
    if not isinstance(data, dict):
        return None
    periods, events = data.get('periods'), data.get('events')
    wake, ct = data.get('wake'), data.get('ct')
    if not (isinstance(periods, list) and 0 < len(periods) <= 32):
        return None
    if not (isinstance(events, list) and len(events) <= 32):
        return None
    if not (isinstance(wake, dict) and isinstance(ct, dict)):
        return None

    out = {'periods': [], 'events': []}
    for p in periods:
        if not (isinstance(p, dict) and _is_int(p.get('start'), 0, _DAY_S - 1)
                and _is_int(p.get('end'), 0, _DAY_S - 1)
                and _is_level(p.get('from')) and _is_level(p.get('to'))):
            return None
        out['periods'].append({'start': p['start'], 'end': p['end'],
                               'from': float(p['from']), 'to': float(p['to'])})
    for e in events:
        if not (isinstance(e, dict) and _is_int(e.get('at'), 0, _DAY_S - 1)
                and e.get('kind') in EVENT_KINDS):
            return None
        row = {'at': e['at'], 'kind': e['kind']}
        if e['kind'] in ('light_on', 'light_off'):
            if e.get('light') not in LIGHTS:
                return None
            row['light'] = e['light']
        out['events'].append(row)

    joins = wake.get('joins')
    if not (_is_level(wake.get('from')) and _is_level(wake.get('to'))
            and _is_int(wake.get('fade_s'), 1, _DAY_S) and isinstance(joins, dict)
            and all(k in LIGHTS and _is_int(v, 0, _DAY_S) for k, v in joins.items())):
        return None
    out['wake'] = {'from': float(wake['from']), 'to': float(wake['to']),
                   'fade_s': wake['fade_s'], 'joins': dict(joins)}

    if not (_is_int(ct.get('cool'), 153, 500) and _is_int(ct.get('warm'), 153, 500)
            and _is_level(ct.get('warm_bri'))):
        return None
    out['ct'] = {'cool': ct['cool'], 'warm': ct['warm'], 'warm_bri': float(ct['warm_bri'])}

    if not _is_int(data.get('nap_s'), 0, _DAY_S):
        return None
    out['nap_s'] = data['nap_s']
    return out


def dumps(schedule):
    return json.dumps(schedule, separators=(',', ':'))
