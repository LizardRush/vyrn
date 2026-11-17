"""Vyrn Link in the worker (protocol.md sections 3.2, 3.4 and 4.3; docs/dev/link.md): vyrn.game, vyrn.events,
vyrn.connected, vyrn.send() and the "game ..." effects. The app hosts the WebSocket server; here the requests carry
the game's state and the replies carry the AI's buttons and messages, as the app sends and reads them."""

import json
import subprocess
import sys

import numpy as np
import pytest

import vyrn
from vyrn import _effects, _link
from vyrn._state import VyrnUserError

from testkit import TOOLS_DIR

GAME = {
    "inputs.py": """
        import vyrn

        def see():
            return [vyrn.game.get("y", 0), len(vyrn.events), int(vyrn.connected)]
        """,
    "responses.py": """
        jump = "game jump"
        tap_jump = "tap game jump"
        both = "game left+Jump"
        wait = None
        """,
    "points.py": """
        import vyrn

        def points():
            if any(event.get("name") == "death" for event in vyrn.events):
                return -10
            return vyrn.game["x"] / 10

        def game_over():
            return vyrn.game.get("dead", False)

        def restart():
            vyrn.send("restart")
        """,
}
JUMP, TAP_JUMP, BOTH, WAIT = range(4)
STATE = {"type": "state", "frame": 1234, "x": 512.0, "y": 105.0, "dead": False}
# The state docs/user/vyrn-link.md suggests for Geometry Dash.
GD_STATE = {"type": "state", "frame": 1234, "x": 512.0, "y": 105.0, "speed_y": 0.0, "on_ground": 1, "mode": 0,
            "percent": 12.5, "dead": False, "ahead": [90, 0, 1, 150, 30, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0]}


def obs(reply):
    return np.asarray(reply["obs"]).tolist()


# ---- effects ----------------------------------------------------------------------------------------------------

@pytest.mark.parametrize("text, kind, buttons, described", [
    ("game jump", "game_hold", ("jump",), "hold game button jump"),
    ("tap game jump", "game_tap", ("jump",), "tap game button jump"),
    ("  Game  Jump + Duck ", "game_hold", ("jump", "duck"), "hold game buttons jump+duck"),
    ("TAP GAME p2_jump", "game_tap", ("p2_jump",), "tap game button p2_jump"),
    ("game left+left", "game_hold", ("left",), "hold game button left"),
])
def test_game_effects(text, kind, buttons, described):
    effect = _effects.parse_effect(text)
    assert (effect.kind, effect.buttons, effect.keys, effect.describe()) == (kind, buttons, (), described)
    assert _effects.is_game_effect(text)


@pytest.mark.parametrize("text, words", [
    ("game", "needs the name of a game button"),
    ("tap game", "needs the name of a game button"),
    ("game jump high", "is one word"),
    ("game jump+", "name is missing"),
    ("game ju!mp", "can't be a game button name"),
    ("game tap jump", 'Write "tap game jump"'),
    ("hold game jump", 'Write just "game jump"'),
])
def test_bad_game_effects_explain_themselves(text, words):
    with pytest.raises(_effects.EffectError) as info:
        _effects.parse_effect(text, "jump")
    assert words in info.value.message
    assert info.value.message.startswith(f'jump = "{text}": ')
    assert "game button" in info.value.hint


def test_keys_are_still_keys():
    assert not _effects.is_game_effect("g") and not _effects.is_game_effect("tap space")
    assert _effects.parse_effect("g").describe() == "hold G"
    assert _effects.parse_effect("tap g").kind == "tap"


# ---- vyrn.game, vyrn.send() in this process ---------------------------------------------------------------------

def test_vyrn_game_explains_a_missing_value():
    empty = _link.game_state(None)
    assert empty == {} and _link.game_state([1, 2]) == {}
    with pytest.raises(KeyError) as info:            # still a KeyError for  except KeyError
        empty["y"]
    assert "no state has come from the game" in str(info.value)
    assert "vyrn.game.get('y', 0)" in info.value.hint
    state = _link.game_state(STATE)
    assert state["x"] == 512.0 and state.get("speed_y", 0) == 0 and isinstance(state, dict)
    with pytest.raises(_link.MissingGameValue) as info:
        state["speed_y"]
    assert info.value.message == "vyrn.game has no 'speed_y': the game sent frame, x, y, dead."
    assert _link.game_events([{"name": "death"}, "junk", 3]) == [{"name": "death"}]
    assert _link.game_events(None) == []


@pytest.fixture
def outbox(state, monkeypatch):
    """vyrn._state.S as a real-time run, with an empty vyrn.send() queue."""
    monkeypatch.setattr(state, "outbox", [])
    monkeypatch.setattr(state, "outbox_bytes", 0)
    return state


def test_send_queues_plain_json(outbox):
    vyrn.send("restart")
    vyrn.send("speed", value=np.float32(2.0), steps=np.int64(3), flags=np.array([1, 0], dtype=np.uint8),
              deep={"a": (1, 2.5, None, True, np.bool_(False))})
    vyrn.send({"type": "pause"})
    vyrn.send({"type": "resume"}, at=5)
    sent = _link.take_outbox()
    assert sent == [{"type": "restart"},
                    {"type": "speed", "value": 2.0, "steps": 3, "flags": [1, 0],
                     "deep": {"a": [1, 2.5, None, True, False]}},
                    {"type": "pause"},
                    {"type": "resume", "at": 5}]
    assert type(sent[1]["steps"]) is int and type(sent[1]["value"]) is float
    json.dumps(sent, allow_nan=False)
    assert _link.take_outbox() == []


@pytest.mark.parametrize("args, fields, words", [
    (("",), {}, "needs the type of message"),
    ((3,), {}, "needs the type of message"),
    (({"value": 1},), {}, "needs the type of message"),
    (("input",), {"hold": ["jump"]}, 'Vyrn Link uses "input" messages'),
    (("stop",), {}, 'Vyrn Link uses "stop" messages'),
    (("x" * 65,), {}, "a short type"),
    (("speed",), {"value": float("nan")}, "a game only understands ordinary numbers"),
    (("speed",), {"value": object()}, "can't send object values"),
    (("speed",), {"value": {1: 2}}, "text keys"),
])
def test_send_refuses_what_a_game_cant_use(outbox, args, fields, words):
    with pytest.raises(VyrnUserError) as info:
        vyrn.send(*args, **fields)
    assert words in info.value.message
    assert _link.take_outbox() == []


def test_send_in_a_check_is_only_described(outbox):
    outbox.check = True
    vyrn.send("speed", value=2)
    assert outbox.did == ['would send {"type":"speed","value":2}']
    assert _link.take_outbox() == []


def test_send_while_loading_is_skipped(outbox):
    outbox.loading = True
    vyrn.send("restart")
    assert _link.take_outbox() == []
    assert len(outbox.warnings) == 1 and "sent nothing" in outbox.warnings[0]["message"]


def test_send_in_a_simulation_is_an_error(outbox):
    outbox.mode = "sim"
    with pytest.raises(VyrnUserError, match="a simulation has no game"):
        vyrn.send("restart")


def test_send_has_limits(outbox):
    for _ in range(_link.MAX_SENDS):
        vyrn.send("tick")
    with pytest.raises(VyrnUserError, match=f"more than {_link.MAX_SENDS} times"):
        vyrn.send("tick")
    assert len(_link.take_outbox()) == _link.MAX_SENDS
    with pytest.raises(VyrnUserError, match="can't send 600 KiB"):
        vyrn.send("big", data="x" * (600 * 1024))
    assert _link.take_outbox() == []


# ---- the worker ---------------------------------------------------------------------------------------------------

@pytest.fixture
def linked(spawn, make_project):
    return spawn(make_project(GAME, sim=False))


def test_spec_lists_the_game_buttons(linked):
    spec = linked.load()
    assert spec["effects"] == ["hold game button jump", "tap game button jump", "hold game buttons left+jump",
                               "do nothing"]
    assert spec["buttons"] == ["jump", "left"]
    assert spec["uses"] == {"screen": False, "keys": False, "game": True}
    assert spec["obs"]["shape"] == [3]
    assert obs({"obs": spec["preview"]}) == [0.0, 0.0, 0.0], "vyrn.game is {} while the scripts load"


def test_requests_bring_the_game(linked):
    linked.load()
    first = linked.request("observe", first=True, game=STATE, game_events=[], connected=True)
    assert obs(first) == [105.0, 0.0, 1.0]
    death = linked.request("observe", first=False, game=dict(STATE, x=100.0, y=8.0), connected=True,
                           game_events=[{"type": "event", "name": "death"}, {"type": "event", "name": "reset"}])
    assert obs(death) == [8.0, 2.0, 1.0] and death["points"] == -10
    # Without "game" and "connected" the values stay; without "game_events" there are no events this tick.
    same = linked.request("observe", first=False)
    assert obs(same) == [8.0, 0.0, 1.0] and same["points"] == 10.0
    gone = linked.error("observe", first=False, game={}, connected=False)
    assert gone["message"] == "vyrn.game has no 'x' yet: no state has come from the game (vyrn.game is {})."
    assert gone["script"] == "points.py" and gone["line"] == 6
    assert linked.request("check_over", game=dict(STATE, dead=True))["game_over"] is True
    assert linked.request("check_over", game=STATE)["game_over"] is False


def test_act_returns_the_game_buttons_and_presses_no_key(linked):
    linked.load()
    linked.request("observe", first=True, game=STATE, connected=True)
    assert linked.act(JUMP)["game_input"] == {"hold": ["jump"], "tap": []}
    assert linked.act(TAP_JUMP)["game_input"] == {"hold": [], "tap": ["jump"]}
    reply = linked.act(BOTH)
    assert reply["game_input"] == {"hold": ["left", "jump"], "tap": []} and reply["did"] == []
    assert linked.act(WAIT)["game_input"] == {"hold": [], "tap": []}
    assert not linked.keys.exists() or linked.keys.actions() == [], "game buttons are not keys on this computer"


def test_messages_go_out_with_the_reply(linked):
    linked.load()
    reply = linked.request("restart", game=dict(STATE, dead=True), connected=True)
    assert reply["send"] == [{"type": "restart"}]
    assert "send" not in linked.request("check_over", game=STATE), "each message goes out once"


def test_a_check_describes_buttons_and_messages(spawn, make_project):
    rig = spawn(make_project(GAME, sim=False))
    rig.load(check=True)
    reply = rig.act(JUMP)
    assert reply["did"] == ["would hold game button jump"] and reply["game_input"] == {"hold": [], "tap": []}
    assert rig.act(TAP_JUMP)["did"] == ["would tap game button jump"]
    restart = rig.request("restart")
    assert restart["did"] == ['would send {"type":"restart"}'] and "send" not in restart


def test_a_failed_request_sends_nothing(spawn, make_project):
    rig = spawn(make_project(dict(GAME, **{"points.py": """
        import vyrn

        def points():
            vyrn.send("oops")
            return vyrn.game["missing"]

        def restart():
            vyrn.send()
        """}), sim=False))
    rig.load()
    rig.request("observe", first=True, game=STATE, connected=True)
    error = rig.error("observe", first=False, game=STATE)
    assert "send" not in error
    assert error["message"] == "vyrn.game has no 'missing': the game sent frame, x, y, dead."
    assert (error["script"], error["line"]) == ("points.py", 5)
    assert "vyrn.game.get('missing', 0)" in error["hint"]
    wrong_call = rig.error("restart")
    assert 'vyrn.send("restart")' in wrong_call["hint"] and "send" not in wrong_call
    assert "send" not in rig.request("check_over", game=STATE), '"oops" was dropped with the failed request'


def test_scripts_that_misuse_the_link(spawn, make_project):
    sim = spawn(make_project({"responses.py": 'jump = "game jump"\nstay = None\n'})).load_error()
    assert "a simulation has no game to press buttons in" in sim["message"] and sim["line"] == 1

    press = spawn(make_project(dict(GAME, **{"points.py": """
        import vyrn

        def points():
            return 0

        def restart():
            vyrn.press("game jump")
        """}), sim=False)).load_error()
    assert "presses keys on this computer, not buttons in a game" in press["message"] and press["line"] == 7

    loading = spawn(make_project(dict(GAME, **{"inputs.py": """
        import vyrn
        from vyrn import game

        vyrn.send("hello_there")

        def see():
            return [game.get("y", 0)]
        """}), sim=False)).load()
    messages = [w["message"] for w in loading["warnings"]]
    assert any("from vyrn import game copies the value once" in m for m in messages), messages
    assert any("vyrn.send() at the top of a file sent nothing" in m for m in messages), messages
    assert "send" not in loading


# ---- the templates ------------------------------------------------------------------------------------------------

@pytest.mark.parametrize("template", ["link-runner"])
def test_link_templates_pass_check_without_a_game(fakes, copy_template, template):
    project = copy_template(template)
    result = subprocess.run([sys.executable, str(TOOLS_DIR / "check.py"), str(project)], env=fakes.env(),
                            capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, result.stdout + result.stderr
    for words in ("hold game button jump", "Uses: a game connected with Vyrn Link", "No warnings.",
                  'restart(): would send {"type":"restart"}'):
        assert words in result.stdout, result.stdout
    assert not fakes.keys.exists() or fakes.keys.actions() == []
    settings = json.loads((project.path / "vyrn.json").read_text(encoding="utf-8"))
    assert settings["link"] == {"port": 7457} and settings["template"] == template
    assert (project.path / "CLAUDE.md").read_text(encoding="utf-8") == "@AGENTS.md\n"
    assert "## Vyrn Link" in (project.path / "AGENTS.md").read_text(encoding="utf-8")


