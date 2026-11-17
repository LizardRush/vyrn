"""Screen capture with Pillow's ImageGrab: macOS, Windows and Linux under X11 (docs/dev/platforms.md).

Coordinates, the same for vyrn.screen(), vyrn.pixel(), vyrn.on_screen() and the app's Pick area / Snapshot:
- Windows and Linux X11: the main screen's own pixels (Pillow makes its Windows capture DPI-aware).
- macOS: points, the logical coordinates of macOS, Qt and screencapture. A Retina screen has 2x2 pixels per point,
  and ImageGrab returns pixels: every capture is scaled down to one pixel per point (a box filter), so an area of
  100x50 points is a 100x50 picture on any screen. The scale is the whole-screen capture's width over the main
  display's width in points (CGDisplayBounds); an area capture is simply scaled to the area's size.
"""

import ctypes
import os

import numpy as np

from .._state import VyrnUserError

__all__ = ["NAME", "probe", "screen_capture_allowed", "main_display_points", "scale_to", "ImageGrabCapture",
           "Capture", "preload"]

NAME = "imagegrab"
CORE_GRAPHICS = "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics"

SCREEN_RECORDING_PROBLEM = ("needs the Screen Recording permission (System Settings → Privacy & Security → "
                            "Screen Recording: turn on Vyrn, then start Vyrn again).")
_HINTS = {
    "macos": ("macOS only shows Vyrn the screen with the Screen Recording permission: System Settings → Privacy & "
              "Security → Screen Recording, turn on Vyrn (for check.py: your terminal app)."),
    "windows": "Vyrn reads the main screen with Pillow's ImageGrab: put your game on the main screen.",
    "linux": "Vyrn reads the X11 screen ($DISPLAY) with Pillow's ImageGrab. Run Vyrn on the desktop, not over SSH.",
}


def _image_grab():
    from PIL import ImageGrab
    return ImageGrab


def preload():
    try:
        _image_grab()                      # it imports subprocess, tempfile, shutil...
    except ImportError:
        pass


def screen_capture_allowed(cg=None):
    """macOS: True/False from CGPreflightScreenCaptureAccess (asks nothing), None when macOS can't tell."""
    try:
        cg = ctypes.CDLL(CORE_GRAPHICS) if cg is None else cg
        preflight = cg.CGPreflightScreenCaptureAccess
    except (OSError, AttributeError):
        return None                        # before macOS 10.15 there was no such permission
    preflight.argtypes = []
    preflight.restype = ctypes.c_bool
    return bool(preflight())


class _CGPoint(ctypes.Structure):
    _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double)]


class _CGSize(ctypes.Structure):
    _fields_ = [("width", ctypes.c_double), ("height", ctypes.c_double)]


class _CGRect(ctypes.Structure):
    _fields_ = [("origin", _CGPoint), ("size", _CGSize)]


def main_display_points(cg=None):
    """macOS: (width, height) of the main display in points."""
    cg = ctypes.CDLL(CORE_GRAPHICS) if cg is None else cg
    cg.CGMainDisplayID.argtypes = []
    cg.CGMainDisplayID.restype = ctypes.c_uint32
    cg.CGDisplayBounds.argtypes = [ctypes.c_uint32]
    cg.CGDisplayBounds.restype = _CGRect
    bounds = cg.CGDisplayBounds(cg.CGMainDisplayID())
    return round(bounds.size.width), round(bounds.size.height)


def scale_to(image, size):
    """A PIL image scaled down to `size` by averaging: exactly each n x n block for a whole factor n (a Retina
    screen's 2x2 pixels per point), else a box filter."""
    size = (int(size[0]), int(size[1]))
    if tuple(image.size) == size:
        return image
    factor = image.size[0] // size[0] if size[0] else 0
    if factor > 1 and tuple(image.size) == (size[0] * factor, size[1] * factor):
        return image.reduce(factor)
    from PIL import Image
    return image.resize(size, getattr(Image, "Resampling", Image).BOX)


def probe(system, environ=None, image_grab=None, screen_access=None):
    """("imagegrab", None) when the screen can be read, else (None, plain-English problem). Captures nothing."""
    env = os.environ if environ is None else environ
    if system == "linux" and not env.get("DISPLAY"):
        return None, ("Vyrn can't find the desktop (neither WAYLAND_DISPLAY nor DISPLAY is set): "
                      "reading the screen needs a desktop session.")
    try:
        image_grab = _image_grab() if image_grab is None else image_grab
    except ImportError as err:
        return None, f"Pillow's ImageGrab is missing, so Vyrn can't read the screen ({err})."
    if system == "linux":
        from PIL import features
        if not features.check("xcb"):
            return None, "This Pillow can't read an X11 screen (it was built without XCB support)."
    if system == "macos" and (screen_capture_allowed if screen_access is None else screen_access)() is False:
        return None, SCREEN_RECORDING_PROBLEM
    return NAME, None


class ImageGrabCapture:
    """One ImageGrab.grab() per capture of a rectangle (x0, y0, x1, y1), or of the whole main screen for None."""

    name = NAME

    def __init__(self, system, grab=None, points=None):
        self._system = system
        self._grab = grab                  # stands in for ImageGrab.grab (tests)
        self._points = points              # stands in for main_display_points (tests)
        self.hint = _HINTS.get(system, _HINTS["linux"])
        self.scale = None                  # macOS: pixels per point, from the latest whole-screen capture

    def capture(self, rect):
        grab = _image_grab().grab if self._grab is None else self._grab
        try:
            image = grab() if rect is None else grab(bbox=tuple(int(v) for v in rect))
            if rect is None:
                size = self._screen_size(tuple(image.size))
            else:
                size = (rect[2] - rect[0], rect[3] - rect[1])
            image = scale_to(image.convert("RGB"), size)   # HiDPI: pixels -> points
            return np.asarray(image, dtype=np.uint8)
        except VyrnUserError:
            raise
        except Exception as err:           # screencapture failed, no X server, ...
            raise VyrnUserError("exception", f"Vyrn couldn't read the screen ({err}).", self.hint) from None

    def _screen_size(self, pixels):
        """The whole screen's size in vyrn.screen() coordinates: points on macOS, else the picture's own size."""
        if self._system != "macos":
            return pixels
        try:
            points = (main_display_points if self._points is None else self._points)()
        except (OSError, AttributeError, ValueError):
            points = None
        if not points or points[0] <= 0 or points[1] <= 0:
            return pixels
        self.scale = pixels[0] / points[0]
        return int(points[0]), int(points[1])


Capture = ImageGrabCapture
