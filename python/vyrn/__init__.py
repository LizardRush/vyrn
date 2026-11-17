"""Vyrn: teach an AI to play a game with three small Python scripts.

Use these in inputs.py, responses.py, points.py and world.py:

    vyrn.screen(x, y, width, height)    picture of an area of the screen (no numbers: whole screen)
    vyrn.pixel(x, y)                    (r, g, b) color of one screen pixel
    vyrn.on_screen("name.png", x, y)    True if that Snapshot is showing there
    vyrn.press("space")                 tap a key right now (for restart())
    vyrn.wait(seconds)                  pause for a moment
    vyrn.canvas(width, height)          a blank picture to draw on
    vyrn.show(name, value)              put a number, text or picture on the dashboard
    vyrn.choice, vyrn.tick, vyrn.round  last choice, tick in this round, round number

For a game connected with Vyrn Link (a mod that sends its state, like the Geometry Dash template):

    vyrn.game                           the game's newest state, a dict like {"y": 105.0, "dead": False}
    vyrn.events                         the game's events since the last tick, like [{"name": "death"}]
    vyrn.connected                      True while the game is connected
    vyrn.send("restart")                send the game a message
"""

import math as _math
import numbers as _numbers
import time as _time

import numpy as _np

from . import _link, _state
from ._canvas import Canvas as _Canvas, PictureError as _PictureError, to_rgb as _to_rgb

__version__ = _state.VERSION

__all__ = ["screen", "pixel", "on_screen", "press", "wait", "canvas", "show", "choice", "tick", "round",
           "game", "events", "connected", "send"]

#: The choice the AI made last in this round, like "left" (None at the start of a round).
#: Write vyrn.choice: "from vyrn import choice" copies it once, so it never changes.
choice = None

#: Ticks since this round started (0 on the round's first see()).
#: Write vyrn.tick: "from vyrn import tick" copies it once, so it never changes.
tick = 0

#: The round number, starting at 1 (0 while your scripts load).
#: Write vyrn.round: "from vyrn import round" copies it once, and hides Python's own round().
round = 0  # noqa: A001 - the public name is vyrn.round; this module never uses the builtin

#: The newest state from a game connected with Vyrn Link: a dict like {"y": 105.0, "dead": False}.
#: It is {} until the game sends a state, so read it with .get(): vyrn.game.get("y", 0).
#: Write vyrn.game: "from vyrn import game" copies it once, so it never changes.
game = _link.GameState()

#: The game's events since the last tick, oldest first, like [{"type": "event", "name": "death"}].
#: Write vyrn.events: "from vyrn import events" copies it once, so it never changes.
events = []

#: True while a game is connected with Vyrn Link.
#: Write vyrn.connected: "from vyrn import connected" copies it once, so it never changes.
connected = False


def _service(name, function):
    service = getattr(_state.S, name, None)
    if service is None:
        raise _state.VyrnUserError(
            "exception", f"vyrn.{function}() only works while Vyrn runs your project.",
            "Use Check, Train or Watch in Vyrn (or python3 python/tools/check.py <project>).")
    return service


def screen(x=None, y=None, width=None, height=None):
    """Picture of an area of the screen; with no numbers, the whole screen.

    x, y is the area's top-left corner and width, height its size, in screen pixels.
    Toolbar → Pick area fills these in for you. The result is a picture: a numpy array
    of shape [height, width, 3] (red, green, blue).

        def see():
            return vyrn.screen(x=0, y=0, width=640, height=480)
    """
    return _service("screen", "screen").grab(x, y, width, height)


def pixel(x, y):
    """Color of one screen pixel as (red, green, blue), each 0-255.

        r, g, b = vyrn.pixel(20, 20)
        return r > 200 and g < 80      # is it red?
    """
    return _service("screen", "pixel").pixel(x, y)


def on_screen(name, x=None, y=None, match=0.9):
    """True if the Snapshot picture called name is showing at x, y on the screen right now.

    Take Snapshots with toolbar → Snapshot; they are saved in your project's pictures folder.
    Leave out x and y to look where the Snapshot was taken. match is how alike they must be:
    1.0 means exactly, 0.9 (the default) allows small changes.

        def game_over():
            return vyrn.on_screen("game_over.png")
    """
    return _service("screen", "on_screen").on_screen(name, x, y, match)


def press(key):
    """Tap a key (or a combo like "ctrl+c") right now, e.g. in restart(): vyrn.press("space").

    Keys can only be pressed while the AI plays (Train or Watch) and your game window is active.
    """
    keyboard = _service("keyboard", "press")
    from . import _effects
    try:
        keyboard.press_now(key)
    except _effects.EffectError as err:
        raise _state.VyrnUserError("exception", err.message, err.hint) from None


def wait(seconds):
    """Pause for this many seconds, e.g. vyrn.wait(0.5) to give a game time to restart."""
    if isinstance(seconds, bool) or not isinstance(seconds, _numbers.Real):
        raise _state.VyrnUserError("exception", "vyrn.wait() needs a number of seconds, "
                                   f"not {_state.safe_repr(seconds)}.",
                                   "For example: vyrn.wait(0.5) waits half a second.")
    try:
        seconds = float(seconds)
    except OverflowError:
        seconds = _math.inf
    if not _math.isfinite(seconds) or seconds < 0:
        raise _state.VyrnUserError("exception", f"vyrn.wait() can't wait {seconds} seconds.",
                                   "Use a positive number, like vyrn.wait(0.5).")
    S = _state.S
    if S.mode == "sim":             # warned in a Check too, which itself never sleeps
        script, line = S.user_location()
        S.warn_once("wait-in-sim", script, line,
                    "vyrn.wait() slows training down a lot in a simulation. Watch already plays at human speed.")
    if S.check:
        return                      # a Check never sleeps
    end = _time.monotonic() + seconds
    while not S.stopping:
        left = end - _time.monotonic()
        if left <= 0:
            break
        _time.sleep(min(left, 0.05))


def canvas(width, height, color="black"):
    """A blank picture to draw on: picture.dot(x, y, color) and picture.box(x, y, width, height, color).

        picture = vyrn.canvas(10, 10)
        picture.dot(3, 4, "white")
        return picture
    """
    return _Canvas(width, height, color)


def show(name, value):
    """Show a number, text or picture on the dashboard under this name, e.g. vyrn.show("speed", 3.5)."""
    _state.S.show_changed[_text(name)] = _show_value(value)


def send(type, **fields):  # noqa: A002 - "type" is the word the game reads
    """Send a message to the game now, like vyrn.send("restart") or vyrn.send("speed", value=2.0).

    The game gets {"type": "restart"} or {"type": "speed", "value": 2.0}, and its mod decides what to do with
    them (the Geometry Dash mod restarts the level on "restart"). Values can be numbers, text, True/False,
    None, and lists or dicts of those. It works with a game connected with Vyrn Link, for example in restart():

        def restart():
            vyrn.send("restart")
    """
    _link.send(type, fields)


def _text(value):
    """str(value) cut to 200 characters; never fails (a number over 4300 digits is summarized)."""
    try:
        return str(value)[:200]
    except ValueError:
        return _state.safe_repr(value)[:200]


def _show_value(value):
    """What the dashboard gets: a float, text up to 200 characters, or a uint8 [H, W, 3] picture."""
    if isinstance(value, (bool, _np.bool_)):
        return str(bool(value))
    if isinstance(value, _numbers.Real):
        try:
            number = float(value)
        except OverflowError:
            return _text(value)
        return number if _math.isfinite(number) else str(number)
    if isinstance(value, str):
        return value[:200]
    try:
        picture = _to_rgb(value)
    except _PictureError as err:
        raise _state.VyrnUserError("exception", "vyrn.show() can't use this picture: " + err.message,
                                   err.hint) from None
    if picture is not None:
        return picture
    return _text(value)
