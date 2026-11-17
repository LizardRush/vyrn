"""Effect strings: what a choice in responses.py does (protocol.md section 4.3).

    effect := ["tap "] combo  |  ["tap "] "game " buttons
    combo := key ("+" key)*        buttons := button ("+" button)*

Case-insensitive, spaces around "+" allowed. A plain combo is *held* while the AI keeps choosing it;
"tap ..." presses and releases once. "game jump" holds the button "jump" of a game connected with Vyrn Link
(docs/dev/link.md) and "tap game jump" taps it: nothing is pressed on this computer. None does nothing and a
function is called.

Only keys in KEYS can ever be pressed. Keys in BLOCKED are refused with a plain reason, and unknown
names get a did-you-mean suggestion.
"""

import difflib
import inspect
import re
from dataclasses import dataclass

from ._state import safe_repr

__all__ = [
    "KEYS", "MODIFIERS", "BLOCKED", "ALLOWED_CODES", "GAME_KINDS", "EffectError",
    "normalize_key", "label", "Effect", "parse_effect", "is_game_effect",
]


def _build_keys():
    keys = {c: f"KEY_{c.upper()}" for c in "abcdefghijklmnopqrstuvwxyz"}
    keys.update({d: f"KEY_{d}" for d in "0123456789"})
    keys.update({f"f{n}": f"KEY_F{n}" for n in range(1, 13)})
    keys.update({
        "space": "KEY_SPACE",
        "enter": "KEY_ENTER", "return": "KEY_ENTER",
        "tab": "KEY_TAB",
        "esc": "KEY_ESC", "escape": "KEY_ESC",
        "backspace": "KEY_BACKSPACE",
        "delete": "KEY_DELETE",
        "insert": "KEY_INSERT",
        "home": "KEY_HOME",
        "end": "KEY_END",
        "pageup": "KEY_PAGEUP",
        "pagedown": "KEY_PAGEDOWN",
        "up": "KEY_UP", "down": "KEY_DOWN", "left": "KEY_LEFT", "right": "KEY_RIGHT",
        "shift": "KEY_LEFTSHIFT",
        "ctrl": "KEY_LEFTCTRL", "control": "KEY_LEFTCTRL",
        "-": "KEY_MINUS", "minus": "KEY_MINUS",
        "=": "KEY_EQUAL", "equal": "KEY_EQUAL",
        "[": "KEY_LEFTBRACE", "bracketleft": "KEY_LEFTBRACE",
        "]": "KEY_RIGHTBRACE", "bracketright": "KEY_RIGHTBRACE",
        ";": "KEY_SEMICOLON", "semicolon": "KEY_SEMICOLON",
        "'": "KEY_APOSTROPHE", "apostrophe": "KEY_APOSTROPHE", "quote": "KEY_APOSTROPHE",
        "`": "KEY_GRAVE", "grave": "KEY_GRAVE", "backtick": "KEY_GRAVE",
        "\\": "KEY_BACKSLASH", "backslash": "KEY_BACKSLASH",
        ",": "KEY_COMMA", "comma": "KEY_COMMA",
        ".": "KEY_DOT", "period": "KEY_DOT", "dot": "KEY_DOT",
        "/": "KEY_SLASH", "slash": "KEY_SLASH",
    })
    return keys


#: friendly lowercase name -> evdev code name. The only keys Vyrn will ever press.
KEYS = _build_keys()

#: Every evdev code the virtual keyboard declares (nothing else exists on the device).
ALLOWED_CODES = frozenset(KEYS.values())

#: Pressed first and released last.
MODIFIERS = frozenset({"KEY_LEFTSHIFT", "KEY_LEFTCTRL"})


def _build_blocked():
    reasons = {
        "Alt shortcuts (like Alt+Tab and Alt+F4) can switch or close windows": (
            "alt", "altgr", "lalt", "leftalt", "alt_l", "ralt", "rightalt", "alt_r", "option", "opt"),
        "the Super (Windows) key opens menus and runs desktop shortcuts": (
            "super", "win", "windows", "meta", "cmd", "command", "logo",
            "lsuper", "leftsuper", "super_l", "lwin", "leftwin", "lmeta", "leftmeta",
            "rsuper", "rightsuper", "super_r", "rwin", "rightwin", "rmeta", "rightmeta"),
        "lock keys stay switched on after Vyrn stops and change how your own keyboard types": (
            "capslock", "caps", "numlock", "scrolllock"),
        "it takes screenshots or sends special commands to the system": (
            "print", "printscreen", "prtsc", "prtscr", "sysrq"),
        "it could switch off, restart or suspend your computer": (
            "power", "poweroff", "shutdown", "sleep", "suspend", "hibernate", "wake", "wakeup"),
        "media keys change system settings like volume, brightness or what is playing": (
            "mute", "volume", "volumeup", "volumedown", "play", "pause", "playpause", "stop", "break",
            "next", "nextsong", "previous", "prev", "previoussong", "media", "eject",
            "brightness", "brightnessup", "brightnessdown"),
        "the Menu key opens menus on the desktop": ("menu", "compose"),
        "the Fn key works inside the keyboard itself, so no program can press it": ("fn",),
        'only the left-hand Shift and Ctrl are allowed (write "shift" or "ctrl")': (
            "rshift", "rightshift", "shift_r", "rctrl", "rightctrl", "rcontrol", "rightcontrol", "ctrl_r"),
    }
    return {name: reason for reason, names in reasons.items() for name in names}


#: friendly name -> plain-English reason why Vyrn refuses to press it.
BLOCKED = _build_blocked()

# Shifted symbols on a US keyboard: they are not keys of their own.
_SHIFTED = {
    "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8", "(": "9", ")": "0",
    "_": "-", "{": "[", "}": "]", ":": ";", '"': "'", "<": ",", ">": ".", "?": "/", "~": "`", "|": "\\",
}

_ALLOWED_HINT = ("Vyrn can press letters, digits, punctuation, the arrows, space, enter, tab, esc, "
                 "backspace, delete, insert, home, end, pageup, pagedown, shift, ctrl and f1 to f12. "
                 'Join keys with "+", like "shift+d".')

_TAP_HINT = '"tap" needs a key after it, with a space, like "tap space". (If you meant the Tab key, write "tab".)'

_LABELS = {
    "KEY_LEFTSHIFT": "Shift", "KEY_LEFTCTRL": "Ctrl", "KEY_SPACE": "Space", "KEY_ENTER": "Enter",
    "KEY_TAB": "Tab", "KEY_ESC": "Esc", "KEY_BACKSPACE": "Backspace", "KEY_DELETE": "Delete",
    "KEY_INSERT": "Insert", "KEY_HOME": "Home", "KEY_END": "End", "KEY_PAGEUP": "Page Up",
    "KEY_PAGEDOWN": "Page Down", "KEY_UP": "Up", "KEY_DOWN": "Down", "KEY_LEFT": "Left",
    "KEY_RIGHT": "Right", "KEY_MINUS": "Minus", "KEY_EQUAL": "Equal", "KEY_LEFTBRACE": "Left Bracket",
    "KEY_RIGHTBRACE": "Right Bracket", "KEY_SEMICOLON": "Semicolon", "KEY_APOSTROPHE": "Apostrophe",
    "KEY_GRAVE": "Backtick", "KEY_BACKSLASH": "Backslash", "KEY_COMMA": "Comma", "KEY_DOT": "Period",
    "KEY_SLASH": "Slash",
}

_TAP_PREFIX = re.compile(r"tap(?:\s+|$)", re.IGNORECASE)
_GAME_PREFIX = re.compile(r"game(?:\s+|$)", re.IGNORECASE)
_HELD_GAME = re.compile(r"(?:hold|press|push)\s+game(?:\s+|$)", re.IGNORECASE)
_BUTTON_NAME = re.compile(r"[a-z0-9_-]{1,32}\Z")

#: Effect kinds that press buttons of a game connected with Vyrn Link instead of keys on this computer.
GAME_KINDS = frozenset({"game_hold", "game_tap"})

#: The hint for vyrn.press("game jump"): game buttons are choices (or messages), not keys.
PRESS_GAME_HINT = ('To press a game button, make it a choice in responses.py, like  jump = "tap game jump", or '
                   'send the game a message with vyrn.send().')

_GAME_HINT = ('A game button is a name your game\'s mod understands (Vyrn Link), like "game jump" (held while the '
              'AI keeps choosing it) or "tap game jump" (pressed once). Names use letters, digits, "_" and "-".')

# Words beginners write in quotes when they mean "do nothing".
_NOTHING_WORDS = frozenset({"none", "nothing", "null", "noop", "no-op", "nop", "idle", "wait", "stay", "rest",
                            "do nothing", "donothing"})
# Mouse actions (without spaces): Vyrn has no mouse in v1.
_MOUSE_WORDS = frozenset({"click", "mouse", "lmb", "rmb", "mmb", "mouse1", "mouse2", "mouse3", "leftclick",
                          "rightclick", "middleclick", "doubleclick", "button1", "button2", "button3", "btnleft",
                          "btnright", "btn_left", "btn_right", "scroll", "scrollup", "scrolldown", "wheel",
                          "wheelup", "wheeldown", "leftmouse", "rightmouse", "mouseleft", "mouseright"})


class EffectError(ValueError):
    """A choice's effect (or a vyrn.press() key) that Vyrn can't use. Has .message and .hint."""

    def __init__(self, message, hint=""):
        super().__init__(message)
        self.message = message
        self.hint = hint


def normalize_key(name):
    """Turn one friendly key name ("a", "Space", "ctrl") into its evdev code name ("KEY_A").

    Raises EffectError with a plain reason for blocked keys and a did-you-mean for unknown ones.
    """
    if not isinstance(name, str):
        raise EffectError(f'A key name must be text, like "space" (got {safe_repr(name)}).', _ALLOWED_HINT)
    shown = name.strip()
    key = shown.lower()
    if not key:
        raise EffectError('A key name is missing: check for a "+" with nothing next to it.', _ALLOWED_HINT)
    code = KEYS.get(key)
    if code is not None:
        return code
    if key in BLOCKED:
        raise EffectError(f'Vyrn won\'t press "{shown}": {BLOCKED[key]}.', _ALLOWED_HINT)
    if key in _SHIFTED:
        base = _SHIFTED[key]
        raise EffectError(f'"{shown}" is not a key of its own: on a US keyboard it is Shift+{base}, '
                          f'so write "shift+{base}".', _ALLOWED_HINT)
    if key == "tap":
        raise EffectError(_TAP_HINT, _ALLOWED_HINT)
    words = key.split()
    unknown = f'"{shown}" is not a key Vyrn knows.'
    if key in _NOTHING_WORDS or " ".join(words) in _NOTHING_WORDS:
        raise EffectError(f'"{shown}" is not a key. To do nothing, write None without quotes, like  wait = None',
                          "None (without quotes) is Python's word for nothing.")
    if "".join(words) in _MOUSE_WORDS:
        raise EffectError(f'Vyrn can\'t use the mouse ("{shown}"): that keeps your own mouse free to click Vyrn '
                          "and stop the AI.", _ALLOWED_HINT)
    if len(words) == 2 and words[0] in ("hold", "press", "push"):
        raise EffectError(f'Write just the key, like "{words[1]}": a key is held while the AI keeps '
                          f'choosing it. To press it once each time, write "tap {words[1]}".', _ALLOWED_HINT)
    if len(words) > 1 and words[0] == "tab" and " ".join(words[1:]) in KEYS:
        raise EffectError(f'{unknown} Did you mean "tap {" ".join(words[1:])}"? ("tab" is the Tab key; "tap" '
                          "presses a key once.)", _ALLOWED_HINT)
    for parts in (key.split("-"), key.split(","), words):
        parts = [part.strip() for part in parts]
        if len(parts) > 1 and all(part in KEYS for part in parts):
            raise EffectError(f'{unknown} Did you mean "{"+".join(parts)}"? Join keys with "+".', _ALLOWED_HINT)
    guess = (difflib.get_close_matches(key, list(KEYS), n=1, cutoff=0.6)
             or difflib.get_close_matches("".join(words), list(KEYS), n=1, cutoff=0.6))
    message = unknown
    if guess:
        message += f' Did you mean "{guess[0]}"?'
    raise EffectError(message, _ALLOWED_HINT)


def label(code):
    """Friendly label for an evdev code name: "KEY_A" -> "A", "KEY_LEFTSHIFT" -> "Shift"."""
    if code in _LABELS:
        return _LABELS[code]
    name = code[4:] if code.startswith("KEY_") else code
    return name if len(name) <= 3 else name.title()


def _takes_world(func):
    """True when func has exactly one required positional parameter (Vyrn passes it the world)."""
    try:
        params = inspect.signature(func).parameters.values()
    except (TypeError, ValueError):
        return False
    required = [p for p in params
                if p.kind in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD) and p.default is p.empty]
    return len(required) == 1


@dataclass(frozen=True, eq=False)
class Effect:
    """What one choice does: kind is "none", "hold", "tap", "call", "game_hold" or "game_tap"."""

    kind: str
    keys: tuple = ()          # evdev code names, modifiers first (hold / tap)
    func: object = None       # the function (call)
    buttons: tuple = ()       # game button names, lowercase (game_hold / game_tap)

    def describe(self):
        """Plain English, e.g. "do nothing", "hold Shift+D", "tap Space", "call left(world)",
        "hold game button jump"."""
        if self.kind in GAME_KINDS:
            verb = "tap" if self.kind == "game_tap" else "hold"
            noun = "game button" if len(self.buttons) == 1 else "game buttons"
            return f"{verb} {noun} " + "+".join(self.buttons)
        if self.kind in ("hold", "tap"):
            return f"{self.kind} " + "+".join(label(code) for code in self.keys)
        if self.kind == "call":
            fname = getattr(self.func, "__name__", "")
            if not fname.isidentifier():
                return "call a function"
            return f"call {fname}(world)" if _takes_world(self.func) else f"call {fname}()"
        return "do nothing"


def parse_effect(value, name=""):
    """Parse one choice's effect: a key string, None or a function. Raises EffectError.

    name is the choice name; it only makes error messages point at the right line of responses.py.
    """
    if value is None:
        return Effect("none")
    if isinstance(value, str):
        try:
            return _parse_string(value)
        except EffectError as err:
            where = f'{name} = "{value}": ' if name else ""
            raise EffectError(where + err.message, err.hint) from None
    if callable(value) and not isinstance(value, type):
        return Effect("call", func=value)
    what = f'The choice "{name}"' if name else "This effect"
    raise EffectError(f"{what} is {safe_repr(value)}, which Vyrn can't use as an effect.",
                      'Use a key like "a", "tap space", None to do nothing, or a function.')


def is_game_effect(text):
    """True when the effect string presses game buttons ("game jump", "tap game jump"), not keys."""
    if not isinstance(text, str):
        return False
    body = text.strip()
    tap = _TAP_PREFIX.match(body)
    if tap:
        body = body[tap.end():].strip()
    return bool(_GAME_PREFIX.match(body))


def _parse_buttons(rest):
    """The button names after "game": lowercase, in order, each once."""
    rest = rest.strip()
    if not rest:
        raise EffectError('"game" needs the name of a game button after it, like "game jump".', _GAME_HINT)
    tap = _TAP_PREFIX.match(rest)
    if tap:
        raise EffectError(f'Write "tap game {rest[tap.end():].strip()}": "tap" comes first.', _GAME_HINT)
    names = []
    for part in rest.split("+"):
        shown = part.strip()
        name = shown.lower()
        if not name:
            raise EffectError('A game button name is missing: check for a "+" with nothing next to it.', _GAME_HINT)
        if len(name.split()) > 1:
            raise EffectError(f'"{shown}" has a space in it, but a game button name is one word, like "jump" or '
                              '"move_left". Join buttons with "+".', _GAME_HINT)
        if not _BUTTON_NAME.match(name):
            raise EffectError(f'"{shown}" can\'t be a game button name.', _GAME_HINT)
        if name not in names:
            names.append(name)
    return tuple(names)


def _parse_string(text):
    body = text.strip()
    if not body:
        raise EffectError('The effect is empty: write a key like "a", or None to do nothing.', _ALLOWED_HINT)
    kind = "hold"
    tap = _TAP_PREFIX.match(body)
    if tap:
        kind = "tap"
        body = body[tap.end():].strip()
        if not body:
            raise EffectError(_TAP_HINT, _ALLOWED_HINT)
    game = _GAME_PREFIX.match(body)
    if game:
        return Effect("game_tap" if kind == "tap" else "game_hold", buttons=_parse_buttons(body[game.end():]))
    held = _HELD_GAME.match(body)
    if held and kind == "hold":
        rest = body[held.end():].strip() or "jump"
        raise EffectError(f'Write just "game {rest}": a game button is held while the AI keeps choosing it. To '
                          f'press it once each time, write "tap game {rest}".', _GAME_HINT)
    codes = []
    for part in body.split("+"):
        code = normalize_key(part)
        if code not in codes:
            codes.append(code)
    ordered = [c for c in codes if c in MODIFIERS] + [c for c in codes if c not in MODIFIERS]
    return Effect(kind, tuple(ordered))
