"""Focus helper reader: protocol.md section 6 and python-internals.md section 10 (vyrn._focus).

The helper is fakes/fake_focus.py, which prints the protocol's lines as its control file changes.
"""

import time

import pytest

from testkit import wait_until
from vyrn import _focus, _state


@pytest.fixture
def watcher(fakes):
    """watcher() -> a FocusWatcher running the fake helper; stopped after the test."""
    made = []

    def start(path=None):
        made.append(_focus.FocusWatcher(str(path or fakes.focus.program)))
        return made[-1]

    yield start
    for item in made:
        try:
            item.stop()
        except Exception:  # noqa: BLE001
            pass


def window(watcher_, key, value, timeout=2.0):
    """Wait until the watcher's active window has `key` == value."""
    return wait_until(lambda: (watcher_.active() or {}).get(key) == value, timeout,
                      f"active {key} == {value!r} (now {watcher_.active()!r})")


def test_ready_and_active_window(fakes, watcher):
    fakes.focus.activate(1, "game", "My Game")
    focus = watcher()
    assert focus.wait_ready(timeout=2.0) is True
    assert focus.active() == {"id": 1, "app_id": "game", "title": "My Game"}
    assert focus.is_focused() is True, "not locked yet"


def test_lock_follows_the_window_not_its_title(fakes, watcher):
    fakes.focus.activate(1, "game", "My Game")
    focus = watcher()
    assert focus.wait_ready(timeout=2.0)
    locked = focus.lock()
    assert locked["app_id"] == "game" and locked["title"] == "My Game"
    assert focus.is_focused() is True

    fakes.focus.activate(1, "game", "My Game - level 2")
    window(focus, "title", "My Game - level 2")
    assert focus.is_focused() is True, "titles change; the lock is on the window id"

    fakes.focus.activate(2, "foot", "Terminal")
    window(focus, "id", 2)
    assert focus.is_focused() is False

    fakes.focus.activate(1, "game", "My Game")
    window(focus, "id", 1)
    assert focus.is_focused() is True

    fakes.focus.deactivate()
    wait_until(lambda: focus.active() is None, 2.0, "no active window")
    assert focus.is_focused() is False

    fakes.focus.activate(1, "game", "My Game")
    window(focus, "id", 1)
    fakes.focus.close_window(1)
    wait_until(lambda: focus.active() is None, 2.0, "the closed window to stop being active")
    assert focus.is_focused() is False


def test_lock_refuses_vyrn_itself(fakes, watcher):
    fakes.focus.activate(3, "vyrn", "Vyrn - catch")
    focus = watcher()
    assert focus.wait_ready(timeout=2.0)
    with pytest.raises(_state.FocusError) as info:
        focus.lock()
    assert info.value.kind == "focus"


def test_lock_refuses_when_no_window_is_active(fakes, watcher):
    focus = watcher()
    assert focus.wait_ready(timeout=2.0)
    assert focus.active() is None
    with pytest.raises(_state.FocusError):
        focus.lock()


def test_helper_error_means_not_ready(fakes, watcher):
    fakes.focus.fail("the compositor has no foreign-toplevel protocol")
    focus = watcher()
    assert focus.wait_ready(timeout=1.0) is False
    with pytest.raises(_state.FocusError):
        focus.lock()


def test_helper_that_never_gets_ready(fakes, watcher):
    fakes.focus.activate(1, "game", "My Game")
    fakes.focus.never_ready()
    focus = watcher()
    started = time.monotonic()
    assert focus.wait_ready(timeout=0.3) is False
    assert time.monotonic() - started < 1.0


def test_missing_helper(watcher, tmp_path):
    try:
        focus = watcher(tmp_path / "no-such-helper")
    except Exception:  # noqa: BLE001 - refusing to start is fine too
        return
    assert focus.wait_ready(timeout=1.0) is False
    with pytest.raises(_state.FocusError):
        focus.lock()


def test_stop_ends_the_helper(fakes, watcher):
    fakes.focus.activate(1, "game", "My Game")
    focus = watcher()
    assert focus.wait_ready(timeout=2.0)
    assert fakes.focus.lifecycle()[0].startswith("start")
    focus.stop()
    wait_until(lambda: any(line.startswith("exit") for line in fakes.focus.lifecycle()), 1.0, "the helper to exit")


def test_probe_reports_a_missing_helper(tmp_path):
    name, problem = _focus.probe(str(tmp_path / "no-such-helper"))
    assert name is None and isinstance(problem, str) and problem.strip()


def test_probe_without_a_helper_path():
    name, problem = _focus.probe(None)
    assert (name is None) != (problem is None)
