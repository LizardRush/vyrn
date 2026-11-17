"""Windows keys: SendInput with set-1 scan codes through ctypes (docs/dev/platforms.md section 2).

Scan codes (KEYEVENTF_SCANCODE) rather than virtual-key codes, so games that read DirectInput or raw input see the
keys too; the arrows and the Insert/Delete/Home/End/Page block also need KEYEVENTF_EXTENDEDKEY (their E0 prefix).
Synthetic keys don't come up by themselves when the worker dies: the app sends key-ups for every allowed key then.
"""

import ctypes

from .._state import VyrnUserError
from .keys_base import Backend

__all__ = ["NAME", "HINT", "SCAN", "EXTENDED", "INPUT", "KEYBDINPUT", "probe", "SendInputBackend", "Keyboard",
           "preload"]

NAME = "win32"
HINT = ("Windows only lets Vyrn press keys in programs that don't run as administrator (unless Vyrn does too). "
        "Start Vyrn again if this keeps happening.")

INPUT_KEYBOARD = 1
KEYEVENTF_EXTENDEDKEY = 0x0001
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_SCANCODE = 0x0008

#: evdev code name (vyrn._effects.ALLOWED_CODES) -> scan code set 1 (without the E0 prefix of EXTENDED keys).
SCAN = {
    "KEY_ESC": 0x01, "KEY_1": 0x02, "KEY_2": 0x03, "KEY_3": 0x04, "KEY_4": 0x05, "KEY_5": 0x06, "KEY_6": 0x07,
    "KEY_7": 0x08, "KEY_8": 0x09, "KEY_9": 0x0A, "KEY_0": 0x0B, "KEY_MINUS": 0x0C, "KEY_EQUAL": 0x0D,
    "KEY_BACKSPACE": 0x0E, "KEY_TAB": 0x0F,
    "KEY_Q": 0x10, "KEY_W": 0x11, "KEY_E": 0x12, "KEY_R": 0x13, "KEY_T": 0x14, "KEY_Y": 0x15, "KEY_U": 0x16,
    "KEY_I": 0x17, "KEY_O": 0x18, "KEY_P": 0x19, "KEY_LEFTBRACE": 0x1A, "KEY_RIGHTBRACE": 0x1B, "KEY_ENTER": 0x1C,
    "KEY_LEFTCTRL": 0x1D,
    "KEY_A": 0x1E, "KEY_S": 0x1F, "KEY_D": 0x20, "KEY_F": 0x21, "KEY_G": 0x22, "KEY_H": 0x23, "KEY_J": 0x24,
    "KEY_K": 0x25, "KEY_L": 0x26, "KEY_SEMICOLON": 0x27, "KEY_APOSTROPHE": 0x28, "KEY_GRAVE": 0x29,
    "KEY_LEFTSHIFT": 0x2A, "KEY_BACKSLASH": 0x2B,
    "KEY_Z": 0x2C, "KEY_X": 0x2D, "KEY_C": 0x2E, "KEY_V": 0x2F, "KEY_B": 0x30, "KEY_N": 0x31, "KEY_M": 0x32,
    "KEY_COMMA": 0x33, "KEY_DOT": 0x34, "KEY_SLASH": 0x35, "KEY_SPACE": 0x39,
    "KEY_F1": 0x3B, "KEY_F2": 0x3C, "KEY_F3": 0x3D, "KEY_F4": 0x3E, "KEY_F5": 0x3F, "KEY_F6": 0x40,
    "KEY_F7": 0x41, "KEY_F8": 0x42, "KEY_F9": 0x43, "KEY_F10": 0x44, "KEY_F11": 0x57, "KEY_F12": 0x58,
    "KEY_HOME": 0x47, "KEY_UP": 0x48, "KEY_PAGEUP": 0x49, "KEY_LEFT": 0x4B, "KEY_RIGHT": 0x4D, "KEY_END": 0x4F,
    "KEY_DOWN": 0x50, "KEY_PAGEDOWN": 0x51, "KEY_INSERT": 0x52, "KEY_DELETE": 0x53,
}

#: Keys whose scan code has the E0 prefix: sent with KEYEVENTF_EXTENDEDKEY (else they'd be the numeric keypad's).
EXTENDED = frozenset({"KEY_HOME", "KEY_UP", "KEY_PAGEUP", "KEY_LEFT", "KEY_RIGHT", "KEY_END", "KEY_DOWN",
                      "KEY_PAGEDOWN", "KEY_INSERT", "KEY_DELETE"})

# Fixed-size fields: Windows' LONG and DWORD are 32-bit on every Windows (unlike C long on Linux/macOS).
_ULONG_PTR = ctypes.c_size_t


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", ctypes.c_int32), ("dy", ctypes.c_int32), ("mouseData", ctypes.c_uint32),
                ("dwFlags", ctypes.c_uint32), ("time", ctypes.c_uint32), ("dwExtraInfo", _ULONG_PTR)]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", ctypes.c_uint16), ("wScan", ctypes.c_uint16), ("dwFlags", ctypes.c_uint32),
                ("time", ctypes.c_uint32), ("dwExtraInfo", _ULONG_PTR)]


class HARDWAREINPUT(ctypes.Structure):
    _fields_ = [("uMsg", ctypes.c_uint32), ("wParamL", ctypes.c_uint16), ("wParamH", ctypes.c_uint16)]


class _INPUTUNION(ctypes.Union):
    # MOUSEINPUT is the biggest member: without it sizeof(INPUT) is wrong and SendInput rejects every call.
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT), ("hi", HARDWAREINPUT)]


class INPUT(ctypes.Structure):
    _anonymous_ = ("u",)
    _fields_ = [("type", ctypes.c_uint32), ("u", _INPUTUNION)]


def preload():
    pass


def load():
    """user32 with the SendInput prototype."""
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    user32.SendInput.argtypes = [ctypes.c_uint, ctypes.POINTER(INPUT), ctypes.c_int]
    user32.SendInput.restype = ctypes.c_uint
    return user32


def probe(user32=None):
    """("win32", None) when SendInput is there, else (None, plain-English problem). Presses nothing."""
    try:
        (load() if user32 is None else user32).SendInput  # noqa: B018  (only looks it up)
    except (OSError, AttributeError) as err:
        return None, f"Vyrn can't use Windows' keyboard functions ({err})."
    return NAME, None


def key_input(code, down):
    """The INPUT record for one key-down or key-up of an allowed key."""
    flags = KEYEVENTF_SCANCODE | (KEYEVENTF_EXTENDEDKEY if code in EXTENDED else 0) | (0 if down else KEYEVENTF_KEYUP)
    record = INPUT(type=INPUT_KEYBOARD)
    record.ki = KEYBDINPUT(wVk=0, wScan=SCAN[code], dwFlags=flags, time=0, dwExtraInfo=0)
    return record


class SendInputBackend(Backend):
    """One SendInput call per key event; only the allowed keys have scan codes, so nothing else can be pressed."""

    name = NAME
    hint = HINT

    def __init__(self, user32=None):
        super().__init__()
        if user32 is None:
            try:
                user32 = load()
            except (OSError, AttributeError) as err:
                raise VyrnUserError("input", f"Vyrn can't use Windows' keyboard functions: {err}", HINT) from None
        self._user32 = user32

    def _emit(self, code, value):
        records = (INPUT * 1)(key_input(code, bool(value)))
        if self._user32.SendInput(1, records, ctypes.sizeof(INPUT)) != 1:
            error = getattr(ctypes, "get_last_error", lambda: 0)()
            raise OSError(f"Windows didn't take the key (SendInput failed, error {error})")


Keyboard = SendInputBackend
