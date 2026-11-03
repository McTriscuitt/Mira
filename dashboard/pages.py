"""The HTML pages: the terminal, and the old dashboard behind the admin login."""

from flask import Blueprint, redirect, render_template, request, url_for

from auth_api import record_visit
from security import csrf_token, is_admin

bp = Blueprint('pages', __name__)


def _terminal(start_dir):
    record_visit('page', request.path)
    admin = is_admin()
    return render_template('landing.html', start_dir=start_dir, admin=admin,
                           csrf=csrf_token() if admin else '')


@bp.route('/')
def landing():
    return _terminal('')


@bp.route('/mira')
def mira():
    """A shareable link, and a refresh inside Mira, land inside Mira."""
    return _terminal('mira')


def _legacy_page(template):
    if not is_admin():
        return redirect(url_for('pages.landing'))
    record_visit('page', request.path)
    return render_template(template, csrf=csrf_token())


@bp.route('/legacy')
def legacy():
    return _legacy_page('index.html')


@bp.route('/legacy/logs')
def legacy_logs():
    return _legacy_page('logs.html')


@bp.route('/legacy/lux')
def legacy_lux():
    return _legacy_page('lux_curve.html')
