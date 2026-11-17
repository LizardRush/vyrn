"""Effect-string grammar and the key table: protocol.md section 4.3, python-internals.md section 7."""

import re
from pathlib import Path

import pytest

from vyrn import _effects

LETTERS = {c: f"KEY_{c.upper()}" for c in "abcdefghijklmnopqrstuvwxyz"}
DIGITS = {str(d): f"KEY_{d}" for d in range(10)}
FUNCTION_KEYS = {f"f{n}": f"KEY_F{n}" for n in range(1, 13)}
NAMED = {
    "space": "KEY_SPACE", "enter": "KEY_ENTER", "return": "KEY_ENTER", "tab": "KEY_TAB", "esc": "KEY_ESC",
    "escape": "KEY_ESC", "backspace": "KEY_BACKSPACE", "delete": "KEY_DELETE", "insert": "KEY_INSERT",
    "home": "KEY_HOME", "end": "KEY_END", "pageup": "KEY_PAGEUP", "pagedown": "KEY_PAGEDOWN", "up": "KEY_UP",
    "down": "KEY_DOWN", "left": "KEY_LEFT", "right": "KEY_RIGHT", "shift": "KEY_LEFTSHIFT", "ctrl": "KEY_LEFTCTRL",
    "control": "KEY_LEFTCTRL",
}
PUNCTUATION = {
    "-": "KEY_MINUS", "minus": "KEY_MINUS", "=": "KEY_EQUAL", "equal": "KEY_EQUAL",
    "[": "KEY_LEFTBRACE", "bracketleft": "KEY_LEFTBRACE", "]": "KEY_RIGHTBRACE", "bracketright": "KEY_RIGHTBRACE",
    ";": "KEY_SEMICOLON", "semicolon": "KEY_SEMICOLON", "'": "KEY_APOSTROPHE", "apostrophe": "KEY_APOSTROPHE",
    "quote": "KEY_APOSTROPHE", "`": "KEY_GRAVE", "grave": "KEY_GRAVE", "backtick": "KEY_GRAVE",
    "\\": "KEY_BACKSLASH", "backslash": "KEY_BACKSLASH", ",": "KEY_COMMA", "comma": "KEY_COMMA",
    ".": "KEY_DOT", "period": "KEY_DOT", "dot": "KEY_DOT", "/": "KEY_SLASH", "slash": "KEY_SLASH",
}
ALLOWED = {**LETTERS, **DIGITS, **FUNCTION_KEYS, **NAMED, **PUNCTUATION}
ALLOWED_CODES = set(ALLOWED.values())

# Named explicitly in protocol.md 4.3 as refused (they must be in BLOCKED with a reason).
REFUSED = ["alt", "altgr", "super", "win", "windows", "meta", "cmd", "capslock", "numlock", "scrolllock", "print",
           "printscreen", "sysrq", "power", "sleep", "suspend", "wake"]
# Media/volume keys and right-hand modifiers: refused or unknown, but never accepted.
NEVER_ACCEPTED = ["volumeup", "volumedown", "mute", "playpause", "nextsong", "rightshift", "rightctrl", "rightalt",
                  "leftalt", "leftmeta", "rightmeta", "compose", "menu"]

INPUT_EVENT_CODES = Path("/usr/include/linux/input-event-codes.h")


def error_text(error):
    return f"{error.message} {error.hint}"


# ---- the key table --------------------------------------------------------------------------------------------

@pytest.mark.parametrize("name, code", sorted(ALLOWED.items()))
def test_every_allowed_name_maps_to_its_evdev_code(name, code):
    assert _effects.normalize_key(name) == code


@pytest.mark.parametrize("name, code", [("A", "KEY_A"), ("SPACE", "KEY_SPACE"), ("Shift", "KEY_LEFTSHIFT"),
                                        ("F12", "KEY_F12"), ("PageUp", "KEY_PAGEUP"), ("Ctrl", "KEY_LEFTCTRL"),
                                        ("ESC", "KEY_ESC")])
def test_key_names_are_case_insensitive(name, code):
    assert _effects.normalize_key(name) == code


def test_key_table_holds_exactly_the_allowed_codes():
    assert set(_effects.KEYS.values()) == ALLOWED_CODES
    for name, code in _effects.KEYS.items():
        assert name == name.lower(), f"KEYS names are lowercase: {name!r}"
        assert _effects.normalize_key(name) == code


@pytest.mark.skipif(not INPUT_EVENT_CODES.exists(), reason="kernel headers not installed")
def test_key_codes_are_real_evdev_names():
    defined = set(re.findall(r"#define\s+(KEY_\w+)\s", INPUT_EVENT_CODES.read_text()))
    assert set(_effects.KEYS.values()) <= defined


def test_modifiers_are_left_shift_and_left_ctrl():
    assert set(_effects.MODIFIERS) == {"KEY_LEFTSHIFT", "KEY_LEFTCTRL"}


@pytest.mark.parametrize("code, text", [("KEY_A", "A"), ("KEY_LEFTSHIFT", "Shift"), ("KEY_SPACE", "Space"),
                                        ("KEY_D", "D")])
def test_label(code, text):
    assert _effects.label(code) == text


def test_every_allowed_code_has_a_friendly_label():
    for code in ALLOWED_CODES:
        text = _effects.label(code)
        assert isinstance(text, str) and text.strip() and not text.startswith("KEY_"), (code, text)


# ---- refused and unknown keys ---------------------------------------------------------------------------------

def test_effect_error_is_a_value_error():
    assert issubclass(_effects.EffectError, ValueError)


@pytest.mark.parametrize("name", REFUSED)
def test_refused_keys_have_a_plain_reason(name):
    assert name in _effects.BLOCKED
    reason = _effects.BLOCKED[name]
    assert isinstance(reason, str) and reason.strip()
    for spelling in (name, name.upper()):
        with pytest.raises(_effects.EffectError) as info:
            _effects.normalize_key(spelling)
        assert reason.lower() in error_text(info.value).lower()


@pytest.mark.parametrize("name", REFUSED + NEVER_ACCEPTED)
def test_dangerous_keys_are_never_accepted(name):
    for effect in (name, f"tap {name}", f"{name}+tab", f"shift+{name}"):
        with pytest.raises(_effects.EffectError):
            _effects.parse_effect(effect)


def test_no_blocked_name_is_in_the_key_table():
    assert not set(_effects.BLOCKED) & set(_effects.KEYS)


@pytest.mark.parametrize("typo, suggestion", [("spcae", "space"), ("enterr", "enter"), ("lfet", "left"),
                                              ("shfit", "shift"), ("pagdown", "pagedown"), ("ctlr", "ctrl"),
                                              ("rigth", "right")])
def test_unknown_keys_get_a_did_you_mean(typo, suggestion):
    with pytest.raises(_effects.EffectError) as info:
        _effects.normalize_key(typo)
    assert suggestion in error_text(info.value)
    with pytest.raises(_effects.EffectError) as info:
        _effects.parse_effect(f"tap {typo}")
    assert suggestion in error_text(info.value)


def test_unknown_key_message_names_the_key():
    with pytest.raises(_effects.EffectError) as info:
        _effects.normalize_key("banana")
    assert "banana" in error_text(info.value)
    assert info.value.message.strip()


# ---- the grammar ----------------------------------------------------------------------------------------------

def check_keys(effect, modifiers, others):
    """Modifiers come first in Effect.keys; the order inside each group is not part of the contract."""
    keys = tuple(effect.keys)
    assert set(keys[:len(modifiers)]) == set(modifiers), keys
    assert set(keys[len(modifiers):]) == set(others), keys
    assert len(keys) == len(modifiers) + len(others), keys


@pytest.mark.parametrize("text, kind, modifiers, others, description", [
    ("a", "hold", [], ["KEY_A"], "hold A"),
    ("space", "hold", [], ["KEY_SPACE"], "hold Space"),
    ("shift+d", "hold", ["KEY_LEFTSHIFT"], ["KEY_D"], "hold Shift+D"),
    ("Shift + D", "hold", ["KEY_LEFTSHIFT"], ["KEY_D"], "hold Shift+D"),
    ("d+shift", "hold", ["KEY_LEFTSHIFT"], ["KEY_D"], None),
    ("a+space", "hold", [], ["KEY_A", "KEY_SPACE"], None),
    ("a + space", "hold", [], ["KEY_A", "KEY_SPACE"], None),
    ("ctrl+shift+a", "hold", ["KEY_LEFTCTRL", "KEY_LEFTSHIFT"], ["KEY_A"], None),
    ("tap space", "tap", [], ["KEY_SPACE"], "tap Space"),
    ("TAP Space", "tap", [], ["KEY_SPACE"], "tap Space"),
    ("tap ctrl+c", "tap", ["KEY_LEFTCTRL"], ["KEY_C"], None),
    ("tap shift + 1", "tap", ["KEY_LEFTSHIFT"], ["KEY_1"], None),
    ("ctrl+-", "hold", ["KEY_LEFTCTRL"], ["KEY_MINUS"], None),
    ("shift+=", "hold", ["KEY_LEFTSHIFT"], ["KEY_EQUAL"], None),
    ("shift", "hold", ["KEY_LEFTSHIFT"], [], None),
])
def test_effect_strings(text, kind, modifiers, others, description):
    effect = _effects.parse_effect(text)
    assert effect.kind == kind
    check_keys(effect, modifiers, others)
    if description is not None:
        assert effect.describe() == description


def test_none_does_nothing():
    effect = _effects.parse_effect(None)
    assert effect.kind == "none"
    assert tuple(effect.keys) == ()
    assert effect.describe() == "do nothing"


def test_functions_are_called():
    def left(world):
        pass

    def jump():
        pass

    effect = _effects.parse_effect(left, name="left")
    assert effect.kind == "call" and effect.func is left and tuple(effect.keys) == ()
    assert effect.describe() == "call left(world)"
    assert _effects.parse_effect(jump, name="jump").describe().startswith("call jump")


@pytest.mark.parametrize("text", ["", "   ", "tap", "tap ", "a+", "+a", "a++b", "a b", "hold a", "tap tap space",
                                  "shift+", "a,b"])
def test_malformed_effect_strings_are_refused(text):
    with pytest.raises(_effects.EffectError) as info:
        _effects.parse_effect(text)
    assert info.value.message.strip()


@pytest.mark.parametrize("text, words", [
    ("ctrl-c", ['"ctrl+c"', 'Join keys with "+"']),
    ("ctrl c", ['"ctrl+c"']),
    ("w,a", ['"w+a"']),
    ("tab space", ['"tap space"']),
    ("None", ["None without quotes"]),
    ("nothing", ["None without quotes"]),
    ("idle", ["None without quotes"]),
    ("click", ["mouse"]),
    ("left click", ["mouse"]),
    ("mouse1", ["mouse"]),
])
def test_common_beginner_spellings_get_a_fitting_suggestion(text, words):
    with pytest.raises(_effects.EffectError) as info:
        _effects.parse_effect(text)
    for word in words:
        assert word in error_text(info.value), error_text(info.value)
    assert '"backtick"' not in error_text(info.value) and '"delete"' not in error_text(info.value)
