"""Windows focus: GetForegroundWindow through ctypes, polled at 20 Hz (docs/dev/platforms.md).

The id is the window handle (hwnd), the app_id the program's file name ("game.exe", from QueryFullProcessImageNameW)
and the title the window's text. A handle that IsWindow() no longer knows is reported closed.
"""

import ctypes
import ntpath

from .focus_base import PollSource, Window, app_pid, looks_like_vyrn

__all__ = ["NAME", "probe", "source", "Win32Query", "preload"]

NAME = "win32"
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
_MAX_PATH = 32768


def preload():
    pass


def load():
    """(user32, kernel32) with the prototypes this backend calls."""
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    hwnd = ctypes.c_void_p
    user32.GetForegroundWindow.argtypes = []
    user32.GetForegroundWindow.restype = hwnd
    user32.GetWindowTextLengthW.argtypes = [hwnd]
    user32.GetWindowTextLengthW.restype = ctypes.c_int
    user32.GetWindowTextW.argtypes = [hwnd, ctypes.c_wchar_p, ctypes.c_int]
    user32.GetWindowTextW.restype = ctypes.c_int
    user32.GetWindowThreadProcessId.argtypes = [hwnd, ctypes.POINTER(ctypes.c_uint32)]
    user32.GetWindowThreadProcessId.restype = ctypes.c_uint32
    user32.IsWindow.argtypes = [hwnd]
    user32.IsWindow.restype = ctypes.c_int
    kernel32.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
    kernel32.OpenProcess.restype = ctypes.c_void_p
    kernel32.QueryFullProcessImageNameW.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_wchar_p,
                                                    ctypes.POINTER(ctypes.c_uint32)]
    kernel32.QueryFullProcessImageNameW.restype = ctypes.c_int
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel32.CloseHandle.restype = ctypes.c_int
    return user32, kernel32


class Win32Query:
    """Which window is in front, asked of user32/kernel32 (replaceable with fakes in tests)."""

    def __init__(self, user32=None, kernel32=None):
        if user32 is None or kernel32 is None:
            user32, kernel32 = load()
        self._user32, self._kernel32 = user32, kernel32
        self._exe_names = {}                  # (hwnd, pid) -> program file name (a window never changes process)
        self._app = app_pid()

    def active(self):
        hwnd = self._user32.GetForegroundWindow()
        if not hwnd:
            return None
        hwnd = int(hwnd)
        length = max(0, int(self._user32.GetWindowTextLengthW(hwnd)))
        text = ctypes.create_unicode_buffer(length + 1)
        self._user32.GetWindowTextW(hwnd, text, length + 1)
        pid = ctypes.c_uint32(0)
        self._user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        exe = self._exe_name(hwnd, pid.value)
        return Window(hwnd, exe, text.value, pid.value, looks_like_vyrn(exe) or pid.value == self._app)

    def exists(self, hwnd):
        return bool(self._user32.IsWindow(hwnd))

    def close(self):
        pass

    def _exe_name(self, hwnd, pid):
        """"game.exe" for a window's process ("" when Windows won't say, e.g. for a program run as administrator)."""
        if not pid:
            return ""
        name = self._exe_names.get((hwnd, pid))
        if name is not None:
            return name
        name = ""
        handle = self._kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if handle:
            try:
                size = ctypes.c_uint32(_MAX_PATH)
                path = ctypes.create_unicode_buffer(_MAX_PATH)
                if self._kernel32.QueryFullProcessImageNameW(handle, 0, path, ctypes.byref(size)):
                    name = ntpath.basename(path.value[:size.value])
            finally:
                self._kernel32.CloseHandle(handle)
        if len(self._exe_names) > 256:
            self._exe_names.clear()
        self._exe_names[(hwnd, pid)] = name
        return name


def probe(helper_path=None, user32=None):
    """("win32", None) when Windows can tell Vyrn which window is in front, else (None, plain-English problem)."""
    try:
        (load()[0] if user32 is None else user32).GetForegroundWindow  # noqa: B018  (only looks it up)
    except (OSError, AttributeError) as err:
        return None, f"Vyrn can't use Windows' window functions ({err})."
    return NAME, None


def source(helper_path=None, query=None):
    """A PollSource over GetForegroundWindow (helper_path is for Linux only)."""
    from .._state import FocusError
    if query is None:
        try:
            query = Win32Query()
        except (OSError, AttributeError) as err:
            raise FocusError(f"Vyrn can't use Windows' window functions: {err}", "Start Vyrn again.") from None
    return PollSource(query, NAME, "Windows stopped telling Vyrn which window is in front",
                      "Start Vyrn again. If it keeps happening, restart Windows.")
