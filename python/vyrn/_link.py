"""Vyrn Link in the worker: vyrn.game, vyrn.events, vyrn.connected, vyrn.send() and the "game ..." effects.

The app hosts the WebSocket server the game connects to (docs/dev/link.md). The worker never talks to the game
itself: real-time requests bring the game's newest state and events (protocol.md section 3.4), and the replies
carry the AI's game buttons ("game_input") and the scripts' messages ("send") back to the app.
"""

import json
import math
import numbers

import numpy as np

from . import _state
from ._effects import GAME_KINDS
from ._state import VyrnUserError, safe_repr

#: Message types the scripts can't send: Vyrn sends these itself, or only the game sends them.
RESERVED = frozenset({"input", "stop", "welcome", "error", "hello", "state", "event"})
MAX_SENDS = 64                  # vyrn.send() calls per reply
MAX_SEND_BYTES = 512 * 1024     # all messages of one reply together (a whole reply is at most 1 MiB of JSON)
MAX_TYPE_LENGTH = 64
MAX_DEPTH = 32

_SEND_EXAMPLE = 'For example: vyrn.send("restart") or vyrn.send("speed", value=2.0).'
_VALUES_HINT = "Send numbers, text, True/False, None, and lists or dicts of those."


class MissingGameValue(VyrnUserError, KeyError):
    """vyrn.game["y"] when the game didn't send "y". A KeyError, so  except KeyError  still catches it."""

    def __init__(self, key, state):
        name = safe_repr(key)
        if not state:
            message = f"vyrn.game has no {name} yet: no state has come from the game (vyrn.game is {{}})."
            hint = (f"Write vyrn.game.get({name}, 0) to use 0 until the game sends it. Then your code also works "
                    "before the game connects, and in Check.")
        else:
            names = [str(k) for k in state if k != "type"]
            shown = ", ".join(names[:12]) + (", ..." if len(names) > 12 else "")
            message = f"vyrn.game has no {name}: the game sent {shown or 'nothing else'}."
            hint = (f"Check the spelling against what the game's mod sends, or write vyrn.game.get({name}, 0) to "
                    "use 0 when it is missing.")
        super().__init__("exception", message, hint)

    def __str__(self):
        return self.message


class GameState(dict):
    """vyrn.game: a plain dict of the game's newest state, except that a missing key explains itself."""

    __slots__ = ()

    def __missing__(self, key):
        raise MissingGameValue(key, self)


def game_state(value):
    """vyrn.game for a request's "game": the state as a GameState ({} when there is none)."""
    return GameState(value) if isinstance(value, dict) else GameState()


def game_events(value):
    """vyrn.events for a request's "game_events": the event dicts, oldest first."""
    if not isinstance(value, list):
        return []
    return [dict(event) for event in value if isinstance(event, dict)]


def game_input(effect):
    """The game buttons the chosen effect presses this tick, for the act reply: {"hold": [...], "tap": [...]}.

    "hold" is every button that must be down until the next tick (so any other choice lets go of them). A Check
    describes the buttons instead ("would hold game button jump") and presses nothing.
    """
    hold, tap = [], []
    if effect.kind in GAME_KINDS:
        S = _state.S
        if S.check:
            S.did.append("would " + effect.describe())
        elif effect.kind == "game_hold":
            hold = list(effect.buttons)
        else:
            tap = list(effect.buttons)
    return {"hold": hold, "tap": tap}


def send(kind, fields):
    """vyrn.send(): check the message and queue it for the next reply (protocol.md section 3.4)."""
    S = _state.S
    if S.mode == "sim":
        raise VyrnUserError("exception", "vyrn.send() sends messages to a game connected with Vyrn Link, and a "
                            "simulation has no game.",
                            "A simulation changes its world in world.py and in its choice functions instead.")
    message = _message(kind, fields)
    text = json.dumps(message, separators=(",", ":"), ensure_ascii=False, allow_nan=False)
    size = len(text.encode("utf-8", "surrogatepass"))
    if size > MAX_SEND_BYTES:
        raise VyrnUserError("exception", f"vyrn.send() can't send {size // 1024} KiB at once.",
                            "Send the game only what it needs, like short lists of numbers.")
    if S.loading:
        script, line, func = S.user_place()
        where = "at the top of a file" if func in ("", "<module>") else f"in {func}()"
        S.warn_once(f"send-while-loading:{script}:{line}", script, line,
                    f"vyrn.send() {where} sent nothing: Vyrn sends no messages while it loads your scripts. Send "
                    "them from restart() or a choice instead.")
        return
    if S.check:
        S.did.append("would send " + text)
        return
    with S._lock:
        if len(S.outbox) >= MAX_SENDS:
            raise VyrnUserError("exception", f"vyrn.send() was used more than {MAX_SENDS} times in one tick.",
                                "Send fewer messages: the game gets them all at once with the AI's next decision.")
        if S.outbox_bytes + size > MAX_SEND_BYTES:
            raise VyrnUserError("exception", "vyrn.send() sent too much in one tick.",
                                "Send the game only what it needs, like short lists of numbers.")
        S.outbox.append(message)
        S.outbox_bytes += size


def take_outbox():
    """The messages vyrn.send() queued since the last reply; the queue is empty afterwards."""
    S = _state.S
    with S._lock:
        out, S.outbox, S.outbox_bytes = S.outbox, [], 0
    return out


def _message(kind, fields):
    """The message dict for vyrn.send(kind, **fields), or a dict passed whole: vyrn.send({"type": "restart"})."""
    if isinstance(kind, dict):
        given = dict(kind)
        given.update(fields)
        kind = given.pop("type", None)
        fields = given
    if not isinstance(kind, str) or not kind.strip():
        raise VyrnUserError("exception", "vyrn.send() needs the type of message first, as text, not "
                            f"{safe_repr(kind)}.", _SEND_EXAMPLE)
    kind = kind.strip()
    if kind in RESERVED:
        if kind == "input":
            hint = ('To press game buttons, make choices like  jump = "game jump"  in responses.py: Vyrn sends '
                    'them to the game as "input" messages.')
        elif kind == "stop":
            hint = "Vyrn tells the game to stop when the AI pauses or stops."
        else:
            hint = 'Pick another type, like "restart".'
        raise VyrnUserError("exception", f'vyrn.send("{kind}") isn\'t possible: Vyrn Link uses "{kind}" '
                            "messages for itself.", hint)
    if len(kind) > MAX_TYPE_LENGTH:
        raise VyrnUserError("exception", f"vyrn.send() needs a short type, at most {MAX_TYPE_LENGTH} characters.",
                            _SEND_EXAMPLE)
    message = {"type": kind}
    for key, value in fields.items():
        if not isinstance(key, str):
            raise VyrnUserError("exception", f"vyrn.send() needs text names for the values, not {safe_repr(key)}.",
                                _SEND_EXAMPLE)
        message[key] = _plain(value, 0)
    return message


def _plain(value, depth):
    """value as plain JSON data: numpy numbers and arrays become numbers and lists. Raises VyrnUserError."""
    if depth > MAX_DEPTH:
        raise VyrnUserError("exception", "vyrn.send() got lists or dicts nested too deeply.", _VALUES_HINT)
    if value is None or isinstance(value, (bool, str)):
        return value
    if isinstance(value, np.bool_):
        return bool(value)
    if isinstance(value, numbers.Integral):
        return int(value)
    if isinstance(value, numbers.Real):
        try:
            number = float(value)
        except (OverflowError, ValueError):
            number = math.inf
        if not math.isfinite(number):
            raise VyrnUserError("exception", f"vyrn.send() can't send {safe_repr(value)}: a game only understands "
                                "ordinary numbers.", "Check the number before sending it (math.isfinite).")
        return number
    if isinstance(value, np.ndarray):
        return _plain(value.tolist(), depth)
    if isinstance(value, (list, tuple)):
        return [_plain(item, depth + 1) for item in value]
    if isinstance(value, dict):
        out = {}
        for key, item in value.items():
            if not isinstance(key, str):
                raise VyrnUserError("exception", "vyrn.send() needs text keys in a dict, not "
                                    f"{safe_repr(key)}.", _VALUES_HINT)
            out[key] = _plain(item, depth + 1)
        return out
    raise VyrnUserError("exception", f"vyrn.send() can't send {type(value).__name__} values.", _VALUES_HINT)
