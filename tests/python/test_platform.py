"""The platform backends (vyrn._platform, docs/dev/platforms.md, python-internals.md section 13).

Only Linux runs here, so the macOS and Windows backends run against fakes: a recording user32/kernel32, CoreGraphics
and Objective-C runtime, a fake kqueue, a fake ImageGrab. Windows' transport (TCP + token + control connection) is
selected explicitly and runs for real: in-process over socket pairs, and end to end with real workers
(engine.Worker(transport="tcp") sets VYRN_TRANSPORT=tcp). Nothing here presses a real key or captures the screen.
"""

import ctypes
import os
import re
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
import pytest
from PIL import Image
from PIL.PngImagePlugin import PngInfo

from testkit import PYTHON_DIR, wait_until
from vyrn import _effects, _focus, _keys, _platform, _screen, _state
from vyrn._platform import (focus_base, focus_helper, focus_mac, focus_win32, keys_quartz, keys_uinput, keys_win32,
                            screen_grim, screen_imagegrab, transport)

INPUT_EVENT_CODES = Path("/usr/include/linux/input-event-codes.h")


def kernel_key_codes():
    text = INPUT_EVENT_CODES.read_text()
    return {name: int(value, 0) for name, value in re.findall(r"#define\s+(KEY_\w+)\s+(0x[0-9a-fA-F]+|\d+)\b", text)}


# ---- the chooser --------------------------------------------------------------------------------------------------

def test_this_system_and_its_transport(monkeypatch):
    assert _platform.SYSTEM == "linux" and _platform.TRANSPORT == "fd"
    monkeypatch.delenv("VYRN_TRANSPORT", raising=False)
    assert _platform.transport_kind() == "fd"
    assert _platform.transport_kind({"VYRN_TRANSPORT": "tcp"}) == "tcp", "tests can select Windows' transport"


@pytest.mark.parametrize("system, keys, focus", [
    ("linux", keys_uinput, focus_helper), ("macos", keys_quartz, focus_mac), ("windows", keys_win32, focus_win32)])
def test_backends_are_chosen_by_system(system, keys, focus):
    assert _platform.keys_module(system) is keys
    assert _platform.focus_module(system) is focus
    assert _platform.focus_module(system, helper_path="/x/fake-focus") is focus_helper, "a named helper works anywhere"


def test_screen_backend_choice():
    choose = _platform.screen_module
    assert choose("linux", {"WAYLAND_DISPLAY": "wayland-0", "DISPLAY": ":0"}) is screen_grim
    assert choose("linux", {"DISPLAY": ":0"}) is screen_imagegrab, "Linux under X11"
    assert choose("linux", {}) is screen_grim, "no desktop: grim's probe explains"
    assert choose("macos", {}) is screen_imagegrab and choose("windows", {}) is screen_imagegrab
    for system in ("linux", "macos", "windows"):
        assert choose(system, {"VYRN_GRIM": "/fake/grim"}) is screen_grim, "the tests' fake grim works everywhere"


def platform_checks(path):
    """(line, text) of every sys.platform / os.name / platform.system() in the code (strings and comments aside)."""
    import io
    import tokenize

    with open(path, encoding="utf-8") as file:
        tokens = [t for t in tokenize.generate_tokens(io.StringIO(file.read()).readline)
                  if t.type in (tokenize.NAME, tokenize.OP)]
    found = []
    for index in range(len(tokens) - 2):
        words = tuple(t.string for t in tokens[index:index + 3])
        if words in (("sys", ".", "platform"), ("os", ".", "name"), ("platform", ".", "system")):
            found.append((tokens[index].start[0], "".join(words)))
    return found


def test_only_the_chooser_looks_at_the_platform():
    chooser = PYTHON_DIR / "vyrn" / "_platform" / "__init__.py"
    found = [f"{path.relative_to(PYTHON_DIR)}:{line}: {what}" for path in sorted(PYTHON_DIR.rglob("*.py"))
             if path != chooser for line, what in platform_checks(path)]
    assert not found, "platform checks belong in vyrn/_platform/__init__.py:\n" + "\n".join(found)
    assert platform_checks(chooser), "the chooser does look"


IMPORT_ELSEWHERE = r"""
import os, select, signal, socket, subprocess, sys, threading, ctypes, ctypes.util, json, ast, inspect, numbers, io
import numpy, numpy.random, PIL.Image, PIL.ImageColor, PIL.PngImagePlugin
system = sys.argv[1]
sys.platform = {"windows": "win32", "macos": "darwin"}[system]
for module, names in ((os, ["pipe2", "setpgrp", "killpg", "posix_spawn", "WNOHANG"] if system == "windows" else ["pipe2"]),
                      (select, ["POLLRDHUP", "epoll"] + (["poll", "kqueue"] if system == "windows" else [])),
                      (signal, ["SIGUSR1", "SIGUSR2", "SIGHUP", "SIGKILL"] if system == "windows" else [])):
    for name in names:
        if hasattr(module, name):
            delattr(module, name)
for name in ("evdev", "PIL.ImageGrab") + (("fcntl", "resource", "termios", "grp", "pwd") if system == "windows" else ()):
    sys.modules[name] = None
sys.path[:0] = [sys.argv[2], os.path.join(sys.argv[2], "tools")]
import vyrn, vyrn._host, vyrn._keys, vyrn._screen, vyrn._focus, vyrn._link, vyrn._canvas
import engine, check
from vyrn import _platform
from vyrn._platform import (transport, keys_base, keys_uinput, keys_quartz, keys_win32, screen_grim, screen_imagegrab,
                            focus_base, focus_helper, focus_mac, focus_win32)
_platform.preload()
print(_platform.SYSTEM, _platform.transport_kind(), _platform.keys_module().NAME, _platform.screen_module().NAME,
      _platform.focus_module().NAME, engine.Worker.__name__)
if system == "macos":
    a, b = socket.socketpair()
    link = _platform.connect({"VYRN_FD": str(b.fileno())})
    print(type(link.watch()).__name__)
"""


@pytest.mark.parametrize("system, expected", [
    ("windows", "windows tcp win32 imagegrab win32 Worker"),
    ("macos", "macos fd quartz imagegrab cgwindowlist Worker\nKqueueWatch"),
])
def test_vyrn_imports_where_linux_only_calls_are_missing(system, expected):
    """Pretend to be Windows (no SIGUSR1, poll, pipe2, fcntl, resource, evdev...) or macOS (no POLLRDHUP, epoll,
    pipe2): every module of the library, the worker host and the tools still imports, and the chooser picks that
    system's backends."""
    done = subprocess.run([sys.executable, "-B", "-c", IMPORT_ELSEWHERE, system, str(PYTHON_DIR)],
                          capture_output=True, text=True, timeout=120, env=dict(os.environ, VYRN_GRIM=""))
    assert done.returncode == 0, done.stderr
    assert done.stdout.strip() == expected


# ---- key code tables ----------------------------------------------------------------------------------------------

TABLES = {"quartz": keys_quartz.KVK, "win32": {code: (scan, code in keys_win32.EXTENDED)
                                                for code, scan in keys_win32.SCAN.items()}}


@pytest.mark.parametrize("name", sorted(TABLES))
def test_every_allowed_key_has_exactly_one_code(name):
    table = TABLES[name]
    assert set(table) == set(_effects.ALLOWED_CODES), (
        f"missing: {sorted(set(_effects.ALLOWED_CODES) - set(table))}, extra: {sorted(set(table) - set(_effects.ALLOWED_CODES))}")
    assert len(set(table.values())) == len(table), "two keys share a code"
    for friendly, code in _effects.KEYS.items():
        assert code in table, f"{friendly!r} has no {name} code"


@pytest.mark.skipif(not INPUT_EVENT_CODES.exists(), reason="kernel headers not installed")
def test_every_allowed_key_is_an_evdev_key():
    codes = kernel_key_codes()
    assert set(_effects.ALLOWED_CODES) <= set(codes), sorted(set(_effects.ALLOWED_CODES) - set(codes))


@pytest.mark.skipif(not INPUT_EVENT_CODES.exists(), reason="kernel headers not installed")
def test_windows_scan_codes_match_the_kernels_numbering():
    # Linux numbers the main keyboard block by its set-1 scan codes; only the E0-prefixed navigation keys differ.
    codes = kernel_key_codes()
    for code, scan in keys_win32.SCAN.items():
        if code not in keys_win32.EXTENDED:
            assert scan == codes[code], f"{code}: scan code {scan:#x}, kernel {codes[code]:#x}"
    assert keys_win32.EXTENDED == {"KEY_HOME", "KEY_UP", "KEY_PAGEUP", "KEY_LEFT", "KEY_RIGHT", "KEY_END",
                                   "KEY_DOWN", "KEY_PAGEDOWN", "KEY_INSERT", "KEY_DELETE"}


def test_no_table_reaches_a_blocked_key():
    # macOS: Command, right Command, Option, right Option, Caps Lock, Fn, right Shift, right Control, volume, mute
    assert not set(keys_quartz.KVK.values()) & {0x37, 0x36, 0x3A, 0x3D, 0x39, 0x3F, 0x3C, 0x3E, 0x48, 0x49, 0x4A}
    # Windows: Alt (0x38, E0 38), Windows keys and Menu (E0 5B/5C/5D), Caps/Num/Scroll Lock, right Shift, right Ctrl
    plain = {scan for code, scan in keys_win32.SCAN.items() if code not in keys_win32.EXTENDED}
    extended = {scan for code, scan in keys_win32.SCAN.items() if code in keys_win32.EXTENDED}
    assert not plain & {0x38, 0x3A, 0x45, 0x46, 0x36}
    assert not extended & {0x38, 0x5B, 0x5C, 0x5D, 0x1D, 0x37}


# ---- Windows keys: SendInput with scan codes ----------------------------------------------------------------------

class FakeUser32:
    """Records SendInput calls: (type, wVk, wScan, dwFlags, cbSize) per INPUT record."""

    def __init__(self, result=None):
        self.sent = []
        self.result = result

    def SendInput(self, count, records, size):
        for index in range(count):
            record = records[index]
            self.sent.append((record.type, record.ki.wVk, record.ki.wScan, record.ki.dwFlags, size))
        return count if self.result is None else self.result


def test_input_record_has_windows_layout():
    assert ctypes.sizeof(keys_win32.INPUT) == (40 if ctypes.sizeof(ctypes.c_void_p) == 8 else 28)
    assert ctypes.sizeof(keys_win32.KEYBDINPUT) == (24 if ctypes.sizeof(ctypes.c_void_p) == 8 else 16)


def test_sendinput_sends_scan_codes():
    user32 = FakeUser32()
    keyboard = keys_win32.SendInputBackend(user32=user32)
    keyboard.down("KEY_LEFTSHIFT")
    keyboard.down("KEY_D")
    keyboard.up("KEY_D")
    keyboard.up("KEY_LEFTSHIFT")
    keyboard.down("KEY_UP")
    keyboard.close()                       # releases the held arrow
    scancode, keyup, extended = (keys_win32.KEYEVENTF_SCANCODE, keys_win32.KEYEVENTF_KEYUP,
                                 keys_win32.KEYEVENTF_EXTENDEDKEY)
    size = ctypes.sizeof(keys_win32.INPUT)
    assert user32.sent == [
        (1, 0, 0x2A, scancode, size), (1, 0, 0x20, scancode, size), (1, 0, 0x20, scancode | keyup, size),
        (1, 0, 0x2A, scancode | keyup, size), (1, 0, 0x48, scancode | extended, size),
        (1, 0, 0x48, scancode | extended | keyup, size)]


def test_sendinput_refuses_keys_off_the_allow_list_and_reports_failures():
    user32 = FakeUser32()
    keyboard = keys_win32.SendInputBackend(user32=user32)
    with pytest.raises(ValueError):
        keyboard.down("KEY_LEFTALT")
    assert user32.sent == []
    failing = keys_win32.SendInputBackend(user32=FakeUser32(result=0))
    with pytest.raises(OSError):
        failing.down("KEY_A")


def test_controller_drives_the_windows_backend(state, monkeypatch):
    """The policy layer is the same everywhere: with Windows' backend the held set diff becomes SendInput calls."""
    user32 = FakeUser32()
    monkeypatch.delenv("VYRN_KEYS_BACKEND")
    monkeypatch.setattr(_platform, "keys_module", lambda system=None: keys_win32)
    monkeypatch.setattr(_platform, "make_keyboard", lambda: keys_win32.SendInputBackend(user32=user32))
    state.allow_input = True
    controller = _keys.Controller(state)
    try:
        assert _keys.probe()[0] == "win32" or _keys.probe()[1]
        controller.apply(_effects.parse_effect("shift+d"))
        assert [(scan, flags & keys_win32.KEYEVENTF_KEYUP) for _, _, scan, flags, _ in user32.sent] == [
            (0x2A, 0), (0x20, 0)]
        assert controller.held() == ["Shift", "D"]
    finally:
        controller.close()
    assert [(scan, bool(flags & keys_win32.KEYEVENTF_KEYUP)) for _, _, scan, flags, _ in user32.sent][2:] == [
        (0x20, True), (0x2A, True)], "close() releases D first, Shift last"


def test_windows_probe():
    assert keys_win32.probe(user32=FakeUser32()) == ("win32", None)
    name, problem = keys_win32.probe(user32=object())
    assert name is None and "keyboard" in problem


# ---- macOS keys: Quartz events --------------------------------------------------------------------------------------

class FakeQuartz:
    """CoreGraphics + CoreFoundation stand-in: keeps every event and what happened to it."""

    def __init__(self):
        self.events = {}
        self.posted = []
        self.released = []

    def CGEventCreateKeyboardEvent(self, source, key, down):
        ref = 0x1000 + len(self.events)
        self.events[ref] = {"source": source, "key": key, "down": down, "flags": None}
        return ref

    def CGEventSetFlags(self, event, flags):
        self.events[event]["flags"] = flags

    def CGEventPost(self, tap, event):
        self.posted.append((tap, dict(self.events[event])))

    def CFRelease(self, event):
        self.released.append(event)


class FakeAccessibility:
    def __init__(self, trusted):
        self.trusted = trusted

    def AXIsProcessTrusted(self):
        return self.trusted


def test_quartz_posts_kvk_codes_with_modifier_flags():
    cg = FakeQuartz()
    keyboard = keys_quartz.QuartzBackend(cg=cg, cf=cg, ax=FakeAccessibility(True))
    for code, down in (("KEY_LEFTSHIFT", 1), ("KEY_D", 1), ("KEY_D", 0), ("KEY_LEFTSHIFT", 0), ("KEY_LEFTCTRL", 1),
                       ("KEY_C", 1)):
        (keyboard.down if down else keyboard.up)(code)
    keyboard.close()
    shift, ctrl = keys_quartz.FLAG_SHIFT, keys_quartz.FLAG_CONTROL
    assert [(tap, e["key"], e["down"], e["flags"], e["source"]) for tap, e in cg.posted] == [
        (0, 0x38, True, shift, None), (0, 0x02, True, shift, None), (0, 0x02, False, shift, None),
        (0, 0x38, False, 0, None), (0, 0x3B, True, ctrl, None), (0, 0x08, True, ctrl, None),
        (0, 0x08, False, ctrl, None), (0, 0x3B, False, 0, None)]
    assert sorted(cg.released) == sorted(cg.events), "every event is released"


def test_quartz_needs_the_accessibility_permission():
    with pytest.raises(_state.VyrnUserError) as info:
        keys_quartz.QuartzBackend(cg=FakeQuartz(), cf=FakeQuartz(), ax=FakeAccessibility(False))
    assert info.value.kind == "input" and "Accessibility" in info.value.hint
    name, problem = keys_quartz.probe(ax=FakeAccessibility(False))
    assert name is None
    assert problem.startswith("needs the Accessibility permission") and "System Settings → Privacy & Security" in problem
    assert keys_quartz.probe(ax=FakeAccessibility(True)) == ("quartz", None)


# ---- screen: ImageGrab and HiDPI -----------------------------------------------------------------------------------

def physical_screen(width, height, scale):
    """A screen of width x height points as a Retina capture: each point is a scale x scale block whose pixels
    average to (4x, 5y, 7)."""
    points = np.zeros((height, width, 3), dtype=np.uint8)
    points[..., 0] = 4 * np.arange(width, dtype=np.uint8)[None, :]
    points[..., 1] = 5 * np.arange(height, dtype=np.uint8)[:, None]
    points[..., 2] = 7
    pixels = np.repeat(np.repeat(points, scale, axis=0), scale, axis=1).astype(np.int16)
    pixels[0::2, 0::2] += 1                # not flat inside a block: only an average gives the point back
    pixels[1::2, 1::2] -= 1
    return points, Image.fromarray(np.clip(pixels, 0, 255).astype(np.uint8))


class FakeGrab:
    """Stands in for PIL.ImageGrab.grab: crops `image` like macOS' screencapture (bbox in points, pixels back)."""

    def __init__(self, image, scale=1, resize=False):
        self.image, self.scale, self.resize, self.calls = image, scale, resize, []

    def __call__(self, bbox=None):
        self.calls.append(bbox)
        if bbox is None:
            return self.image.copy()
        part = self.image.crop(tuple(v * self.scale for v in bbox))
        return part.resize((bbox[2] - bbox[0], bbox[3] - bbox[1])) if self.resize else part


@pytest.mark.parametrize("resize", [False, True], ids=["pixels", "already-points"])
def test_macos_capture_is_in_points(resize):
    points, image = physical_screen(64, 48, 2)
    grab = FakeGrab(image, scale=2, resize=resize)
    capture = screen_imagegrab.ImageGrabCapture("macos", grab=grab, points=lambda: (64, 48))
    whole = capture.capture(None)
    assert whole.shape == (48, 64, 3) and capture.scale == 2
    assert np.array_equal(whole, points), "2x2 pixels average to their point"
    area = capture.capture((10, 5, 30, 15))
    assert area.shape == (10, 20, 3)
    if not resize:
        assert np.array_equal(area, points[5:15, 10:30])
    assert grab.calls == [None, (10, 5, 30, 15)], "areas are asked for in points"


def test_windows_capture_is_in_pixels():
    _, image = physical_screen(40, 30, 1)
    capture = screen_imagegrab.ImageGrabCapture("windows", grab=FakeGrab(image), points=lambda: (20, 15))
    assert capture.capture(None).shape == (30, 40, 3), "no scaling outside macOS"
    assert capture.capture((1, 2, 5, 4)).shape == (2, 4, 3)


def test_screen_on_a_retina_mac(state, tmp_path):
    """vyrn.screen()/pixel()/on_screen() all work in points, with one capture per request, and a Snapshot saved with
    the screen's pixels (2x its area) still matches."""
    points, image = physical_screen(64, 48, 2)
    grab = FakeGrab(image, scale=2)
    screen = _screen.Screen(backend=screen_imagegrab.ImageGrabCapture("macos", grab=grab, points=lambda: (64, 48)))
    assert screen.size() == (64, 48)
    state.generation += 1
    assert screen.grab(20, 10, 5, 4).shape == (4, 5, 3)
    assert screen.pixel(20, 10) == (80, 50, 7) and screen.pixel(24, 13) == (96, 65, 7)
    assert screen.captures == 2, "the first size() plus one capture for this request"
    pictures = Path(state.project_dir) / "pictures"
    pictures.mkdir()
    info = PngInfo()
    info.add_text("vyrn-area", "20,10,5,4")
    image.crop((40, 20, 50, 28)).save(pictures / "retina.png", pnginfo=info)   # 10x8 pixels for a 5x4 area
    assert screen.on_screen("retina.png") is True
    assert screen.on_screen("retina.png", x=0, y=0) is False


def test_imagegrab_failures_explain_themselves():
    def broken(bbox=None):
        raise OSError("screencapture: cannot run")

    capture = screen_imagegrab.ImageGrabCapture("macos", grab=broken, points=lambda: (10, 10))
    with pytest.raises(_state.VyrnUserError) as info:
        capture.capture(None)
    assert "Screen Recording" in info.value.hint


def test_imagegrab_probe():
    probe = screen_imagegrab.probe
    name, problem = probe("macos", image_grab=object(), screen_access=lambda: False)
    assert name is None
    assert problem.startswith("needs the Screen Recording permission (System Settings → Privacy & Security")
    assert probe("macos", image_grab=object(), screen_access=lambda: True) == ("imagegrab", None)
    assert probe("macos", image_grab=object(), screen_access=lambda: None) == ("imagegrab", None), "before 10.15"
    assert probe("windows", image_grab=object()) == ("imagegrab", None)
    name, problem = probe("linux", environ={})
    assert name is None and "DISPLAY" in problem
    name, problem = probe("windows")                     # the real ImageGrab is blocked in the tests
    assert name is None and "ImageGrab" in problem


# ---- focus: the polling source --------------------------------------------------------------------------------------

class FakeWindows:
    """user32 + kernel32 stand-ins for Win32Query: windows (hwnd -> title, pid), programs (pid -> path), the front."""

    def __init__(self):
        self.windows = {}
        self.programs = {}
        self.front = None
        self.open_handles = 0
        self.fail = None

    def add(self, hwnd, title, pid, path):
        self.windows[hwnd] = (title, pid)
        self.programs[pid] = path

    def GetForegroundWindow(self):
        if self.fail:
            raise OSError(self.fail)
        return self.front

    def GetWindowTextLengthW(self, hwnd):
        return len(self.windows[hwnd][0])

    def GetWindowTextW(self, hwnd, buffer, size):
        buffer.value = self.windows[hwnd][0][:size - 1]
        return len(buffer.value)

    def GetWindowThreadProcessId(self, hwnd, pid):
        pid._obj.value = self.windows[hwnd][1]
        return 1

    def IsWindow(self, hwnd):
        return hwnd in self.windows

    def OpenProcess(self, access, inherit, pid):
        assert access == focus_win32.PROCESS_QUERY_LIMITED_INFORMATION and not inherit
        if pid not in self.programs:
            return None
        self.open_handles += 1
        return 5000 + pid

    def QueryFullProcessImageNameW(self, handle, flags, buffer, size):
        path = self.programs[handle - 5000]
        buffer.value = path
        size._obj.value = len(path)
        return 1

    def CloseHandle(self, handle):
        self.open_handles -= 1
        return 1


@pytest.fixture
def windows():
    """A FocusWatcher over Win32Query over FakeWindows; the watchers are stopped after the test."""
    fake = FakeWindows()
    fake.add(0x10, "My Game", 42, r"C:\Games\My Game\game.exe")
    fake.add(0x20, "Notes\tand\nmore", 43, r"C:\Windows\notepad.exe")
    fake.front = 0x10
    made = []

    def watch():
        made.append(_focus.FocusWatcher(source=focus_win32.source(query=focus_win32.Win32Query(fake, fake))))
        return made[-1]

    yield fake, watch
    for watcher in made:
        watcher.stop()


def test_windows_focus_reports_like_the_helper(windows):
    fake, watch = windows
    watcher = watch()
    assert watcher.wait_ready(1.0)
    assert watcher.active() == {"id": 0x10, "app_id": "game.exe", "title": "My Game"}
    fake.front = 0x20
    wait_until(lambda: (watcher.active() or {}).get("id") == 0x20, 1.0, "the poll thread to see the new window")
    assert watcher.active() == {"id": 0x20, "app_id": "notepad.exe", "title": "Notes and more"}
    fake.front = None
    wait_until(lambda: watcher.active() is None, 1.0, "no active window")
    assert fake.open_handles == 0, "every process handle is closed"
    assert focus_base.PollSource.INTERVAL == 0.05, "20 Hz"


def test_windows_focus_lock_follows_the_window(windows):
    fake, watch = windows
    watcher = watch()
    assert watcher.wait_ready(1.0)
    assert watcher.lock()["app_id"] == "game.exe"
    assert watcher.is_focused() is True
    fake.front = 0x20
    assert watcher.is_focused() is False, "asked right away, not at the next poll"
    fake.front = 0x10
    fake.windows[0x10] = ("My Game - level 2", 42)
    assert watcher.is_focused() is True, "titles change; the lock is on the window"
    del fake.windows[0x10]
    fake.front = None
    assert watcher.is_focused() is False
    fake.add(0x10, "A new window that got the old handle", 44, r"C:\x.exe")
    fake.front = 0x10
    assert watcher.is_focused() is False, "a closed window's lock is never trusted again"


def test_windows_focus_notices_a_reused_handle_at_once(windows):
    fake, watch = windows
    watcher = watch()
    assert watcher.wait_ready(1.0)
    watcher.lock()
    fake.add(0x10, "My Game", 99, r"C:\Other\game.exe")    # closed and reused by another program between two polls
    assert watcher.is_focused() is False
    assert watcher.active() == {"id": 0x10, "app_id": "game.exe", "title": "My Game"}, "still reported as active"


@pytest.mark.parametrize("path, pid", [(r"C:\Program Files\Vyrn\Vyrn.exe", 50), (r"C:\x\game.exe", os.getppid())])
def test_windows_focus_refuses_vyrn_itself(windows, path, pid):
    fake, watch = windows
    fake.add(0x30, "Vyrn", pid, path)
    fake.front = 0x30
    watcher = watch()
    assert watcher.wait_ready(1.0)
    with pytest.raises(_state.FocusError, match="Vyrn itself"):
        watcher.lock()


def test_windows_focus_fails_closed_when_windows_stops_answering(windows):
    fake, watch = windows
    watcher = watch()
    assert watcher.wait_ready(1.0)
    watcher.lock()
    fake.fail = "access denied"
    assert watcher.is_focused() is False
    assert watcher.alive() is False
    with pytest.raises(_state.FocusError, match="Windows stopped telling Vyrn"):
        watcher.lock()


class FakeWindowServer:
    """CoreGraphics + CoreFoundation stand-ins for WindowServer over `windows`, front to back (dicts with "layer",
    "pid", "name"; a missing key is missing from the window's dictionary). Every object is an int handle; `owned` is
    what the code got from Create/Copy and hasn't released (releasing anything else fails the test)."""

    def __init__(self, windows=()):
        self.windows = list(windows)
        self.fail = None                      # "null": CGWindowListCopyWindowInfo returns NULL; "raise": it raises
        self.unreadable = set()               # key names whose CFNumberGetValue fails
        self.objects, self.owned, self.lists = {}, set(), []

    def _new(self, kind, value, owned=False):
        handle = 0x1000 + 16 * len(self.objects)
        self.objects[handle] = (kind, value)
        if owned:
            self.owned.add(handle)
        return handle

    def CGWindowListCopyWindowInfo(self, option, relative_to):
        self.lists.append((option, relative_to))
        if self.fail == "raise":
            raise OSError("the WindowServer went away")
        if self.fail == "null":
            return None
        return self._new("array", [self._new("dict", dict(w)) for w in self.windows], owned=True)

    def CFStringCreateWithCString(self, allocator, text, encoding):
        assert allocator is None and encoding == focus_mac.kCFStringEncodingUTF8
        return self._new("key", text.decode("ascii"), owned=True)

    def CFArrayGetCount(self, array):
        return len(self.objects[array][1])

    def CFArrayGetValueAtIndex(self, array, index):
        return self.objects[array][1][index]

    def CFDictionaryGetValue(self, dictionary, key):
        kind, name = self.objects[key]
        assert kind == "key", "keys are CFStrings the code made"
        field = {"kCGWindowLayer": "layer", "kCGWindowOwnerPID": "pid", "kCGWindowOwnerName": "name"}[name]
        window = self.objects[dictionary][1]
        if field not in window:
            return None
        return self._new("string" if field == "name" else "number", (name, window[field]))

    def CFNumberGetValue(self, number, kind, out):
        assert kind == focus_mac.kCFNumberSInt64Type and self.objects[number][0] == "number"
        name, value = self.objects[number][1]
        if name in self.unreadable:
            return False
        out._obj.value = value
        return True

    def CFStringGetCString(self, string, buffer, size, encoding):
        data = self.objects[string][1][1].encode("utf-8")
        if len(data) >= size:
            return False
        buffer.value = data
        return True

    def CFRelease(self, handle):
        assert handle in self.owned, f"CFRelease of something not owned: {self.objects.get(handle)}"
        self.owned.remove(handle)


GAME = {"layer": 0, "pid": 501, "name": "My Game"}
TERMINAL = {"layer": 0, "pid": 777, "name": "Terminal"}
MENU_BAR = {"layer": 24, "pid": 88, "name": "Window Server"}
DOCK = {"layer": 20, "pid": 89, "name": "Dock"}


def mac_query(fake, objc=None):
    return focus_mac.MacQuery(server=focus_mac.WindowServer(fake, fake), objc=objc, alive=lambda pid: True)


def test_macos_focus_reads_the_front_window():
    fake = FakeWindowServer([MENU_BAR, DOCK, GAME, TERMINAL])
    query = mac_query(fake)
    assert query.active() == focus_base.Window(501, "My Game", "My Game", 501, False, "My Game")
    assert fake.lists == [(focus_mac.kCGWindowListOptionOnScreenOnly | focus_mac.kCGWindowListExcludeDesktopElements,
                           focus_mac.kCGNullWindowID)]
    fake.windows = [MENU_BAR, TERMINAL, GAME]          # another app came to the front: the next call knows
    assert query.active().id == 777
    query.close()
    assert fake.owned == set(), "every list it copied and every key it made is released"


@pytest.mark.parametrize("windows, fail, unreadable", [
    ([], None, ()),                                         # nothing on screen
    ([MENU_BAR, DOCK], None, ()),                           # no normal (layer-0) window
    ([MENU_BAR, GAME], "null", ()),                         # CGWindowListCopyWindowInfo returned NULL
    ([MENU_BAR, GAME], "raise", ()),                        # ... or failed
    ([MENU_BAR, {"layer": 0, "name": "No pid"}, GAME], None, ()),    # the front window's pid is missing
    ([MENU_BAR, GAME, TERMINAL], None, ("kCGWindowOwnerPID",)),        # ... or unreadable
    ([{"pid": 5, "name": "No layer"}, GAME], None, ()),     # can't tell whether the first window is in front
    ([MENU_BAR, {"layer": 0, "pid": 0, "name": "Bad"}, GAME], None, ()),
], ids=["empty", "no-layer-0", "null", "raises", "no-pid", "unreadable-pid", "no-layer", "pid-0"])
def test_macos_focus_fails_closed(windows, fail, unreadable):
    """Never a window further back: anything unclear about the front window means no active window."""
    fake = FakeWindowServer(windows)
    fake.fail = fail
    fake.unreadable = set(unreadable)
    query = mac_query(fake)
    assert query.active() is None
    query.close()
    assert fake.owned == set()


class FakeObjC:
    """The ObjC wrapper's interface for the bundle-id lookup: pid -> bundle id (None = no such app)."""

    def __init__(self, bundles):
        self.bundles, self.pushed, self.popped = bundles, 0, 0

    def cls(self, name):
        assert name == "NSRunningApplication"
        return "NSRunningApplication"

    def push(self):
        self.pushed += 1
        return self.pushed

    def pop(self, pool):
        self.popped += 1

    def text(self, value):
        return value or ""

    def send(self, receiver, selector, *args, restype=None, argtypes=()):
        if not receiver:
            return None
        if selector == "runningApplicationWithProcessIdentifier:":
            return ("app", args[0]) if args[0] in self.bundles else None
        assert selector == "bundleIdentifier"
        return self.bundles[receiver[1]]


def test_macos_bundle_id_is_only_for_display():
    fake = FakeWindowServer([MENU_BAR, GAME])
    objc = FakeObjC({501: "com.example.mygame"})
    query = mac_query(fake, objc)
    assert query.active() == focus_base.Window(501, "com.example.mygame", "My Game", 501, False, "My Game")
    query.active()
    assert objc.pushed == objc.popped == 1, "asked once per app, inside an autorelease pool"
    fake.windows = [{"layer": 0, "pid": 502, "name": "Tool"}]
    assert query.active().app_id == "Tool", "no bundle id: the owner's name"


@pytest.mark.parametrize("window", [{"layer": 0, "pid": 900, "name": "Vyrn"},
                                    {"layer": 0, "pid": os.getppid(), "name": "Python"}])
def test_macos_focus_knows_vyrn(window):
    assert mac_query(FakeWindowServer([window])).active().vyrn is True


def test_macos_focus_drives_the_watcher():
    fake = FakeWindowServer([MENU_BAR, GAME, TERMINAL])
    watcher = _focus.FocusWatcher(source=focus_mac.source(query=mac_query(fake)))
    try:
        assert watcher.wait_ready(1.0)
        assert watcher.lock() == {"id": 501, "app_id": "My Game", "title": "My Game"}
        fake.windows = [MENU_BAR, TERMINAL, GAME]
        assert watcher.is_focused() is False, "a switch is seen right away"
        fake.windows = [MENU_BAR, GAME, TERMINAL]
        assert watcher.is_focused() is True
        fake.fail = "raise"
        assert watcher.is_focused() is False and watcher.active() is None, "a failing call: not focused"
        fake.fail = None
        fake.windows = []
        assert watcher.is_focused() is False
        fake.windows = [dict(GAME, name="Another Program")]     # the pid came back with another name
        assert watcher.is_focused() is False
        fake.windows = [GAME]
        assert watcher.is_focused() is False, "the lock is on pid + name, and that program is gone"
    finally:
        watcher.stop()
    assert fake.owned == set()


def test_macos_focus_refuses_vyrn_itself():
    fake = FakeWindowServer([{"layer": 0, "pid": 900, "name": "Vyrn"}, GAME])
    watcher = _focus.FocusWatcher(source=focus_mac.source(query=mac_query(fake)))
    try:
        assert watcher.wait_ready(1.0)
        with pytest.raises(_state.FocusError, match="Vyrn itself"):
            watcher.lock()
    finally:
        watcher.stop()


def test_focus_probes():
    assert focus_win32.probe(user32=FakeWindows()) == ("win32", None)
    assert focus_mac.probe(load=lambda: (FakeWindowServer(), FakeWindowServer())) == ("cgwindowlist", None)

    def missing():
        raise OSError("no CoreGraphics here")

    name, problem = focus_mac.probe(load=missing)
    assert name is None and "macOS" in problem


# ---- transport: the POSIX watches -----------------------------------------------------------------------------------

class FakeKevent:
    def __init__(self, ident, filter=0, flags=0, fflags=0, data=0, udata=0):
        self.ident, self.filter, self.flags, self.fflags = ident, filter, flags, fflags


class FakeKqueueApi:
    """select's kqueue part: kqueue(), kevent and the constants (values as on macOS)."""

    KQ_FILTER_READ, KQ_FILTER_PROC = -1, -5
    KQ_EV_ADD, KQ_EV_DELETE, KQ_EV_ONESHOT, KQ_EV_CLEAR, KQ_EV_EOF = 0x1, 0x2, 0x10, 0x20, 0x8000
    KQ_NOTE_EXIT = 0x80000000
    kevent = FakeKevent

    def __init__(self, proc_error=None):
        self.changes, self.ready, self.proc_error = [], [], proc_error

    def kqueue(self):
        return self

    def control(self, changes, max_events, timeout=None):
        for change in changes or ():
            if change.filter == self.KQ_FILTER_PROC and self.proc_error:
                raise self.proc_error
            self.changes.append((change.ident, change.filter, change.flags, change.fflags))
        ready, self.ready = self.ready, []
        return ready[:max_events]


@pytest.fixture
def pair():
    a, b = socket.socketpair()
    yield a, b
    a.close()
    b.close()


def test_kqueue_watches_the_app_and_its_socket(pair):
    api = FakeKqueueApi()
    sock = pair[0]
    watch = transport.KqueueWatch(sock, os.getppid(), kq=api)
    assert watch.wait(0) == []
    assert (sock.fileno(), api.KQ_FILTER_READ, api.KQ_EV_ADD | api.KQ_EV_CLEAR, 0) in api.changes
    assert (os.getppid(), api.KQ_FILTER_PROC, api.KQ_EV_ADD | api.KQ_EV_ONESHOT, api.KQ_NOTE_EXIT) in api.changes
    api.ready = [FakeKevent(sock.fileno(), api.KQ_FILTER_READ, 0)]
    assert watch.wait(0) == [], "a request arriving is not the app's death"
    api.ready = [FakeKevent(sock.fileno(), api.KQ_FILTER_READ, api.KQ_EV_EOF)]
    assert watch.wait(0) == ["gone"]
    assert (sock.fileno(), api.KQ_FILTER_READ, api.KQ_EV_DELETE, 0) in api.changes, "reported once"
    api.ready = [FakeKevent(os.getppid(), api.KQ_FILTER_PROC, api.KQ_EV_EOF, api.KQ_NOTE_EXIT)]
    assert watch.wait(0) == ["gone"]


def test_kqueue_notices_an_app_that_is_already_gone(pair):
    assert transport.KqueueWatch(pair[0], os.getpid(), kq=FakeKqueueApi()).wait(0) == ["gone"], "re-parented"
    vanished = FakeKqueueApi(proc_error=ProcessLookupError(3, "No such process"))
    assert transport.KqueueWatch(pair[0], os.getppid(), kq=vanished).wait(0) == ["gone"]


def test_kqueue_watch_reads_the_signals(pair):
    api = FakeKqueueApi()
    watch = transport.KqueueWatch(pair[0], os.getppid(), kq=api)
    wake_r, wake_w = os.pipe()
    try:
        os.set_blocking(wake_r, False)
        watch._wake_r = wake_r                   # what install() sets up (it needs the main thread's signals)
        os.write(wake_w, bytes([signal.SIGUSR1, signal.SIGTERM, signal.SIGINT]))
        assert watch.wait(0) == []
        assert (wake_r, api.KQ_FILTER_READ, api.KQ_EV_ADD, 0) in api.changes
        api.ready = [FakeKevent(wake_r, api.KQ_FILTER_READ)]
        assert watch.wait(0) == ["freeze", "terminate", "release"]
    finally:
        os.close(wake_r)
        os.close(wake_w)


# ---- transport: Windows' TCP connections, in process ------------------------------------------------------------------

def test_control_watch_bytes_and_eof(pair):
    app, worker = pair
    watch = transport.ControlWatch(worker)
    assert watch.wait(0.01) == []
    app.sendall(b"I")
    assert watch.wait(1.0) == ["interrupt"]
    app.sendall(b"FxI")
    assert watch.wait(1.0) == ["freeze", "interrupt"], "unknown bytes are ignored"
    app.close()
    assert watch.wait(1.0) == ["gone"]
    started = time.monotonic()
    assert watch.wait(0.05) == [], "reported once, and it doesn't spin afterwards"
    assert time.monotonic() - started >= 0.04


def listen():
    server = socket.create_server(("127.0.0.1", 0))
    server.settimeout(5)
    return server, f"127.0.0.1:{server.getsockname()[1]}"


def read_line(conn):
    line = b""
    while not line.endswith(b"\n"):
        line += conn.recv(1)
    return line


def test_connect_tcp_opens_main_then_ctl_with_the_token():
    server, addr = listen()
    token = "0123456789abcdef0123456789abcdef"
    env = {"VYRN_ADDR": addr, "VYRN_TOKEN": token}
    try:
        link = transport.connect_tcp(env)
        first, second = server.accept()[0], server.accept()[0]
        assert read_line(first) == f"{token} main\n".encode()
        assert read_line(second) == f"{token} ctl\n".encode()
        assert link.kind == "tcp" and link.ctl is not None
        assert link.sock.getsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY)
        assert "VYRN_TOKEN" not in env, "programs the scripts start never see the token"
        first.sendall(b"VYRN")
        assert link.sock.recv(4) == b"VYRN", "after the line, the main connection carries the frames"
        assert isinstance(link.watch(), transport.ControlWatch)
        for sock in (first, second, link.sock, link.ctl):
            sock.close()
    finally:
        server.close()


@pytest.mark.parametrize("env", [{}, {"VYRN_ADDR": "127.0.0.1:1"}, {"VYRN_ADDR": "nowhere", "VYRN_TOKEN": "t"},
                                 {"VYRN_ADDR": "127.0.0.1:99999", "VYRN_TOKEN": "t"},
                                 {"VYRN_ADDR": "127.0.0.1:5", "VYRN_TOKEN": "has space"}])
def test_connect_tcp_refuses_what_the_app_never_sets(env):
    with pytest.raises(transport.ConnectError, match="vyrn_worker.py"):
        transport.connect_tcp(dict(env))


def closed_port():
    sock = socket.create_server(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


@pytest.mark.parametrize("env", [
    {"VYRN_TRANSPORT": "tcp", "VYRN_ADDR": "127.0.0.1:{port}", "VYRN_TOKEN": "0" * 32},
    {"VYRN_TRANSPORT": "tcp"},
    {},                                      # POSIX: no socket on fd 3
], ids=["refused", "no-address", "no-fd-3"])
def test_a_worker_that_cant_connect_exits_with_status_2(fakes, tmp_path, env):
    environment = fakes.env({"VYRN_FD": None, "VYRN_ADDR": None, "VYRN_TOKEN": None, "VYRN_TRANSPORT": None})
    environment.update({key: value.format(port=closed_port()) for key, value in env.items()})
    done = subprocess.run([sys.executable, "-u", "-X", "utf8", str(PYTHON_DIR / "vyrn_worker.py"), str(tmp_path)],
                          env=environment, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60)
    assert done.returncode == 2
    assert "started by the Vyrn app" in done.stderr or "couldn't connect to the Vyrn app" in done.stderr


def test_engine_accepts_only_the_token_line():
    import engine

    server, addr = listen()
    try:
        for line, role in ((b"secret main\n", "main"), (b"secret ctl\n", "ctl"), (b"wrong main\n", None),
                           (b"secret\n", None), (b"", None)):
            client = socket.create_connection(server.getsockname())
            client.sendall(line + b"VYRN")
            if not line:
                client.shutdown(socket.SHUT_WR)
            conn = server.accept()[0]
            assert engine.Worker._hello_line(conn, "secret", time.monotonic() + 2) == role
            if role:
                assert conn.recv(4) == b"VYRN", "nothing after the line is consumed"
            conn.close()
            client.close()
    finally:
        server.close()


# ---- transport: Windows' TCP connections, end to end ----------------------------------------------------------------

PLAYER = {
    "inputs.py": """
        import os

        def see():
            return [1.0 if os.environ.get("VYRN_TOKEN") else 0.0, 2.0]
        """,
    "responses.py": """
        left = "a"
        right = "shift+d"
        wait = None
        """,
    "points.py": """
        def points():
            return 1
        """,
}
LEFT, RIGHT, WAIT = range(3)
STUCK_POINTS = """
    import time

    def points():
        open({marker!r}, "w").close()
        while True: time.sleep(0.01)
    """


@pytest.fixture
def tcp_player(spawn, make_project):
    """A real-time worker on Windows' transport, allowed to press (recorded) keys, with a slow decision rate (the
    dead-man timer can't interfere)."""

    def start(files=None):
        rig = spawn(make_project(dict(PLAYER, **(files or {})), sim=False), transport="tcp")
        assert rig.worker.transport == "tcp" and rig.worker.ctl is not None
        rig.load(allow_input=True, decisions_per_second=0.05)
        return rig

    return start


def test_tcp_worker_plays_a_simulation(spawn, make_project):
    rig = spawn(make_project(), transport="tcp")
    assert rig.hello["pid"] == rig.pid and rig.hello["backends"]["screen"] == "grim"
    assert os.getpgid(rig.pid) != os.getpgid(0)
    assert rig.load(seed=3)["mode"] == "sim"
    assert rig.reset()["first"] is True
    assert rig.step(1)["t"] == "result"
    assert rig.worker.close() == 0


def test_tcp_worker_hides_the_token_from_the_scripts(tcp_player):
    rig = tcp_player()
    assert rig.observe(first=True)["obs"].tolist() == [0.0, 2.0]


def test_tcp_freeze_releases_keys_until_resume(tcp_player):
    rig = tcp_player()
    rig.act(LEFT)
    assert rig.keys.held() == {"KEY_A"}
    sent = time.monotonic()
    rig.worker.freeze()                      # "F" on the control connection
    events = rig.keys.wait_all_up(1.0)
    assert events[-1].t - sent < 1.0
    assert rig.status()["frozen"] is True
    mark = rig.keys.mark()
    assert rig.act(RIGHT)["t"] == "ok"
    time.sleep(0.1)
    assert rig.keys.actions(mark) == [], "skipped while frozen"
    rig.request("resume")
    rig.act(RIGHT)
    assert rig.keys.actions(mark) == [("down", "KEY_LEFTSHIFT"), ("down", "KEY_D")]


def test_tcp_interrupt_stops_stuck_code_at_its_line(tcp_player, tmp_path):
    marker = tmp_path / "stuck"
    rig = tcp_player({"points.py": STUCK_POINTS.format(marker=str(marker))})
    rig.observe(first=True)
    rig.act(LEFT)
    started = time.monotonic()
    # engine's timeout path sends "I", exactly like the Windows app; the watchdog interrupts the main thread
    error = rig.error("observe", timeout=1.0)
    assert marker.exists() and rig.worker.interrupted
    assert error["kind"] == "timeout" and error["func"] == "points"
    assert (error["script"], error["line"]) == ("points.py", rig.project.line("points.py", "while True"))
    assert time.monotonic() - started < 4.0
    assert rig.keys.held() == set()
    assert rig.status()["t"] == "ok", "the worker goes on serving requests"


def test_tcp_control_eof_means_the_app_died(tcp_player, tmp_path):
    marker = tmp_path / "stuck"
    rig = tcp_player({"points.py": STUCK_POINTS.format(marker=str(marker))})
    rig.observe(first=True)
    rig.act(LEFT)
    worker = rig.worker
    worker._send({"t": "observe", "id": 99, "first": False})     # points() loops forever, A held
    wait_until(marker.exists, 5.0, "points() to start looping")
    closed = time.monotonic()
    worker.ctl.close()                       # the main connection stays open: only the control EOF tells
    assert worker.wait(2.0) == 1
    events = rig.keys.events()
    assert rig.keys.held(events) == set() and events[-1].t - closed < 1.0


# ---- check.py ------------------------------------------------------------------------------------------------------------

def test_check_names_what_each_system_needs():
    import check

    line = check.backend_line
    assert line("Screen capture", "grim", None, "linux") == "Screen capture: grim"
    problem = screen_imagegrab.SCREEN_RECORDING_PROBLEM
    mac = line("Screen capture", None, problem, "macos")
    assert mac.startswith("Screen capture: needs the Screen Recording permission (System Settings → Privacy & "
                          "Security") and "terminal" in mac
    assert line("Keyboard", None, keys_quartz.ACCESSIBILITY_PROBLEM, "macos").startswith(
        "Keyboard: needs the Accessibility permission")
    linux = line("Keyboard", None, "/dev/uinput is not writable: run ./install.sh --keyboard", "linux")
    assert linux == "Keyboard: /dev/uinput is not writable: run ./install.sh --keyboard"
    assert line("Focus guard", None, None, "windows") == "Focus guard: not available"
