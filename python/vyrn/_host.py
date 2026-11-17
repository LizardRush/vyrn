"""The worker host: talks to the Vyrn app, loads the user's scripts and runs their hooks.

docs/dev/protocol.md is the contract this implements; python-internals.md section 5 the design.

The main thread serves one request at a time: read a request, run the user's code, send exactly
one reply. A watchdog thread keeps the keyboard safe whatever the main thread is doing: it releases
the keys and exits when the app goes away, handles the freeze (SIGUSR1, or "F" on Windows' control
connection) and SIGTERM, releases the keys as soon as the locked game window loses focus, and runs the
dead-man timer. An interrupt (SIGINT from the app on a timeout, or "I" on Windows) interrupts the user's
code only. How the app is reached and watched on each system is vyrn._platform's business.
"""

import ast
import atexit
import difflib
import faulthandler
import gc
import importlib.util
import inspect
import io
import linecache
import math
import numbers
import os
import platform
import random
import re
import sys
import threading
import time
import traceback
import types

import numpy as np

import vyrn as api
from . import _canvas, _effects, _focus, _keys, _link, _platform, _proto, _screen
from ._state import KINDS, S, VERSION, FocusError, InputNotAllowedError, VyrnUserError, safe_repr

PROTO = 1
HOOKS = ("see", "points", "game_over", "restart", "step")
SCRIPTS = ("world", "inputs", "responses", "points")      # import order
REQUIRED_SCRIPTS = ("inputs", "responses", "points")
TIMINGS = ("see", "points", "game_over", "respond", "step")
MAX_NUMBERS = 4096
MAX_MESSAGE = 1000          # characters of an exception's text that go into an error reply
# A simulation's round-end result carries two pictures (final_obs and the next round's obs) in one frame.
MAX_SIM_PICTURE_BYTES = (_proto.MAX_BIN - _proto.ALIGN) // 2
STUCK_EXIT_EXTRA = 5.0      # seconds on top of twice the app's observe timeout (Host.arm_stuck_exit)
CLOSE_GRACE = 1.0           # once closing and the app is gone: how long a clean exit may take

_USES_KEYS = re.compile(r"\bvyrn\s*\.\s*press\s*\(")
_USES_SCREEN = re.compile(r"\bvyrn\s*\.\s*(?:screen|pixel|on_screen)\s*\(")
_USES_GAME = re.compile(r"\bvyrn\s*\.\s*(?:game|events|connected|send)\b")
_GAME_NAMES = frozenset({"game", "events", "connected", "send"})
_SCREEN_FUNCTIONS = frozenset({"screen", "pixel", "on_screen"})
# from vyrn import tick: a copy that never changes
_COPIED_VALUES = ("tick", "round", "choice", "game", "events", "connected")
_COMMON_MODULES = {"random", "math", "time", "os", "sys", "numpy", "np", "PIL", "json", "itertools"}
_HOOK_LIST = "see, points, game_over, restart and step"

#: How to call Vyrn's own functions, for "wrong number of inputs" mistakes.
_VYRN_CALLS = {
    "screen": "vyrn.screen() takes x, y, width and height, like vyrn.screen(0, 0, 640, 480), or nothing at all "
              "for the whole screen.",
    "pixel": "vyrn.pixel() needs x and y, like vyrn.pixel(20, 20).",
    "on_screen": 'vyrn.on_screen() needs the name of a Snapshot, like vyrn.on_screen("game_over.png"), and '
                 "maybe x and y.",
    "press": 'vyrn.press() needs one key, like vyrn.press("space").',
    "wait": "vyrn.wait() needs a number of seconds, like vyrn.wait(0.5).",
    "canvas": "vyrn.canvas() needs a width and a height, like vyrn.canvas(10, 10).",
    "show": 'vyrn.show() needs a name and a value, like vyrn.show("speed", 3.5).',
    "send": 'vyrn.send() needs the type of message first, like vyrn.send("restart") or '
            'vyrn.send("speed", value=2.0).',
    "Canvas.dot": 'picture.dot() needs x and y, and maybe a color, like picture.dot(3, 4, "white").',
    "Canvas.box": 'picture.box() needs x, y, width and height, and maybe a color, like '
                  'picture.box(0, 9, 3, 1, "gray").',
}


def _log(text):
    """A line for the app's console (stderr). Never raises."""
    try:
        sys.stderr.write(f"[vyrn] {text}\n")
        sys.stderr.flush()
    except Exception:
        pass


def _is_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


def _clip(text, limit):
    """text, cut to at most `limit` characters (with "…" when something was cut)."""
    return text if len(text) <= limit else text[:limit - 1] + "…"


def _safe_str(exc):
    """str(exc), or "" when the exception can't even describe itself (a broken __str__)."""
    try:
        return str(exc)
    except Exception:
        return ""


def _join(words):
    """"a", "a and b", "a, b and c"."""
    words = list(words)
    return words[0] if len(words) == 1 else ", ".join(words[:-1]) + " and " + words[-1]


def _char_col(filename, lineno, colno):
    """1-based column, counted in characters, of a traceback or ast position (0 when unknown).

    Traceback frames and ast nodes count UTF-8 bytes from 0, while SyntaxError.offset counts characters
    from 1; converting here makes `col` mean the same in every error, also on lines with "é" or "×".
    """
    if colno is None:
        return 0
    line = linecache.getline(filename, lineno) if lineno else ""
    if not line:
        return colno + 1
    return len(line.encode("utf-8")[:colno].decode("utf-8", "ignore")) + 1


def _first_char_col(text):
    """1-based column of the first non-blank character of a line (0 when unknown)."""
    if not text or not text.strip():
        return 0
    return len(text) - len(text.lstrip()) + 1


def _nbytes(value):
    return value.nbytes if isinstance(value, np.ndarray) else 0


def _not_a_normal_number(value):
    """Why a number can't be an input: NaN or infinity, or simply too big for the brain's float32."""
    try:
        number = float(value)
    except OverflowError:
        number = math.inf
        if isinstance(value, numbers.Integral):
            return "it is too big to be an input"
    if math.isnan(number) or math.isinf(number):
        return "it must be a normal number, not NaN or infinity"
    return "it is too big to be an input (the limit is about 3.4e38)"


def _plural(count, word):
    return f"{count} {word}{'' if count == 1 else 's'}"


def _describe(value):
    """A short plain-English description of a value, for messages."""
    if isinstance(value, np.generic):
        value = value.item()                  # np.float64(nan) -> nan
    if value is None:
        return "None (nothing)"
    if isinstance(value, bool):
        return str(value)
    if isinstance(value, str):
        text = value if len(value) <= 40 else value[:37] + "..."
        return f"the text {safe_repr(text)}"
    if isinstance(value, numbers.Number):
        text = safe_repr(value)
        if isinstance(value, int) and len(text) > 40:
            return f"a whole number with {len(text.lstrip('-'))} digits"
        return text
    if isinstance(value, (list, tuple)):
        return f"a {type(value).__name__} of {_plural(len(value), 'item')}"
    if isinstance(value, dict):
        return f"a dict of {_plural(len(value), 'item')}"
    if isinstance(value, np.ndarray):
        return f"a numpy array of shape {list(value.shape)} ({value.dtype})"
    return f"a {type(value).__name__}"


class ReplyError(Exception):
    """An error reply the host builds itself: bad returns, load problems, protocol mismatches."""

    def __init__(self, kind, message, hint="", script="", line=0, col=0, func="", fatal=False,
                 trace=None, exc_type=""):
        super().__init__(message)
        self.fields = {"fatal": fatal, "kind": kind, "script": script, "line": line, "col": col,
                       "func": func, "exc_type": exc_type, "message": message, "hint": hint,
                       "trace": list(trace or [])}


class Hook:
    """A hook function found in the user's scripts."""

    __slots__ = ("name", "fn", "takes_world", "script", "line")

    def __init__(self, name, fn, takes_world, script, line):
        self.name, self.fn, self.takes_world, self.script, self.line = name, fn, takes_world, script, line


class Choice:
    """One of the AI's choices: its name, effect and where it is written."""

    __slots__ = ("name", "label", "effect", "takes_world", "script", "line")

    def __init__(self, name, effect, takes_world, script, line):
        self.name = name
        self.label = name.replace("_", " ")
        self.effect = effect
        self.takes_world = takes_world
        self.script = script
        self.line = line


# -- reading the user's source --------------------------------------------------------------------

def _parse(path):
    try:
        with open(path, "rb") as file:
            return ast.parse(file.read(), path)
    except (OSError, SyntaxError, ValueError):
        return None


def _target_names(target):
    if isinstance(target, ast.Name):
        yield target.id
    elif isinstance(target, (ast.Tuple, ast.List)):
        for item in target.elts:
            yield from _target_names(item)
    elif isinstance(target, ast.Starred):
        yield from _target_names(target.value)


def _module_statements(body):
    """Every statement in body that runs at module level, in source order: also those inside if, try, with,
    for, while and match blocks, but not inside def or class bodies."""
    for node in body:
        yield node
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            continue
        for field in ("body", "orelse", "finalbody"):
            block = getattr(node, field, None)
            if isinstance(block, list):
                yield from _module_statements(block)
        for part in getattr(node, "handlers", None) or getattr(node, "cases", None) or ():
            yield from _module_statements(part.body)


def _bindings(tree):
    """name -> (line, how) of each name's last binding by code that runs at module level.

    how is "own" (def, class, assignment...), "import" (import x, from x import y) or "vyrn" (from vyrn
    import y). Names that only a star import brings in are missing.
    """
    found = {}
    for node in _module_statements(tree.body if tree else ()):
        names, how = [], "own"
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            names = [node.name]
        elif isinstance(node, ast.Assign):
            names = [name for target in node.targets for name in _target_names(target)]
        elif isinstance(node, (ast.AnnAssign, ast.AugAssign)):
            names = list(_target_names(node.target))
        elif isinstance(node, (ast.For, ast.AsyncFor)):
            names = list(_target_names(node.target))
        elif isinstance(node, (ast.With, ast.AsyncWith)):
            names = [name for item in node.items if item.optional_vars is not None
                     for name in _target_names(item.optional_vars)]
        elif isinstance(node, ast.Import):
            names, how = [(alias.asname or alias.name).split(".")[0] for alias in node.names], "import"
        elif isinstance(node, ast.ImportFrom):
            names = [alias.asname or alias.name for alias in node.names if alias.name != "*"]
            how = "vyrn" if node.module == "vyrn" and not node.level else "import"
        for name in names:
            found[name] = (node.lineno, how)
    return found


def _star_import_line(tree):
    """Line of the first  from x import *  at module level, or 0."""
    for node in _module_statements(tree.body if tree else ()):
        if isinstance(node, ast.ImportFrom) and any(alias.name == "*" for alias in node.names):
            return node.lineno
    return 0


def _dict_key_lines(tree, name):
    """For a top-level  name = {...}  literal: key -> line of that key."""
    lines = {}
    for node in tree.body if tree else ():
        if (isinstance(node, ast.Assign) and isinstance(node.value, ast.Dict)
                and any(name in _target_names(t) for t in node.targets)):
            lines = {key.value: key.lineno for key in node.value.keys
                     if isinstance(key, ast.Constant) and isinstance(key.value, str)}
    return lines


def _def_line(code, tree):
    """The line of a function's `def`: co_firstlineno is the first decorator's line when it has any."""
    for node in ast.walk(tree) if tree else ():
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name == code.co_name
                and node.decorator_list and min(d.lineno for d in node.decorator_list) == code.co_firstlineno):
            return node.lineno
    return code.co_firstlineno


def _nested_defs(body, outer=None):
    """(outer function name, def node) for every def written inside another function."""
    for node in _module_statements(body):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if outer is not None:
                yield outer, node
            yield from _nested_defs(node.body, node.name)


def _lookalike(name, hooks):
    """The hook among `hooks` that the function name looks like a misspelling of (game_ovr, Step...), or None."""
    if name in HOOKS:
        return None
    plain = name.lower().replace("_", "")
    for hook in hooks:
        if plain == hook.replace("_", ""):
            return hook
    match = difflib.get_close_matches(name.lower(), list(hooks), n=1, cutoff=0.8)
    return match[0] if match else None


def _vyrn_names(tree):
    """How a script refers to vyrn: (names bound to the module, alias -> vyrn function name)."""
    modules, functions = set(), {}
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                if alias.name == "vyrn":
                    modules.add(alias.asname or "vyrn")
        elif isinstance(node, ast.ImportFrom) and node.module == "vyrn" and not node.level:
            for alias in node.names:
                if alias.name == "*":
                    functions.update((name, name) for name in api.__all__)
                else:
                    functions[alias.asname or alias.name] = alias.name
    return modules, functions


def _vyrn_calls(tree):
    """(call node, vyrn function name, enclosing function name or "") for each call of a vyrn function."""
    modules, functions = _vyrn_names(tree)
    if not modules and not functions:
        return

    def visit(node, where):
        for child in ast.iter_child_nodes(node):
            inner = child.name if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef)) else where
            if isinstance(child, ast.Call):
                func = child.func
                if isinstance(func, ast.Attribute) and isinstance(func.value, ast.Name) and func.value.id in modules:
                    yield child, func.attr, where
                elif isinstance(func, ast.Name) and func.id in functions:
                    yield child, functions[func.id], where
            yield from visit(child, inner)

    yield from visit(tree, "")


# -- friendly hints for Python's own errors -------------------------------------------------------

def _python_suggestion(exc):
    """Python's own "Did you mean" / "Did you forget to import" suggestion, if it has one."""
    try:
        text = "".join(traceback.TracebackException.from_exception(exc).format_exception_only())
    except Exception:
        return None, None
    found = re.search(r"Did you mean:? '?([\w.]+)'?\?", text)
    if found:
        return "mean", found.group(1)
    found = re.search(r"Did you forget to import '?([\w.]+)'?\?", text)
    if found:
        return "import", found.group(1)
    return None, None


def _calls_vyrn(name, frame, line):
    """True when the user's line calls Vyrn's own function `name` (vyrn.pixel(...) or an imported pixel(...))."""
    function = getattr(api, name, None) if name in api.__all__ else None
    if function is None or frame is None or not line:
        return False
    scope = frame.f_globals
    for found in re.finditer(r"([A-Za-z_]\w*)\s*\.\s*" + re.escape(name) + r"\s*\(", line):
        if scope.get(found.group(1)) is api:
            return True
    return bool(re.search(r"(?<![\w.])" + re.escape(name) + r"\s*\(", line)) and scope.get(name) is function


def _call_hint(text, frame, line):
    """A hint for a TypeError about the inputs a call gets (missing, too many, unknown name), or None."""
    found = re.match(r"([\w.]+)\(\) (?:missing \d+ required (?:positional|keyword-only) arguments?: (.+)"
                     r"|takes |got an unexpected keyword argument '(\w+)'|got multiple values)", text)
    if not found:
        return None
    name, missing, keyword = found.group(1), re.findall(r"'(\w+)'", found.group(2) or ""), found.group(3)
    short = name.rsplit(".", 1)[-1]
    if name in ("Canvas.dot", "Canvas.box") or _calls_vyrn(name, frame, line):
        return _VYRN_CALLS[name]
    if "world" in missing and "." not in name:
        return f"{name}() needs the world when you call it yourself: write {name}(world)."
    if keyword:
        return f"{name}() has no input called {keyword}: compare this line with the def {short}(...) line."
    return f"This line gives {name}() the wrong number of inputs: compare it with the def {short}(...) line."


def _hint_for(exc, world, func, frame=None, line=""):
    """A plain-English hint for a beginner, for the common Python errors.

    frame and line are the deepest frame inside the project and its source line (for hints about calls).
    """
    how, suggestion = _python_suggestion(exc)
    if isinstance(exc, UnboundLocalError):
        return ("This function changes that variable, so Python treats it as new inside the function. "
                "Give it a value first, or keep the value on world (world.score = 0).")
    if isinstance(exc, NameError):
        name = getattr(exc, "name", None)
        if name == "world":
            where = f"def {func}(world):" if func and func.isidentifier() else "def points(world):"
            return f"To use the world here, give the function an input: {where}"
        if name == "vyrn":
            return "Add  import vyrn  at the top of this file."
        if how == "import" or name in _COMMON_MODULES:
            module = suggestion or name
            return f"Add  import {'numpy as np' if module == 'np' else module}  at the top of this file."
        if how == "mean":
            return f"Did you mean {suggestion}? Names must be spelled exactly the same everywhere."
        return "Is it spelled the same as where you made it?"
    if isinstance(exc, AttributeError):
        name = getattr(exc, "name", None)
        obj = getattr(exc, "obj", None)
        if obj is api and name:
            guess = difflib.get_close_matches(name, api.__all__, n=1)
            if guess:
                return f"Did you mean vyrn.{guess[0]}?"
            return "vyrn has: " + ", ".join(api.__all__) + "."
        if world is not None and obj is world and name:
            first = f"Did you mean world.{suggestion}? If not: " if how == "mean" else ""
            return (f"{first}world.{name} needs a value before it is used, for example in "
                    f"restart(world): world.{name} = 0")
        if _safe_str(exc).startswith("'NoneType' object has no attribute"):
            attribute = f".{name}" if name else "that"
            return (f"This value is None (nothing), so it has no {attribute}. Did the function that made it "
                    "forget to return it? End that function with a return line, like  return picture")
        if how == "mean":
            return f"Did you mean .{suggestion}? Check the spelling after the dot."
        return "Check the spelling after the dot."
    if isinstance(exc, TypeError):
        text = _safe_str(exc)
        call = _call_hint(text, frame, line)
        if call:
            return call
        if "NoneType" in text:
            return "Something here is None. Did a function forget to return a value?"
        if (re.search(r"unsupported operand|can only concatenate|must be str|must be real number", text)
                or ("not supported between instances of" in text and "'str'" in text)):
            return ("You can't mix text and numbers like that. Use str(x) to make text, "
                    "or int(x) / float(x) to make a number.")
        return "A value here has the wrong type for what this line does. print() it to see what it holds."
    if isinstance(exc, ZeroDivisionError):
        return "Something was divided by zero. Make sure the number after / can never be 0."
    if isinstance(exc, IndexError):
        return "That position is past the end. Positions start at 0, so the last one is len(x) - 1."
    if isinstance(exc, KeyError):
        return "That key isn't in the dict. Check its spelling, or use .get(key, default)."
    if isinstance(exc, ModuleNotFoundError):
        return (f"Python can't find the module {safe_repr(exc.name)}. Check the spelling. "
                "If it is a library, it needs to be installed first.")
    if isinstance(exc, ImportError):
        extra = f" Did you mean {suggestion}?" if how == "mean" else ""
        return "Python found the module, but not that name in it." + extra
    if isinstance(exc, RecursionError):
        return "A function keeps calling itself without stopping. Make sure it ends at some point."
    if isinstance(exc, FileNotFoundError):
        return "That file doesn't exist. File names are relative to your project folder."
    if isinstance(exc, ValueError):
        if "values to unpack" in _safe_str(exc):
            return ("The number of names before the = must match the number of values after it. For example "
                    "r, g, b = vyrn.pixel(20, 20) works because vyrn.pixel() gives 3 values, but "
                    "vyrn.screen() gives one picture.")
        return 'A value has the right type but can\'t be used here, like int("abc").'
    if isinstance(exc, AssertionError):
        return "An assert line found its condition False."
    if isinstance(exc, OverflowError):
        return "A number got too big."
    if isinstance(exc, MemoryError):
        return "Python ran out of memory. Is something keeping lots of pictures?"
    return ""


def _syntax_hint(exc):
    if isinstance(exc, TabError):
        return ("This line is indented with a Tab, but the lines around it use spaces (or the other way "
                "round). Use spaces only, 4 per level.")
    if isinstance(exc, IndentationError):
        return ("The spaces at the start of the lines don't line up. Lines inside a def or an if "
                "need the same indent (4 spaces is usual).")
    if "Maybe you meant '=='" in (exc.msg or ""):
        return "To compare, use == (two equals signs). One = gives a name a value."
    return "Check this line (and the one above it) for a missing colon, bracket or quote."


def _deepest_project_frame(exc):
    """(frame, source line) of the deepest traceback entry inside the project, or (None, "")."""
    found, tb = (None, ""), exc.__traceback__
    while tb is not None:
        code = tb.tb_frame.f_code
        if S.in_project(code.co_filename):
            found = (tb.tb_frame, linecache.getline(code.co_filename, tb.tb_lineno))
        tb = tb.tb_next
    return found


def _same_file(a, b):
    try:
        return bool(a) and bool(b) and os.path.realpath(a) == os.path.realpath(b)
    except (TypeError, ValueError):
        return False


def preload():
    """Import now the modules Vyrn would otherwise import later, while the project folder is not on sys.path
    yet: a user's helper called glob.py or secrets.py must never run in their place, inside Vyrn.

    (evdev imports glob, asyncio, ssl...; PIL.ImageGrab imports tempfile and subprocess; numpy.random imports secrets,
    hashlib and hmac.)
    """
    try:
        _platform.preload()
        import numpy.random  # noqa: F401
    except Exception as err:          # a broken install: the problem shows up later, where it matters
        _log(f"couldn't preload a module: {err!r}")


def exit_worker(code):
    """End the worker process with this exit status, even when the user's scripts started threads that
    never end (a normal interpreter exit would wait for them forever).

    Without such a thread this is a normal exit. Otherwise the user's own atexit clean-up runs (gpiozero, for
    example, switches its pins off there), the files the scripts left open are flushed, and the process ends
    right away.
    """
    main = threading.main_thread()
    if not any(thread.is_alive() and not thread.daemon for thread in threading.enumerate() if thread is not main):
        sys.exit(code)
    try:
        atexit._run_exitfuncs()
    except BaseException:
        pass
    _flush_open_files()
    os._exit(code)


def _flush_open_files():
    """Flush every file object still open, as a normal exit would (os._exit() doesn't): a log the user's
    scripts write without closing it must not lose its last lines."""
    for obj in gc.get_objects():
        if isinstance(obj, io.IOBase):
            try:
                if not obj.closed:
                    obj.flush()
            except Exception:
                pass


# -- the host ---------------------------------------------------------------------------------------

class Host:
    """Serves the app's requests for one project (one worker process)."""

    def __init__(self, project, link):
        self.project = os.path.abspath(project)
        self.link = link             # vyrn._platform.transport.Link (None: in-process, vyrn.runtime)
        self.sock = link.sock if link is not None else None
        self.watch = None            # what the watchdog waits on (made by install_signals)
        self.world = types.SimpleNamespace()
        self.modules = {}
        self.sources = {}            # script name -> source text
        self.trees = {}              # script name -> parsed source (None when it can't be parsed)
        self.hooks = {}
        self.choices = []
        self.load_tried = False
        self.loaded = False
        self.round_limit = None
        self.focus_helper = None
        self.closing = False
        self.shut_down = False
        self.round = 0
        self.tick = 0
        self.obs_kind = None
        self.obs_names = None
        self.obs_shape = None
        self.last_show = {}
        self.ms = {}
        self.in_user_code = False
        self.running = ""            # what user code runs right now, for timeout messages
        self.handlers = {
            "load": self.on_load,
            "reset": self.on_reset, "step": self.on_step,
            "lock_focus": self.on_lock_focus, "restart": self.on_restart,
            "check_over": self.on_check_over, "observe": self.on_observe,
            "act": self.on_act, "status": self.on_status,
            "pause": self.on_pause, "resume": self.on_resume, "close": self.on_close,
        }

    # -- main loop ---------------------------------------------------------------------------------

    def run(self):
        self.install_signals()
        self.start_services()
        try:
            if not self.send(self.hello()):
                return 1
            while True:
                try:
                    msg = _proto.recv(self.sock)
                except _proto.ProtocolError as err:
                    self.protocol_failure(err)
                    return 2
                except OSError:
                    return 1                     # the connection broke: the app is gone
                if msg is None:
                    return 0                     # the app closed the connection
                if not self.send(self.handle(msg)):
                    return 1
                if self.closing:
                    return 0
        finally:
            self.shutdown()

    def start_services(self):
        """What the requests need before the first one: the screen, the keyboard and the watchdog that keeps the
        keys safe. self.watch must be set first (install_signals(), or vyrn.runtime's own watch in-process)."""
        S.project_dir = self.project
        S.screen = _screen.Screen()
        S.keyboard = _keys.Controller(S)
        atexit.register(self.release_keys)
        Watchdog(self).start()

    def hello(self):
        backends, problems = {}, {}
        probes = (("screen", _screen.probe), ("keys", _keys.probe), ("focus", lambda: _focus.probe(None)))
        for name, probe in probes:
            try:
                backend, problem = probe()
            except Exception as err:
                backend, problem = None, f"Vyrn couldn't check this: {err}"
            backends[name] = backend
            if problem:
                problems[name] = problem
        return {"t": "hello", "id": 0, "proto": PROTO, "vyrn": VERSION,
                "python": platform.python_version(), "pid": os.getpid(),
                "backends": backends, "problems": problems}

    def handle(self, msg):
        """Turn one request into exactly one reply (a normal reply or an error)."""
        kind = msg.get("t")
        rid = msg.get("id")
        good_id = _is_int(rid)
        rid = rid if good_id else 0
        S.generation += 1
        S.did = []
        if kind != "status":
            # The dead-man timer counts from the last request that is part of playing. "status" runs no user
            # code and is polled while the app holds, when the keys must be up anyway: it doesn't count.
            S.last_request = time.monotonic()
        self.ms = {}
        try:
            handler = self.handlers.get(kind) if isinstance(kind, str) else None
            if handler is None or not good_id:
                raise ReplyError("protocol", f"The worker doesn't understand the request {safe_repr(kind)} "
                                 f"(id {safe_repr(msg.get('id'))}).", "The Vyrn app and its Python library "
                                 "don't match: reinstall Vyrn.", fatal=True)
            reply_type, fields = handler(msg)
            reply = {"t": reply_type, "id": rid}
            reply.update((key, value) for key, value in fields.items() if key not in reply)
            sends = _link.take_outbox()       # vyrn.send() messages go to the game with this reply
            if sends:
                reply["send"] = sends
            return reply
        except BaseException as exc:          # user code may raise anything, even SystemExit
            self.release_keys()               # first: describing the error may run the user's own code
            _link.take_outbox()               # a request that failed sends nothing to the game
            error = self.describe_error(exc)
            if kind == "load":
                error["fatal"] = True         # anything during load ends the run
            return {"t": "error", "id": rid, **error}

    def send(self, msg):
        try:
            frame = _proto.encode(msg)
        except (_proto.ProtocolError, ValueError, TypeError) as err:
            self.release_keys()
            frame = _proto.encode(self.encoding_failure(msg, err))
        try:
            self.sock.sendall(frame)
            return True
        except OSError:
            return False

    def encoding_failure(self, msg, err):
        """The error sent instead of a reply that doesn't fit in a frame (or can't be encoded at all)."""
        part = getattr(err, "part", None)              # _proto.FrameTooBig: "json" or "data"
        see = self.hooks.get("see")
        see_bytes = sum(_nbytes(msg.get(key)) for key in ("obs", "final_obs", "preview"))
        if part == "data" and see is not None and see_bytes > _proto.MAX_BIN:
            fields = ReplyError("bad_return", "see() returned a picture too big to send to Vyrn.",
                                "Use a smaller area of the screen, or a smaller canvas.",
                                script=see.script, line=see.line, func="see").fields
        elif part == "data":
            fields = ReplyError("exception", "The pictures given to vyrn.show() are too big to send to Vyrn.",
                                "Show smaller pictures, or fewer of them.").fields
        elif part == "json":
            fields = ReplyError("exception", "This reply holds too much text to send to Vyrn (over 1 MB).",
                                "This usually comes from vyrn.show() with very many different names: show a "
                                "few values and update them instead.").fields
        else:
            fields = ReplyError("protocol", f"The worker couldn't send its reply: {err}", fatal=True).fields
        if msg.get("t") == "spec":
            fields["fatal"] = True                     # anything during load ends the run
        return {"t": "error", "id": msg.get("id", 0), **fields}

    def protocol_failure(self, err):
        _log(f"bad message from the app: {err}")
        self.release_keys()
        fields = ReplyError("protocol", f"The worker got a broken message: {err}", fatal=True).fields
        self.send({"t": "error", "id": 0, **fields})

    # -- signals, safety and shutdown ------------------------------------------------------------

    def install_signals(self):
        # The watchdog thread reacts to the freeze, SIGTERM and the app's death through the link's watch (on POSIX
        # through signal.set_wakeup_fd), so they work even while the user's code is stuck.
        self.watch = self.link.watch()
        self.watch.install(self.on_sigint)

    def on_sigint(self, signum, frame):
        # The app sends SIGINT (on Windows "I", which the watchdog turns into _thread.interrupt_main()) when a
        # request takes too long. Interrupt the user's code only: a KeyboardInterrupt inside the host could cut a
        # reply in half.
        if self.in_user_code:
            raise KeyboardInterrupt

    def release_keys(self):
        keyboard = S.keyboard
        if keyboard is not None:
            try:
                keyboard.release_all()
            except Exception as err:
                _log(f"couldn't release the keys: {err!r}")

    def emergency_exit(self, code):
        """Release every key and leave right now (called from the watchdog thread)."""
        S.stopping = True
        S.frozen = True               # refuse any key action that is still on its way
        self.release_keys()
        try:
            if S.keyboard is not None:
                S.keyboard.close()
        finally:
            os._exit(code)

    def shutdown(self):
        if self.shut_down:
            return
        self.shut_down = True
        S.stopping = True
        S.frozen = True               # from now on nothing presses keys, not even a thread the scripts started
        self.closing = True
        self.release_keys()
        for thing in (S.keyboard, S.focus):
            if thing is None:
                continue
            try:
                thing.close() if thing is S.keyboard else thing.stop()
            except Exception as err:
                _log(f"problem while shutting down: {err!r}")

    # -- running user code -----------------------------------------------------------------------

    def user_code(self, fn, args, timing, running):
        """Run a piece of the user's code: time it and let SIGINT interrupt it."""
        self.running = running
        armed = self.arm_stuck_exit()
        start = time.perf_counter()
        self.in_user_code = True
        try:
            return fn(*args)
        finally:
            self.in_user_code = False
            if armed:
                faulthandler.cancel_dump_traceback_later()
            if timing:
                self.ms[timing] = self.ms.get(timing, 0.0) + (time.perf_counter() - start) * 1000.0

    def arm_stuck_exit(self):
        """While keys are down, a last-resort timer that ends the process if the user's code never returns.

        The watchdog thread (app gone, SIGUSR1, dead-man timer) needs the GIL, and one long C call in the
        user's code (a runaway regular expression, huge integer maths...) can keep it. faulthandler's timer
        is a C thread that needs no GIL: it ends the process, and the kernel then releases every key of the
        virtual keyboard. It is set well past the app's own timeouts, so it only fires when the app couldn't
        stop the worker itself (for example because the app is gone).
        """
        keyboard = S.keyboard
        if S.mode != "realtime" or S.check or keyboard is None or not keyboard.keys_down():
            return False
        app_timeout = max(1.0, 5.0 / max(S.decisions_per_second, 1e-6))   # protocol.md section 7: observe, act
        try:
            faulthandler.dump_traceback_later(2 * app_timeout + STUCK_EXIT_EXTRA, exit=True, file=2)
        except (OSError, ValueError, RuntimeError):
            return False
        return True

    def run_hook(self, name):
        hook = self.hooks[name]
        return self.user_code(hook.fn, (self.world,) if hook.takes_world else (), name, f"{name}()")

    def timings(self):
        ms = dict.fromkeys(TIMINGS, 0.0)
        ms.update(self.ms)
        return {name: round(value, 3) for name, value in ms.items()}

    def def_location(self, fn):
        """(script, line of its def) of a function written in the project; ("", 0) for any other."""
        try:
            fn = inspect.unwrap(fn)             # a decorator made with functools.wraps: the user's own function
        except ValueError:
            pass
        code = getattr(fn, "__code__", None)
        if code is None or not S.in_project(code.co_filename):
            return "", 0
        return S.script_name(code.co_filename), _def_line(code, self.tree_of(code.co_filename))

    def tree_of(self, path):
        """The parsed source of a project file (parsed once)."""
        for name, module in self.modules.items():
            if _same_file(getattr(module, "__file__", None), path) and name in self.trees:
                return self.trees[name]
        key = os.path.realpath(path)
        if key not in self.trees:
            self.trees[key] = _parse(path)
        return self.trees[key]

    def bad_return(self, hook, message, hint=""):
        return ReplyError("bad_return", message, hint, script=hook.script, line=hook.line, func=hook.name,
                          trace=[f"{hook.script}:{hook.line} in {hook.name}"])

    # -- errors ---------------------------------------------------------------------------------

    def describe_error(self, exc):
        """The fields of an error reply for exc, whatever the user's code inside it does.

        Building the message may run the user's code (an exception class with its own __str__), so it runs
        like user code: the app's SIGINT can stop it, and then the reply says which line was stuck.
        """
        try:
            self.in_user_code = True
            try:
                return self.error_fields(exc)
            finally:
                self.in_user_code = False
        except KeyboardInterrupt as stuck:
            fields = self.error_fields(stuck)
            if not fields["script"]:              # interrupted in Vyrn's own code: point at the original error
                fields.update({key: value for key, value in self.error_location(exc)[0].items()
                               if key in ("script", "line", "col", "func", "trace")})
            return fields
        except Exception as err:                  # e.g. an exception class whose __str__ itself fails
            fields = self.error_location(exc)[0]
            fields["message"] = (f"Your code raised {type(exc).__name__}, but Vyrn couldn't read its message "
                                 f"({type(err).__name__}).")
            return fields

    def error_location(self, exc):
        """(fields, project frames) for an exception: where it happened, without running any user code."""
        frames = [f for f in traceback.extract_tb(exc.__traceback__) if S.in_project(f.filename)]
        fields = {"fatal": False, "kind": "exception", "script": "", "line": 0, "col": 0, "func": "",
                  "exc_type": type(exc).__name__, "message": type(exc).__name__, "hint": "",
                  "trace": [f"{S.script_name(f.filename)}:{f.lineno} in {f.name}" for f in reversed(frames)]}
        if frames:
            deepest = frames[-1]
            fields.update(script=S.script_name(deepest.filename), line=deepest.lineno or 0,
                          col=_char_col(deepest.filename, deepest.lineno, getattr(deepest, "colno", None)),
                          func=deepest.name)
        return fields, frames

    def error_fields(self, exc):
        """The fields of an error reply for an exception (protocol.md section 3.7)."""
        if isinstance(exc, ReplyError):
            return dict(exc.fields)
        fields, frames = self.error_location(exc)
        fields["message"] = _safe_str(exc) or type(exc).__name__
        if isinstance(exc, SyntaxError) and exc.filename and S.in_project(exc.filename):
            col = exc.offset or 0
            if isinstance(exc, IndentationError) and "unindent does not match" in (exc.msg or ""):
                col = _first_char_col(exc.text) or col    # Python points past the end of the line
            fields.update(kind="syntax", script=S.script_name(exc.filename), line=exc.lineno or 0,
                          col=col, func="", message=f"Python can't read this line: {exc.msg}",
                          hint=_syntax_hint(exc))
        elif isinstance(exc, KeyboardInterrupt):
            fields.update(kind="timeout",
                          message=f"{self.running or 'Your code'} took too long — is there a loop that never ends?",
                          hint="Vyrn stopped it on the marked line. Make sure every loop can finish quickly.")
        elif isinstance(exc, VyrnUserError):
            fields.update(kind=exc.kind if exc.kind in KINDS else "exception", message=exc.message,
                          hint=exc.hint, exc_type="")          # Vyrn's own words: no Python class name
        elif isinstance(exc, (_effects.EffectError, _canvas.PictureError)):
            fields.update(message=exc.message, hint=exc.hint, exc_type="")
        elif isinstance(exc, SystemExit):
            fields.update(message="Your script called exit() or sys.exit(), which stops Python.",
                          hint="Vyrn starts and stops your scripts itself, so remove that line.")
        else:
            if isinstance(exc, KeyError) and exc.args:
                fields["message"] = f"The key {safe_repr(exc.args[0])} isn't in the dict."
            elif (isinstance(exc, AttributeError) and getattr(exc, "obj", None) is self.world
                  and getattr(exc, "name", None)):
                fields["message"] = f"world has no {exc.name} (world.{exc.name} was never given a value)."
            frame, line = _deepest_project_frame(exc)
            fields["hint"] = _hint_for(exc, self.world, fields["func"], frame, line)
            if not frames:
                # Nothing in the user's project: this looks like a problem inside Vyrn itself.
                traceback.print_exception(exc, file=sys.stderr)
        fields["message"] = _clip(fields["message"], MAX_MESSAGE)    # a huge str(exc) must not break the reply
        return fields

    # -- load -------------------------------------------------------------------------------------

    def on_load(self, msg):
        if self.load_tried:
            raise ReplyError("protocol", "The app sent load twice.", fatal=True)
        self.load_tried = True
        self.apply_settings(msg)
        warnings = []
        S.loading = True              # key actions are skipped (never sent) until the scripts are loaded
        try:
            self.import_scripts()
            self.find_hooks(warnings)
            self.find_choices(warnings)
            sources = self.project_sources()
            if S.mode == "realtime":
                self.check_pressed_keys(sources)
            self.copied_value_warnings(warnings)
            if S.check and S.mode == "realtime":
                self.unchecked_choice_warning(warnings)
            # Infer what the AI sees. A real-time project never runs restart() here: it may press keys.
            if S.mode == "sim" and "restart" in self.hooks:
                self.run_hook("restart")
            preview = self.see()
        finally:
            S.loading = False
        self.loaded = True
        uses_keys, uses_screen = self.uses(sources)
        buttons = []
        for choice in self.choices:
            buttons += [button for button in choice.effect.buttons if button not in buttons]
        return "spec", {
            "mode": S.mode,
            "obs": {"kind": self.obs_kind, "shape": list(self.obs_shape), "names": self.obs_names},
            "choices": [c.name for c in self.choices],
            "labels": [c.label for c in self.choices],
            "effects": [c.effect.describe() for c in self.choices],
            "uses": {"screen": uses_screen, "keys": uses_keys, "game": bool(buttons) or self.uses_game(sources)},
            "buttons": buttons,
            "hooks": {name: f"{hook.script}:{hook.line}" for name, hook in self.hooks.items()},
            "preview": preview,
            "warnings": warnings + S.take_warnings(),
            "ms": self.timings(),
        }

    def apply_settings(self, msg):
        allow = msg.get("allow_input")
        if allow is not None and not isinstance(allow, bool):
            # A safety switch: anything but true keeps it off, and a value that isn't a boolean at all
            # means the app and the library don't match.
            raise ReplyError("protocol", f"The app sent allow_input = {safe_repr(allow)}; it must be true or false.",
                             "The Vyrn app and its Python library don't match: reinstall Vyrn.", fatal=True)
        screen = msg.get("screen")
        if screen is not None and not isinstance(screen, bool):
            raise ReplyError("protocol", f"The app sent screen = {safe_repr(screen)}; it must be true or false.",
                             "The Vyrn app and its Python library don't match: reinstall Vyrn.", fatal=True)
        project = os.path.abspath(str(msg.get("project") or self.project))
        if not os.path.isdir(project):
            raise ReplyError("load", f"Vyrn can't find the project folder {project}.",
                             "It may have been moved or renamed.", fatal=True)
        if project != self.project:
            sys.path[:] = [project if entry == self.project else entry for entry in sys.path]
            if project not in sys.path:
                sys.path.insert(1, project)
            self.project = project
        S.project_dir = project
        os.chdir(project)              # so open("data.txt") finds the project's own files

        S.check = bool(msg.get("check"))
        dps = msg.get("decisions_per_second")
        S.decisions_per_second = float(dps) if isinstance(dps, numbers.Real) and dps > 0 else 10.0
        limit = msg.get("round_limit")
        valid = isinstance(limit, numbers.Real) and not isinstance(limit, bool) and math.isfinite(limit)
        self.round_limit = int(limit) if valid and limit >= 1 else None
        self.focus_helper = msg.get("focus_helper") or None
        seed = msg.get("seed")
        if _is_int(seed):
            random.seed(seed)
            np.random.seed(seed % 2**32)
        sim = os.path.isfile(os.path.join(project, "world.py"))
        S.mode = "sim" if sim else "realtime"
        S.allow_input = allow is True and not sim
        S.screen_allowed = screen is not False
        api.choice, api.tick, api.round = None, 0, 0

    def import_scripts(self):
        if S.mode == "sim":
            see_example = ("import vyrn\n\ndef see(world):\n    picture = vyrn.canvas(10, 10)\n"
                           "    picture.dot(world.x, 5, \"white\")\n    return picture")
            responses_example = "def left(world):\n    world.x = world.x - 1\n\nstay = None"
            points_example = "def points(world):\n    return 1"
        else:
            see_example = "import vyrn\n\ndef see():\n    return vyrn.screen()"
            responses_example = 'left = "a"\nright = "d"\nwait = None'
            points_example = "def points():\n    return 1"
        missing = {
            "inputs": ("Your project needs a file called inputs.py: it says what the AI sees.",
                       "Create inputs.py with a see() function, for example:\n\n" + see_example),
            "responses": ("Your project needs a file called responses.py: it lists the AI's choices.",
                          "Create responses.py, for example:\n\n" + responses_example),
            "points": ("Your project needs a file called points.py: it says how the AI scores.",
                       "Create points.py, for example:\n\n" + points_example),
        }
        for name in REQUIRED_SCRIPTS:
            if not os.path.isfile(os.path.join(self.project, name + ".py")):
                message, hint = missing[name]
                raise ReplyError("load", message, hint, script=f"{name}.py", fatal=True)
        for name in SCRIPTS:
            path = os.path.join(self.project, name + ".py")
            if name == "world" and not os.path.isfile(path):
                continue
            try:
                with open(path, encoding="utf-8", errors="replace") as file:
                    self.sources[name] = file.read()
            except OSError as err:
                raise ReplyError("load", f"Vyrn can't read {name}.py: {err.strerror}.", script=f"{name}.py",
                                 fatal=True) from None
            self.trees[name] = _parse(path)
            module = sys.modules.get(name)
            if module is None or not _same_file(getattr(module, "__file__", None), path):
                spec = importlib.util.spec_from_file_location(name, path)
                module = importlib.util.module_from_spec(spec)
                sys.modules[name] = module
                self.user_code(spec.loader.exec_module, (module,), None, f"Loading {name}.py")
            # else: a script above already did  import points  (which ran it): use that one module, so the
            # scripts share its values and it doesn't run twice.
            self.modules[name] = module

    def takes_world(self, fn, what, script, line, choice=False):
        """True when fn wants the world (one required input). A load error for more than one."""
        try:
            params = list(inspect.signature(fn).parameters.values())
        except (TypeError, ValueError):
            return False
        required = [p.name for p in params if p.default is p.empty
                    and p.kind in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD)]
        keyword_only = [p.name for p in params if p.default is p.empty and p.kind == p.KEYWORD_ONLY]
        if len(required) <= 1 and not keyword_only:
            return len(required) == 1
        names = required + keyword_only
        name = getattr(fn, "__name__", "")
        name = name if name.isidentifier() else "it"
        hint = (f"Write it with one input to get the shared world, like def {name}(world):, "
                f"or with none, like def {name}():.")
        if choice:
            hint += (f" Every function in responses.py is one of the AI's choices: if {name} is a helper, "
                     f"rename it _{name} or move it to another file.")
        raise ReplyError("load", f"{what} takes {len(names)} inputs ({', '.join(names)}), but Vyrn can give "
                         "it at most one: the world.", hint,
                         script=script, line=line, func=getattr(fn, "__name__", ""), fatal=True)

    def find_hooks(self, warnings):
        found = {}
        for module_name, module in self.modules.items():          # in import order
            for name in HOOKS:
                fn = module.__dict__.get(name)
                if not inspect.isfunction(fn) or fn.__module__ != module_name:
                    continue                                       # imported, or not a function
                script, line = self.def_location(fn)
                if name in found:
                    first = found[name]
                    raise ReplyError("load", f"{name}() is defined twice: in {first.script} (line {first.line}) "
                                     f"and in {script} (line {line}).", "Keep only one of them.",
                                     script=script, line=line, func=name, fatal=True)
                found[name] = Hook(name, fn, self.takes_world(fn, f"{name}()", script, line), script, line)
        # Hooks Vyrn would use but didn't find (step only runs in a simulation).
        wanted = [name for name in HOOKS if name not in found and (name != "step" or S.mode == "sim")]
        lookalikes = self.hook_lookalikes(wanted)
        for name, home in (("see", "inputs.py"), ("points", "points.py")):
            if name not in found:
                raise self.missing_hook(name, home, lookalikes.get(name))
        for name in wanted:
            if name in lookalikes:
                script, line, message = lookalikes[name]
                warnings.append({"script": script, "line": line, "message": message})
        if "step" in found and S.mode != "sim":
            hook = found["step"]
            warnings.append({"script": hook.script, "line": hook.line,
                             "message": "step() only runs in a simulation (a project with a world.py), "
                                        "so Vyrn won't call it here."})
        self.hooks = found

    def hook_lookalikes(self, wanted):
        """hook -> (script, line, message) for a function that was surely meant to be that missing hook:
        a misspelling (gameover, Step, point) or a hook written inside another function."""
        found = {}
        for module_name, tree in list(self.trees.items()):
            if module_name not in self.modules or tree is None:
                continue
            script = f"{module_name}.py"
            for outer, node in _nested_defs(tree.body):
                if node.name in wanted and node.name not in found:
                    found[node.name] = (script, node.lineno,
                                        f"{node.name}() is inside {outer}(), so Vyrn can't find it. Move it to "
                                        "the left edge (no indent).")
            for node in _module_statements(tree.body):
                if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                    hook = _lookalike(node.name, wanted)
                    if hook and hook not in found:
                        found[hook] = (script, node.lineno,
                                       f"{node.name}() looks like {hook}(), but Vyrn only uses functions named "
                                       f"exactly {_HOOK_LIST}. Rename it to {hook}.")
        return found

    def missing_hook(self, name, home, lookalike=None):
        arg = "world" if S.mode == "sim" else ""
        examples = {
            "see": f"import vyrn\n\ndef see({arg}):\n    return vyrn.screen()" if not arg else
                   f"import vyrn\n\ndef see(world):\n    picture = vyrn.canvas(10, 10)\n    return picture",
            "points": f"def points({arg}):\n    return 1",
        }
        what = {"see": "it returns what the AI sees", "points": "it returns the AI's points each tick"}[name]
        if lookalike:
            script, line, message = lookalike
            return ReplyError("load", f"Vyrn can't find a {name}() function. {message}",
                              "For example:\n\n" + examples[name], script=script, line=line, fatal=True)
        for module_name, module in self.modules.items():
            if name in module.__dict__:
                return ReplyError("load", f"{name} in {module_name}.py isn't a function written in that file, "
                                  f"so Vyrn can't use it as {name}().", "For example:\n\n" + examples[name],
                                  script=f"{module_name}.py", fatal=True)
        return ReplyError("load", f"Vyrn can't find a {name}() function. It goes in {home}: {what}.",
                          "For example:\n\n" + examples[name], script=home, fatal=True)

    def find_choices(self, warnings):
        module = self.modules["responses"]
        tree = self.trees.get("responses")
        bindings = _bindings(tree) if tree is not None else None
        entries = []                                    # (name, effect value, line)
        not_choices = []                                # (name, value): plain values that aren't choices
        star_only = []                                  # would-be choices that only a star import brings in
        if "RESPONSES" in module.__dict__:
            table = module.__dict__["RESPONSES"]
            table_line = (bindings or {}).get("RESPONSES", (0, ""))[0]
            if not isinstance(table, dict):
                raise ReplyError("load", "RESPONSES must be a dict.",
                                 'For example: RESPONSES = {"left": "a", "right": "d", "wait": None}',
                                 script="responses.py", line=table_line, fatal=True)
            key_lines = _dict_key_lines(tree, "RESPONSES")
            for key, value in table.items():
                if not isinstance(key, str) or not key:
                    raise ReplyError("load", f"Each name in RESPONSES must be text, not {safe_repr(key)}.",
                                     'For example: RESPONSES = {"left": "a", "wait": None}',
                                     script="responses.py", line=table_line, fatal=True)
                entries.append((key, value, key_lines.get(key, table_line)))
        else:
            for name, value in module.__dict__.items():
                if name.startswith("_") or name in HOOKS:
                    continue
                if not (value is None or isinstance(value, str)
                        or (inspect.isfunction(value) and value.__module__ == "responses")):
                    if isinstance(value, (numbers.Number, list, tuple, dict, set)):
                        not_choices.append((name, value))
                    continue
                binding = bindings.get(name) if bindings is not None else (0, "own")
                if binding is None:
                    # Only  from x import *  brought it in: it isn't written in responses.py, so it has no
                    # place in the file's order. (from vyrn import * brings vyrn.choice.)
                    if name not in api.__all__:
                        star_only.append(name)
                    continue
                if binding[1] == "vyrn":
                    continue                            # from vyrn import choice: Vyrn's value, not a choice
                entries.append((name, value, binding[0]))
        if star_only:
            many = len(star_only) > 1
            warnings.append({"script": "responses.py", "line": _star_import_line(tree),
                             "message": f"{_join(star_only)} come{'' if many else 's'} from a  from ... import *  "
                                        f"line, so {'they are not choices' if many else 'it is not a choice'}. "
                                        "Write each choice in responses.py itself, like  wait = None"})

        choices = []
        for name, value, line in entries:
            script = "responses.py"
            takes = False
            if callable(value) and not isinstance(value, (str, type)):
                fn_script, fn_line = self.def_location(value)
                if fn_script:
                    script, line = fn_script, fn_line
                takes = self.takes_world(value, f"The choice {name}", script, line, choice=True)
            if S.mode == "sim" and _effects.is_game_effect(value):
                raise ReplyError("load", f'{name} = "{value}": a simulation has no game to press buttons in; use '
                                 "a function.",
                                 "Game buttons are for a game connected with Vyrn Link, a project without world.py. "
                                 "In a simulation a choice is a function that changes the world, like:\n\n"
                                 "def left(world):\n    world.x = world.x - 1",
                                 script=script, line=line, func=name, fatal=True)
            if S.mode == "sim" and isinstance(value, str):
                raise ReplyError("load", f'{name} = "{value}": simulations can\'t press keys; use a function.',
                                 "In a simulation a choice is a function that changes the world, like:\n\n"
                                 "def left(world):\n    world.x = world.x - 1\n\n"
                                 f"If {name} is just a value your code uses, start its name with _ (like _{name}), "
                                 "so it isn't a choice.",
                                 script=script, line=line, func=name, fatal=True)
            try:
                effect = _effects.parse_effect(value, name)
            except _effects.EffectError as err:
                raise ReplyError("load", err.message, err.hint, script=script, line=line, func=name,
                                 fatal=True) from None
            choices.append(Choice(name, effect, takes, script, line))
        if len(choices) < 2:
            raise self.too_few_choices(choices, not_choices)
        if not any(c.effect.kind == "none" for c in choices):
            warnings.append({"script": "responses.py", "line": 0,
                             "message": "No do-nothing choice: add  wait = None  so the AI can stand still"})
        self.choices = choices

    def too_few_choices(self, choices, not_choices):
        message = f"The AI needs at least 2 choices, but responses.py has {len(choices)}."
        if not_choices:
            def kind(value):
                if isinstance(value, bool):
                    return "True/False value"
                if isinstance(value, numbers.Number):
                    return "number"
                return "list" if isinstance(value, (list, tuple, set)) else "dict"
            kinds = {kind(value) for _, value in not_choices}
            one = len(not_choices) == 1
            noun = kinds.pop() if len(kinds) == 1 else "value"
            shown = [f"{name} = {_clip(_describe(value), 20)}" for name, value in not_choices[:4]]
            if len(not_choices) > 4:
                shown.append("others")
            message += (f" {_join(shown)} {'is a ' + noun if one else 'are ' + noun + 's'}, so "
                        f"{'it is not a choice' if one else 'they are not choices'}. A choice is a key in quotes "
                        '("a"), None to do nothing, or a function.')
        if len(choices) == 1:
            hint = "Add another one, for example:  wait = None"
        elif S.mode == "sim":
            hint = "For example:\n\ndef left(world):\n    world.x = world.x - 1\n\nstay = None"
        else:
            hint = 'For example:\n\nleft = "a"\nright = "d"\nwait = None'
        return ReplyError("load", message, hint, script="responses.py", fatal=True)

    # -- reading the scripts without running them ----------------------------------------------------

    def project_sources(self):
        """(script, path, tree, text) for the user's scripts and every other project module they imported."""
        out, seen = [], set()
        roots = (self.project + os.sep, os.path.realpath(self.project) + os.sep)
        others = sorted((name, module) for name, module in list(sys.modules.items()) if name not in self.modules)
        for name, module in list(self.modules.items()) + others:
            path = getattr(module, "__file__", None)
            if (not isinstance(path, str) or not path.endswith(".py") or not path.startswith(roots)
                    or not S.in_project(path)):
                continue
            key = os.path.realpath(path)
            if key in seen:
                continue
            seen.add(key)
            text = self.sources.get(name) if name in self.modules else None
            if text is None:
                try:
                    with open(path, encoding="utf-8", errors="replace") as file:
                        text = file.read()
                except OSError:
                    text = ""
            out.append((S.script_name(path), path, self.tree_of(path), text))
        return out

    def uses(self, sources):
        """(uses keys, uses the screen), from the choices and from reading the scripts (protocol.md 3.2)."""
        keys = any(c.effect.kind in ("hold", "tap") for c in self.choices)
        screen = False
        for _, _, tree, text in sources:
            keys = keys or bool(_USES_KEYS.search(text))
            screen = screen or bool(_USES_SCREEN.search(text))
            for _, name, _ in _vyrn_calls(tree) if tree is not None else ():
                keys = keys or name == "press"
                screen = screen or name in _SCREEN_FUNCTIONS
        return keys, screen

    def uses_game(self, sources):
        """True when the scripts use vyrn.game, vyrn.events, vyrn.connected or vyrn.send (read, not run)."""
        for _, _, tree, text in sources:
            if _USES_GAME.search(text):
                return True
            for node in ast.walk(tree) if tree is not None else ():
                if (isinstance(node, ast.ImportFrom) and node.module == "vyrn" and not node.level
                        and any(alias.name in _GAME_NAMES for alias in node.names)):
                    return True
        return False

    def check_pressed_keys(self, sources):
        """A fatal load error for vyrn.press("spcae"): a key that can't be pressed, found without running the
        code, so a Check finds it even inside restart() or a choice function it doesn't run."""
        for script, path, tree, _ in sources:
            for call, name, where in _vyrn_calls(tree) if tree is not None else ():
                if name != "press":
                    continue
                arg = call.args[0] if call.args else next((k.value for k in call.keywords if k.arg == "key"), None)
                if not (isinstance(arg, ast.Constant) and isinstance(arg.value, str)):
                    continue
                message = hint = None
                if not arg.value.strip():
                    message = 'vyrn.press("") needs the name of a key.'
                    hint = 'For example: vyrn.press("space") or vyrn.press("ctrl+c").'
                else:
                    try:
                        parsed = _effects.parse_effect(arg.value)
                    except _effects.EffectError as err:
                        message, hint = f'vyrn.press("{arg.value}"): {err.message}', err.hint
                    else:
                        if parsed.kind in _effects.GAME_KINDS:
                            message = (f'vyrn.press("{arg.value.strip()}") presses keys on this computer, not '
                                       "buttons in a game connected with Vyrn Link.")
                            hint = _effects.PRESS_GAME_HINT
                if message:
                    raise ReplyError("load", message, hint, script=script, line=arg.lineno,
                                     col=_char_col(path, arg.lineno, arg.col_offset), func=where, fatal=True)

    def copied_value_warnings(self, warnings):
        """from vyrn import tick (or *): a copy made once, when the script loads, that never changes."""
        for module_name, tree in list(self.trees.items()):
            if module_name not in self.modules or tree is None:
                continue
            for node in ast.walk(tree):
                if not (isinstance(node, ast.ImportFrom) and node.module == "vyrn" and not node.level):
                    continue
                names = [alias.name for alias in node.names]
                if "*" in names:
                    message = ("from vyrn import * copies vyrn.tick, vyrn.round, vyrn.choice and vyrn.game once, "
                               "when the script loads, so they never change, and it hides Python's own round(). "
                               "Write  import vyrn  and use vyrn.tick, vyrn.round, vyrn.choice and vyrn.game "
                               "instead.")
                else:
                    copied = [name for name in names if name in _COPIED_VALUES]
                    if not copied:
                        continue
                    many = len(copied) > 1
                    message = (f"from vyrn import {', '.join(copied)} copies {'these values' if many else 'the value'} "
                               f"once, when the script loads, so {'they never change' if many else 'it never changes'}. "
                               f"Write  import vyrn  and use {_join('vyrn.' + name for name in copied)} instead.")
                    if "round" in copied:
                        message += " (It also hides Python's own round().)"
                warnings.append({"script": f"{module_name}.py", "line": node.lineno, "message": message})

    def unchecked_choice_warning(self, warnings):
        """Check describes a real-time choice function but never runs it: say so (PLAN.md section 2)."""
        calls = [c for c in self.choices if c.effect.kind == "call"]
        if not calls:
            return
        many = len(calls) > 1
        names = _join(f"{c.name}()" for c in calls)
        warnings.append({"script": calls[0].script, "line": calls[0].line,
                         "message": f"Check doesn't run the choice function{'s' if many else ''} {names}: in a real "
                                    f"game {'they' if many else 'it'} may move real things. Mistakes inside "
                                    f"{'them' if many else 'it'} show up only when the AI plays."})

    # -- what the AI sees, and its points ----------------------------------------------------------

    def see(self):
        hook = self.hooks["see"]
        value = self.run_hook("see")
        # Turning the value into a picture or numbers may run the user's code too (__float__, __array__...).
        kind, array, names = self.user_code(self.observation, (value, hook), None, "see()")
        self.check_obs(kind, array, names, hook)
        return array

    def observation(self, value, hook):
        """(kind, array, names) for what see() returned."""
        try:
            picture = _canvas.to_rgb(value)
        except _canvas.PictureError as err:
            raise self.bad_return(hook, "see() returned a picture Vyrn can't use. " + err.message,
                                  err.hint) from None
        if picture is None:
            array, names = self.to_numbers(value, hook)
            return "numbers", array, names
        if S.mode == "sim" and picture.nbytes > MAX_SIM_PICTURE_BYTES:
            height, width = picture.shape[:2]
            raise self.bad_return(hook, f"see() returned a {width}×{height} picture, which is too big for a "
                                  "simulation: at the end of each round Vyrn sends two pictures at once (the "
                                  "round's last one and the next round's first), and together they must fit "
                                  "in 64 MB.", "Use a smaller canvas: up to about 3300×3300 pixels fits, and "
                                  "small pictures (like 84×84) train much faster.")
        return "picture", picture, None

    def to_numbers(self, value, hook):
        """Convert see()'s numbers to a float32 array [D] (+ names for a dict)."""
        nested = "return a flat list of numbers, or a picture as a numpy array"
        sim = S.mode == "sim"
        names = None
        if isinstance(value, dict):
            names = [str(key) for key in value]
            items = [(f"the value for {safe_repr(key)}", item) for key, item in value.items()]
        elif isinstance(value, (list, tuple)):
            items = [(f"item {index} of the list", item) for index, item in enumerate(value)]
        elif isinstance(value, np.ndarray):          # pictures (2-D and up) were handled already
            if value.dtype.kind not in "biuf":
                raise self.bad_return(hook, f"see() returned {_describe(value)}, but numbers are needed.",
                                      nested)
            flat = value.reshape(-1).astype(np.float64)
            bad = ~np.isfinite(flat) | (np.abs(flat) > 3.4e38)
            if bad.any():
                index = int(np.argmax(bad))
                raise self.bad_return(hook, f"see() returned {_describe(flat[index])} as item {index} of the "
                                      f"array: {_not_a_normal_number(flat[index])}.",
                                      "Check for a division by zero or a value that grows without limit.")
            if not 1 <= flat.size <= MAX_NUMBERS:
                raise self.bad_return(hook, f"see() returned {flat.size} numbers; it must return 1 to "
                                      f"{MAX_NUMBERS}.", "Use fewer numbers." if flat.size else nested)
            return flat.astype("<f4"), None
        elif isinstance(value, (numbers.Real, np.bool_)):
            items = [("the number", value)]
        elif value is None:
            raise self.bad_return(hook, "see() didn't return anything (it returned None).",
                                  "End see() with a return line, like  return picture  (your vyrn.canvas) or a "
                                  "list of numbers." if sim else
                                  "End see() with a return line, like  return vyrn.screen()  or a list of numbers.")
        else:
            raise self.bad_return(hook, f"see() returned {_describe(value)}, but it must return a picture "
                                  "or numbers.", "Return a vyrn.canvas, a number, a list of numbers or a dict of "
                                  "name: number." if sim else "Return vyrn.screen(...), a vyrn.canvas, a number, "
                                  "a list of numbers or a dict of name: number.")
        values = []
        for where, item in items:
            if isinstance(item, np.ndarray) and item.ndim == 0:
                item = item.item()                     # a numpy array holding a single number
            if isinstance(item, (list, tuple, dict, np.ndarray)):
                raise self.bad_return(hook, f"see() returned a list inside a list ({where}).", nested)
            if not isinstance(item, (numbers.Real, np.bool_)):
                raise self.bad_return(hook, f"see() returned {_describe(item)} as {where}, which isn't a number.",
                                      "Every input must be a number (True/False count as 1/0).")
            try:
                number = float(item)
            except OverflowError:
                number = math.inf
            if not math.isfinite(number) or abs(number) > 3.4e38:
                raise self.bad_return(hook, f"see() returned {_describe(item)} as {where}: "
                                      f"{_not_a_normal_number(item)}.", "Check for a division by zero or a value "
                                      "that grows without limit.")
            values.append(number)
        if not 1 <= len(values) <= MAX_NUMBERS:
            raise self.bad_return(hook, f"see() returned {len(values)} numbers; it must return 1 to "
                                  f"{MAX_NUMBERS}.", nested if not values else "Use fewer numbers.")
        return np.array(values, dtype="<f4"), names

    def check_obs(self, kind, array, names, hook):
        """The first observation sets what the AI sees; later ones must match it."""
        if self.obs_kind is None:
            self.obs_kind, self.obs_shape, self.obs_names = kind, array.shape, names
            return
        if kind != self.obs_kind:
            raise self.bad_return(hook, f"see() returned {self.obs_kind} the first time but {kind} now.",
                                  "see() must always return the same kind of thing.")
        if kind == "numbers":
            if array.shape != self.obs_shape:
                raise self.bad_return(hook, f"see() returned {self.obs_shape[0]} numbers the first time but "
                                      f"{array.shape[0]} now.", "see() must always return the same number "
                                      "of numbers.")
            if names != self.obs_names:
                raise self.bad_return(hook, f"see() used the names {self.obs_names} the first time but "
                                      f"{names} now.", "Keep the same names, in the same order.")
        elif array.shape != self.obs_shape:
            (h0, w0), (h1, w1) = self.obs_shape[:2], array.shape[:2]
            S.warn_once("picture-size", hook.script, hook.line,
                        f"see() returned a {w1}×{h1} picture, but the first one was {w0}×{h0}. Vyrn resizes "
                        "it, but keeping one size works best.")

    def points(self):
        hook = self.hooks["points"]
        value = self.run_hook("points")
        return self.user_code(self.points_value, (value, hook), None, "points()")

    def points_value(self, value, hook):
        """points()'s value as a float (protocol.md section 4.1)."""
        if value is None:
            S.warn_once("points-none", hook.script, hook.line,
                        "points() returned None, which counts as 0. Make sure every way through points() "
                        "ends with return and a number.")
            return 0.0
        if isinstance(value, np.ndarray) and value.ndim == 0:
            value = value.item()
        if isinstance(value, (bool, np.bool_)):
            return float(bool(value))
        if isinstance(value, numbers.Real):
            try:
                number = float(value)
            except OverflowError:
                raise self.bad_return(hook, f"points() returned {_describe(value)}, which is too big to be "
                                      "points.", "Check for a value that grows without limit, like "
                                      "2 ** world.n.") from None
            if math.isfinite(number):
                return number
            raise self.bad_return(hook, f"points() returned {_describe(value)}: it must be a normal number, not "
                                  "NaN or infinity.", "Check for a division by zero.")
        hint = "Return a number: for example 1 for good, -1 for bad, 0 for nothing special."
        if isinstance(value, tuple) and len(value) == 1:
            hint = "Is there a comma after the number? return 1, makes a tuple: remove the comma."
        raise self.bad_return(hook, f"points() returned {_describe(value)}, but it must return a number.", hint)

    def game_over(self):
        hook = self.hooks.get("game_over")
        if hook is None:
            return False
        value = self.run_hook("game_over")
        return self.user_code(self.truth, (value, hook), None, "game_over()")

    def truth(self, value, hook):
        try:
            return bool(value)
        except Exception:
            raise self.bad_return(hook, f"game_over() returned {_describe(value)}, which is neither true "
                                  "nor false.", "Return True when the round is over, otherwise False.") from None

    # -- choices --------------------------------------------------------------------------------

    def choice_index(self, msg):
        index = msg.get("a")
        if not _is_int(index) or not 0 <= index < len(self.choices):
            raise ReplyError("protocol", f"There is no choice number {safe_repr(index)}.", fatal=True)
        return index

    def apply_choice(self, index):
        choice = self.choices[index]
        api.choice = choice.name
        effect = choice.effect
        if S.mode == "realtime":
            # Real time: the keyboard diffs the held set. None and function choices empty it.
            self.press(effect, choice)
            if effect.kind == "call" and S.check:
                S.did.append("would " + effect.describe())     # a Check never runs real-world effects
                return
        if effect.kind == "call":
            name = getattr(effect.func, "__name__", choice.name)
            self.user_code(effect.func, (self.world,) if choice.takes_world else (), "respond", f"{name}()")

    def press(self, effect, choice):
        start = time.perf_counter()
        try:
            S.keyboard.apply(effect)
        except VyrnUserError as err:
            raise ReplyError(err.kind if err.kind in KINDS else "input", err.message, err.hint,
                             script=choice.script, line=choice.line, func=choice.name) from None
        except (OSError, ImportError) as err:
            raise ReplyError("input", f"Vyrn couldn't press keys: {err}", _keys.keyboard_hint(),
                             script=choice.script, line=choice.line, func=choice.name) from None
        finally:
            self.ms["respond"] = self.ms.get("respond", 0.0) + (time.perf_counter() - start) * 1000.0

    # -- replies ------------------------------------------------------------------------------------

    def show_changes(self):
        """The vyrn.show() values that changed since the previous result."""
        changed, S.show_changed = S.show_changed, {}
        out = {}
        for name, value in changed.items():
            old = self.last_show.get(name, _MISSING)
            if old is not _MISSING and _same(old, value):
                continue
            self.last_show[name] = value
            out[name] = value
        return out

    def focus_state(self):
        """(a focus lock is active, the locked window is in front) as the focus guard sees it right now."""
        focus = S.focus
        if focus is not None and focus.locked is not None:
            return True, bool(focus.is_focused())
        if S.focus_locked:
            return True, False                 # a lock was taken, but its watcher is gone: fail closed
        return False, True

    def result(self, obs, points, over, truncated, final_obs, first):
        locked, focused = self.focus_state()
        return "result", {
            "obs": obs, "points": points, "game_over": bool(over), "truncated": bool(truncated),
            "first": bool(first), "final_obs": final_obs, "focused": focused if locked else None,
            "show": self.show_changes(), "did": list(S.did), "warnings": S.take_warnings(),
            "ms": self.timings(),
        }

    def need(self, mode):
        if not self.loaded:
            raise ReplyError("protocol", "The app sent a request before the scripts loaded.", fatal=True)
        if S.mode != mode:
            what = "simulations" if mode == "sim" else "real-time projects"
            raise ReplyError("protocol", f"That request is only for {what}.", fatal=True)

    def new_round(self):
        self.round += 1
        self.tick = 0
        api.round, api.tick, api.choice = self.round, 0, None

    # -- simulation requests ------------------------------------------------------------------------

    def on_reset(self, msg):
        self.need("sim")
        self.new_round()
        if "restart" in self.hooks:
            self.run_hook("restart")
        return self.result(self.see(), None, False, False, None, first=True)

    def on_step(self, msg):
        self.need("sim")
        self.apply_choice(self.choice_index(msg))
        if "step" in self.hooks:
            self.run_hook("step")
        self.tick += 1
        api.tick = self.tick
        obs = self.see()
        points = self.points()
        over = self.game_over()
        truncated = not over and self.round_limit is not None and self.tick >= self.round_limit
        if not (over or truncated):
            return self.result(obs, points, False, False, None, first=False)
        final_obs = obs
        self.new_round()                              # auto-restart: the brain gets the next round's start
        if "restart" in self.hooks:
            self.run_hook("restart")
        return self.result(self.see(), points, over, truncated, final_obs, first=True)

    # -- real-time requests -------------------------------------------------------------------------

    def on_lock_focus(self, msg):
        self.need("realtime")
        if not S.allow_input:
            raise InputNotAllowedError()
        current = S.focus
        locked_before = current is not None and current.locked is not None
        if current is not None and current.alive():
            watcher = current
        else:
            try:
                watcher = _focus.FocusWatcher(self.focus_helper)    # None: look in the install places
            except OSError as err:
                raise FocusError(f"Vyrn couldn't start watching the windows: {err}", "Start Vyrn again.") from None
        ready = watcher.wait_ready(1.0)
        try:
            info = watcher.lock()          # FocusError: no window, Vyrn itself, or the helper failed
        except FocusError:
            # Never trade an established lock for a watcher that holds none: the old one keeps failing closed.
            if watcher is not current:
                if ready and not locked_before:
                    self.use_watcher(watcher)          # a working helper: the next lock_focus reuses it
                else:
                    watcher.stop()
            elif not ready and not locked_before:
                S.focus = None                         # a broken helper: start a fresh one next time
                watcher.stop()
            raise
        self.use_watcher(watcher)
        S.focus_locked = True
        if not S.check:
            try:
                S.keyboard.ensure_device()
            except (OSError, ImportError) as err:
                raise VyrnUserError("input", f"Vyrn couldn't create its virtual keyboard: {err}",
                                    _keys.keyboard_hint()) from None
        return "ok", {"app_id": info.get("app_id"), "title": info.get("title")}

    def use_watcher(self, watcher):
        """Make watcher the one the focus guard asks (stopping the one it replaces)."""
        old, S.focus = S.focus, watcher
        if old is not None and old is not watcher:
            old.stop()

    def take_game(self, msg):
        """vyrn.game, vyrn.events and vyrn.connected from a real-time request (protocol.md section 3.4).

        "game" and "connected" change only when the request has them; "game_events" are this tick's events, so
        vyrn.events is empty when the request has none."""
        if "game" in msg:
            api.game = _link.game_state(msg.get("game"))
        if "connected" in msg:
            api.connected = msg.get("connected") is True
        api.events = _link.game_events(msg.get("game_events"))

    def on_restart(self, msg):
        self.need("realtime")
        self.take_game(msg)
        self.release_keys()            # a round ended: the next one starts with no key held
        if "restart" in self.hooks:
            self.run_hook("restart")
        return "ok", {"did": list(S.did), "ms": self.timings()}

    def on_check_over(self, msg):
        self.need("realtime")
        self.take_game(msg)
        self.release_keys()            # restart polling: nothing is held between rounds
        return "ok", {"game_over": self.game_over(), "ms": self.timings()}

    def on_observe(self, msg):
        self.need("realtime")
        self.take_game(msg)
        if msg.get("first"):
            self.release_keys()        # a new round (after a game over, or at the round limit) starts empty
            self.new_round()
            return self.result(self.see(), None, False, False, None, first=True)
        self.tick += 1
        api.tick = self.tick
        obs = self.see()
        points = self.points()
        over = self.game_over()
        if over:
            self.release_keys()        # the round is over: let go of the keys now, not at the next request
        return self.result(obs, points, over, False, None, first=False)

    def on_act(self, msg):
        self.need("realtime")
        index = self.choice_index(msg)
        self.apply_choice(index)
        # The game buttons this choice holds or taps (a Check only describes them). The app sends them to a game
        # connected with Vyrn Link; every other button is up until the next act.
        game_input = _link.game_input(self.choices[index].effect)
        return "ok", {"did": list(S.did), "ms": self.timings(), "game_input": game_input}

    # -- both modes ---------------------------------------------------------------------------------

    def on_status(self, msg):
        focus = S.focus
        _, focused = self.focus_state()
        active = focus.active() if focus is not None else None
        held = S.keyboard.held() if S.keyboard is not None else []
        return "ok", {"focused": focused, "active": active, "held": list(held), "frozen": bool(S.frozen)}

    def on_pause(self, msg):
        self.release_keys()
        return "ok", {}

    def on_resume(self, msg):
        S.frozen = False
        return "ok", {}

    def on_close(self, msg):
        self.closing = True
        S.stopping = True
        self.shutdown()
        return "bye", {}


_MISSING = object()


def _same(old, new):
    if isinstance(old, np.ndarray) or isinstance(new, np.ndarray):
        return (isinstance(old, np.ndarray) and isinstance(new, np.ndarray)
                and old.shape == new.shape and np.array_equal(old, new))
    return type(old) is type(new) and old == new


class Watchdog(threading.Thread):
    """Keeps the keyboard safe whatever the main thread is doing (protocol.md section 5).

    It waits on the link's watch (vyrn._platform.transport), which reports:
    - "gone": the app died (POLLRDHUP on fd 3; on macOS its exit or EOF; on Windows EOF on the control connection):
      release the keys and exit
    - "freeze" (SIGUSR1, or "F"): release the keys; key actions are skipped until "resume"
    - "terminate" (SIGTERM): release the keys and exit
    - "release" (SIGINT, which interrupts the user's code by itself) and "interrupt" ("I" on Windows: the watchdog
      interrupts the main thread like SIGINT would): release the keys (the app is about to stop a stuck request)
    and in between:
    - focus guard (real time, while keys are down): the locked window isn't active any more → release
    - dead-man timer (real time, while keys are down): no request but "status" for max(0.5 s, 3 ticks) → release

    All of this needs the GIL; Host.arm_stuck_exit covers user code that keeps it in one long C call.
    """

    def __init__(self, host):
        super().__init__(name="vyrn-watchdog", daemon=True)
        self.host = host
        self.exit_at = None            # closing and the app is gone: leave at this time at the latest

    def run(self):
        watch = self.host.watch
        while True:
            try:
                for event in watch.wait(self.timeout_ms() / 1000.0):
                    self.on_event(event)
                if self.exit_at is not None and time.monotonic() >= self.exit_at:
                    self.host.emergency_exit(0)
                self.focus_guard()
                self.dead_man()
            except Exception as err:                    # this thread must never die
                _log(f"watchdog problem: {err!r}")
                time.sleep(0.1)

    def timeout_ms(self):
        keyboard = S.keyboard
        if S.mode == "realtime" and keyboard is not None and keyboard.keys_down():
            return 50
        return 250

    def on_event(self, event):
        if event == "terminate":
            self.host.emergency_exit(0)
        elif event == "freeze":
            S.frozen = True
            self.host.release_keys()
        elif event in ("release", "interrupt"):
            self.host.release_keys()
            if event == "interrupt":
                self.host.watch.interrupt_main()
        elif event == "gone":
            if self.host.closing:
                # "bye" is (being) sent and the app is gone: give the clean exit a moment (the user's atexit
                # clean-up), then leave anyway.
                self.exit_at = time.monotonic() + CLOSE_GRACE
            else:
                self.host.emergency_exit(1)            # the app is gone

    def focus_guard(self):
        """Release the keys as soon as the locked game window isn't the active one, instead of waiting for the
        next key action to notice (the app may only poll "status" while it holds)."""
        keyboard = S.keyboard
        if S.mode != "realtime" or keyboard is None or not S.focus_locked or not keyboard.keys_down():
            return
        focus = S.focus
        if focus is None or not focus.is_focused():
            keyboard.release_all()
            _log("Your game window isn't the active window any more, so the AI's keys were released.")

    def dead_man(self):
        keyboard = S.keyboard
        if S.mode != "realtime" or keyboard is None:
            return
        limit = max(0.5, 3.0 / max(S.decisions_per_second, 1e-6))
        if time.monotonic() - S.last_request > limit and keyboard.keys_down():
            keyboard.release_all()
            _log("Vyrn stopped sending decisions, so the AI's keys were released.")


def main(project, link=None):
    """Run the worker on its link to the app (vyrn._platform.connect(): fd 3, or TCP on Windows). Returns the
    process exit status (2 when the app can't be reached)."""
    faulthandler.enable()
    if link is None:
        from ._platform.transport import ConnectError
        try:
            link = _platform.connect()
        except ConnectError as err:
            sys.stderr.write(f"{err}\n")
            return 2
    return Host(project, link).run()
