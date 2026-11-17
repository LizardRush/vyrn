"""Process-wide runtime state of a Vyrn worker, and the errors library code raises.

Everything here is shared by the host (_host.py) and the library modules (_screen, _keys, _focus...).
There is exactly one State object per worker process: ``S``.
"""

import os
import sys
import threading

VERSION = "0.1.0"

#: Every error kind the protocol knows (protocol.md section 3.7).
KINDS = frozenset({"syntax", "load", "exception", "bad_return", "timeout", "input", "focus", "protocol"})


class VyrnUserError(Exception):
    """Raised by library functions for mistakes in the user's scripts.

    The host reports it as an error whose location is the user's calling line (the deepest frame
    inside the project). Construct it as ``VyrnUserError(kind, message, hint="")``; subclasses with
    a fixed kind also accept ``Error(message)`` / ``Error(message, hint)`` and have a default message.
    """

    default_kind = "exception"
    default_message = "Something went wrong in your script."
    default_hint = ""

    def __init__(self, kind=None, message=None, hint=None):
        if kind is not None and kind not in KINDS:
            # Called as Error(message) or Error(message, hint): the class decides the kind.
            kind, message, hint = None, kind, (hint if message is None else message)
        self.kind = kind or self.default_kind
        self.message = str(message) if message is not None else self.default_message
        self.hint = str(hint) if hint is not None else self.default_hint
        super().__init__(self.message)


class SimOnlyError(VyrnUserError):
    """Screen or keyboard functions used in a simulation (a project with a world.py)."""

    default_kind = "input"
    default_message = "Screen and keyboard functions don't work in a simulation."
    default_hint = ("This project has a world.py, so it is a simulation: see() should draw the world "
                    "with vyrn.canvas() or return numbers, and choices should be functions.")


class InputNotAllowedError(VyrnUserError):
    """A key action in a run that may not press keys (protocol.md section 5)."""

    default_kind = "input"
    default_message = ("This run isn't allowed to press keys. Use Train in Vyrn: it counts down and "
                       "asks you to click your game first.")
    default_hint = ""


class FocusError(VyrnUserError):
    """Vyrn can't tell (or lock onto) the active window."""

    default_kind = "focus"
    default_message = "Vyrn couldn't lock onto your game window."
    default_hint = "Click your game window during the countdown, so it is the active window."


def safe_repr(value):
    """repr(value) for a message. Never fails: Python refuses to turn a whole number of over 4300
    digits into text, and a user's own __repr__ may raise."""
    try:
        return repr(value)
    except ValueError:
        if isinstance(value, int):
            digits = int(value.bit_length() * 0.30103) + 1
            return f"a whole number with about {digits} digits"
    except Exception:
        pass
    return f"a {type(value).__name__}"


class State:
    """Runtime state of this worker. The host owns it; library modules read it."""

    def __init__(self):
        self.mode = "realtime"            # "sim" | "realtime"
        self.check = False                # check mode: describe key actions, never send them
        self.describe_keys = False        # vyrn.runtime without --allow-keys: key actions go into `did`, never out
        self.allow_input = False          # may this run press keys at all?
        self.screen_allowed = True        # may this run read the screen? (false: an agent's Check without permission)
        self.loading = False              # the scripts are loading: key actions are skipped
        self.frozen = False               # set by SIGUSR1, cleared by "resume"
        self.stopping = False             # the worker is shutting down (vyrn.wait returns early)
        self.focus_locked = False         # a lock_focus succeeded: from now on the focus guard fails closed
        self.project_dir = ""
        self.decisions_per_second = 10.0
        self.generation = 0               # +1 by the host at the start of EVERY request (screen cache key)
        self.did = []                     # check-mode descriptions for the current request
        self.warnings = []                # pending one-time warnings for the next reply
        self.warned = set()               # keys of warnings already sent (one-time)
        self.show_changed = {}            # name -> value since the last reply
        self.outbox = []                  # vyrn.send() messages for the next reply (guarded by _lock)
        self.outbox_bytes = 0             # their size as JSON
        self.last_request = 0.0           # time.monotonic() of the last request (dead-man timer)
        self.screen = None                # _screen.Screen
        self.keyboard = None              # _keys.Controller
        self.focus = None                 # _focus.FocusWatcher | None
        self._lock = threading.Lock()

    def warn_once(self, key, script, line, message):
        """Queue a warning for the next reply, unless one with this key was already sent.

        Returns True when the warning was queued.
        """
        with self._lock:
            if key in self.warned:
                return False
            self.warned.add(key)
            self.warnings.append({"script": script or "", "line": int(line or 0), "message": str(message)})
            return True

    def take_warnings(self):
        """Return the pending warnings and clear them (the host puts them in a reply)."""
        with self._lock:
            pending, self.warnings = self.warnings, []
        return pending

    # -- locations inside the user's project ------------------------------------------------

    def in_project(self, path):
        """True when the file path lies inside the project folder (and is not Vyrn's own code)."""
        if not path or not self.project_dir or not os.path.isabs(path):
            return False      # "<frozen importlib._bootstrap>", "<string>", ...
        real = os.path.realpath(path)
        root = os.path.realpath(self.project_dir)
        return real.startswith(root + os.sep) and not real.startswith(_LIBRARY_DIR + os.sep)

    def script_name(self, path):
        """Path of a project file relative to the project folder, with "/" separators."""
        rel = os.path.relpath(os.path.realpath(path), os.path.realpath(self.project_dir))
        return rel.replace(os.sep, "/")

    def user_location(self):
        """(script, line) of the innermost stack frame inside the project, or ("", 0).

        Library code uses it to point a warning at the user's line that called it.
        """
        script, line, _ = self.user_place()
        return script, line

    def user_place(self):
        """(script, line, function name) of the innermost stack frame inside the project, or ("", 0, "").

        The function name is "<module>" for code at the top of a file.
        """
        frame = sys._getframe(1)
        while frame is not None:
            filename = frame.f_code.co_filename
            if self.in_project(filename):
                return self.script_name(filename), frame.f_lineno, frame.f_code.co_name
            frame = frame.f_back
        return "", 0, ""


_LIBRARY_DIR = os.path.dirname(os.path.realpath(__file__))

#: The one State of this process.
S = State()
