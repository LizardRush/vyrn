"""Linux keys: a virtual keyboard via /dev/uinput (python-evdev) that declares only the allowed keys.

The kernel destroys the device, and with it every held key, when the worker dies, even by SIGKILL. Its code table
is evdev's own: the allow-list (vyrn._effects.ALLOWED_CODES) already names evdev codes.
"""

import os

from .._effects import ALLOWED_CODES
from .._state import VyrnUserError
from .keys_base import Backend

__all__ = ["NAME", "HINT", "UINPUT_PATH", "probe", "UInputBackend", "Keyboard", "preload"]

NAME = "uinput"
DEVICE_NAME = "Vyrn AI keyboard"
SETTLE_SECONDS = 0.2            # give the compositor time to notice a new keyboard
UINPUT_PATH = "/dev/uinput"
HINT = "Run ./install.sh --keyboard once (it needs your password), then log out and back in."


def _import_evdev():
    """The evdev module, or None when python3-evdev is not installed."""
    try:
        import evdev
    except ImportError:
        return None
    return evdev


def preload():
    _import_evdev()


def probe():
    """("uinput", None) when Vyrn could create its virtual keyboard, else (None, plain-English problem).

    Only looks: it never creates a device.
    """
    if _import_evdev() is None:
        return None, ("The Python package evdev is missing, so Vyrn can't press keys: "
                      "run ./install.sh --deps (it installs python3-evdev).")
    if not os.path.exists(UINPUT_PATH):
        return None, f"{UINPUT_PATH} is missing (the uinput kernel module isn't loaded): run ./install.sh --keyboard"
    if not os.access(UINPUT_PATH, os.W_OK):
        return None, f"{UINPUT_PATH} is not writable: run ./install.sh --keyboard"
    return NAME, None


class UInputBackend(Backend):
    """A real virtual keyboard via /dev/uinput that declares only the allowed keys."""

    name = NAME
    settle_seconds = SETTLE_SECONDS
    hint = HINT

    def __init__(self):
        super().__init__()
        evdev = _import_evdev()
        if evdev is None:
            raise VyrnUserError("input", "Vyrn can't press keys: the Python package evdev is missing.",
                                "Run ./install.sh --deps (it installs python3-evdev).")
        ecodes = evdev.ecodes
        self._ev_key = ecodes.EV_KEY
        self._codes = {name: ecodes.ecodes[name] for name in sorted(ALLOWED_CODES)}
        try:
            self._ui = evdev.UInput(events={ecodes.EV_KEY: sorted(self._codes.values())}, name=DEVICE_NAME)
        except Exception as err:               # UInputError / PermissionError / OSError
            raise VyrnUserError("input", f"Vyrn couldn't create its virtual keyboard: {err}", HINT) from None
        try:
            _keep_private(self._ui)
        except OSError as err:
            self._ui.close()
            raise VyrnUserError("input", f"Vyrn couldn't set up its virtual keyboard safely: {err}", HINT) from None

    def _emit(self, code, value):
        self._ui.write(self._ev_key, self._codes[code], value)
        self._ui.syn()

    def _close(self):
        self._ui.close()


Keyboard = UInputBackend


def _keep_private(ui):
    """Make the virtual keyboard's file descriptors close-on-exec.

    python-evdev opens /dev/uinput without O_CLOEXEC, so a program the user's scripts start (os.system,
    os.posix_spawn, ...) would inherit it. A SIGKILL of the worker would then not destroy the device: the
    kernel sends no key-ups, and a held key stays down until that program ends.
    """
    device = getattr(ui, "device", None)
    for fd in (getattr(ui, "fd", None), getattr(device, "fd", None)):
        if isinstance(fd, int) and fd >= 0:
            os.set_inheritable(fd, False)
