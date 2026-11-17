"""What the focus sources share (python-internals.md section 10).

A focus source feeds _focus.FocusWatcher the lines of the vyrn-focus helper (protocol.md section 6), whatever the
system: "ready", "active<TAB>id<TAB>app_id<TAB>title", "none", "closed<TAB>id", "error<TAB>message". On Linux they
come from the helper itself; on macOS and Windows a PollSource asks the system 20 times a second (and once more right
before every key-down, through refresh()) and turns the answers into the same lines.
"""

import collections
import os
import threading

__all__ = ["Window", "looks_like_vyrn", "clean", "PollSource"]

#: One answer of a poll query. `id` stays the same while that window lives; `vyrn` = it belongs to the Vyrn app;
#: `key` (optional) completes the identity: the same id with another (pid, key) is a new window, and the old one closed.
Window = collections.namedtuple("Window", "id app_id title pid vyrn key", defaults=(None,))

_VYRN_NAMES = frozenset({"vyrn", "vyrn.desktop", "vyrn.exe", "vyrn.app"})


def looks_like_vyrn(name):
    """True for the names the Vyrn app goes by: app id "vyrn" / "vyrn.desktop", "Vyrn.exe", a bundle id "*.vyrn"."""
    name = (name or "").strip().lower()
    return name in _VYRN_NAMES or name.endswith(".vyrn")


def clean(text):
    """One line of text without tabs or line breaks (they separate the fields)."""
    return " ".join(str(text or "").replace("\t", " ").splitlines()).strip()


def app_pid():
    """The process that started the worker: the Vyrn app (or check.py)."""
    return os.getppid()


class PollSource:
    """Asks `query` which window is active, 20 times a second, and reports changes as helper lines.

    query.active() -> Window | None (no active window); query.exists(id) -> bool (closed windows are reported once);
    query.close(). Anything it raises is reported as an "error" line, after which the source stops, so the focus
    guard fails closed.
    """

    INTERVAL = 0.05             # 20 Hz

    def __init__(self, query, name, error_text, error_hint):
        self.name = name
        self.error_text = error_text          # "<error_text>: <message>" when the query fails
        self.error_hint = error_hint
        self.ended_text = "Vyrn stopped watching the windows."
        self.install_hint = "Start Vyrn again."
        self.late_text = "Vyrn couldn't see the windows in time."
        self.late_hint = "Try again."
        self._query = query
        self._lock = threading.Lock()         # one poll at a time; always taken before the watcher's lock
        self._stopped = threading.Event()
        self._broken = False
        self._ready = False
        self._last = None                     # (id, app_id, title) last reported, None = no window
        self._seen = {}                       # id -> Window of every window reported active and not closed since
        self._on_line = self._on_end = None
        self._thread = None

    def start(self, on_line, on_end):
        self._on_line, self._on_end = on_line, on_end
        self._thread = threading.Thread(target=self._run, name="vyrn-focus-poll", daemon=True)
        self._thread.start()

    def refresh(self):
        """Poll now, on the calling thread (the FocusWatcher does this right before it answers is_focused())."""
        with self._lock:
            if self._stopped.is_set() or self._broken or self._on_line is None:
                return
            try:
                window = self._query.active()
                current = None if window is None else int(window.id)
                gone = [wid for wid in self._seen if wid != current and not self._query.exists(wid)]
            except Exception as err:            # ctypes trouble: stop, and let the guard fail closed
                self._broken = True
                self._on_line(f"error\t{clean(err) or type(err).__name__}")
                return
            known = self._seen.get(current)
            if known is not None and (known.pid, known.key) != (window.pid, window.key):
                gone.append(current)            # the same id for another program's window: the old one closed
                self._last = None
            for wid in gone:
                del self._seen[wid]
                self._on_line(f"closed\t{wid}")
            state = None if window is None else (current, clean(window.app_id), clean(window.title))
            if state != self._last:
                self._last = state
                if state is None:
                    self._on_line("none")
                else:
                    self._seen[current] = window
                    self._on_line("active\t%d\t%s\t%s" % state)
            if not self._ready:
                self._ready = True
                self._on_line("ready")

    def is_vyrn(self, window):
        """Is this active window (a FocusWatcher dict) the Vyrn app itself?"""
        seen = self._seen.get(window.get("id"))
        return looks_like_vyrn(window.get("app_id")) or (seen is not None and bool(seen.vyrn))

    def stop(self):
        self._stopped.set()
        thread = self._thread
        if thread is not None and thread is not threading.current_thread():
            thread.join(timeout=1.0)
        try:
            self._query.close()
        except Exception:
            pass

    def _run(self):
        try:
            while not self._stopped.is_set():
                self.refresh()
                if self._broken:
                    break
                self._stopped.wait(self.INTERVAL)
        finally:
            self._on_end()
