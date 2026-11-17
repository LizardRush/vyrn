"""What every keyboard backend shares, plus the recording backend the tests use on every system
(python-internals.md section 8).

Backends are mechanism only: one key-down or key-up of an allowed key, named by its evdev code name ("KEY_A").
Each real backend has its own code table for every allowed key (tests/python/test_platform.py enforces that).
"""

import os
import time

from .._effects import ALLOWED_CODES

__all__ = ["Backend", "RecordingBackend", "complain"]


def complain(message):
    """Report a problem on stderr without taking any lock (safe inside signal handlers)."""
    try:
        os.write(2, f"vyrn: {message}\n".encode())
    except OSError:
        pass


class Backend:
    """Sends single key events. Remembers what it pressed so close() can release it all first.

    Bookkeeping is pessimistic: a key counts as pressed *before* its key-down is sent and stops counting
    only *after* its key-up was sent. An interruption (KeyboardInterrupt, a signal handler) can then at
    worst cause a harmless extra key-up, never a key that stays down unnoticed.
    """

    name = "?"
    settle_seconds = 0.0       # wait this long after creating the keyboard before the first key (uinput)
    hint = ""                  # plain-English fix when this keyboard stops working

    def __init__(self):
        self._pressed = []

    def down(self, code):
        if code not in ALLOWED_CODES:          # last line of defence: never send other keys
            raise ValueError(f"refusing to press {code}: it is not on Vyrn's allow-list")
        if code not in self._pressed:
            self._pressed.append(code)
        self._emit(code, 1)

    def up(self, code):
        self._emit(code, 0)
        if code in self._pressed:
            self._pressed.remove(code)

    def close(self):
        try:
            for code in reversed(list(self._pressed)):
                try:
                    self.up(code)
                except Exception as err:       # keep going: the device closes anyway
                    complain(f"could not release {code}: {err}")
        finally:
            self._close()

    def _emit(self, code, value):
        raise NotImplementedError

    def _close(self):
        pass


class RecordingBackend(Backend):
    """Test backend: appends "<monotonic> down|up <CODE>" lines to a file instead of pressing keys."""

    name = "record"

    def __init__(self, path):
        super().__init__()
        # One unbuffered os.write() per line: safe even when a signal handler writes in between. os.open() makes a
        # non-inheritable fd everywhere; O_BINARY keeps Windows from turning "\n" into "\r\n".
        flags = os.O_WRONLY | os.O_APPEND | os.O_CREAT | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_BINARY", 0)
        self._fd = os.open(path, flags, 0o644)

    def _emit(self, code, value):
        os.write(self._fd, f"{time.monotonic():.6f} {'down' if value else 'up'} {code}\n".encode())

    def _close(self):
        os.close(self._fd)
