"""Simulation requests: reset/step, auto-restart, truncation, points/game_over conversion, counters, show
(protocol.md sections 3.3, 3.6 and 4.1)."""

import time

import numpy as np
import pytest

COUNTER = {
    "world.py": """
        def restart(world):
            world.n = 0
            world.restarts = getattr(world, "restarts", 0) + 1

        def step(world):
            world.n += 1
        """,
    "inputs.py": """
        import vyrn

        CODES = {None: 0, "left": 1, "right": 2, "stay": 3}

        def see(world):
            return {"n": world.n, "restarts": world.restarts, "tick": vyrn.tick, "round": vyrn.round,
                    "choice": CODES[vyrn.choice]}
        """,
    "responses.py": """
        def left(world):
            world.last = "left"

        def right(world):
            world.last = "right"

        stay = None
        """,
    "points.py": """
        def points(world):
            return world.n * 10

        def game_over(world):
            return world.n >= 3
        """,
}
NAMES = ["n", "restarts", "tick", "round", "choice"]


def obs_info(spec):
    return {key: spec["obs"].get(key) for key in ("kind", "shape", "names")}


def seen(obs):
    """The counter project's observation as a dict."""
    assert obs.dtype == np.float32
    return dict(zip(NAMES, (int(v) for v in obs)))


def test_reset_and_step(spawn, make_project):
    rig = spawn(make_project(COUNTER))
    spec = rig.load()
    assert obs_info(spec) == {"kind": "numbers", "shape": [5], "names": NAMES}

    first = rig.reset()
    assert first["t"] == "result" and first["first"] is True and first["points"] is None
    assert not first.get("game_over") and not first.get("truncated") and first.get("final_obs") is None
    assert seen(first["obs"]) == {"n": 0, "restarts": 2, "tick": 0, "round": 1, "choice": 0}

    result = rig.step(0)
    assert seen(result["obs"]) == {"n": 1, "restarts": 2, "tick": 1, "round": 1, "choice": 1}
    assert result["points"] == 10 and isinstance(result["points"], (int, float))
    assert (result["game_over"], result["truncated"], result["first"], result["final_obs"]) == (False, False, False,
                                                                                                None)
    result = rig.step(1)
    assert seen(result["obs"]) == {"n": 2, "restarts": 2, "tick": 2, "round": 1, "choice": 2}
    assert result["points"] == 20

    end = rig.step(2)  # n reaches 3: game over, then the worker restarts by itself
    assert (end["game_over"], end["truncated"], end["first"]) == (True, False, True)
    assert end["points"] == 30
    assert seen(end["final_obs"]) == {"n": 3, "restarts": 2, "tick": 3, "round": 1, "choice": 3}
    assert seen(end["obs"]) == {"n": 0, "restarts": 3, "tick": 0, "round": 2, "choice": 0}

    result = rig.step(0)
    assert seen(result["obs"]) == {"n": 1, "restarts": 3, "tick": 1, "round": 2, "choice": 1}
    assert result["first"] is False and result["final_obs"] is None

    again = rig.reset()
    assert seen(again["obs"]) == {"n": 0, "restarts": 4, "tick": 0, "round": 3, "choice": 0}


def test_truncated_at_the_round_limit(spawn, make_project):
    rig = spawn(make_project(dict(COUNTER, **{"points.py": "def points(world):\n    return world.n * 10\n"})))
    rig.load(round_limit=2)
    rig.reset()
    assert rig.step(0)["truncated"] is False
    end = rig.step(0)
    assert (end["game_over"], end["truncated"], end["first"]) == (False, True, True)
    assert end["points"] == 20
    assert seen(end["final_obs"])["tick"] == 2 and seen(end["final_obs"])["round"] == 1
    assert seen(end["obs"]) == {"n": 0, "restarts": 3, "tick": 0, "round": 2, "choice": 0}
    assert seen(rig.step(1)["obs"])["round"] == 2


def test_game_over_wins_over_truncation(spawn, make_project):
    rig = spawn(make_project(COUNTER))
    rig.load(round_limit=3)
    rig.reset()
    rig.step(0)
    rig.step(0)
    end = rig.step(0)
    assert (end["game_over"], end["truncated"], end["first"]) == (True, False, True)


def test_game_over_truthiness(spawn, make_project):
    values = ["None", "0", "''", "[]", "'yes'", "1", "[0]", "np.bool_(True)", "np.bool_(False)", "0.0"]
    expected = [False, False, False, False, True, True, True, True, False, False]
    rig = spawn(make_project({"points.py": f"""
        import numpy as np

        VALUES = [{', '.join(values)}]
        calls = [0]

        def points(world):
            return 0

        def game_over(world):
            calls[0] += 1
            return VALUES[calls[0] - 1]
        """}))
    rig.load()
    rig.reset()
    assert [rig.step(1)["game_over"] for _ in values] == expected


def test_no_game_over_hook_means_never_over(spawn, make_project):
    rig = spawn(make_project())
    rig.load()
    rig.reset()
    assert [rig.step(1)["game_over"] for _ in range(5)] == [False] * 5


def test_points_conversions(spawn, make_project):
    project = make_project({"points.py": """
        import numpy as np

        VALUES = [None, None, True, False, np.float32(2.5), np.int64(3), 7, -1.5, np.uint8(200)]
        calls = [0]

        def points(world):
            calls[0] += 1
            return VALUES[calls[0] - 1]
        """})
    rig = spawn(project)
    rig.load()
    rig.reset()
    results = [rig.step(1) for _ in range(9)]
    assert [r["points"] for r in results] == [0, 0, 1, 0, 2.5, 3, 7, -1.5, 200]
    assert all(isinstance(r["points"], (int, float)) and not isinstance(r["points"], bool) for r in results)
    warnings = [r.get("warnings") or [] for r in results]
    assert len(warnings[0]) == 1 and sum(map(len, warnings)) == 1, "None counts as 0 with a one-time warning"
    assert warnings[0][0]["script"] == "points.py" and warnings[0][0]["message"].strip()


@pytest.mark.parametrize("value", ["float('nan')", "float('inf')", "-float('inf')", "'3'", "[1]", "{'a': 1}",
                                   "np.float32('nan')"], ids=["nan", "inf", "-inf", "text", "list", "dict",
                                                              "numpy-nan"])
def test_bad_points_point_at_the_def_line(spawn, make_project, value):
    project = make_project({"points.py": f"""
        import numpy as np


        def points(world):
            return {value}
        """})
    rig = spawn(project)
    rig.load()
    rig.reset()
    error = rig.error("step", a=1)
    assert error["kind"] == "bad_return" and error["fatal"] is False
    assert (error["script"], error["line"], error["func"]) == ("points.py", project.line("points.py", "def points"),
                                                               "points")
    assert error["message"].strip()


def test_restart_and_step_hooks_are_optional(spawn, make_project):
    rig = spawn(make_project({
        "world.py": "# a world with no hooks: the choices change it\n",
        "inputs.py": """
            import vyrn

            def see(world):
                return [getattr(world, "n", 0), vyrn.tick]
            """,
        "responses.py": """
            def bump(world):
                world.n = getattr(world, "n", 0) + 1

            stay = None
            """,
        "points.py": "def points(world):\n    return getattr(world, 'n', 0)\n",
    }))
    assert rig.load()["mode"] == "sim"
    assert rig.reset()["obs"].tolist() == [0, 0]
    result = rig.step(0)
    assert result["obs"].tolist() == [1, 1] and result["points"] == 1
    assert rig.step(1)["obs"].tolist() == [1, 2]


def test_show_sends_only_changed_values(spawn, make_project):
    rig = spawn(make_project({"inputs.py": """
        import vyrn

        def see(world):
            vyrn.show("speed", world.n * 1.5)
            vyrn.show("count", 3)
            vyrn.show("label", "x" * 300 if world.n == 2 else "same")
            vyrn.show("view", vyrn.canvas(3, 2, "blue" if world.n == 4 else "red"))
            return [world.n]
        """}))
    rig.load()
    rig.reset()
    shows = [rig.step(1).get("show") or {} for _ in range(4)]  # n = 1, 2, 3, 4
    assert [s.get("speed") for s in shows] == [1.5, 3.0, 4.5, 6.0]
    assert all(isinstance(s["speed"], float) for s in shows)
    assert "count" not in shows[0] and "label" not in shows[0], shows[0]
    assert shows[1]["label"] == "x" * 200, "text is cut to 200 characters"
    assert shows[2]["label"] == "same"
    assert "label" not in shows[3]
    view = shows[3]["view"]
    assert isinstance(view, np.ndarray) and view.dtype == np.uint8 and view.shape == (2, 3, 3)
    assert (view == (0, 0, 255)).all()


def test_show_numbers_are_floats(spawn, make_project):
    rig = spawn(make_project({"inputs.py": """
        import vyrn

        def see(world):
            vyrn.show("count", world.n)
            return [world.n]
        """}))
    rig.load()
    rig.reset()
    shows = [rig.step(1).get("show") or {} for _ in range(2)]
    assert [s.get("count") for s in shows] == [1.0, 2.0]
    assert all(isinstance(s["count"], float) for s in shows)


def test_timings(spawn, make_project):
    rig = spawn(make_project(COUNTER))
    rig.load()
    rig.reset()
    ms = rig.step(0)["ms"]
    for hook in ("see", "points", "game_over"):
        assert isinstance(ms[hook], (int, float)) and 0 <= ms[hook] < 5000


def test_simulations_never_create_a_keyboard(spawn, make_project):
    project = make_project({"responses.py": """
        import vyrn

        def grow(world):
            world.n += 1

        def jump(world):
            vyrn.press("space")

        stay = None
        """})
    rig = spawn(project)
    rig.load(allow_input=True)
    rig.reset()
    for _ in range(5):
        rig.step(0)
    assert not rig.keys.exists() or rig.keys.actions() == []
    error = rig.error("step", a=1)
    assert error["kind"] == "input" and error["fatal"] is False
    assert (error["script"], error["line"]) == ("responses.py", project.line("responses.py", 'vyrn.press("space")'))
    time.sleep(0.1)
    assert not rig.keys.exists() or rig.keys.actions() == []


def test_simulations_cannot_look_at_the_screen(spawn, make_project):
    project = make_project({"inputs.py": """
        import vyrn

        def see(world):
            return vyrn.screen(0, 0, 10, 10)
        """})
    error = spawn(project).load_error()
    assert error["kind"] == "input"
    assert (error["script"], error["line"]) == ("inputs.py", project.line("inputs.py", "vyrn.screen("))


def test_screen_use_during_a_step_is_an_input_error(spawn, make_project):
    project = make_project({"points.py": """
        import vyrn

        def points(world):
            if world.n > 0:
                return vyrn.pixel(1, 1)[0]
            return 0
        """})
    rig = spawn(project)
    rig.load()
    rig.reset()
    error = rig.error("step", a=1)
    assert error["kind"] == "input" and error["fatal"] is False
    assert (error["script"], error["line"]) == ("points.py", project.line("points.py", "vyrn.pixel("))
    assert rig.fakes.grim.count() == 0 or all(c["geometry"] is None for c in rig.fakes.grim.calls())


def test_the_catch_templates_play(spawn, copy_template):
    for name, kind in (("catch-pictures", "picture"), ("catch-numbers", "numbers")):
        rig = spawn(copy_template(name))
        spec = rig.load()
        assert spec["mode"] == "sim" and spec["obs"]["kind"] == kind
        if kind == "picture":
            assert spec["obs"]["shape"] == [10, 10, 3]
            preview = spec["preview"]
            assert (preview[9, 4:7] == preview[9, 4]).all() and preview[9, 4].any(), "the basket"
            assert (preview[0] == 255).all(axis=1).sum() == 1, "one white fruit on the top row"
        else:
            assert obs_info(spec) == {"kind": "numbers", "shape": [3], "names": ["fruit x", "fruit y", "basket"]}
        rig.reset()
        results = [rig.step(i % 3) for i in range(27)]
        assert {r["points"] for r in results} <= {-1, 0, 1}
        assert [i for i, r in enumerate(results) if r["game_over"]] == [8, 17, 26], "a fruit falls in 9 ticks"
