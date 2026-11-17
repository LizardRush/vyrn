"""Real-time requests with fake grim, recorded keys and the fake focus helper (protocol.md sections 3.4, 3.5, 4.3,
5 and 6). No real key is ever pressed: every worker writes its key events to a file."""

import time

import numpy as np
import pytest
from PIL import Image
from PIL.PngImagePlugin import PngInfo

from testkit import wait_until

GAME = {
    "inputs.py": """
        import vyrn

        def see():
            return vyrn.screen(0, 0, 8, 6)
        """,
    "responses.py": """
        left = "a"
        right = "shift+d"
        jump = "tap space"
        wait = None
        """,
    "points.py": """
        import vyrn

        def points():
            r, g, b = vyrn.pixel(1, 1)
            return r

        def game_over():
            return vyrn.pixel(2, 2)[1] > 200

        def restart():
            vyrn.press("enter")
        """,
}
LEFT, RIGHT, JUMP, WAIT = range(4)


@pytest.fixture
def game(spawn, make_project):
    """A real-time project whose screen is a solid colour (red = points, green > 200 = game over)."""
    rig = spawn(make_project(GAME, sim=False))
    rig.grim.set(color=(10, 20, 30))
    return rig


def all_up_after(rig, mark=0, timeout=1.0):
    """Wait until every key pressed so far is released again; returns the new events."""
    rig.keys.wait_all_up(timeout)
    return rig.keys.actions(mark)


# ---- observe / act / restart / check_over / status ----------------------------------------------------------------

def test_spec_for_a_real_game(game):
    spec = game.load(allow_input=True)
    assert spec["mode"] == "realtime"
    assert spec["obs"]["kind"] == "picture" and spec["obs"]["shape"] == [6, 8, 3]
    assert (spec["preview"] == (10, 20, 30)).all()
    assert spec["choices"] == ["left", "right", "jump", "wait"]
    assert spec["effects"] == ["hold A", "hold Shift+D", "tap Space", "do nothing"]
    assert {name: spec["uses"][name] for name in ("screen", "keys")} == {"screen": True, "keys": True}
    assert set(name for name, where in spec["hooks"].items() if where) == {"see", "points", "game_over", "restart"}


def test_observe_check_over_and_restart(game):
    game.load(allow_input=True)
    first = game.observe(first=True)
    assert first["t"] == "result"
    assert first["obs"].shape == (6, 8, 3) and (first["obs"] == (10, 20, 30)).all()
    assert first["points"] is None and first["game_over"] is False
    assert first["focused"] is None, "no focus lock is active"

    game.grim.set(color=(42, 250, 0))
    result = game.observe()
    assert result["points"] == 42 and result["game_over"] is True
    assert (result["obs"] == (42, 250, 0)).all()

    reply = game.request("check_over")
    assert reply["t"] == "ok" and reply["game_over"] is True
    game.grim.set(color=(42, 20, 0))
    assert game.request("check_over")["game_over"] is False

    reply = game.request("restart")
    assert reply["t"] == "ok" and not reply.get("did")
    assert all_up_after(game) == [("down", "KEY_ENTER"), ("up", "KEY_ENTER")]


def test_whole_screen(spawn, make_project):
    rig = spawn(make_project({"inputs.py": "import vyrn\n\ndef see():\n    return vyrn.screen()\n"}, sim=False))
    rig.grim.set(width=40, height=30, color=(1, 2, 3))
    spec = rig.load()
    assert spec["obs"]["shape"] == [30, 40, 3]
    assert any(call["geometry"] is None for call in rig.grim.calls())


def test_counters_in_real_time(spawn, make_project):
    rig = spawn(make_project({
        "inputs.py": """
            import vyrn

            CODES = {None: 0, "go": 1, "stay": 2}

            def see():
                return [vyrn.tick, vyrn.round, CODES[vyrn.choice]]
            """,
        "responses.py": "def go():\n    pass\n\nstay = None\n",
        "points.py": """
            import vyrn

            def points():
                return vyrn.tick * 100 + vyrn.round
            """,
    }, sim=False))
    rig.load()
    assert rig.observe(first=True)["obs"].tolist() == [0, 1, 0]
    assert rig.act(1)["t"] == "ok"
    result = rig.observe()
    assert result["obs"].tolist() == [1, 1, 2] and result["points"] == 101
    rig.act(0)
    assert rig.observe()["obs"].tolist() == [2, 1, 1]
    assert rig.observe(first=True)["obs"].tolist() == [0, 2, 0]


def test_status(game):
    game.load(allow_input=True)
    status = game.status()
    assert status["t"] == "ok" and status["frozen"] is False and status["held"] == []
    assert status["active"] is None, "no focus helper is running"
    game.act(RIGHT)
    assert sorted(game.status()["held"]) == ["D", "Shift"]


def test_held_keys_are_diffed_on_every_act(spawn, make_project):
    rig = spawn(make_project({"responses.py": """
        a = "a"
        sd = "shift+d"
        sa = "shift+a"
        both = "a+space"
        tap = "tap space"
        tapcc = "tap ctrl+c"
        wait = None
        """}, sim=False))
    choices = rig.load(allow_input=True)["choices"]
    index = {name: i for i, name in enumerate(choices)}
    keys = rig.keys

    def act(name):
        mark = keys.mark()
        rig.act(index[name])
        return mark

    assert keys.actions(act("a")) == [("down", "KEY_A")]
    assert keys.actions(act("a")) == []
    assert keys.actions(act("sd")) == [("up", "KEY_A"), ("down", "KEY_LEFTSHIFT"), ("down", "KEY_D")]
    assert keys.actions(act("sa")) == [("up", "KEY_D"), ("down", "KEY_A")], "Shift stays down"
    assert keys.actions(act("both")) == [("up", "KEY_LEFTSHIFT"), ("down", "KEY_SPACE")]

    mark = act("tap")
    events = keys.wait_count(mark + 4)[mark:]
    actions = [(e.action, e.code) for e in events]
    assert sorted(actions[:2]) == [("up", "KEY_A"), ("up", "KEY_SPACE")], "a tap empties the held set"
    assert actions[2:] == [("down", "KEY_SPACE"), ("up", "KEY_SPACE")]
    assert 0.03 <= events[3].t - events[2].t <= 0.3, "taps are released about 50 ms later"

    mark = act("tapcc")
    keys.wait_count(mark + 4)
    assert keys.actions(mark) == [("down", "KEY_LEFTCTRL"), ("down", "KEY_C"), ("up", "KEY_C"),
                                  ("up", "KEY_LEFTCTRL")]

    mark = act("wait")
    time.sleep(0.1)
    assert keys.actions(mark) == []

    mark = act("sd")
    act("wait")
    assert keys.actions(mark) == [("down", "KEY_LEFTSHIFT"), ("down", "KEY_D"), ("up", "KEY_D"),
                                  ("up", "KEY_LEFTSHIFT")]

    mark = act("tap")
    act("a")
    time.sleep(0.15)
    assert keys.actions(mark) == [("down", "KEY_SPACE"), ("up", "KEY_SPACE"), ("down", "KEY_A")], \
        "a pending tap is released before the next key action, once"
    assert rig.status()["held"] == ["A"]


def test_act_does_not_wait_for_the_tap_release(game):
    game.load(allow_input=True)
    later = []
    for _ in range(3):
        mark = game.keys.mark()
        game.act(JUMP)
        replied = time.monotonic()
        events = game.keys.wait_count(mark + 2)[mark:]
        later.append(events[1].t > replied)
        time.sleep(0.05)
    assert any(later), "the reply comes before the tap's key-up"


# ---- input gating --------------------------------------------------------------------------------------------------

def test_input_not_allowed(game):
    game.load(allow_input=False)
    assert game.act(WAIT)["t"] == "ok", "doing nothing is not a key action"
    error = game.error("act", a=LEFT)
    assert error["kind"] == "input" and error["fatal"] is False
    assert "allowed to press keys" in error["message"]
    assert not game.keys.exists() or game.keys.actions() == []


def test_press_not_allowed(game):
    game.load(allow_input=False)
    error = game.error("restart")
    assert error["kind"] == "input"
    assert (error["script"], error["line"]) == ("points.py", game.project.line("points.py", 'vyrn.press("enter")'))
    assert not game.keys.exists() or game.keys.actions() == []


@pytest.mark.parametrize("allow_input", [False, True])
def test_check_mode_describes_and_presses_nothing(spawn, make_project, allow_input):
    project = make_project(dict(GAME, **{"points.py": """
        import vyrn

        def points():
            return 1

        def restart():
            vyrn.press("enter")
            vyrn.wait(5)
        """}), sim=False)
    rig = spawn(project)
    rig.load(check=True, allow_input=allow_input)
    assert rig.act(LEFT)["did"] == ["would hold A"]
    assert "would hold Shift+D" in rig.act(RIGHT)["did"]
    assert "would tap Space" in rig.act(JUMP)["did"]
    started = time.monotonic()
    did = rig.request("restart")["did"]
    assert time.monotonic() - started < 2.0, "vyrn.wait() does not sleep in check mode"
    assert any(text.startswith("would tap") and "enter" in text.lower() for text in did), did
    assert rig.observe(first=True)["t"] == "result"
    time.sleep(0.1)
    assert not rig.keys.exists() or rig.keys.actions() == [], "check mode never presses keys"


def test_check_mode_describes_function_choices_without_running_them(spawn, make_project, tmp_path):
    # PLAN.md section 2, Check mode: effects are described, never executed (a real-time function may move hardware).
    marker = tmp_path / "go-was-called"
    rig = spawn(make_project({"responses.py": f"""
        def go():
            open({str(marker)!r}, "w").close()

        wait = None
        """}, sim=False))
    rig.load(check=True)
    assert rig.act(0)["did"] == ["would call go()"]
    assert rig.act(1)["did"] == [], "doing nothing has nothing to describe"
    assert not marker.exists()


def test_lock_focus_needs_allow_input(game):
    game.focus.activate(1, "game", "My Game")
    game.load(allow_input=False, focus_helper=str(game.focus.program))
    error = game.error("lock_focus")
    assert error["kind"] in ("input", "focus") and error["fatal"] is False
    assert error["message"].strip()
    assert game.focus.lifecycle() == [], "the focus helper is not even started"
    assert not game.keys.exists() or game.keys.actions() == []

def test_wait_sleeps_outside_check_mode(spawn, make_project):
    rig = spawn(make_project({"points.py": """
        import vyrn

        def points():
            return 1

        def restart():
            vyrn.wait(0.3)
        """}, sim=False))
    rig.load()
    started = time.monotonic()
    rig.request("restart")
    assert time.monotonic() - started >= 0.28


# ---- focus ---------------------------------------------------------------------------------------------------------

def test_lock_focus_and_the_focus_guard(game):
    game.focus.activate(1, "game", "My Game")
    game.load(allow_input=True, focus_helper=str(game.focus.program))
    locked = game.request("lock_focus")
    assert locked["t"] == "ok" and locked["app_id"] == "game" and locked["title"] == "My Game"

    assert game.observe(first=True)["focused"] is True
    game.act(LEFT)
    assert game.keys.actions() == [("down", "KEY_A")]
    status = game.status()
    assert status["focused"] is True and status["active"]["id"] == 1 and status["held"] == ["A"]

    game.focus.activate(1, "game", "My Game - level 2")  # same window, new title: still focused
    wait_until(lambda: game.status()["active"]["title"] == "My Game - level 2", 2.0, "the new title")
    assert game.status()["focused"] is True

    game.focus.activate(2, "foot", "Terminal")
    # The watchdog lets go as soon as the game window loses focus, without waiting for a key action.
    game.keys.wait_all_up(1.0)
    assert game.keys.actions() == [("down", "KEY_A"), ("up", "KEY_A")]
    wait_until(lambda: (game.status()["active"] or {}).get("id") == 2, 2.0, "the terminal to become active")
    mark = game.keys.mark()
    assert game.act(RIGHT)["t"] == "ok"
    time.sleep(0.05)
    assert game.keys.actions(mark) == [], "nothing is pressed while unfocused"
    assert game.keys.held() == set()
    assert game.observe()["focused"] is False
    status = game.status()
    assert status["focused"] is False and status["held"] == []

    mark = game.keys.mark()
    game.request("restart")  # vyrn.press("enter") while unfocused: skipped
    game.act(JUMP)
    time.sleep(0.1)
    assert game.keys.actions(mark) == []

    game.focus.activate(1, "game", "My Game")
    wait_until(lambda: game.status()["focused"] is True, 2.0, "the game window to be active again")
    mark = game.keys.mark()
    game.act(RIGHT)
    assert game.keys.actions(mark) == [("down", "KEY_LEFTSHIFT"), ("down", "KEY_D")]
    assert game.observe()["focused"] is True

    game.focus.close_window(1)
    wait_until(lambda: game.status()["focused"] is False, 2.0, "the closed window to lose focus")
    mark = game.keys.mark()
    game.act(LEFT)
    assert game.keys.held() == set()
    assert ("down", "KEY_A") not in game.keys.actions(mark)


@pytest.mark.parametrize("setup", ["nothing-active", "vyrn-active", "helper-missing", "helper-error"])
def test_lock_focus_errors(game, setup):
    helper = str(game.focus.program)
    if setup == "vyrn-active":
        game.focus.activate(7, "vyrn", "Vyrn - my game")
    elif setup == "helper-missing":
        helper = str(game.project / "no-such-helper")
    elif setup == "helper-error":
        game.focus.fail("the compositor has no foreign-toplevel protocol")
    game.load(allow_input=True, focus_helper=helper)
    error = game.error("lock_focus")
    assert error["kind"] == "focus" and error["fatal"] is False
    assert error["message"].strip()
    assert not game.keys.exists() or game.keys.actions() == []


def test_a_failed_relock_keeps_the_focus_guard_on(game):
    """The locked helper dies and a new lock_focus fails: the old lock keeps failing closed (no key may go to
    whatever window is in front), instead of turning the focus guard off."""
    game.focus.activate(1, "game", "My Game")
    game.load(allow_input=True, focus_helper=str(game.focus.program))
    game.request("lock_focus")
    game.focus.fail("the compositor went away")       # the helper prints an error and exits
    wait_until(lambda: game.status()["focused"] is False, 2.0, "the dead helper to count as unfocused")
    assert game.error("lock_focus")["kind"] == "focus"  # a fresh helper fails the same way
    status = game.status()
    assert status["focused"] is False
    game.act(LEFT)
    assert game.observe(first=True)["focused"] is False, "a lock is still active: false, not null"
    assert not game.keys.exists() or game.keys.actions() == []


def test_a_new_lock_replaces_a_dead_one(game):
    game.focus.activate(1, "game", "My Game")
    game.load(allow_input=True, focus_helper=str(game.focus.program))
    game.request("lock_focus")
    game.focus.fail("the compositor went away")
    wait_until(lambda: game.status()["focused"] is False, 2.0, "the dead helper to count as unfocused")
    game.focus._config["error"] = None                 # the desktop is back
    game.focus.activate(3, "game", "My Game")
    assert game.request("lock_focus")["app_id"] == "game"
    game.act(LEFT)
    assert game.keys.actions() == [("down", "KEY_A")]
    assert game.observe(first=False)["focused"] is True


def test_nothing_is_pressed_while_the_scripts_load(spawn, make_project):
    """load runs the scripts' top-level code and see() before any lock_focus: key actions there are skipped (with a
    warning), even with allow_input, so they can't type into whatever window is active (Vyrn's editor, a terminal)."""
    project = make_project(dict(GAME, **{"inputs.py": """
        import vyrn

        vyrn.press("enter")

        def see():
            vyrn.press("y")
            return vyrn.screen(0, 0, 8, 6)
        """}), sim=False)
    rig = spawn(project)
    rig.focus.activate(2, "foot", "Terminal")
    spec = rig.load(allow_input=True, focus_helper=str(rig.focus.program))
    time.sleep(0.1)
    assert not rig.keys.exists() or rig.keys.actions() == []
    assert rig.focus.lifecycle() == [], "no focus helper was needed"
    lines = {(w["script"], w["line"]) for w in spec["warnings"] if "skipped" in w["message"]}
    assert lines == {("inputs.py", project.line("inputs.py", 'press("enter")')),
                     ("inputs.py", project.line("inputs.py", 'press("y")'))}
    rig.observe(first=True)  # once loaded, see() presses as usual
    rig.keys.wait_count(2)
    assert rig.keys.actions()[:2] == [("down", "KEY_Y"), ("up", "KEY_Y")]


def test_check_says_it_does_not_run_real_time_choice_functions(spawn, make_project):
    rig = spawn(make_project({"responses.py": "def jump():\n    pass\n\nleft = 'a'\nwait = None\n"}, sim=False))
    warnings = [w["message"] for w in rig.load(check=True)["warnings"]]
    assert any("jump()" in w and "Check doesn't run" in w for w in warnings), warnings


def test_close_stops_the_focus_helper(game):
    game.focus.activate(1, "game", "My Game")
    game.load(allow_input=True, focus_helper=str(game.focus.program))
    game.request("lock_focus")
    assert game.focus.lifecycle()[0].startswith("start")
    assert game.worker.close() == 0
    wait_until(lambda: any(line.startswith("exit") for line in game.focus.lifecycle()), 1.0, "the helper to exit")


# ---- the screen: one capture per request ----------------------------------------------------------------------------

def coords(x, y, w, h, blue=7):
    ys, xs = np.mgrid[y:y + h, x:x + w]
    return np.stack([xs % 256, ys % 256, np.full_like(xs, blue)], axis=-1).astype(np.uint8)


def test_one_capture_per_request(spawn, make_project):
    rig = spawn(make_project({
        "inputs.py": """
            import vyrn

            def see():
                whole = vyrn.screen(5, 4, 10, 8)
                vyrn.show("part", vyrn.screen(7, 6, 3, 2))
                return whole
            """,
        "responses.py": "def go():\n    pass\n\nstay = None\n",
        "points.py": """
            import vyrn

            def points():
                r, g, b = vyrn.pixel(30, 20)
                return r + 1000 * g

            def game_over():
                r, g, b = vyrn.pixel(6, 5)
                return (r, g) != (6, 5)
            """,
    }, sim=False))
    rig.grim.set(width=64, height=48, pattern="coords", color=(0, 0, 7))
    count = rig.grim.count

    before = count()
    spec = rig.load()
    assert count() - before == 1
    np.testing.assert_array_equal(spec["preview"], coords(5, 4, 10, 8))

    before = count()
    first = rig.observe(first=True)  # see() only
    assert count() - before == 1
    np.testing.assert_array_equal(first["obs"], coords(5, 4, 10, 8))

    before = count()
    result = rig.observe()  # see, then points() needs a new area: the union grows, this tick pays twice
    assert count() - before == 2
    assert result["points"] == 30 + 1000 * 20 and result["game_over"] is False

    before = count()
    result = rig.observe()  # the union is captured once and every area is cropped from it
    assert count() - before == 1
    np.testing.assert_array_equal(result["obs"], coords(5, 4, 10, 8))
    assert result["points"] == 20030 and result["game_over"] is False
    part = (result.get("show") or {}).get("part")
    if part is not None:
        np.testing.assert_array_equal(part, coords(7, 6, 3, 2))

    before = count()
    rig.status()
    rig.act(1)
    assert count() == before, "requests that run no screen code capture nothing"


def test_on_screen_in_a_worker(spawn, make_project, tmp_path):
    target = (np.arange(60, dtype=np.uint8).reshape(4, 5, 3) * 4)
    screen = np.zeros((48, 64, 3), dtype=np.uint8)
    screen[10:14, 20:25] = target
    Image.fromarray(screen).save(tmp_path / "screen.png")
    project = make_project({"inputs.py": """
        import vyrn

        def see():
            return [vyrn.on_screen("target.png", x=20, y=10), vyrn.on_screen("target.png", x=0, y=0),
                    vyrn.on_screen("area.png"), vyrn.on_screen("game_over.png", x=0, y=0)]
        """}, sim=False)
    (project / "pictures").mkdir()
    Image.fromarray(target).save(project / "pictures" / "target.png")
    info = PngInfo()
    info.add_text("vyrn-area", "20,10,5,4")
    Image.fromarray(target).save(project / "pictures" / "area.png", pnginfo=info)
    rig = spawn(project)
    rig.grim.set(image=tmp_path / "screen.png")
    spec = rig.load()
    results = [rig.observe(first=True), rig.observe(), rig.observe()]
    for result in results:
        assert result["obs"].tolist() == [1.0, 0.0, 1.0, 0.0]
    warnings = list(spec.get("warnings") or []) + [w for r in results for w in r.get("warnings") or []]
    missing = [w for w in warnings if "game_over.png" in w["message"]]
    assert len(missing) == 1, warnings
    assert "Snapshot" in missing[0]["message"]
