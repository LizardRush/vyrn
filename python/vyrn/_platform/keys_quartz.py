"""macOS keys: Quartz events posted with CoreGraphics through ctypes (docs/dev/platforms.md section 2).

Each key is a CGEventCreateKeyboardEvent with its kVK_* virtual key code, posted at the HID event tap. The held
modifiers are set as the event's flags (like a real keyboard does), so "shift+d" types a capital D. macOS only
delivers these events when Vyrn has the Accessibility permission (AXIsProcessTrusted). Synthetic keys don't come up
by themselves when the worker dies: the app sends key-ups for every allowed key then (platform::release_all_keys).
"""

import ctypes

from .._state import VyrnUserError
from .keys_base import Backend

__all__ = ["NAME", "HINT", "KVK", "MODIFIER_FLAGS", "probe", "QuartzBackend", "Keyboard", "preload"]

NAME = "quartz"
CORE_GRAPHICS = "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics"
CORE_FOUNDATION = "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation"
APPLICATION_SERVICES = "/System/Library/Frameworks/ApplicationServices.framework/ApplicationServices"

K_CG_HID_EVENT_TAP = 0                 # kCGHIDEventTap
FLAG_SHIFT = 0x00020000                # kCGEventFlagMaskShift
FLAG_CONTROL = 0x00040000              # kCGEventFlagMaskControl

HINT = ("Allow Vyrn to control your computer: System Settings → Privacy & Security → Accessibility, turn on Vyrn "
        "(for check.py: your terminal app), then start it again.")
ACCESSIBILITY_PROBLEM = ("needs the Accessibility permission to press keys (System Settings → Privacy & Security → "
                         "Accessibility: turn on Vyrn, then start Vyrn again).")

#: evdev code name (vyrn._effects.ALLOWED_CODES) -> macOS virtual key code (kVK_*, HIToolbox/Events.h, ANSI layout).
KVK = {
    "KEY_A": 0x00, "KEY_S": 0x01, "KEY_D": 0x02, "KEY_F": 0x03, "KEY_H": 0x04, "KEY_G": 0x05, "KEY_Z": 0x06,
    "KEY_X": 0x07, "KEY_C": 0x08, "KEY_V": 0x09, "KEY_B": 0x0B, "KEY_Q": 0x0C, "KEY_W": 0x0D, "KEY_E": 0x0E,
    "KEY_R": 0x0F, "KEY_Y": 0x10, "KEY_T": 0x11, "KEY_O": 0x1F, "KEY_U": 0x20, "KEY_I": 0x22, "KEY_P": 0x23,
    "KEY_L": 0x25, "KEY_J": 0x26, "KEY_K": 0x28, "KEY_N": 0x2D, "KEY_M": 0x2E,
    "KEY_1": 0x12, "KEY_2": 0x13, "KEY_3": 0x14, "KEY_4": 0x15, "KEY_6": 0x16, "KEY_5": 0x17, "KEY_9": 0x19,
    "KEY_7": 0x1A, "KEY_8": 0x1C, "KEY_0": 0x1D,
    "KEY_F1": 0x7A, "KEY_F2": 0x78, "KEY_F3": 0x63, "KEY_F4": 0x76, "KEY_F5": 0x60, "KEY_F6": 0x61,
    "KEY_F7": 0x62, "KEY_F8": 0x64, "KEY_F9": 0x65, "KEY_F10": 0x6D, "KEY_F11": 0x67, "KEY_F12": 0x6F,
    "KEY_ENTER": 0x24, "KEY_TAB": 0x30, "KEY_SPACE": 0x31, "KEY_BACKSPACE": 0x33, "KEY_ESC": 0x35,
    "KEY_LEFTSHIFT": 0x38, "KEY_LEFTCTRL": 0x3B,
    "KEY_INSERT": 0x72,                # kVK_Help sits where PC keyboards have Insert
    "KEY_HOME": 0x73, "KEY_PAGEUP": 0x74, "KEY_DELETE": 0x75, "KEY_END": 0x77, "KEY_PAGEDOWN": 0x79,
    "KEY_LEFT": 0x7B, "KEY_RIGHT": 0x7C, "KEY_DOWN": 0x7D, "KEY_UP": 0x7E,
    "KEY_EQUAL": 0x18, "KEY_MINUS": 0x1B, "KEY_RIGHTBRACE": 0x1E, "KEY_LEFTBRACE": 0x21, "KEY_APOSTROPHE": 0x27,
    "KEY_SEMICOLON": 0x29, "KEY_BACKSLASH": 0x2A, "KEY_COMMA": 0x2B, "KEY_SLASH": 0x2C, "KEY_DOT": 0x2F,
    "KEY_GRAVE": 0x32,
}

#: Modifier keys and the event flag each one sets while it is down.
MODIFIER_FLAGS = {"KEY_LEFTSHIFT": FLAG_SHIFT, "KEY_LEFTCTRL": FLAG_CONTROL}


def preload():
    import ctypes.util  # noqa: F401


def load():
    """(CoreGraphics, CoreFoundation) with the prototypes this backend calls."""
    cg = ctypes.CDLL(CORE_GRAPHICS)
    cf = ctypes.CDLL(CORE_FOUNDATION)
    cg.CGEventCreateKeyboardEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_bool]
    cg.CGEventCreateKeyboardEvent.restype = ctypes.c_void_p
    cg.CGEventSetFlags.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
    cg.CGEventSetFlags.restype = None
    cg.CGEventPost.argtypes = [ctypes.c_uint32, ctypes.c_void_p]
    cg.CGEventPost.restype = None
    cf.CFRelease.argtypes = [ctypes.c_void_p]
    cf.CFRelease.restype = None
    return cg, cf


def load_accessibility():
    """ApplicationServices, for AXIsProcessTrusted()."""
    ax = ctypes.CDLL(APPLICATION_SERVICES)
    ax.AXIsProcessTrusted.argtypes = []
    ax.AXIsProcessTrusted.restype = ctypes.c_bool
    return ax


def trusted(ax=None):
    """True when macOS lets this process (in fact the app that started it) post key events."""
    ax = load_accessibility() if ax is None else ax
    return bool(ax.AXIsProcessTrusted())


def probe(ax=None):
    """("quartz", None) when Vyrn may press keys, else (None, plain-English problem). Presses nothing."""
    try:
        allowed = trusted(ax)
    except (OSError, AttributeError) as err:
        return None, f"Vyrn can't use macOS's keyboard functions ({err})."
    if not allowed:
        return None, ACCESSIBILITY_PROBLEM
    return NAME, None


class QuartzBackend(Backend):
    """Posts kVK key events; only the allowed keys have codes, so nothing else can be pressed."""

    name = NAME
    hint = HINT

    def __init__(self, cg=None, cf=None, ax=None):
        super().__init__()
        try:
            if not trusted(ax):
                raise VyrnUserError("input", "macOS doesn't let Vyrn press keys yet.", HINT)
            if cg is None:
                cg, cf = load()
        except (OSError, AttributeError) as err:
            raise VyrnUserError("input", f"Vyrn can't use macOS's keyboard functions: {err}", HINT) from None
        self._cg, self._cf = cg, cf
        self._flags = 0                    # the modifier flags of the keys held right now

    def _emit(self, code, value):
        key = KVK[code]
        flag = MODIFIER_FLAGS.get(code, 0)
        if value:
            self._flags |= flag            # a modifier's own key-down already carries its flag
        else:
            self._flags &= ~flag           # ... and its key-up no longer does
        event = self._cg.CGEventCreateKeyboardEvent(None, key, bool(value))
        if not event:
            raise OSError("macOS couldn't make a key event (CGEventCreateKeyboardEvent)")
        try:
            self._cg.CGEventSetFlags(event, self._flags)
            self._cg.CGEventPost(K_CG_HID_EVENT_TAP, event)
        finally:
            self._cf.CFRelease(event)


Keyboard = QuartzBackend
