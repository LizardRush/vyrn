"""Virtual keyboard: held-set diffing, taps and the input policy (protocol.md 4.3 and 5, python-internals.md 8).

Keys always go to the recording backend (VYRN_KEYS_BACKEND=record:<file>, set by conftest.py); the real evdev
module is blocked. The UInput test installs a fake evdev that only records what it is asked to do.
"""

import os
import re
import sys
import threading
import time
import types
from pathlib import Path

import pytest

from testkit import wait_until
from vyrn import _effects, _keys, _state

E = _effects.parse_effect
INPUT_EVENT_CODES = Path("/usr/include/linux/input-event-codes.h")


class FocusStub:
    """Stands in for _focus.FocusWatcher with a lock active; `focused` says whether the locked window is active."""

    def __init__(self, focused=True):
        self.focused = focused

    def is_focused(self):
        return self.focused

    def active(self):
        return {"id": 1 if self.focused else 2, "app_id": "game" if self.focused else "foot", "title": "x"}

    def lock(self):
        return {"id": 1, "app_id": "game", "title": "x"}

    def stop(self):
        pass


@pytest.fixture
def keyboard(state):
    """keyboard(**state_values) -> (Controller, state). The state is vyrn._state.S, restored after the test."""
    made = []

    def make(**values):
        state.allow_input = True
        for name, value in values.items():
            setattr(state, name, value)
        controller = _keys.Controller(state)
        made.append(controller)
        return controller, state

    yield make
    for controller in made:
        for cleanup in (controller.release_all, controller.close):
            try:
                cleanup()
            except Exception:  # noqa: BLE001
                pass


def applied(controller, log, effect):
    """Apply an effect string (or None) and return the key events it caused right away."""
    mark = log.mark()
    controller.apply(E(effect))
    return log.actions(mark)


def taps_done(log, timeout=1.0):
    """Wait for pending tap releases; returns the events."""
    return log.wait_all_up(timeout)


# ---- backends -------------------------------------------------------------------------------------------------

def test_recording_backend_line_format(fakes):
    backend = _keys.make_backend()
    assert isinstance(backend, _keys.RecordingBackend)
    before = time.monotonic()
    backend.down("KEY_A")
    backend.up("KEY_A")
    after = time.monotonic()
    backend.close()
    lines = fakes.keys.path.read_text().splitlines()
    assert [line.split()[1:] for line in lines] == [["down", "KEY_A"], ["up", "KEY_A"]]
    for line in lines:
        assert before - 0.01 <= float(line.split()[0]) <= after + 0.01, "timestamps are time.monotonic()"


def test_probe_returns_a_name_or_a_problem():
    name, problem = _keys.probe()
    assert (name is None) != (problem is None)
    assert isinstance(name or problem, str) and (name or problem).strip()


def fake_evdev():
    """A recording evdev stand-in built from the kernel's key code table."""
    codes = {name: int(value, 0) for name, value in
             re.findall(r"#define\s+((?:KEY|BTN)_\w+)\s+(0x[0-9a-fA-F]+|\d+)\b", INPUT_EVENT_CODES.read_text())}
    ecodes = types.ModuleType("evdev.ecodes")
    ecodes.EV_SYN, ecodes.EV_KEY, ecodes.SYN_REPORT = 0, 1, 0
    ecodes.ecodes = dict(codes, EV_SYN=0, EV_KEY=1, SYN_REPORT=0)
    ecodes.KEY = ecodes.keys = {value: name for name, value in codes.items()}
    for name, value in codes.items():
        setattr(ecodes, name, value)
    created = []

    class UInput:
        def __init__(self, events=None, name="py-evdev-uinput", *args, **kwargs):
            self.events, self.name, self.written, self.closed = events, name, [], False
            created.append(self)

        def write(self, etype, code, value):
            self.written.append((etype, code, value))

        def syn(self):
            self.written.append("syn")

        def close(self):
            self.closed = True

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            self.close()

    module = types.ModuleType("evdev")
    module.__path__ = []
    module.UInput, module.ecodes = UInput, ecodes
    module.UInputError = type("UInputError", (Exception,), {})
    return module, ecodes, created


@pytest.mark.skipif(not INPUT_EVENT_CODES.exists(), reason="kernel headers not installed")
def test_uinput_device_declares_only_the_allowed_keys(monkeypatch):
    for value in vars(_keys).values():
        origin = value.__name__ if isinstance(value, types.ModuleType) else getattr(value, "__module__", None)
        if isinstance(origin, str) and origin.split(".")[0] == "evdev":
            pytest.skip("vyrn._keys bound evdev at import time; refusing to build a device")
    module, ecodes, created = fake_evdev()
    monkeypatch.setitem(sys.modules, "evdev", module)
    monkeypatch.setitem(sys.modules, "evdev.ecodes", ecodes)
    try:
        backend = _keys.UInputBackend()
    except OSError as error:  # an implementation may check /dev/uinput before importing evdev
        pytest.skip(f"UInputBackend refused before using evdev: {error}")
    try:
        assert len(created) == 1
        device = created[0]
        assert device.name == "Vyrn AI keyboard"
        declared = set(device.events[ecodes.EV_KEY])
        assert declared == {ecodes.ecodes[code] for code in _effects.KEYS.values()}
        assert set(device.events) <= {ecodes.EV_KEY, ecodes.EV_SYN}
        backend.down("KEY_A")
        backend.up("KEY_A")
        writes = [item for item in device.written if item != "syn"]
        assert writes == [(ecodes.EV_KEY, ecodes.KEY_A, 1), (ecodes.EV_KEY, ecodes.KEY_A, 0)]
        assert device.written.count("syn") >= 2, "a SYN after each event"
        assert device.written[-1] == "syn"
    finally:
        backend.close()
    assert device.closed


@pytest.mark.skipif(not INPUT_EVENT_CODES.exists(), reason="kernel headers not installed")
def test_uinput_device_is_never_inherited_by_programs_the_scripts_start(monkeypatch, tmp_path):
    # python-evdev opens /dev/uinput without O_CLOEXEC. If os.system() & co inherited the fd, a SIGKILLed worker
    # would leave the device (and a held key) alive. The fake opens real files and leaves them inheritable.
    module, ecodes, created = fake_evdev()
    fds = []

    class InheritingUInput(module.UInput):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, **kwargs)
            self.fd = os.open(tmp_path / "uinput", os.O_RDWR | os.O_CREAT)
            self.device = types.SimpleNamespace(fd=os.open(tmp_path / "event", os.O_RDWR | os.O_CREAT))
            for fd in (self.fd, self.device.fd):
                os.set_inheritable(fd, True)
                fds.append(fd)

    module.UInput = InheritingUInput
    monkeypatch.setitem(sys.modules, "evdev", module)
    monkeypatch.setitem(sys.modules, "evdev.ecodes", ecodes)
    backend = _keys.UInputBackend()
    try:
        assert len(fds) == 2
        assert [os.get_inheritable(fd) for fd in fds] == [False, False]
    finally:
        backend.close()
        for fd in fds:
            os.close(fd)


# ---- held-set diffing ---------------------------------------------------------------------------------------------

def test_held_set_diffing(keyboard, fakes):
    controller, _ = keyboard()
    log = fakes.keys
    assert applied(controller, log, "a") == [("down", "KEY_A")]
    assert applied(controller, log, "a") == [], "keeping the same choice sends nothing"
    assert applied(controller, log, "shift+d") == [("up", "KEY_A"), ("down", "KEY_LEFTSHIFT"), ("down", "KEY_D")]
    assert set(controller.held()) == {"Shift", "D"}
    assert applied(controller, log, "d") == [("up", "KEY_LEFTSHIFT")]
    assert applied(controller, log, "shift+a") == [("up", "KEY_D"), ("down", "KEY_LEFTSHIFT"), ("down", "KEY_A")]
    assert applied(controller, log, "ctrl+shift+a") == [("down", "KEY_LEFTCTRL")]
    released = applied(controller, log, None)
    assert released[0] == ("up", "KEY_A"), "modifiers are released last"
    assert set(released[1:]) == {("up", "KEY_LEFTSHIFT"), ("up", "KEY_LEFTCTRL")} and len(released) == 3
    assert controller.held() == []
    assert log.held() == set()


def test_modifiers_are_pressed_first_and_released_last(keyboard, fakes):
    controller, _ = keyboard()
    assert applied(controller, fakes.keys, "d+shift") == [("down", "KEY_LEFTSHIFT"), ("down", "KEY_D")]
    assert applied(controller, fakes.keys, None) == [("up", "KEY_D"), ("up", "KEY_LEFTSHIFT")]


def test_a_function_choice_releases_held_keys(keyboard, fakes):
    controller, _ = keyboard()
    applied(controller, fakes.keys, "a+space")
    effect = E(lambda: None)
    mark = fakes.keys.mark()
    controller.apply(effect)
    assert sorted(fakes.keys.actions(mark)) == [("up", "KEY_A"), ("up", "KEY_SPACE")]


# ---- taps ---------------------------------------------------------------------------------------------------------

def test_tap_releases_about_50_ms_later_without_blocking(keyboard, fakes):
    controller, _ = keyboard()
    controller.apply(E("tap space"))
    returned = time.monotonic()
    events = taps_done(fakes.keys)
    assert [(e.action, e.code) for e in events] == [("down", "KEY_SPACE"), ("up", "KEY_SPACE")]
    assert 0.03 <= events[1].t - events[0].t <= 0.3
    assert events[1].t > returned, "apply() must not wait for the release"


def test_tap_combo(keyboard, fakes):
    controller, _ = keyboard()
    controller.apply(E("tap ctrl+c"))
    events = taps_done(fakes.keys)
    assert [(e.action, e.code) for e in events] == [("down", "KEY_LEFTCTRL"), ("down", "KEY_C"), ("up", "KEY_C"),
                                                    ("up", "KEY_LEFTCTRL")]


def test_tap_empties_the_held_set(keyboard, fakes):
    controller, _ = keyboard()
    applied(controller, fakes.keys, "a")
    controller.apply(E("tap space"))
    taps_done(fakes.keys)
    assert fakes.keys.actions() == [("down", "KEY_A"), ("up", "KEY_A"), ("down", "KEY_SPACE"), ("up", "KEY_SPACE")]
    assert controller.held() == []


def test_next_key_action_releases_a_pending_tap_first(keyboard, fakes):
    controller, _ = keyboard()
    controller.apply(E("tap space"))
    controller.apply(E("a"))
    time.sleep(0.15)
    assert fakes.keys.actions() == [("down", "KEY_SPACE"), ("up", "KEY_SPACE"), ("down", "KEY_A")]


def test_release_all_cancels_pending_taps(keyboard, fakes):
    controller, _ = keyboard()
    controller.apply(E("tap shift+x"))
    controller.release_all()
    time.sleep(0.15)
    assert fakes.keys.actions() == [("down", "KEY_LEFTSHIFT"), ("down", "KEY_X"), ("up", "KEY_X"),
                                    ("up", "KEY_LEFTSHIFT")]


def test_release_all_works_from_another_thread(keyboard, fakes):
    controller, _ = keyboard()
    applied(controller, fakes.keys, "shift+d")
    thread = threading.Thread(target=controller.release_all)
    thread.start()
    thread.join(2)
    assert fakes.keys.actions(2) == [("up", "KEY_D"), ("up", "KEY_LEFTSHIFT")]
    assert controller.held() == []


def test_press_now_taps(keyboard, fakes):
    controller, _ = keyboard()
    controller.press_now("ctrl+c")
    controller.press_now("space")
    controller.press_now("space")
    events = taps_done(fakes.keys)
    assert [(e.action, e.code) for e in events] == [
        ("down", "KEY_LEFTCTRL"), ("down", "KEY_C"), ("up", "KEY_C"), ("up", "KEY_LEFTCTRL"),
        ("down", "KEY_SPACE"), ("up", "KEY_SPACE"), ("down", "KEY_SPACE"), ("up", "KEY_SPACE")]


def test_press_now_releases_before_it_returns(keyboard, fakes):
    # A tap must not outlive vyrn.press(): the user's next line may hold the GIL for seconds (a long C call), and
    # then neither the tap-release thread nor the watchdog could let go.
    controller, _ = keyboard()
    controller.apply(E("a"))
    controller.press_now("shift+space")
    events = fakes.keys.events()
    assert [(e.action, e.code) for e in events] == [
        ("down", "KEY_A"), ("down", "KEY_LEFTSHIFT"), ("down", "KEY_SPACE"), ("up", "KEY_SPACE"),
        ("up", "KEY_LEFTSHIFT")], "released by the time press() returns; the held A stays down"
    assert 0.04 <= events[3].t - events[2].t <= 0.3, "still a 50 ms tap"
    assert controller.held() == ["A"]


def test_press_now_is_released_when_interrupted(keyboard, fakes):
    controller, _ = keyboard()
    timer = threading.Timer(0.01, controller.release_all)  # e.g. the watchdog on SIGUSR1, mid-tap
    timer.start()
    controller.press_now("space")
    timer.join()
    assert fakes.keys.actions() == [("down", "KEY_SPACE"), ("up", "KEY_SPACE")], "exactly one key-up"


@pytest.mark.parametrize("combo", ["alt", "spcae", "tap", "ctrl+alt+delete", ""])
def test_press_now_refuses_bad_keys(keyboard, fakes, combo):
    controller, _ = keyboard()
    with pytest.raises(Exception):
        controller.press_now(combo)
    time.sleep(0.07)
    assert fakes.keys.actions() == []


def test_close_releases_held_keys(keyboard, fakes):
    controller, _ = keyboard()
    applied(controller, fakes.keys, "a")
    controller.close()
    assert fakes.keys.held() == set()


# ---- policy (checked in this order: sim, check, allow_input, frozen, focus) ------------------------------------

def test_error_classes():
    assert issubclass(_state.SimOnlyError, _state.VyrnUserError)
    assert issubclass(_state.InputNotAllowedError, _state.VyrnUserError)
    assert issubclass(_state.FocusError, _state.VyrnUserError)


@pytest.mark.parametrize("check", [False, True])
def test_simulations_never_press_keys(keyboard, fakes, check):
    controller, _ = keyboard(mode="sim", check=check)
    with pytest.raises(_state.SimOnlyError) as info:
        controller.apply(E("a"))
    assert info.value.kind == "input"
    with pytest.raises(_state.SimOnlyError):
        controller.press_now("space")
    assert not fakes.keys.exists() or fakes.keys.actions() == []


@pytest.mark.parametrize("allow_input", [False, True])
def test_check_mode_describes_and_sends_nothing(keyboard, fakes, allow_input):
    controller, state = keyboard(check=True, allow_input=allow_input)
    controller.apply(E("a"))
    assert state.did == ["would hold A"]
    controller.apply(E("shift+d"))
    controller.apply(E("tap space"))
    controller.press_now("space")
    assert "would hold Shift+D" in state.did
    assert state.did.count("would tap Space") == 2
    time.sleep(0.07)
    assert not fakes.keys.exists() or fakes.keys.actions() == []


def test_input_not_allowed(keyboard, fakes):
    controller, _ = keyboard(allow_input=False)
    controller.apply(E(None))  # not a key action
    with pytest.raises(_state.InputNotAllowedError) as info:
        controller.apply(E("a"))
    assert info.value.kind == "input"
    assert "allowed to press keys" in str(getattr(info.value, "message", info.value))
    with pytest.raises(_state.InputNotAllowedError):
        controller.press_now("space")
    assert not fakes.keys.exists() or fakes.keys.actions() == []


def test_frozen_skips_key_actions_silently(keyboard, fakes):
    controller, _ = keyboard(frozen=True)
    controller.apply(E("a"))
    controller.press_now("space")
    time.sleep(0.07)
    assert fakes.keys.actions() == []


def test_focus_guard_releases_and_skips(keyboard, fakes):
    focus = FocusStub(focused=True)
    controller, _ = keyboard(focus=focus)
    assert applied(controller, fakes.keys, "shift+a") == [("down", "KEY_LEFTSHIFT"), ("down", "KEY_A")]
    focus.focused = False
    assert applied(controller, fakes.keys, "d") == [("up", "KEY_A"), ("up", "KEY_LEFTSHIFT")]
    controller.press_now("space")
    time.sleep(0.07)
    assert fakes.keys.actions(4) == [], "nothing is sent while the game window is not active"
    focus.focused = True
    assert applied(controller, fakes.keys, "d") == [("down", "KEY_D")]


def test_nothing_is_pressed_while_the_scripts_load(keyboard, fakes):
    # load runs code at the top of the scripts and see(): no focus lock exists yet, so keys would go to any window.
    controller, state = keyboard(loading=True)
    controller.press_now("space")
    controller.press_now("enter")
    assert not fakes.keys.exists() or fakes.keys.actions() == []
    assert state.warnings and all("skipped" in w["message"] for w in state.warnings)
    state.loading = False
    controller.press_now("space")
    assert fakes.keys.actions() == [("down", "KEY_SPACE"), ("up", "KEY_SPACE")]


def test_nothing_is_pressed_once_the_worker_is_closing(keyboard, fakes):
    controller, state = keyboard()
    controller.apply(E("a"))
    controller.close()
    controller.apply(E("d"))
    controller.press_now("space")
    assert fakes.keys.actions() == [("down", "KEY_A"), ("up", "KEY_A")], "a closed keyboard never comes back"
    controller2, _ = keyboard(stopping=True)
    controller2.press_now("space")
    assert fakes.keys.actions() == [("down", "KEY_A"), ("up", "KEY_A")]


def test_a_lock_whose_watcher_is_gone_fails_closed(keyboard, fakes):
    controller, state = keyboard(focus_locked=True, focus=None)
    controller.apply(E("a"))
    controller.press_now("space")
    assert not fakes.keys.exists() or fakes.keys.actions() == []


def test_held_labels(keyboard, fakes):
    controller, _ = keyboard()
    assert controller.held() == []
    controller.apply(E("shift+d"))
    assert sorted(controller.held()) == ["D", "Shift"]
    controller.apply(E("tap space"))
    wait_until(lambda: not fakes.keys.held(), 1.0, "the tap to be released")
    assert controller.held() == []
