"""Worker start-up and `load`: hello, spec inference for every see() return type, choice discovery and hook rules
(protocol.md sections 3.1, 3.2, 4.1, 4.2, 4.4). Every test talks to a real worker through tools/engine.py."""

import os
import platform

import numpy as np
import pytest
from PIL import Image, ImageColor

# ---- hello ------------------------------------------------------------------------------------------------------


def test_hello(spawn, make_project):
    rig = spawn(make_project())
    hello = rig.hello
    assert hello["t"] == "hello" and hello["id"] == 0 and hello["proto"] == 1
    assert isinstance(hello["vyrn"], str) and hello["vyrn"].strip()
    assert hello["python"] == platform.python_version()
    assert hello["pid"] == rig.pid
    backends, problems = hello["backends"], hello.get("problems") or {}
    assert {"screen", "keys", "focus"} <= set(backends)
    for name in ("screen", "keys", "focus"):
        assert backends[name] is None or (isinstance(backends[name], str) and backends[name].strip())
        if backends[name] is None:
            assert isinstance(problems.get(name), str) and problems[name].strip(), f"no problem text for {name}"
    assert backends["screen"] == "grim", "fake grim + WAYLAND_DISPLAY: the screen backend works"


def test_hello_without_a_wayland_display(spawn, make_project):
    rig = spawn(make_project(), env={"WAYLAND_DISPLAY": None, "VYRN_GRIM": None})
    assert rig.hello["backends"]["screen"] is None
    assert rig.hello["problems"]["screen"].strip()


def test_worker_runs_in_its_own_process_group(spawn, make_project):
    rig = spawn(make_project())
    assert os.getpgid(rig.pid) != os.getpgid(0)


# ---- spec inference: pictures -------------------------------------------------------------------------------------

INPUTS = """\
import numpy as np
import vyrn
from PIL import Image

def see(world):
    return {expr}
"""


def p_image_expected():
    return np.asarray(Image.new("RGB", (4, 3), (200, 100, 50)).convert("P", palette=Image.Palette.ADAPTIVE)
                      .convert("RGB"))


PICTURES = [
    ("gray", "np.arange(12, dtype=np.uint8).reshape(3, 4)",
     lambda: np.repeat(np.arange(12, dtype=np.uint8).reshape(3, 4)[:, :, None], 3, axis=2)),
    ("one-channel", "np.arange(12, dtype=np.uint8).reshape(3, 4, 1)",
     lambda: np.repeat(np.arange(12, dtype=np.uint8).reshape(3, 4, 1), 3, axis=2)),
    ("rgb", "np.arange(36, dtype=np.uint8).reshape(3, 4, 3)", lambda: np.arange(36, dtype=np.uint8).reshape(3, 4, 3)),
    ("rgba", "np.arange(48, dtype=np.uint8).reshape(3, 4, 4)",
     lambda: np.arange(48, dtype=np.uint8).reshape(3, 4, 4)[:, :, :3]),
    ("strided-view", "np.arange(72, dtype=np.uint8).reshape(3, 8, 3)[:, ::2]",
     lambda: np.arange(72, dtype=np.uint8).reshape(3, 8, 3)[:, ::2]),
    ("pil-rgb", "Image.new('RGB', (4, 3), (10, 20, 30))", lambda: np.full((3, 4, 3), (10, 20, 30), np.uint8)),
    ("pil-gray", "Image.new('L', (4, 3), 77)", lambda: np.full((3, 4, 3), 77, np.uint8)),
    ("pil-rgba", "Image.new('RGBA', (4, 3), (1, 2, 3, 4))", lambda: np.full((3, 4, 3), (1, 2, 3), np.uint8)),
    ("pil-palette", "Image.new('RGB', (4, 3), (200, 100, 50)).convert('P', palette=Image.Palette.ADAPTIVE)",
     p_image_expected),
    ("canvas", "vyrn.canvas(4, 3, 'red')", lambda: np.full((3, 4, 3), ImageColor.getrgb("red"), np.uint8)),
    ("one-pixel", "np.full((1, 1, 3), 9, dtype=np.uint8)", lambda: np.full((1, 1, 3), 9, np.uint8)),
    ("widest", "np.zeros((1, 4096), dtype=np.uint8)", lambda: np.zeros((1, 4096, 3), np.uint8)),
    ("tallest", "np.zeros((4096, 1, 3), dtype=np.uint8)", lambda: np.zeros((4096, 1, 3), np.uint8)),
]


@pytest.mark.parametrize("expr, expected", [case[1:] for case in PICTURES], ids=[case[0] for case in PICTURES])
def test_see_returns_a_picture(spawn, make_project, expr, expected):
    rig = spawn(make_project({"inputs.py": INPUTS.format(expr=expr)}))
    spec = rig.load()
    want = expected()
    assert spec["t"] == "spec" and spec["mode"] == "sim"
    assert spec["obs"]["kind"] == "picture"
    assert spec["obs"]["shape"] == list(want.shape)
    assert spec["obs"].get("names") is None
    assert spec["preview"].dtype == np.uint8
    np.testing.assert_array_equal(spec["preview"], want)
    obs = rig.reset()["obs"]
    assert obs.dtype == np.uint8 and obs.shape == want.shape
    np.testing.assert_array_equal(obs, want)


# ---- spec inference: numbers --------------------------------------------------------------------------------------

NUMBERS = [
    ("int", "5", [5.0], None),
    ("float", "2.5", [2.5], None),
    ("bool", "True", [1.0], None),
    ("numpy-float", "np.float64(1.25)", [1.25], None),
    ("list", "[1, 2.5, True]", [1.0, 2.5, 1.0], None),
    ("one-item-list", "[7]", [7.0], None),
    ("tuple", "(3, 4, 5)", [3.0, 4.0, 5.0], None),
    ("float-array", "np.array([1.5, -2.5])", [1.5, -2.5], None),
    ("int-array", "np.arange(5)", [0.0, 1.0, 2.0, 3.0, 4.0], None),
    ("uint8-vector", "np.arange(3, dtype=np.uint8)", [0.0, 1.0, 2.0], None),
    ("dict", "{'speed': 1, 'x': 2.5, 'alive': True}", [1.0, 2.5, 1.0], ["speed", "x", "alive"]),
    ("dict-with-number-keys", "{1: 2.0, 'b': 3}", [2.0, 3.0], ["1", "b"]),
    ("zero-dim-arrays", "{'a': np.array(1.5), 'b': np.float32(2)}", [1.5, 2.0], ["a", "b"]),
    ("zero-dim-array-alone", "np.array(4)", [4.0], None),
    ("longest", "list(range(4096))", [float(i) for i in range(4096)], None),
]


@pytest.mark.parametrize("expr, values, names", [case[1:] for case in NUMBERS], ids=[case[0] for case in NUMBERS])
def test_see_returns_numbers(spawn, make_project, expr, values, names):
    rig = spawn(make_project({"inputs.py": INPUTS.format(expr=expr)}))
    spec = rig.load()
    assert spec["obs"]["kind"] == "numbers"
    assert spec["obs"]["shape"] == [len(values)]
    assert spec["obs"].get("names") == names
    obs = rig.reset()["obs"]
    assert obs.dtype == np.float32 and obs.shape == (len(values),)
    np.testing.assert_array_equal(obs, np.array(values, dtype=np.float32))


# ---- spec inference: refused return values ------------------------------------------------------------------------

BAD = [
    ("float-picture", "np.zeros((4, 4, 3), dtype=np.float32)", "astype"),
    ("float-gray", "np.zeros((4, 4))", "astype"),
    ("int64-picture", "np.zeros((4, 4, 3), dtype=np.int64)", None),
    ("bool-picture", "np.zeros((4, 4), dtype=bool)", None),
    ("two-channels", "np.zeros((4, 4, 2), dtype=np.uint8)", None),
    ("four-dimensions", "np.zeros((2, 2, 3, 1), dtype=np.uint8)", None),
    ("zero-height", "np.zeros((0, 4, 3), dtype=np.uint8)", None),
    ("too-wide", "np.zeros((1, 4097), dtype=np.uint8)", None),
    ("too-tall", "np.zeros((4097, 1, 3), dtype=np.uint8)", None),
    ("nested-list", "[[1, 2], [3, 4]]", "flat list"),
    ("nested-tuple", "((1, 2), (3, 4))", "flat list"),
    ("nan-in-list", "[1.0, float('nan')]", None),
    ("inf", "float('inf')", None),
    ("nan-in-dict", "{'a': float('nan')}", None),
    ("nan-in-array", "np.array([1.0, np.nan])", None),
    ("empty-list", "[]", None),
    ("empty-dict", "{}", None),
    ("empty-array", "np.zeros(0)", None),
    ("too-many-numbers", "list(range(4097))", None),
    ("too-long-array", "np.zeros(4097)", None),
    ("none", "None", None),
    ("text", "'hello'", None),
    ("text-in-list", "[1, 'a']", None),
    ("text-in-dict", "{'a': 'b'}", None),
]


@pytest.mark.parametrize("expr, hint", [case[1:] for case in BAD], ids=[case[0] for case in BAD])
def test_see_bad_return_is_a_fatal_load_error(spawn, make_project, expr, hint):
    project = make_project({"inputs.py": INPUTS.format(expr=expr)})
    error = spawn(project).load_error()
    assert error["t"] == "error" and error["kind"] == "bad_return"
    assert error["script"] == "inputs.py"
    assert error["line"] == project.line("inputs.py", "def see")
    assert error["func"] == "see"
    assert error["message"].strip()
    if hint:
        assert hint in f"{error['message']} {error.get('hint', '')}".lower()


# ---- changes during a run -----------------------------------------------------------------------------------------

CHANGING_WORLD = """
    def restart(world):
        world.n = 0
        if not hasattr(world, "k"):
            world.k = 0

    def step(world):
        world.n += 1
        world.k += 1
    """

CHANGES = [
    ("kind", "vyrn.canvas(2, 2) if world.k == 0 else [1, 2]"),
    ("length", "[1, 2] if world.k == 0 else [1, 2, 3]"),
    ("names", "{'a': 1, 'b': 2} if world.k == 0 else {'a': 1, 'c': 2}"),
    ("names-to-list", "{'a': 1, 'b': 2} if world.k == 0 else [1, 2]"),
]


@pytest.mark.parametrize("expr", [case[1] for case in CHANGES], ids=[case[0] for case in CHANGES])
def test_observation_must_not_change_during_a_run(spawn, make_project, expr):
    project = make_project({"world.py": CHANGING_WORLD, "inputs.py": INPUTS.format(expr=expr)})
    rig = spawn(project)
    rig.load()
    rig.reset()
    error = rig.error("step", a=0)
    assert error["kind"] == "bad_return" and error["fatal"] is False
    assert (error["script"], error["line"]) == ("inputs.py", project.line("inputs.py", "def see"))


def test_picture_size_may_change_with_one_warning(spawn, make_project):
    expr = "np.zeros((4, 4, 3), np.uint8) if world.k % 2 == 0 else np.full((6, 5, 3), 9, np.uint8)"
    rig = spawn(make_project({"world.py": CHANGING_WORLD, "inputs.py": INPUTS.format(expr=expr)}))
    rig.load()
    rig.reset()
    results = [rig.step(1) for _ in range(4)]
    assert [r["obs"].shape for r in results] == [(6, 5, 3), (4, 4, 3), (6, 5, 3), (4, 4, 3)]
    warnings = [r.get("warnings") or [] for r in results]
    assert len(warnings[0]) == 1, warnings
    assert sum(len(w) for w in warnings) == 1, "the warning is sent once"
    warning = warnings[0][0]
    assert {"script", "line", "message"} <= set(warning) and warning["message"].strip()


# ---- choices (responses.py) ---------------------------------------------------------------------------------------

def test_choice_discovery_rules(spawn, make_project):
    project = make_project({
        "responses.py": """
            import os
            import random as _random
            from math import floor
            from os.path import join
            from helpers import borrowed
            import helpers

            zeta = "z"
            _private = "x"
            count = 3
            ratio = 0.5
            flags = [1, 2]
            table = {"a": 1}
            enabled = True
            nothing = ()

            class Thing:
                pass

            def alpha():
                pass

            mid = None

            def game_over():
                return False

            def _hidden():
                pass

            beta = "tap space"
            gamma = "shift + d"
            """,
        "helpers.py": """
            def borrowed():
                pass
            """,
    }, sim=False)
    spec = spawn(project).load()
    assert spec["mode"] == "realtime"
    assert spec["choices"] == ["zeta", "alpha", "mid", "beta", "gamma"]
    assert spec["labels"] == spec["choices"]
    effects = spec["effects"]
    assert effects[0] == "hold Z" and effects[2] == "do nothing" and effects[3] == "tap Space"
    assert effects[4] == "hold Shift+D"
    assert effects[1].startswith("call alpha")
    assert spec["hooks"]["game_over"] == f"responses.py:{project.line('responses.py', 'def game_over')}"


def test_labels_replace_underscores(spawn, make_project):
    spec = spawn(make_project({"responses.py": 'move_left = "a"\nstand_still = None\n'}, sim=False)).load()
    assert spec["choices"] == ["move_left", "stand_still"]
    assert spec["labels"] == ["move left", "stand still"]


def test_responses_dict_overrides_discovery(spawn, make_project):
    spec = spawn(make_project({"responses.py": """
        def go():
            pass

        other = "b"

        RESPONSES = {"go_left": "a", "jump": "tap space", "rest": None, "go": go}
        """}, sim=False)).load()
    assert spec["choices"] == ["go_left", "jump", "rest", "go"]
    assert spec["labels"] == ["go left", "jump", "rest", "go"]
    assert spec["effects"][:3] == ["hold A", "tap Space", "do nothing"]
    assert spec["effects"][3].startswith("call go")


def test_sim_effects_are_described(spawn, copy_template):
    spec = spawn(copy_template("catch-pictures")).load()
    assert spec["choices"] == ["left", "right", "stay"]
    assert spec["effects"] == ["call left(world)", "call right(world)", "do nothing"]


@pytest.mark.parametrize("responses", ["only = None\n", "", "_a = None\n_b = None\nnumber = 4\n"],
                         ids=["one", "none", "only-ignored-names"])
def test_fewer_than_two_choices_is_fatal(spawn, make_project, responses):
    error = spawn(make_project({"responses.py": responses})).load_error()
    assert "2 choices" in error["message"]


def test_simulations_refuse_key_strings(spawn, make_project):
    error = spawn(make_project({"responses.py": 'left = "a"\nstay = None\n'})).load_error()
    assert "keys" in error["message"].lower()
    assert error["script"] in ("responses.py", "")


@pytest.mark.parametrize("effect, words", [("spcae", ["space"]), ("alt+tab", ["alt"]), ("tap super", ["super"]),
                                           ("capslock", ["capslock"])])
def test_bad_effect_strings_are_fatal(spawn, make_project, effect, words):
    error = spawn(make_project({"responses.py": f'jump = "{effect}"\nwait = None\n'}, sim=False)).load_error()
    text = f"{error['message']} {error.get('hint', '')}".lower()
    for word in words:
        assert word in text
    assert error["script"] in ("responses.py", "")


def test_choice_function_with_two_required_parameters_is_fatal(spawn, make_project):
    project = make_project({"responses.py": "def jump(world, height):\n    pass\n\nstay = None\n"})
    error = spawn(project).load_error()
    assert (error["script"], error["line"]) == ("responses.py", 1)


def uses(spec):
    return {name: spec["uses"][name] for name in ("screen", "keys")}


def test_uses_flags(spawn, make_project):
    assert uses(spawn(make_project()).load()) == {"screen": False, "keys": False}
    assert uses(spawn(make_project(sim=False)).load()) == {"screen": False, "keys": True}
    project = make_project({
        "responses.py": "def go():\n    pass\n\nwait = None\n",
        "points.py": """
            import vyrn

            # later: vyrn.pixel(1, 2)
            def points():
                return 1

            def restart():
                vyrn.press("space")
            """,
    }, sim=False)
    assert uses(spawn(project).load()) == {"screen": True, "keys": True}


# ---- hooks --------------------------------------------------------------------------------------------------------

def test_hook_locations(spawn, copy_template):
    project = copy_template("catch-pictures")
    spec = spawn(project).load()
    assert spec["hooks"] == {
        "see": f"inputs.py:{project.line('inputs.py', 'def see')}",
        "points": f"points.py:{project.line('points.py', 'def points')}",
        "game_over": f"points.py:{project.line('points.py', 'def game_over')}",
        "restart": f"world.py:{project.line('world.py', 'def restart')}",
        "step": f"world.py:{project.line('world.py', 'def step')}",
    }


def test_missing_optional_hooks_are_not_listed(spawn, make_project):
    hooks = spawn(make_project(sim=False)).load()["hooks"]
    assert set(k for k, v in hooks.items() if v) == {"see", "points"}


def test_hook_defined_twice_names_both_places(spawn, make_project):
    project = make_project({"points.py": """
        def points(world):
            return 0

        def game_over(world):
            return False
        """, "world.py": """
        def restart(world):
            world.n = 0

        def game_over(world):
            return True
        """})
    error = spawn(project).load_error()
    text = f"{error['message']} {error.get('hint', '')}"
    assert "game_over" in text and "points.py" in text and "world.py" in text


@pytest.mark.parametrize("script, source, marker", [
    ("points.py", "def points(world, extra):\n    return 0\n", "def points"),
    ("inputs.py", "def see(world, a, b):\n    return [1]\n", "def see"),
    ("world.py", "def restart(world):\n    world.n = 0\n\ndef step(world, dt):\n    pass\n", "def step"),
    ("points.py", "def points(world):\n    return 0\n\ndef game_over(world, *, strict):\n    return False\n",
     "def game_over"),
])
def test_hook_with_wrong_parameters_is_fatal_at_its_def(spawn, make_project, script, source, marker):
    project = make_project({script: source})
    error = spawn(project).load_error()
    assert (error["script"], error["line"]) == (script, project.line(script, marker))


def test_world_is_passed_only_to_hooks_that_take_it(spawn, make_project):
    rig = spawn(make_project({
        "world.py": """
            calls = {"step": 0, "shout": 0}

            def restart(world):
                world.n = 0

            def step():
                calls["step"] += 1
            """,
        "responses.py": """
            import world as world_module

            def bump(world):
                world.n += 1

            def shout():
                world_module.calls["shout"] += 1

            stay = None
            """,
        "inputs.py": """
            import world as world_module

            def see(world):
                return [world.n, world_module.calls["step"], world_module.calls["shout"]]
            """,
        "points.py": """
            def points(world, scale=2):
                return world.n * scale

            def game_over():
                return False
            """,
    }))
    assert rig.load()["choices"] == ["bump", "shout", "stay"]
    assert rig.reset()["obs"].tolist() == [0, 0, 0]
    result = rig.step(0)
    assert result["obs"].tolist() == [1, 1, 0] and result["points"] == 2
    assert rig.step(1)["obs"].tolist() == [1, 2, 1]


@pytest.mark.parametrize("script, source, word", [
    ("inputs.py", "def look(world):\n    return [1]\n", "see"),
    ("points.py", "def score(world):\n    return 1\n", "points"),
])
def test_required_hooks(spawn, make_project, script, source, word):
    error = spawn(make_project({script: source})).load_error()
    assert word in error["message"]


@pytest.mark.parametrize("missing", ["inputs.py", "responses.py", "points.py"])
def test_required_scripts(spawn, make_project, missing):
    error = spawn(make_project({missing: None})).load_error()
    assert missing in f"{error['message']} {error.get('hint', '')}"


def test_imported_hook_is_not_a_second_definition(spawn, make_project):
    project = make_project({"points.py": """
        from inputs import see

        def points(world):
            return len(see(world))
        """})
    rig = spawn(project)
    spec = rig.load()
    assert spec["hooks"]["see"] == f"inputs.py:{project.line('inputs.py', 'def see')}"
    rig.reset()
    assert rig.step(1)["points"] == 1


def test_hook_defined_in_a_helper_module_does_not_count(spawn, make_project):
    project = make_project({"points.py": "from helpers import points\n",
                            "helpers.py": "def points(world):\n    return 1\n"})
    error = spawn(project).load_error()
    assert "points" in error["message"]


# ---- load behaviour -----------------------------------------------------------------------------------------------

def test_realtime_load_never_calls_restart(spawn, make_project, tmp_path):
    marker = tmp_path / "restart-was-called"
    rig = spawn(make_project({"points.py": f"""
        def points():
            return 1

        def restart():
            open({str(marker)!r}, "w").close()
        """}, sim=False))
    assert rig.load()["mode"] == "realtime"
    assert not marker.exists()
    assert rig.request("restart")["t"] == "ok"
    assert marker.exists()


def test_seeds(spawn, make_project):
    files = {
        "world.py": """
            import random
            import numpy as np

            def restart(world):
                world.a = random.random()
                world.b = float(np.random.rand())
            """,
        "inputs.py": """
            import random

            def see(world):
                return [world.a, world.b, random.random()]
            """,
    }
    seed = 2 ** 40 + 7  # numpy gets seed % 2**32
    runs = []
    for value in (seed, seed, seed + 1):
        rig = spawn(make_project(files))
        assert rig.load(seed=value)["obs"]["kind"] == "numbers"
        runs.append(rig.reset()["obs"].tolist())
    assert runs[0] == runs[1], "the same seed gives the same run"
    assert runs[0] != runs[2]


def test_scripts_cannot_inherit_the_socket(spawn, make_project):
    rig = spawn(make_project({"inputs.py": """
        import os

        def see(world):
            return [os.get_inheritable(3)]
        """}))
    rig.load()
    assert rig.reset()["obs"].tolist() == [0.0]


def test_no_bytecode_is_written_into_the_project(spawn, make_project):
    project = make_project({"inputs.py": "import helpers\n\ndef see(world):\n    return helpers.look(world)\n",
                            "helpers.py": "def look(world):\n    return [world.n]\n"})
    rig = spawn(project)
    rig.load()
    rig.reset()
    rig.step(0)
    assert not (project / "__pycache__").exists()


@pytest.mark.parametrize("helper", ["random.py", "inspect.py", "numbers.py", "platform.py"])
def test_a_helper_named_like_a_standard_module_does_not_break_vyrn(spawn, make_project, helper):
    """Vyrn's own imports come from the standard library even when the project has a file of the same name."""
    project = make_project({helper: "# the user's own helper\nVALUE = 1\n"})
    rig = spawn(project)
    assert rig.load(seed=3)["t"] == "spec"
    assert rig.reset()["obs"].tolist() == [0.0]
    assert rig.step(0)["obs"].tolist() == [11.0]


def test_a_script_another_one_imports_runs_once(spawn, make_project):
    """inputs.py doing  import points  gets the very module Vyrn uses (protocol.md 3.2), so module-level values are
    shared and points.py doesn't run twice (twice would e.g. claim a GPIO pin twice)."""
    project = make_project({
        "inputs.py": "import points\n\ndef see(world):\n    return [points.score]\n",
        "points.py": """
            print("points.py runs")
            score = 0

            def points(world):
                global score
                score += 1
                return score
            """,
    })
    rig = spawn(project)
    rig.load()
    rig.reset()
    # Each step runs see() before points(): see() reads the score points() left in the shared module.
    assert [rig.step(1)["obs"].tolist() for _ in range(3)] == [[0.0], [1.0], [2.0]]
    rig.worker.close()
    assert rig.worker.stdout_text().count("points.py runs") == 1


@pytest.mark.parametrize("helper", ["secrets.py", "hashlib.py", "hmac.py"])
def test_vyrn_imports_what_it_needs_before_the_project_is_on_the_path(spawn, make_project, helper):
    """numpy.random (seeded at load) imports secrets, hashlib and hmac: a helper of that name must not run inside
    Vyrn. (The user's own `import secrets` still finds the user's file only if nothing imported it before.)"""
    rig = spawn(make_project({helper: "print('THE HELPER RAN')\n"}))
    assert rig.load(seed=3)["t"] == "spec"
    rig.reset()
    rig.worker.close()
    assert "THE HELPER RAN" not in rig.worker.stdout_text()


def test_evdev_is_imported_before_the_project_is_on_the_path(spawn, make_project, tmp_path):
    """The real evdev imports glob, asyncio, ssl...: a project helper called glob.py must not run inside Vyrn. This
    evdev stand-in imports glob and then refuses to load, so no keyboard can ever be made with it."""
    fake = tmp_path / "fake-evdev"
    (fake / "evdev").mkdir(parents=True)
    (fake / "evdev" / "__init__.py").write_text("import glob\nraise ImportError('a test stand-in')\n")
    rig = spawn(make_project({"glob.py": "print('THE USER GLOB RAN')\n"}),
                env={"PYTHONPATH": str(fake), "VYRN_KEYS_BACKEND": None})
    assert rig.hello["backends"]["keys"] is None, "hello looked for evdev (and found the stand-in)"
    rig.load()
    rig.worker.close()
    assert "THE USER GLOB RAN" not in rig.worker.stdout_text()


def test_decorated_hooks_point_at_their_def_line(spawn, make_project):
    project = make_project({"points.py": """
        import functools

        def logged(function):
            @functools.wraps(function)
            def inner(*args):
                return function(*args)
            return inner

        def plain(function):
            return function

        @logged
        def points(world):
            return "lots"

        @plain
        @plain
        def game_over(world):
            return False
        """})
    rig = spawn(project)
    spec = rig.load()
    assert spec["hooks"]["points"] == f"points.py:{project.line('points.py', 'def points')}"
    assert spec["hooks"]["game_over"] == f"points.py:{project.line('points.py', 'def game_over')}"
    rig.reset()
    error = rig.error("step", a=1)
    assert (error["kind"], error["line"]) == ("bad_return", project.line("points.py", "def points"))


@pytest.mark.parametrize("source, words", [
    ("def points(world):\n    return 0\n\ndef gameover(world):\n    return True\n", ["gameover()", "game_over"]),
    ("def points(world):\n    return 0\n\ndef Game_Over(world):\n    return True\n", ["Game_Over()", "game_over"]),
    ("def points(world):\n    return 0\n\n    def game_over(world):\n        return True\n",
     ["inside points()", "left edge"]),
], ids=["gameover", "case", "nested"])
def test_a_misspelled_or_hidden_hook_gets_a_warning(spawn, make_project, source, words):
    project = make_project({"points.py": source})
    spec = spawn(project).load()
    assert "game_over" not in spec["hooks"]
    found = [w for w in spec["warnings"] if all(word in w["message"] for word in words)]
    assert found and found[0]["script"] == "points.py"
    assert found[0]["line"] == project.line("points.py", "def ", 2)


def test_a_misspelled_step_gets_a_warning(spawn, make_project):
    project = make_project({"world.py": "def restart(world):\n    world.n = 0\n\ndef Step(world):\n    world.n += 1\n"})
    warnings = spawn(project).load()["warnings"]
    assert any("Step()" in w["message"] and "Rename it to step" in w["message"] for w in warnings), warnings


def test_helpers_that_only_look_a_bit_like_hooks_get_no_warning(spawn, make_project):
    spec = spawn(make_project({"points.py": """
        def points(world):
            return 0

        def game_over(world):
            return False

        def is_game_over(world):
            return False

        def seed(world):
            return 1
        """})).load()
    assert spec["warnings"] == []


def test_a_missing_hook_names_the_lookalike(spawn, make_project):
    project = make_project({"points.py": "def point(world):\n    return 1\n"})
    error = spawn(project).load_error()
    assert "point()" in error["message"] and "points" in error["message"]
    assert (error["script"], error["line"]) == ("points.py", 1)


def test_from_vyrn_import_star_adds_no_choice(spawn, make_project):
    project = make_project({"responses.py": "from vyrn import *\n\ndef grow(world):\n    world.n += 10\n\nstay = None\n"})
    spec = spawn(project).load()
    assert spec["choices"] == ["grow", "stay"], "vyrn.choice (None) is not a do-nothing choice"
    warning = [w for w in spec["warnings"] if "import *" in w["message"]]
    assert warning and warning[0]["line"] == 1 and "round()" in warning[0]["message"]


@pytest.mark.parametrize("script", ["points.py", "responses.py"])
def test_from_vyrn_import_tick_gets_a_warning(spawn, make_project, script):
    files = {"points.py": "from vyrn import tick, choice\n\ndef points(world):\n    return tick\n"}
    if script == "responses.py":
        files = {"responses.py": "from vyrn import choice\n\ndef grow(world):\n    pass\n\nstay = None\n"}
    spec = spawn(make_project(files)).load()
    assert spec["choices"] == ["grow", "stay"]
    warnings = [w for w in spec["warnings"] if "copies" in w["message"]]
    assert warnings and warnings[0]["script"] == script and "import vyrn" in warnings[0]["message"]


def test_star_imported_strings_are_not_choices(spawn, make_project):
    spec = spawn(make_project({"responses.py": "from mykeys import *\n\njump = 'tap space'\nwait = None\n",
                               "mykeys.py": "left = 'a'\nright = 'd'\n"}, sim=False)).load()
    assert spec["choices"] == ["jump", "wait"], "a choice has a place in responses.py"
    assert any("left and right" in w["message"] and "import *" in w["message"] for w in spec["warnings"])


@pytest.mark.parametrize("responses, points", [
    ("from vyrn import press\n\ndef left():\n    press('a')\n\nwait = None\n", "def points():\n    return 1\n"),
    ("import vyrn as v\n\ndef left():\n    v.press('a')\n\nwait = None\n", "def points():\n    return 1\n"),
    ("def left():\n    pass\n\nwait = None\n", "from vyrn import press as tap\n\ndef points():\n    return 1\n\n"
                                               "def restart():\n    tap('space')\n"),
], ids=["from-import", "module-alias", "renamed"])
def test_uses_keys_sees_through_imports(spawn, make_project, responses, points):
    spec = spawn(make_project({"responses.py": responses, "points.py": points}, sim=False)).load()
    assert spec["uses"]["keys"] is True


def test_uses_screen_sees_helper_modules(spawn, make_project):
    project = make_project({"inputs.py": "import look\n\ndef see():\n    return look.it()\n",
                            "look.py": "from vyrn import pixel as px\n\ndef it():\n    return [1]\n\n"
                                       "def later():\n    return px(1, 1)\n",
                            "responses.py": "def go():\n    pass\n\nwait = None\n"}, sim=False)
    assert spawn(project).load()["uses"] == {"screen": True, "keys": False, "game": False}


@pytest.mark.parametrize("check", [False, True])
def test_an_unknown_key_in_vyrn_press_is_found_at_load(spawn, make_project, check):
    """Without running the code: Check runs neither choice functions nor (in the app) restart()."""
    project = make_project({"responses.py": """
        import vyrn

        left = "a"

        def jump():
            vyrn.press("spcae")

        wait = None
        """}, sim=False)
    error = spawn(project).load_error(check=check)
    line = project.line("responses.py", "spcae")
    assert (error["script"], error["line"], error["func"]) == ("responses.py", line, "jump")
    assert error["col"] == project.col("responses.py", line, '"spcae"')
    assert 'Did you mean "space"' in error["message"]


def test_valid_and_computed_keys_in_vyrn_press_pass_load(spawn, make_project):
    spec = spawn(make_project({"points.py": """
        import vyrn

        KEY = "spcae"

        def points():
            return 1

        def restart():
            vyrn.press("tap ctrl+c")
            vyrn.press(key="space")
            if False:
                vyrn.press(KEY)    # computed: only checked when it runs
        """}, sim=False)).load()
    assert spec["t"] == "spec"


@pytest.mark.parametrize("responses, words", [
    ("left = 1\nright = 2\nwait = 0\n", ["left = 1, right = 2 and wait = 0 are numbers", 'in quotes ("a")']),
    ("go = None\nspeed = [1, 2]\n", ["speed = a list of 2 items is a list", "Add another one"]),
], ids=["numbers", "one-choice"])
def test_too_few_choices_says_why(spawn, make_project, responses, words):
    error = spawn(make_project({"responses.py": responses}, sim=False)).load_error()
    text = f"{error['message']} {error['hint']}"
    for word in words:
        assert word in text, text
    if "numbers" in words[0]:
        assert "Add another one" not in text


def test_a_helper_function_in_responses_is_named_in_the_hint(spawn, make_project):
    error = spawn(make_project({"responses.py": "def grow(world):\n    pass\n\nstay = None\n\n"
                                                "def clamp(value, low, high):\n    return value\n"})).load_error()
    assert "def clamp(world):" in error["hint"] and "_clamp" in error["hint"]
    assert "points" not in error["hint"]


def test_a_text_value_in_a_simulation_is_not_called_a_key(spawn, make_project):
    error = spawn(make_project({"responses.py": "NAME = 'catcher'\n\ndef grow(world):\n    pass\n\nstay = None\n"})
                  ).load_error()
    assert "simulations can't press keys" in error["message"] and "_NAME" in error["hint"]
    assert "not a key Vyrn knows" not in error["message"]


@pytest.mark.parametrize("missing, word", [("inputs.py", "canvas"), ("points.py", "def points(world)"),
                                           ("responses.py", "def left(world)")])
def test_missing_scripts_in_a_simulation_get_simulation_examples(spawn, make_project, missing, word):
    error = spawn(make_project({missing: None})).load_error()
    assert word in error["hint"] and "vyrn.screen" not in error["hint"]
