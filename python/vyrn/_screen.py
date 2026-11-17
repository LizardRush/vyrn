"""Reading the screen (python-internals.md section 9).

At most one capture per request generation: the capture covers the union of every area the scripts have asked
for so far (it persists across ticks), and vyrn.screen(), vyrn.pixel() and vyrn.on_screen() are cropped from it.
Only a tick that asks for a new area outside the union pays for a second capture.

The capture itself is a backend from vyrn._platform: grim on Linux under Wayland, Pillow's ImageGrab on macOS,
Windows and Linux X11. Coordinates are the screen's pixels, except on macOS: points there (a Retina screen's 2x2
pixels are one point), the same coordinates the app's Pick area writes.
"""

import operator
import os

import numpy as np

from vyrn import _platform
from vyrn._platform.screen_grim import parse_ppm
from vyrn._platform.screen_imagegrab import scale_to
from vyrn._state import S, SimOnlyError, VyrnUserError, safe_repr

__all__ = ["probe", "parse_ppm", "Screen"]

_SIM_HINT = ("This project has a world.py, so it is a simulation that never looks at the screen. "
             "Draw what the AI sees with vyrn.canvas() instead.")
_SIM_READ_HINT = ("This project has a world.py, so it is a simulation that never looks at the screen. "
                  "Read the world instead, like world.fruit_y.")
_PICK_HINT = '"Pick area" in the toolbar chooses an area on the screen for you.'
#: The start of the error a run gets when it may not read the screen (the MCP server looks for it).
NO_SCREEN = "can't read the screen in this run"
_NO_SCREEN_HINT = ("An AI coding agent started this Check, and the person hasn't let agents see the screen for this "
                   "project. They can allow it in the Vyrn app: Settings → AI coding agents.")


def probe():
    """(backend name, None) when the screen can be read ("grim" or "imagegrab"), else (None, plain-English problem).

    Runs nothing: it only looks.
    """
    return _platform.screen_probe()


class Screen:
    """The screen as the scripts see it. Rectangles are (x0, y0, x1, y1), end-exclusive."""

    def __init__(self, state=None, backend=None):
        self._state = S if state is None else state
        self._backend = backend    # the capture backend (vyrn._platform), made on the first capture
        self._size = None          # (width, height) of the whole screen, from the first full capture
        self._union = None         # every area asked for so far
        self._shot = None          # (generation, rect, image) of the latest capture
        self._pictures = {}        # snapshot path -> ((mtime_ns, size), int16 image, area or None)
        self._missing = set()      # snapshot paths already reported missing
        self._moved = set()        # (snapshot path, x, y) already reported as looked for away from its Snapshot
        self.captures = 0          # captures so far (handy for timings and tests)

    # ----- public -----

    def grab(self, x=None, y=None, width=None, height=None):
        """vyrn.screen(): a copy of that area as uint8 [height, width, 3]; no arguments = whole screen."""
        self._real_only("vyrn.screen()")
        given = {"x": x, "y": y, "width": width, "height": height}
        if all(v is None for v in given.values()):
            w, h = self.size()
            rect = (0, 0, w, h)
        elif any(v is None for v in given.values()):
            missing = ", ".join(k for k, v in given.items() if v is None)
            raise VyrnUserError("exception", f"vyrn.screen() is missing {missing}.",
                                "Give all four (x, y, width, height), or none of them for the whole screen. "
                                + _PICK_HINT)
        else:
            what = (f"vyrn.screen(x={safe_repr(x)}, y={safe_repr(y)}, width={safe_repr(width)}, "
                    f"height={safe_repr(height)})")
            rect = self._area(what, x, y, width, height)
        return self._crop(rect).copy()

    def pixel(self, x, y):
        """vyrn.pixel(): the (r, g, b) colour of one pixel, as plain ints."""
        self._real_only("vyrn.pixel()")
        rect = self._area(f"vyrn.pixel({safe_repr(x)}, {safe_repr(y)})", x, y, 1, 1)
        r, g, b = self._crop(rect)[0, 0]
        return int(r), int(g), int(b)

    def on_screen(self, name, x=None, y=None, match=0.9):
        """vyrn.on_screen(): True when the Snapshot pictures/<name> is showing at (x, y).

        Without x and y, the position saved in the Snapshot (PNG text "vyrn-area") is used.
        A missing file gives False and a one-time warning.
        """
        self._real_only("vyrn.on_screen()")
        if not isinstance(name, str) or not name.strip():
            raise VyrnUserError("exception", 'vyrn.on_screen() needs the name of a Snapshot, like "game_over.png".',
                                "Take Snapshots with the Snapshot button in the toolbar.")
        match = _fraction(match)
        path, shown = self._picture_path(name)
        picture = self._load_picture(path, shown)
        if picture is None:
            self._warn_missing(path, shown)
            return False
        image, area = picture
        if x is None and y is None:
            if area is None:
                raise VyrnUserError(
                    "exception", f"Vyrn doesn't know where on the screen to look for {shown}.",
                    f'Add the position, like vyrn.on_screen("{shown}", x=100, y=50). '
                    "Snapshots taken with the Snapshot button remember their position by themselves.")
            x, y = area[0], area[1]
        elif x is None or y is None:
            raise VyrnUserError("exception", f'vyrn.on_screen("{shown}") needs both x and y, or neither.',
                                "Without x and y, Vyrn uses the position saved in the Snapshot.")
        h, w = image.shape[:2]
        rect = self._area(f'vyrn.on_screen("{shown}", x={safe_repr(x)}, y={safe_repr(y)})', x, y, w, h)
        if area is not None and rect[:2] != tuple(area[:2]):
            self._warn_moved(path, shown, area, rect[0], rect[1])
        seen = self._crop(rect)
        difference = np.abs(seen.astype(np.int16) - image).mean()
        return bool(1.0 - difference / 255.0 >= match)

    def size(self):
        """(width, height) of the whole screen. The first call takes one full capture to find out."""
        self._real_only("vyrn.screen()")
        if self._size is None:
            image = self._capture(None)
            h, w = image.shape[:2]
            self._size = (w, h)
            self._shot = (self._state.generation, (0, 0, w, h), image)
        return self._size

    # ----- capture cache -----

    def _crop(self, rect):
        """A read-only view of rect from this generation's capture, capturing the union if needed."""
        u = self._union
        self._union = rect if u is None else (min(u[0], rect[0]), min(u[1], rect[1]),
                                              max(u[2], rect[2]), max(u[3], rect[3]))
        shot = self._shot
        if shot is None or shot[0] != self._state.generation or not _contains(shot[1], rect):
            shot = self._take(self._union)
        (sx, sy, _, _), image = shot[1], shot[2]
        x0, y0, x1, y1 = rect
        return image[y0 - sy:y1 - sy, x0 - sx:x1 - sx]

    def _take(self, rect):
        whole = self._size is not None and rect == (0, 0, *self._size)
        self._shot = (self._state.generation, rect, self._capture(None if whole else rect))
        return self._shot

    def _capture(self, rect):
        """One capture of rect (None = the whole screen) as uint8 [h, w, 3]."""
        if not self._state.screen_allowed:           # every public function checks this first; never capture anyway
            raise VyrnUserError("input", f"Vyrn {NO_SCREEN}.", _NO_SCREEN_HINT)
        if self._backend is None:
            self._backend = _platform.make_screen_capture()
        image = self._backend.capture(rect)
        self.captures += 1
        if rect is not None and image.shape[:2] != (rect[3] - rect[1], rect[2] - rect[0]):
            raise VyrnUserError("exception", f"Vyrn got a {image.shape[1]}x{image.shape[0]} picture for a "
                                f"{rect[2] - rect[0]}x{rect[3] - rect[1]} area of the screen.",
                                getattr(self._backend, "hint", ""))
        return image

    # ----- helpers -----

    def _real_only(self, what):
        if self._state.mode == "sim":
            # vyrn.screen() stands for what the AI sees (draw a canvas); pixel/on_screen for game facts (the world)
            hint = _SIM_HINT if what == "vyrn.screen()" else _SIM_READ_HINT
            raise SimOnlyError(f"{what} only works with a real game.", hint)
        if not self._state.screen_allowed:
            raise VyrnUserError("input", f"{what} {NO_SCREEN}.", _NO_SCREEN_HINT)

    def _area(self, what, x, y, width, height):
        """Validate an area in screen pixels and return it as a rect."""
        x, y = _whole(what, "x", x), _whole(what, "y", y)
        width, height = _whole(what, "width", width), _whole(what, "height", height)
        if width < 1 or height < 1:
            raise VyrnUserError("exception", f"{what}: width and height must be at least 1.", _PICK_HINT)
        sw, sh = self.size()
        if x < 0 or y < 0 or x + width > sw or y + height > sh:
            where = "is outside the screen" if width == height == 1 else "doesn't fit on the screen"
            raise VyrnUserError("exception", f"{what} {where}, which is {sw}x{sh} pixels "
                                f"(x goes from 0 to {sw - 1}, y from 0 to {sh - 1}).", _PICK_HINT)
        return (x, y, x + width, y + height)

    def _picture_path(self, name):
        shown = name.strip()
        path = os.path.join(self._state.project_dir or ".", "pictures", shown)
        if not os.path.splitext(shown)[1] and not os.path.exists(path):
            shown, path = shown + ".png", path + ".png"     # "game_over" means game_over.png
        return path, shown

    def _load_picture(self, path, shown):
        """(int16 RGB image, area or None) for a Snapshot, cached by mtime; None when it is missing."""
        try:
            stat = os.stat(path)
        except FileNotFoundError:
            return None
        except OSError as err:
            raise VyrnUserError("exception", f"Vyrn can't open pictures/{shown}: {err.strerror}.") from None
        stamp = (stat.st_mtime_ns, stat.st_size)
        cached = self._pictures.get(path)
        if cached is not None and cached[0] == stamp:
            return cached[1], cached[2]
        from PIL import Image
        try:
            with Image.open(path) as im:
                im.load()
                text = dict(getattr(im, "text", None) or {})
                area = _parse_area(text.get("vyrn-area", im.info.get("vyrn-area")))
                rgb = im.convert("RGB")
                if area is not None and area[2] > 0 and area[3] > 0 and rgb.size != (area[2], area[3]):
                    # Saved with the screen's pixels while its area is in points (a HiDPI Mac): compare in points,
                    # like every capture.
                    rgb = scale_to(rgb, (area[2], area[3]))
                rgb = np.asarray(rgb, dtype=np.uint8)
        except (OSError, ValueError, Image.DecompressionBombError) as err:
            raise VyrnUserError("exception", f"pictures/{shown} isn't a picture Vyrn can read ({err}).",
                                "Take the Snapshot again (toolbar → Snapshot).") from None
        image = rgb.astype(np.int16)
        self._pictures[path] = (stamp, image, area)
        return image, area

    def _warn_moved(self, path, shown, area, x, y):
        """on_screen() with an x, y other than where the Snapshot was taken: say so once (it then never matches
        unless the picture really shows up there too)."""
        if (path, x, y) in self._moved:
            return
        self._moved.add((path, x, y))
        script, line = self._state.user_location()
        self._state.warn_once(f"on_screen-moved:{path}:{x},{y}", script, line,
                              f"{shown} was taken at x={area[0]}, y={area[1]}, but this line looks at x={x}, y={y}. "
                              f"Remove  x={x}, y={y}  to look where the Snapshot was taken.")

    def _warn_missing(self, path, shown):
        if path in self._missing:
            return
        self._missing.add(path)
        script, line = self._state.user_location()
        self._state.warn_once(f"on_screen-missing:{path}", script, line,
                              f"Take a Snapshot named {shown} (toolbar → Snapshot). "
                              f'Until then vyrn.on_screen("{shown}") is always False.')


def _contains(outer, inner):
    return outer[0] <= inner[0] and outer[1] <= inner[1] and inner[2] <= outer[2] and inner[3] <= outer[3]


def _whole(what, name, value):
    """A coordinate as an int: ints, numpy ints and whole floats are fine."""
    if not isinstance(value, bool):
        try:
            return operator.index(value)
        except TypeError:
            pass
        if isinstance(value, (float, np.floating)) and float(value).is_integer():
            return int(value)
    raise VyrnUserError("exception", f"{what}: {name} must be a whole number of pixels, not {safe_repr(value)}.",
                        "Use int(...) to turn a number into a whole number.")


def _fraction(match):
    if not isinstance(match, bool):
        try:
            value = float(match)
        except (TypeError, ValueError):
            value = None
        if value is not None and 0.0 <= value <= 1.0:
            return value
    raise VyrnUserError("exception", "vyrn.on_screen(): match must be a number from 0 to 1, "
                        f"not {safe_repr(match)}.",
                        "0.9 means 90% alike (the default); 1 means exactly the same.")


def _parse_area(text):
    """The Snapshot's saved position "x,y,w,h" -> (x, y, w, h), or None when absent or damaged."""
    if not isinstance(text, str):
        return None
    try:
        values = tuple(int(v) for v in text.split(","))
    except ValueError:
        return None
    return values if len(values) == 4 else None

