"""Wakes open status streams when the board posts something new."""

import threading


class Broadcast:
    """Two version counters (status snapshots, log lines) under one Condition. Stream
    generators wait on it and re-query the database when a counter moves. It's local
    to the process, so the Procfile must keep gunicorn at `-w 1` (threads are fine);
    a stream on another worker would never wake.
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


broadcast = Broadcast()
