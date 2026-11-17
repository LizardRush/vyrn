"""vyrn.canvas(): a small blank picture to draw on, plus the picture conversion Vyrn uses everywhere.

x is the column and y the row (y grows downward). Drawing outside the canvas is clipped silently.
"""

import difflib
import math
import numbers

import numpy as np
from PIL import Image, ImageColor

from ._state import VyrnUserError, safe_repr

MAX_SIDE = 4096

_COLOR_HINT = ('Use a color name like "white", "red" or "gray", a code like "#ff8800", '
               'or three numbers from 0 to 255 like (255, 136, 0).')

_color_cache = {}


def parse_color(color):
    """Turn a color into an (r, g, b) tuple of ints 0-255.

    Accepts a name or "#rrggbb" code (anything PIL.ImageColor knows), an (r, g, b) or (r, g, b, a)
    tuple/list, or a single number 0-255 for a gray level.
    """
    if isinstance(color, str):
        key = color.strip().lower()
        rgb = _color_cache.get(key)
        if rgb is None:
            try:
                rgb = tuple(ImageColor.getrgb(key)[:3])
            except ValueError:
                guess = difflib.get_close_matches(key, list(ImageColor.colormap), n=1)
                extra = f' Did you mean "{guess[0]}"?' if guess else ""
                raise VyrnUserError("exception", f'Vyrn doesn\'t know the color "{color}".{extra}',
                                    _COLOR_HINT) from None
            if len(_color_cache) < 1024:
                _color_cache[key] = rgb
        return rgb
    if isinstance(color, (numbers.Real, np.number)) and not isinstance(color, bool):
        return (_channel(color),) * 3
    if isinstance(color, (tuple, list, np.ndarray)) and len(color) in (3, 4):
        return tuple(_channel(c) for c in list(color)[:3])
    raise VyrnUserError("exception", f"{safe_repr(color)} isn't a color Vyrn understands.", _COLOR_HINT)


def _float(value):
    """float(value), or infinity for a whole number too big for a float."""
    try:
        return float(value)
    except OverflowError:
        return math.inf


def _channel(value):
    if isinstance(value, (numbers.Real, np.number)) and not isinstance(value, bool):
        number = _float(value)
        if math.isfinite(number) and 0 <= number <= 255:
            return int(number)
    raise VyrnUserError("exception", f"A color number must be from 0 to 255, not {safe_repr(value)}.",
                        _COLOR_HINT)


def _position(value, what):
    """A drawing coordinate as an int (floats are rounded down, so 4.7 is pixel 4)."""
    if isinstance(value, (numbers.Integral, np.integer)):
        return int(value)
    if isinstance(value, (numbers.Real, np.floating)):
        number = _float(value)
        if math.isfinite(number):
            return math.floor(number)
    raise VyrnUserError("exception", f"The {what} must be a number, not {safe_repr(value)}.",
                        "Positions count pixels from the top-left corner, like picture.dot(3, 4).")


def _side(value, what):
    if isinstance(value, (numbers.Real, np.number)) and not isinstance(value, bool):
        number = _float(value)
        if math.isfinite(number) and number == int(number) and 1 <= number <= MAX_SIDE:
            return int(number)
    raise VyrnUserError("exception", f"The canvas {what} must be a whole number from 1 to {MAX_SIDE}, "
                        f"not {safe_repr(value)}.", "For example: vyrn.canvas(10, 10) makes a 10×10 picture.")


class Canvas:
    """A blank picture you draw on with .dot() and .box(); return it from see()."""

    __slots__ = ("width", "height", "_pixels")

    def __init__(self, width, height, color="black"):
        self.width = _side(width, "width")
        self.height = _side(height, "height")
        self._pixels = np.empty((self.height, self.width, 3), dtype=np.uint8)
        self._pixels[...] = parse_color(color)

    def dot(self, x, y, color="white"):
        """Color one pixel: x counts columns from the left, y counts rows from the top."""
        x = _position(x, "x position")
        y = _position(y, "y position")
        if 0 <= x < self.width and 0 <= y < self.height:
            self._pixels[y, x] = parse_color(color)

    def box(self, x, y, width, height, color="white"):
        """Fill a rectangle whose top-left corner is (x, y)."""
        x = _position(x, "x position")
        y = _position(y, "y position")
        width = _position(width, "width")
        height = _position(height, "height")
        x0, y0 = max(x, 0), max(y, 0)
        x1, y1 = min(x + width, self.width), min(y + height, self.height)
        if x1 > x0 and y1 > y0:
            self._pixels[y0:y1, x0:x1] = parse_color(color)

    @property
    def pixels(self):
        """The live numpy array behind the canvas: uint8, shape [height, width, 3]."""
        return self._pixels

    def __array__(self, dtype=None, copy=None):
        if dtype is not None and np.dtype(dtype) != self._pixels.dtype:
            if copy is False:
                raise ValueError("converting a canvas to another dtype needs a copy")
            return self._pixels.astype(dtype)
        return self._pixels.copy() if copy else self._pixels

    def __repr__(self):
        return f"<vyrn canvas {self.width}×{self.height}>"


# -- picture conversion ---------------------------------------------------------------------------

class PictureError(ValueError):
    """A value that looks like a picture but can't be used as one. Has .message and .hint."""

    def __init__(self, message, hint=""):
        super().__init__(message)
        self.message = message
        self.hint = hint


# The first sentence is protocol.md section 4.4's; the rest keeps it from turning a board of small numbers, or a
# picture made of 0-to-1 values, into an all-black picture.
_UINT8_HINT = ("pictures are whole numbers 0-255: use .astype('uint8'). If these are numbers (like a game board), "
               "return board.flatten() to give them as a list of numbers. If they go from 0 to 1, use "
               "(board * 255).astype('uint8').")


def to_rgb(value):
    """Convert a picture to a fresh, contiguous uint8 array of shape [H, W, 3].

    Pictures are: a vyrn canvas, a PIL image, or a numpy array of shape [H, W], [H, W, 1],
    [H, W, 3] or [H, W, 4] (alpha is dropped, gray is copied to 3 channels).
    Returns None when the value isn't picture-like at all (numbers, text, lists...).
    Raises PictureError when it looks like a picture but can't be used.
    """
    if isinstance(value, Canvas):
        array = value._pixels.copy()
    elif isinstance(value, Image.Image):
        array = np.array(value.convert("RGB"), dtype=np.uint8)
    elif isinstance(value, np.ndarray) and value.ndim >= 2:
        array = _array_to_rgb(value)
    else:
        return None
    height, width = array.shape[:2]
    if not (1 <= height <= MAX_SIDE and 1 <= width <= MAX_SIDE):
        raise PictureError(f"The picture is {width}×{height} pixels; each side must be 1 to {MAX_SIDE}.",
                           "Use a smaller area of the screen, or a smaller canvas.")
    return array


def _array_to_rgb(array):
    if array.ndim > 3:
        raise PictureError(f"The array has {array.ndim} dimensions, but a picture has 2 or 3 "
                           "(height, width and maybe color).",
                           "Return a picture shaped [height, width, 3], or a flat list of numbers.")
    if array.ndim == 3 and array.shape[2] not in (1, 3, 4):
        raise PictureError(f"A picture needs 1, 3 or 4 color channels, but this array has {array.shape[2]} "
                           f"(its shape is {list(array.shape)}).",
                           "Pictures are shaped [height, width, 3] for red, green and blue.")
    if array.dtype != np.uint8:
        raise PictureError(f"The picture's numbers are {array.dtype}, not whole numbers 0-255.", _UINT8_HINT)
    if array.ndim == 2:
        array = array[:, :, None]
    if array.shape[2] == 1:
        return np.repeat(array, 3, axis=2)
    if array.shape[2] == 4:
        return np.ascontiguousarray(array[:, :, :3])
    return np.array(array, dtype=np.uint8, order="C", copy=True)
