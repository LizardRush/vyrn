"""The focus guard's eyes (protocol.md section 6, python-internals.md section 10).

A focus source from vyrn._platform delivers the vyrn-focus helper's lines, on every system:

    ready | active<TAB>id<TAB>app_id<TAB>title | none | closed<TAB>id | error<TAB>message

On Linux they come from the helper (its stdin is a pipe we keep open, so it exits when this process dies); on macOS
(the WindowServer's front window) and Windows (GetForegroundWindow) a thread polls 20 times a second and writes the
same lines, and is_focused() polls once more right before it answers. Everything else just looks at the latest state. If the source
stops while a window is locked, is_focused() says False, so no key is ever sent blind.
"""

import threading

from vyrn import _platform
from vyrn._platform.focus_base import looks_like_vyrn as _is_vyrn  # noqa: F401  (kept for older callers)
from vyrn._state import FocusError

__all__ = ["probe", "FocusWatcher"]

_CLICK_HINT = "During the countdown, click your game window so it is in front, then keep your hands off."


def probe(helper_path=None):
    """(backend name, None) when the focus guard can work here ("vyrn-focus", "cgwindowlist" or "win32"), else
    (None, plain-English problem).

    Only looks: it never starts the helper.
    """
    return _platform.focus_probe(helper_path)


class FocusWatcher:
    """Keeps track of the active window, as reported by this system's focus source."""

    def __init__(self, helper_path=None, source=None):
        if source is None:
            source = _platform.focus_source(helper_path)   # FocusError when it can't run here
        self._source = source
        self._lock = threading.RLock()    # re-entrant: safe even if a signal handler asks too
        self._ready = threading.Event()   # set on "ready", "error" or when the source stops
        self._saw_ready = False
        self._active = None               # {"id", "app_id", "title"} of the active window
        self._locked = None               # the window we locked onto
        self._locked_closed = False
        self._error = None                # the source's own "error" message
        self._ended = False               # the source has stopped
        source.start(self._handle, self._on_end)

    # ----- public -----

    def wait_ready(self, timeout=1.0):
        """Wait up to timeout seconds for the source's first full state. True when it is ready."""
        self._ready.wait(timeout)
        with self._lock:
            return self._saw_ready and self._error is None and not self._ended

    def alive(self):
        """True while the source runs and hasn't reported an error (it may still be starting up)."""
        with self._lock:
            return not self._ended and self._error is None

    def active(self):
        """The active window as {"id": int, "app_id": str, "title": str}, or None (none, or unknown)."""
        with self._lock:
            if self._ended or self._error is not None or self._active is None:
                return None
            return dict(self._active)

    def lock(self):
        """Lock onto the active window and return it. FocusError when there is no usable window."""
        self._source.refresh()            # before taking our lock: a poll calls back into _handle
        with self._lock:
            problem = self._problem()
            if problem:
                raise FocusError(*problem)
            active = self._active
            if active is None:
                raise FocusError("No window is active, so Vyrn doesn't know which window is your game.",
                                 _CLICK_HINT)
            if self._source.is_vyrn(active):
                raise FocusError("Vyrn itself was the active window, so it can't be the game.", _CLICK_HINT)
            self._locked = dict(active)
            self._locked_closed = False
            return dict(active)

    @property
    def locked(self):
        """The window locked onto ({"id", "app_id", "title"}), or None when no lock is active."""
        with self._lock:
            return None if self._locked is None else dict(self._locked)

    def is_focused(self):
        """True when nothing is locked; otherwise True only while the locked window is the active one."""
        if self._locked is not None:
            self._source.refresh()        # polling sources: ask the system right now (not under our lock)
        with self._lock:
            if self._locked is None:
                return True
            if self._ended or self._error is not None or self._locked_closed or self._active is None:
                return False
            return self._active["id"] == self._locked["id"]

    def stop(self):
        """Stop the source and wait for it briefly. Safe to repeat."""
        self._source.stop()

    # ----- the source's lines -----

    def _problem(self):
        """(message, hint) explaining why the source can't be used, or None. Call with the lock held."""
        source = self._source
        if self._error is not None:
            return f"{source.error_text}: {self._error}", source.error_hint
        if self._ended:
            return source.ended_text, source.install_hint
        if not self._saw_ready:
            return source.late_text, source.late_hint
        return None

    def _on_end(self):
        with self._lock:
            self._ended = True
        self._ready.set()

    def _handle(self, line):
        kind, _, rest = line.partition("\t")
        if kind == "ready":
            with self._lock:
                self._saw_ready = True
            self._ready.set()
        elif kind == "active":
            fields = rest.split("\t", 2)
            try:
                wid = int(fields[0])
            except ValueError:
                return
            fields += [""] * (3 - len(fields))
            with self._lock:
                self._active = {"id": wid, "app_id": fields[1], "title": fields[2]}
        elif kind == "none":
            with self._lock:
                self._active = None
        elif kind == "closed":
            try:
                wid = int(rest)
            except ValueError:
                return
            with self._lock:
                if self._active is not None and self._active["id"] == wid:
                    self._active = None
                if self._locked is not None and self._locked["id"] == wid:
                    self._locked_closed = True    # ids may be reused: never trust this lock again
        elif kind == "error":
            with self._lock:
                self._error = rest.strip() or "unknown problem"
            self._ready.set()
        # other lines: ignored, for forward compatibility
