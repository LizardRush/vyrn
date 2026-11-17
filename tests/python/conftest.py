"""Fixtures for the Vyrn Python tests (how to run them: README.md).

Safety first. In this process and in every process a test starts:
- keys go to a recording backend (VYRN_KEYS_BACKEND=record:<file>, protocol.md section 1) and the real `evdev`
  module cannot be imported, so no test can create a real virtual keyboard or press a real key;
- screenshots come from fakes/fake_grim.py (VYRN_GRIM, which selects the grim backend on every system) and focus
  events from fakes/fake_focus.py; Pillow's ImageGrab can't be imported here (its backend is tested with fakes);
- WAYLAND_DISPLAY is a dummy name and DISPLAY is unset, so nothing reaches the real desktop.
"""

import itertools
import shutil
import sys
import time
from pathlib import Path

import pytest

sys.dont_write_bytecode = True  # keep python/ and the test folders free of __pycache__

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "fakes"))

import testkit  # noqa: E402

for _path in (testkit.TOOLS_DIR, testkit.PYTHON_DIR):  # vyrn package first, then tools/ (engine, check)
    if str(_path) not in sys.path:
        sys.path.insert(0, str(_path))

# Block the real evdev in this process: `import evdev` raises ImportError from now on. Tests that need an evdev
# install a fake one with monkeypatch, which restores this block afterwards.
for _name in [name for name in sys.modules if name == "evdev" or name.startswith("evdev.")]:
    sys.modules[_name] = None
sys.modules["evdev"] = None
# Same for Pillow's ImageGrab: no test may capture the real screen (test_platform.py hands the backend a fake grab).
sys.modules["PIL.ImageGrab"] = None


def pytest_configure(config):
    config.addinivalue_line("markers", "slow: takes more than 2 seconds (skip them with -m 'not slow')")


@pytest.fixture(autouse=True)
def fakes(tmp_path, monkeypatch):
    """Fake devices for this test process (used by in-process unit tests) plus an isolated environment."""
    devices = testkit.Fakes(tmp_path / "_fakes")
    for name, value in devices.overrides().items():
        monkeypatch.setenv(name, value)
    monkeypatch.delenv("DISPLAY", raising=False)
    return devices


@pytest.fixture
def state(monkeypatch, tmp_path):
    """vyrn._state.S with known values for one test; everything is restored afterwards."""
    from vyrn import _state

    project = tmp_path / "state-project"
    project.mkdir(exist_ok=True)
    values = dict(mode="realtime", check=False, allow_input=False, screen_allowed=True, loading=False, frozen=False,
                  stopping=False, focus_locked=False, project_dir=str(project), decisions_per_second=10.0, generation=1,
                  did=[], warnings=[], warned=set(), show_changed={}, last_request=time.monotonic(), screen=None,
                  keyboard=None, focus=None)
    for name, value in values.items():
        monkeypatch.setattr(_state.S, name, value, raising=False)
    return _state.S


@pytest.fixture
def make_project(tmp_path):
    """make_project(files=None, sim=True) -> testkit.Project with the default scripts overridden by `files`."""
    counter = itertools.count()

    def make(files=None, sim=True, name=None):
        return testkit.build_project(tmp_path / (name or f"project{next(counter)}"), files, sim)

    return make


@pytest.fixture
def copy_template(tmp_path):
    """copy_template("catch-pictures") -> a private copy of a shipped template (tests never touch the repo)."""

    def copy(name):
        target = tmp_path / f"template-{name}"
        src = testkit.TEMPLATES / name
        if not src.is_dir():
            src = testkit.FIXTURES / name
        shutil.copytree(src, target)
        return testkit.Project(target)

    return copy


@pytest.fixture
def spawn(tmp_path):
    """spawn(project, env=None, transport=None) -> testkit.Rig: a worker started by tools/engine.py with its own fake
    devices. transport="tcp" runs Windows' transport (TCP + token + control connection) on this system."""
    pytest.importorskip("engine", reason="python/tools/engine.py is missing")
    rigs = []

    def start(project, env=None, transport=None):
        rig = testkit.Rig(tmp_path / f"rig{len(rigs)}", project, env, transport=transport)
        rigs.append(rig)
        return rig

    yield start
    for rig in rigs:
        rig.shutdown()
