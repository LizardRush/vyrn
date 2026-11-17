"""Platform backends, chosen in this one place (docs/dev/platforms.md, python-internals.md section 13).

This is the only module that looks at sys.platform. The rest of Vyrn asks it for the backend of each concern:

    concern     Linux (reference)                macOS                                   Windows
    transport   socketpair on fd 3               fd 3                                    TCP 127.0.0.1 + token, 2 connections
    app gone    POLLRDHUP on fd 3                kqueue NOTE_EXIT on the app + socket EOF  EOF on the control connection
    freeze      SIGUSR1                          SIGUSR1                                 control byte "F"
    interrupt   SIGINT                           SIGINT                                  control byte "I"
    keys        uinput (python-evdev)            quartz (CGEventPost)                    win32 (SendInput, scan codes)
    screen      grim (Wayland), ImageGrab (X11)  PIL.ImageGrab, in points                PIL.ImageGrab
    focus       vyrn-focus helper                CGWindowList front window, 20 Hz        GetForegroundWindow, 20 Hz

Every backend module imports on every system: native libraries are loaded only when a backend is created or probed,
so the tests can run each backend on Linux with fakes (and import vyrn works everywhere).
"""

import os
import sys

if sys.platform == "win32":
    SYSTEM = "windows"
elif sys.platform == "darwin":
    SYSTEM = "macos"
else:
    SYSTEM = "linux"            # Linux and other POSIX systems

#: How the app and the worker are connected on this system: "fd" (POSIX) or "tcp" (Windows).
TRANSPORT = "tcp" if SYSTEM == "windows" else "fd"

__all__ = ["SYSTEM", "TRANSPORT", "transport_kind", "connect", "keys_module", "keys_probe", "make_keyboard",
           "screen_module", "screen_probe", "make_screen_capture", "focus_module", "focus_probe", "focus_source",
           "preload"]


# ----- transport ----------------------------------------------------------------------------------------------------

def transport_kind(environ=None):
    """"fd" or "tcp": TRANSPORT, or "tcp" when VYRN_TRANSPORT=tcp (tests only: the Windows transport anywhere)."""
    env = os.environ if environ is None else environ
    return "tcp" if env.get("VYRN_TRANSPORT", "").strip().lower() == "tcp" else TRANSPORT


def connect(environ=None):
    """The worker's transport.Link to the app. transport.ConnectError (with a plain message) when there is none."""
    from . import transport
    env = os.environ if environ is None else environ
    if transport_kind(env) == "tcp":
        return transport.connect_tcp(env)
    return transport.connect_fd(env, transport.KqueueWatch if SYSTEM == "macos" else transport.PollWatch)


# ----- keys -----------------------------------------------------------------------------------------------------------

def keys_module(system=None):
    """The module of this system's real keyboard backend (keys_uinput, keys_quartz or keys_win32)."""
    system = system or SYSTEM
    if system == "macos":
        from . import keys_quartz as module
    elif system == "windows":
        from . import keys_win32 as module
    else:
        from . import keys_uinput as module
    return module


def keys_probe():
    """(backend name, None) or (None, plain-English problem) for the real keyboard. Creates nothing."""
    return keys_module().probe()


def make_keyboard():
    """A new real keyboard backend (a vyrn._platform.keys_base.Backend). VyrnUserError when it can't be made."""
    return keys_module().Keyboard()


# ----- screen ---------------------------------------------------------------------------------------------------------

def screen_module(system=None, environ=None):
    """grim on Linux under Wayland, PIL.ImageGrab elsewhere (macOS, Windows, Linux X11). $VYRN_GRIM (tests: the fake
    grim) selects grim on every system, so a test can never capture the real screen."""
    system = system or SYSTEM
    env = os.environ if environ is None else environ
    if env.get("VYRN_GRIM") or (system == "linux" and (env.get("WAYLAND_DISPLAY") or not env.get("DISPLAY"))):
        from . import screen_grim as module
    else:
        from . import screen_imagegrab as module
    return module


def screen_probe():
    """(backend name, None) or (None, plain-English problem). Captures nothing."""
    return screen_module().probe(SYSTEM)


def make_screen_capture():
    """A capture backend: .capture(rect or None) -> uint8 [h, w, 3] in vyrn.screen() coordinates."""
    return screen_module().Capture(SYSTEM)


# ----- focus ----------------------------------------------------------------------------------------------------------

def focus_module(system=None, helper_path=None):
    """The module that tells which window is active: focus_helper, focus_mac or focus_win32. A helper program named
    in load's "focus_helper" (the app sends null; tests name their fake) is used on every system."""
    system = system or SYSTEM
    if helper_path:
        from . import focus_helper as module
    elif system == "macos":
        from . import focus_mac as module
    elif system == "windows":
        from . import focus_win32 as module
    else:
        from . import focus_helper as module
    return module


def focus_probe(helper_path=None):
    """(backend name, None) or (None, plain-English problem). Starts nothing."""
    return focus_module(helper_path=helper_path).probe(helper_path)


def focus_source(helper_path=None):
    """A started-on-demand source of focus lines for _focus.FocusWatcher (FocusError when it can't run here)."""
    return focus_module(helper_path=helper_path).source(helper_path)


# ----- start-up -------------------------------------------------------------------------------------------------------

def preload():
    """Import what this system's backends would import later (evdev, ctypes, PIL.ImageGrab and the standard modules
    they use) while the project folder is not on sys.path yet (python-internals.md section 1)."""
    from . import transport  # noqa: F401
    for module in (keys_module(), screen_module(), focus_module()):
        module.preload()
