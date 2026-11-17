"""Linux focus: the vyrn-focus helper (wlr-foreign-toplevel), protocol.md section 6.

Its stdin is a pipe that we keep open and never write to: when this process dies the pipe closes and the helper
exits by itself. A daemon thread reads its lines and hands them to the FocusWatcher.
"""

import os
import shutil
import subprocess
import threading

from .._state import FocusError
from .focus_base import looks_like_vyrn

__all__ = ["NAME", "probe", "source", "HelperSource", "preload"]

NAME = "vyrn-focus"
HELPER_NAME = NAME
_INSTALL_HINT = "Run ./install.sh --app to build and install Vyrn's helper programs."


def preload():
    pass


def _find_helper(helper_path):
    if helper_path:
        return shutil.which(helper_path)
    candidates = [shutil.which(HELPER_NAME)]
    data = os.environ.get("VYRN_DATA")
    if data:
        candidates.append(os.path.join(data, "..", "..", "bin", HELPER_NAME))   # <prefix>/share/vyrn
    candidates.append(os.path.expanduser(f"~/.local/bin/{HELPER_NAME}"))
    for path in candidates:
        if path and os.path.isfile(path) and os.access(path, os.X_OK):
            return os.path.normpath(path)
    return None


def probe(helper_path=None):
    """("vyrn-focus", None) when the focus helper can run here, else (None, plain-English problem).

    Only looks: it never starts the helper.
    """
    if not os.environ.get("WAYLAND_DISPLAY"):
        return None, ("Vyrn can't find the desktop (WAYLAND_DISPLAY is not set): "
                      "the focus guard needs the Raspberry Pi desktop.")
    if _find_helper(helper_path) is None:
        return None, f"The focus helper {helper_path or HELPER_NAME} is missing: run ./install.sh --app"
    return NAME, None


def source(helper_path=None):
    return HelperSource(helper_path)


class HelperSource:
    """Runs the helper and passes its lines on."""

    name = NAME
    error_text = "The focus helper can't watch windows"
    error_hint = "Vyrn's focus guard needs the labwc desktop of Raspberry Pi OS."
    ended_text = "The focus helper stopped unexpectedly."
    install_hint = _INSTALL_HINT
    late_text = "The focus helper didn't answer in time."
    late_hint = "Try again. If it keeps happening, run ./install.sh --app to rebuild it."

    def __init__(self, helper_path):
        self._path = _find_helper(helper_path)      # None/"" -> look in the usual install places
        if self._path is None:
            raise FocusError(f"Vyrn's focus helper ({helper_path or HELPER_NAME}) isn't installed, so Vyrn "
                             "can't tell whether your game window is in front.", _INSTALL_HINT)
        self._proc = None
        self._reader = None

    def start(self, on_line, on_end):
        try:
            self._proc = subprocess.Popen([self._path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                          close_fds=True)
        except OSError as err:
            raise FocusError(f"Vyrn couldn't start its focus helper {self._path}: {err.strerror or err}.",
                             _INSTALL_HINT) from None
        self._reader = threading.Thread(target=self._read, args=(on_line, on_end), name="vyrn-focus-reader",
                                        daemon=True)
        self._reader.start()

    def refresh(self):
        """The helper reports every change by itself."""

    def is_vyrn(self, window):
        return looks_like_vyrn(window.get("app_id"))

    def stop(self):
        """Stop the helper (closing its stdin, then SIGTERM) and wait for it briefly. Safe to repeat."""
        proc = self._proc
        if proc is None:
            return
        try:
            proc.stdin.close()
        except OSError:
            pass
        if proc.poll() is None:
            try:
                proc.terminate()
                proc.wait(timeout=1.0)
            except subprocess.TimeoutExpired:
                proc.kill()
                try:
                    proc.wait(timeout=1.0)
                except subprocess.TimeoutExpired:
                    pass
            except OSError:
                pass
        self._reader.join(timeout=1.0)
        if not self._reader.is_alive():
            proc.stdout.close()

    def _read(self, on_line, on_end):
        try:
            for raw in self._proc.stdout:
                on_line(raw.decode("utf-8", "replace").rstrip("\r\n"))
        except (OSError, ValueError):
            pass                      # pipe closed under us by stop()
        finally:
            on_end()
