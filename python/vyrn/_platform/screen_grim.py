"""Linux (Wayland) screen capture: one `grim -g "x,y wxh" -t ppm -` run per capture (python-internals.md section 9).

Coordinates are the screen's own pixels; the display scale must be 100%. $VYRN_GRIM replaces only the program
(the tests' fake grim), never the need for a desktop.
"""

import os
import re
import shutil
import subprocess

import numpy as np

from .._state import VyrnUserError

__all__ = ["NAME", "GRIM_TIMEOUT", "probe", "parse_ppm", "GrimCapture", "Capture", "preload"]

NAME = "grim"
GRIM_TIMEOUT = 10.0            # seconds; the app's own timeout normally fires first

_SEP = rb"(?:\s|#[^\n]*\n)+"   # whitespace and comments between PPM header fields
_PPM_HEADER = re.compile(rb"P6" + _SEP + rb"(\d+)" + _SEP + rb"(\d+)" + _SEP + rb"(\d+)\s")

HINT = ("Vyrn reads the screen with grim, which needs the Raspberry Pi desktop (Wayland). "
        "Run Vyrn on the desktop, not over SSH.")


def preload():
    pass


def probe(system=None):
    """("grim", None) when the screen can be read, else (None, plain-English problem).

    Needs the desktop (WAYLAND_DISPLAY) and the grim program ($VYRN_GRIM replaces only the program).
    Runs nothing: it only looks.
    """
    if not os.environ.get("WAYLAND_DISPLAY"):
        return None, ("Vyrn can't find the desktop (WAYLAND_DISPLAY is not set): "
                      "reading the screen needs the Raspberry Pi desktop.")
    fake = os.environ.get("VYRN_GRIM")
    if fake:                   # test override for the grim program
        if shutil.which(fake):
            return NAME, None
        return None, f"VYRN_GRIM={fake} is not a program Vyrn can run."
    if shutil.which("grim") is None:
        return None, "grim is not installed, so Vyrn can't read the screen: run ./install.sh --deps"
    return NAME, None


def parse_ppm(data):
    """Decode a binary PPM (P6, maxval 255) into a read-only uint8 array [height, width, 3]."""
    if isinstance(data, memoryview):
        data = data.tobytes()
    header = _PPM_HEADER.match(data, 0, min(len(data), 1024))
    if header is None:
        raise ValueError("damaged PPM header" if data[:2] == b"P6" else "not a binary PPM (P6) picture")
    width, height, maxval = (int(v) for v in header.groups())
    if maxval != 255:
        raise ValueError(f"PPM maximum value {maxval} is not supported (expected 255)")
    if width < 1 or height < 1:
        raise ValueError(f"PPM size {width}x{height} is empty")
    size = width * height * 3
    if len(data) - header.end() < size:
        raise ValueError(f"PPM pixels are cut short ({len(data) - header.end()} of {size} bytes)")
    return np.frombuffer(data, dtype=np.uint8, count=size, offset=header.end()).reshape(height, width, 3)


class GrimCapture:
    """Runs grim (or $VYRN_GRIM) for one rectangle (x0, y0, x1, y1), or the whole screen for None."""

    name = NAME
    hint = HINT

    def __init__(self, system=None):
        self._grim = os.environ.get("VYRN_GRIM") or "grim"

    def capture(self, rect):
        cmd = [self._grim]
        if rect is not None:
            x0, y0, x1, y1 = rect
            cmd += ["-g", f"{x0},{y0} {x1 - x0}x{y1 - y0}"]
        cmd += ["-t", "ppm", "-"]
        try:
            done = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, timeout=GRIM_TIMEOUT)
        except FileNotFoundError:
            raise VyrnUserError("exception", f"Vyrn can't read the screen: the program {self._grim} is not installed.",
                                "Run ./install.sh --deps (it installs grim).") from None
        except subprocess.TimeoutExpired:
            raise VyrnUserError("exception", f"Reading the screen took too long: grim didn't answer "
                                f"within {GRIM_TIMEOUT:g} seconds.", HINT) from None
        except OSError as err:
            raise VyrnUserError("exception", f"Vyrn couldn't run {self._grim} to read the screen: "
                                f"{err.strerror or err}.", HINT) from None
        if done.returncode != 0:
            said = _last_line(done.stderr) or f"it stopped with exit status {done.returncode}"
            raise VyrnUserError("exception", f"Vyrn couldn't read the screen (grim said: {said}).", HINT)
        try:
            image = parse_ppm(done.stdout)
        except ValueError as err:
            raise VyrnUserError("exception", f"Vyrn couldn't read the screen: grim sent a picture it "
                                f"doesn't understand ({err}).", HINT) from None
        if rect is not None and image.shape[:2] != (rect[3] - rect[1], rect[2] - rect[0]):
            raise VyrnUserError(
                "exception", f"grim sent a {image.shape[1]}x{image.shape[0]} picture for a "
                f"{rect[2] - rect[0]}x{rect[3] - rect[1]} area of the screen.",
                "Vyrn needs the display scale set to 100% (Screen Configuration).")
        return image


Capture = GrimCapture


def _last_line(raw):
    lines = raw.decode("utf-8", "replace").strip().splitlines()
    return lines[-1].strip() if lines else ""
