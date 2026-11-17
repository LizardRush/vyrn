"""The public `vyrn` module: exactly the documented names, beginner docstrings, and delegation to the runtime
(PLAN.md section 1, python-internals.md section 2)."""

import inspect
import threading
import time

import vyrn

PUBLIC = {"screen", "pixel", "on_screen", "press", "wait", "canvas", "show", "choice", "tick", "round",
          "game", "events", "connected", "send"}
SIGNATURES = {
    "screen": [("x", None), ("y", None), ("width", None), ("height", None)],
    "pixel": [("x", inspect.Parameter.empty), ("y", inspect.Parameter.empty)],
    "on_screen": [("name", inspect.Parameter.empty), ("x", None), ("y", None), ("match", 0.9)],
    "press": [("key", inspect.Parameter.empty)],
    "wait": [("seconds", inspect.Parameter.empty)],
    "canvas": [("width", inspect.Parameter.empty), ("height", inspect.Parameter.empty), ("color", "black")],
    "show": [("name", inspect.Parameter.empty), ("value", inspect.Parameter.empty)],
    "send": [("type", inspect.Parameter.empty), ("fields", inspect.Parameter.empty)],
}


def test_exactly_the_documented_public_names():
    assert sorted(vyrn.__all__) == sorted(PUBLIC)
    for name in PUBLIC:
        assert hasattr(vyrn, name), name


def test_signatures():
    for name, expected in SIGNATURES.items():
        parameters = list(inspect.signature(getattr(vyrn, name)).parameters.values())
        assert [(p.name, p.default) for p in parameters] == expected, name


def test_functions_have_a_one_line_summary():
    for name in SIGNATURES:
        doc = inspect.getdoc(getattr(vyrn, name))
        assert doc, f"vyrn.{name} needs a docstring"
        summary = doc.splitlines()[0].strip()
        assert summary and len(summary) <= 120, f"vyrn.{name}: {summary!r}"


def test_counters_are_plain_attributes():
    assert vyrn.choice is None or isinstance(vyrn.choice, str)
    assert isinstance(vyrn.tick, int) and isinstance(vyrn.round, int)
    assert isinstance(vyrn.game, dict) and isinstance(vyrn.events, list) and isinstance(vyrn.connected, bool)


def test_wait_sleeps(state):
    started = time.monotonic()
    vyrn.wait(0.2)
    assert 0.18 <= time.monotonic() - started < 1.0


def test_wait_does_not_sleep_in_check_mode(state):
    state.check = True
    started = time.monotonic()
    vyrn.wait(5)
    assert time.monotonic() - started < 0.5


def test_wait_returns_early_when_stopping(state):
    timer = threading.Timer(0.1, setattr, args=(state, "stopping", True))
    timer.start()
    started = time.monotonic()
    vyrn.wait(5)
    timer.join()
    assert time.monotonic() - started < 0.5, "wait() sleeps in slices of at most 50 ms"


def test_show_records_the_value(state):
    vyrn.show("speed", 3.5)
    vyrn.show("label", "jumping")
    assert state.show_changed["speed"] == 3.5
    assert state.show_changed["label"] == "jumping"


class KeyboardStub:
    def __init__(self):
        self.calls = []

    def press_now(self, combo):
        self.calls.append(combo)


class ScreenStub:
    def __init__(self):
        self.calls = []

    def grab(self, x=None, y=None, width=None, height=None):
        self.calls.append(("grab", x, y, width, height))
        return "picture"

    def pixel(self, x, y):
        self.calls.append(("pixel", x, y))
        return (1, 2, 3)

    def on_screen(self, name, x=None, y=None, match=0.9):
        self.calls.append(("on_screen", name, x, y, match))
        return True


def test_press_uses_the_keyboard(state):
    state.keyboard = KeyboardStub()
    vyrn.press("space")
    vyrn.press("ctrl+c")
    assert state.keyboard.calls == ["space", "ctrl+c"]


def test_screen_functions_use_the_screen(state):
    state.screen = ScreenStub()
    assert vyrn.screen() == "picture"
    assert vyrn.screen(1, 2, 3, 4) == "picture"
    assert vyrn.pixel(5, 6) == (1, 2, 3)
    assert vyrn.on_screen("game_over.png", x=7, y=8) is True
    assert vyrn.on_screen("x.png", 1, 2, 0.5) is True
    assert state.screen.calls == [("grab", None, None, None, None), ("grab", 1, 2, 3, 4), ("pixel", 5, 6),
                                  ("on_screen", "game_over.png", 7, 8, 0.9), ("on_screen", "x.png", 1, 2, 0.5)]


def test_wait_in_a_simulation_is_warned_in_check_mode_too(state):
    # The warning is the useful part of a Check; a Check itself never sleeps.
    state.check = True
    state.mode = "sim"
    started = time.monotonic()
    vyrn.wait(5)
    assert time.monotonic() - started < 0.5
    assert len(state.warnings) == 1 and "slows training" in state.warnings[0]["message"]
