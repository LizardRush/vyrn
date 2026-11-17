"""Helpers shared by the Vyrn Python tests: fake devices, a worker rig, project builders and polling helpers.

conftest.py puts this folder on sys.path (so tests can `from testkit import ...`) and wraps the pieces in fixtures.
Everything here talks to the implementation only through documented interfaces: tools/engine.py
(python-internals.md section 11) and the environment overrides of protocol.md section 1.
"""

from __future__ import annotations

import collections
import json
import os
import shlex
import signal
import sys
import textwrap
import threading
import time
from pathlib import Path

FAKES = Path(__file__).resolve().parent
TESTS = FAKES.parent
REPO = TESTS.parents[1]
PYTHON_DIR = REPO / "python"
TOOLS_DIR = PYTHON_DIR / "tools"
TEMPLATES = REPO / "templates"
FIXTURES = REPO / "tests" / "fixtures"   # test-only projects that aren't shipped as presets (link-runner)
GOLDEN_FRAME = REPO / "tests" / "golden" / "frame_result.bin"
EVDEV_GUARD = FAKES / "evdev_guard"
DUMMY_WAYLAND = "vyrn-test-no-such-display"

KeyEvent = collections.namedtuple("KeyEvent", "t action code")


# ---- small helpers ---------------------------------------------------------------------------------------------

def wait_until(predicate, timeout=2.0, what="the condition", interval=0.01):
    """Poll predicate() until it returns something truthy (returned) or fail after `timeout` seconds."""
    deadline = time.monotonic() + timeout
    while True:
        value = predicate()
        if value:
            return value
        if time.monotonic() >= deadline:
            raise AssertionError(f"timed out after {timeout} s waiting for {what}")
        time.sleep(interval)


def proc_state(pid):
    """The state letter of /proc/<pid>/stat (R, S, Z, ...) or None when the process is gone."""
    try:
        with open(f"/proc/{pid}/stat", encoding="utf-8", errors="replace") as file:
            data = file.read()
    except (FileNotFoundError, ProcessLookupError):
        return None
    return data[data.rindex(")") + 2]


def proc_gone(pid):
    """True once the process has exited (it may still be an unreaped zombie)."""
    return proc_state(pid) in (None, "Z", "X")


def line_of(path, marker, nth=1):
    """1-based number of the nth line of `path` containing `marker`."""
    for number, line in enumerate(Path(path).read_text(encoding="utf-8").splitlines(), 1):
        if marker in line:
            nth -= 1
            if nth == 0:
                return number
    raise AssertionError(f"{marker!r} not found in {path}")


def col_of(path, line_number, snippet):
    """1-based column where `snippet` starts on line `line_number` of `path`."""
    line = Path(path).read_text(encoding="utf-8").splitlines()[line_number - 1]
    return line.index(snippet) + 1


def atomic_write_json(path, data):
    path = Path(path)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(data), encoding="utf-8")
    os.replace(tmp, path)


def write_wrapper(path, script, env=None, default_env=None):
    """A tiny sh launcher for a fake program: no dependence on PATH, shebangs or exec bits of the repo files."""
    lines = ["#!/bin/sh"]
    for name, value in (default_env or {}).items():
        lines.append(f'if [ -z "${{{name}:-}}" ]; then {name}={shlex.quote(str(value))}; export {name}; fi')
    for name, value in (env or {}).items():
        lines.append(f"{name}={shlex.quote(str(value))}; export {name}")
    lines.append(f'exec {shlex.quote(sys.executable)} {shlex.quote(str(script))} "$@"')
    path = Path(path)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    path.chmod(0o755)
    return path


class Background:
    """Runs fn(*args, **kwargs) on a daemon thread: for requests that block inside user code."""

    def __init__(self, fn, *args, **kwargs):
        self.result = None
        self.exc = None
        self._thread = threading.Thread(target=self._run, args=(fn, args, kwargs), daemon=True)
        self._thread.start()

    def _run(self, fn, args, kwargs):
        try:
            self.result = fn(*args, **kwargs)
        except BaseException as exc:  # noqa: BLE001 - handed to the test
            self.exc = exc

    @property
    def running(self):
        return self._thread.is_alive()

    def join(self, timeout=10.0):
        self._thread.join(timeout)
        if self._thread.is_alive():
            raise AssertionError(f"the background call is still running after {timeout} s")
        return self


# ---- fake devices ----------------------------------------------------------------------------------------------

class KeyLog:
    """Reads the recording key backend's file: one '<monotonic seconds> down|up <KEY_CODE>' line per key event."""

    def __init__(self, path):
        self.path = Path(path)

    def exists(self):
        return self.path.exists()

    def events(self):
        try:
            text = self.path.read_text(encoding="utf-8")
        except FileNotFoundError:
            return []
        lines = text.split("\n")
        if not text.endswith("\n"):
            lines = lines[:-1]  # the last line is still being written
        events = []
        for line in lines:
            if not line.strip():
                continue
            parts = line.split()
            assert len(parts) == 3 and parts[1] in ("down", "up"), f"bad key log line {line!r}"
            events.append(KeyEvent(float(parts[0]), parts[1], parts[2]))
        return events

    def mark(self):
        return len(self.events())

    def actions(self, start=0):
        return [(e.action, e.code) for e in self.events()[start:]]

    def held(self, events=None):
        held = set()
        for event in self.events() if events is None else events:
            if event.action == "down":
                held.add(event.code)
            else:
                held.discard(event.code)
        return held

    def wait_count(self, count, timeout=2.0):
        return wait_until(lambda: (lambda ev: ev if len(ev) >= count else None)(self.events()), timeout,
                          f"{count} key events in {self.path.name} (have {self.actions()})")

    def wait_all_up(self, timeout=1.0):
        """Wait until every pressed key has been released; returns the events."""
        return wait_until(lambda: (lambda ev: ev if ev and not self.held(ev) else None)(self.events()), timeout,
                          f"all keys up (have {self.actions()})")


class FakeGrim:
    """Controls fakes/fake_grim.py through its JSON control file and reads its call log."""

    def __init__(self, root):
        root = Path(root)
        self.control = root / "grim.json"
        self.log = root / "grim.log"
        self.program = write_wrapper(root / "grim", FAKES / "fake_grim.py",
                                     default_env={"VYRN_FAKE_GRIM_CONTROL": self.control})
        self.set()

    def set(self, width=64, height=48, color=(40, 80, 120), pattern="solid", image=None, rects=(), delay=0.0,
            fail=None):
        atomic_write_json(self.control, {
            "width": width, "height": height, "color": list(color), "pattern": pattern,
            "image": str(image) if image else None, "rects": list(rects), "delay": delay, "fail": fail,
            "log": str(self.log)})

    def calls(self):
        try:
            text = self.log.read_text(encoding="utf-8")
        except FileNotFoundError:
            return []
        return [json.loads(line) for line in text.splitlines() if line.strip()]

    def count(self):
        return len(self.calls())


class FakeFocus:
    """Controls fakes/fake_focus.py (the vyrn-focus stand-in). `program` is what goes into load's focus_helper."""

    def __init__(self, root):
        root = Path(root)
        self.control = root / "focus.json"
        self.log = root / "focus.log"
        self.program = write_wrapper(root / "vyrn-focus", FAKES / "fake_focus.py",
                                     env={"VYRN_FAKE_FOCUS_CONTROL": self.control})
        self._config = {"active": None, "closed": [], "error": None, "ready": True, "log": str(self.log)}
        self._write()

    def _write(self):
        atomic_write_json(self.control, self._config)

    def activate(self, window_id, app_id="game", title="My Game"):
        self._config["active"] = {"id": window_id, "app_id": app_id, "title": title}
        self._write()

    def deactivate(self):
        self._config["active"] = None
        self._write()

    def close_window(self, window_id):
        self._config["closed"].append(window_id)
        if (self._config["active"] or {}).get("id") == window_id:
            self._config["active"] = None
        self._write()

    def fail(self, message):
        self._config["error"] = message
        self._write()

    def never_ready(self):
        self._config["ready"] = False
        self._write()

    def lifecycle(self):
        try:
            return self.log.read_text(encoding="utf-8").splitlines()
        except FileNotFoundError:
            return []


class Fakes:
    """One set of fake devices (recorded keys, fake grim, fake focus helper) living in `root`."""

    def __init__(self, root):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        self.keys = KeyLog(self.root / "keys.log")
        self.grim = FakeGrim(self.root)
        self.focus = FakeFocus(self.root)
        self.xdg = self.root / "xdg"
        self.xdg.mkdir(mode=0o700, exist_ok=True)

    def overrides(self):
        """Environment variables that point a process at these fakes (and away from the real desktop)."""
        pythonpath = [str(EVDEV_GUARD)]
        if os.environ.get("PYTHONPATH"):
            pythonpath.append(os.environ["PYTHONPATH"])
        return {
            "VYRN_KEYS_BACKEND": f"record:{self.keys.path}",
            "VYRN_GRIM": str(self.grim.program),
            "VYRN_FAKE_GRIM_CONTROL": str(self.grim.control),
            "VYRN_FAKE_FOCUS_CONTROL": str(self.focus.control),
            "WAYLAND_DISPLAY": DUMMY_WAYLAND,
            "XDG_RUNTIME_DIR": str(self.xdg),
            "PYTHONDONTWRITEBYTECODE": "1",
            "PYTHONPATH": os.pathsep.join(pythonpath),
        }

    def env(self, extra=None):
        """A complete environment for a child process; `extra` values of None remove a variable."""
        env = dict(os.environ)
        env.pop("DISPLAY", None)
        env.update(self.overrides())
        for name, value in (extra or {}).items():
            if value is None:
                env.pop(name, None)
            else:
                env[name] = str(value)
        return env


# ---- projects --------------------------------------------------------------------------------------------------

SIM_FILES = {
    "world.py": """
        def restart(world):
            world.n = 0

        def step(world):
            world.n += 1
        """,
    "inputs.py": """
        def see(world):
            return [world.n]
        """,
    "responses.py": """
        def grow(world):
            world.n += 10

        stay = None
        """,
    "points.py": """
        def points(world):
            return world.n
        """,
}

RT_FILES = {
    "inputs.py": """
        def see():
            return [1.0, 2.0]
        """,
    "responses.py": """
        left = "a"
        right = "d"
        wait = None
        """,
    "points.py": """
        def points():
            return 1
        """,
}


class Project:
    """A temporary Vyrn project folder (protocol.md section 8)."""

    def __init__(self, path):
        self.path = Path(path)

    def __fspath__(self):
        return str(self.path)

    def __str__(self):
        return str(self.path)

    def __truediv__(self, name):
        return self.path / name

    def write(self, name, source):
        target = self.path / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(source, bytes):
            target.write_bytes(source)
        else:
            target.write_text(textwrap.dedent(source).lstrip("\n"), encoding="utf-8")
        return target

    def line(self, name, marker, nth=1):
        return line_of(self.path / name, marker, nth)

    def col(self, name, line_number, snippet):
        return col_of(self.path / name, line_number, snippet)


def build_project(path, files=None, sim=True):
    """Write a project: the default sim (or real-time) scripts, overridden by `files` (None removes a file)."""
    project = Project(path)
    project.path.mkdir(parents=True)
    (project.path / "vyrn.json").write_text('{"name": "test project"}\n', encoding="utf-8")
    merged = dict(SIM_FILES if sim else RT_FILES)
    merged.update(files or {})
    for name, source in merged.items():
        if source is not None:
            project.write(name, source)
    return project


# ---- a worker on the other end of tools/engine.py --------------------------------------------------------------

class Rig:
    """A worker started through tools/engine.py with its own fake devices."""

    def __init__(self, root, project, env=None, transport=None):
        import engine  # python/tools/engine.py (conftest puts python/tools on sys.path)

        self.engine = engine
        self.fakes = Fakes(root)
        self.keys, self.grim, self.focus = self.fakes.keys, self.fakes.grim, self.fakes.focus
        self.project = project if isinstance(project, Project) else Project(project)
        # engine.Worker's env holds changes to this process's environment (None removes a variable).
        changes = dict(self.fakes.overrides(), DISPLAY=None)
        changes.update(env or {})
        self.env = self.fakes.env(env)  # the complete environment the worker sees
        self.worker = engine.Worker(str(self.project.path), env=changes, transport=transport)

    # -- protocol shortcuts
    @property
    def hello(self):
        return self.worker.hello

    @property
    def pid(self):
        return self.worker.pid

    def load(self, **fields):
        return self.worker.load(**fields)

    def load_error(self, **fields):
        """Load, expecting a fatal error reply; returns the error dict."""
        try:
            spec = self.worker.load(**fields)
        except self.engine.WorkerError as error:
            assert error.error.get("fatal") is True, error.error
            return error.error
        raise AssertionError(f"expected the load to fail, got the spec {spec!r}")

    def request(self, t, timeout=10.0, **fields):
        return self.worker.request(t, timeout=timeout, **fields)

    def error(self, t, timeout=10.0, **fields):
        """Send a request expecting an `error` reply; returns the error dict."""
        try:
            reply = self.worker.request(t, timeout=timeout, **fields)
        except self.engine.WorkerError as error:
            return error.error
        raise AssertionError(f"expected an error reply to {t!r}, got {reply!r}")

    def reset(self):
        return self.request("reset")

    def step(self, a):
        return self.request("step", a=a)

    def observe(self, first=False):
        return self.request("observe", first=first)

    def act(self, a):
        return self.request("act", a=a)

    def status(self):
        return self.request("status")

    def signal(self, signum):
        self.worker.signal(signum)

    def gone(self):
        return proc_gone(self.worker.pid)

    def wait_gone(self, timeout=1.0):
        wait_until(self.gone, timeout, f"worker {self.worker.pid} to exit")

    def shutdown(self):
        """Best-effort cleanup that never raises."""
        worker = self.worker
        if not proc_gone(worker.pid):
            try:
                worker.close(timeout=1.0)
            except Exception:  # noqa: BLE001
                pass
        try:
            worker.kill()
        except Exception:  # noqa: BLE001
            pass
        kill_worker_group(worker.pid)


def kill_worker_group(pid):
    """SIGKILL a leftover worker's process group, but only if it really is a Vyrn worker leading its own group."""
    if proc_gone(pid):
        return
    try:
        cmdline = Path(f"/proc/{pid}/cmdline").read_bytes()
        if b"vyrn_worker" in cmdline and os.getpgid(pid) == pid:
            os.killpg(pid, signal.SIGKILL)
    except (OSError, ProcessLookupError):
        pass
