"""Error replies point at the right file, line and column, with plain-English hints (protocol.md section 3.7), and
print() from user code never corrupts the protocol (stdout/stderr are separate pipes from fd 3)."""

import signal
import time

import pytest

from testkit import Background, wait_until


def test_exception_deep_in_a_helper(spawn, make_project):
    project = make_project({
        "inputs.py": """
            import helpers

            def see(world):
                return helpers.compute(world)
            """,
        "helpers.py": """
            def compute(world):
                scale = world.n - 1
                return [10 / scale]
            """,
    })
    rig = spawn(project)
    rig.load()
    rig.reset()
    error = rig.error("step", a=1)  # n becomes 1: division by zero inside helpers.compute
    line = project.line("helpers.py", "10 / scale")
    assert error["t"] == "error" and error["kind"] == "exception" and error["fatal"] is False
    assert (error["script"], error["line"], error["func"]) == ("helpers.py", line, "compute")
    assert error["col"] == project.col("helpers.py", line, "10 / scale"), "col is 1-based"
    assert error["exc_type"] == "ZeroDivisionError"
    assert error["message"].strip() and error["hint"].strip()
    see_line = project.line("inputs.py", "helpers.compute")
    assert f"helpers.py:{line} in compute" in error["trace"]
    assert f"inputs.py:{see_line} in see" in error["trace"]


def test_exception_inside_a_library_points_at_the_users_line(spawn, make_project):
    project = make_project({"points.py": """
        import json

        def points(world):
            if world.n > 0:
                return json.loads("{not json")
            return 0
        """})
    rig = spawn(project)
    rig.load()
    rig.reset()
    error = rig.error("step", a=1)
    assert error["kind"] == "exception" and error["exc_type"] == "JSONDecodeError"
    assert (error["script"], error["line"], error["func"]) == ("points.py", project.line("points.py", "json.loads"),
                                                               "points")


def test_name_error_location(spawn, make_project):
    project = make_project({"points.py": """
        def points(world):
            score = world.n
            return scor
        """})
    rig = spawn(project)
    rig.load()
    rig.reset()
    error = rig.error("step", a=1)
    line = project.line("points.py", "return scor")
    assert (error["script"], error["line"], error["col"]) == ("points.py", line, project.col("points.py", line, "scor"))
    assert error["exc_type"] == "NameError" and "scor" in error["message"]
    assert error["hint"].strip()


def test_col_counts_characters_on_lines_with_non_ascii_text(spawn, make_project):
    # Python's traceback positions count UTF-8 bytes; SyntaxError.offset counts characters. col is characters.
    project = make_project({"points.py": """
        def points(world):
            größe = "×2"; return größe + missing_name
        """})
    rig = spawn(project)
    rig.load()
    rig.reset()
    error = rig.error("step", a=1)
    line = project.line("points.py", "missing_name")
    assert (error["script"], error["line"]) == ("points.py", line)
    assert error["col"] == project.col("points.py", line, "missing_name")


def test_huge_exception_text_is_shortened(spawn, make_project):
    project = make_project({"points.py": """
        def points(world):
            raise ValueError("x" * 3_000_000)
        """})
    rig = spawn(project)
    rig.load()
    rig.reset()
    error = rig.error("step", a=1)
    assert error["exc_type"] == "ValueError" and 100 < len(error["message"]) <= 2000
    assert (error["script"], error["line"]) == ("points.py", project.line("points.py", "raise ValueError"))


def test_exception_while_loading_is_fatal(spawn, make_project):
    project = make_project({"inputs.py": """
        def see(world):
            return [world.speed]
        """})
    error = spawn(project).load_error()
    assert error["kind"] in ("exception", "load")
    assert error["exc_type"] == "AttributeError"
    assert (error["script"], error["line"]) == ("inputs.py", project.line("inputs.py", "world.speed"))


def test_exception_at_import_time_is_fatal(spawn, make_project):
    project = make_project({"responses.py": """
        def grow(world):
            world.n += 1

        stay = None
        oops = [1, 2][5]
        """})
    error = spawn(project).load_error()
    assert error["exc_type"] == "IndexError"
    assert (error["script"], error["line"]) == ("responses.py", project.line("responses.py", "oops"))


@pytest.mark.parametrize("script, source", [
    ("inputs.py", "def see(world:\n    return [1]\n"),
    ("points.py", "def points(world):\nreturn 1\n"),
    ("points.py", "def points(world):\n    return 1\n  x = 2\n"),
    ("helpers.py", "def look(world)\n    return [1]\n"),
], ids=["syntax", "indentation", "unindent", "in-a-helper"])
def test_syntax_errors_point_at_the_line_and_column(spawn, make_project, script, source):
    files = {script: source}
    if script == "helpers.py":
        files["inputs.py"] = "import helpers\n\ndef see(world):\n    return helpers.look(world)\n"
    project = make_project(files)
    try:
        compile((project / script).read_text(), script, "exec")
    except SyntaxError as expected:
        want = (expected.lineno, expected.offset or 0)
        unindent = "unindent does not match" in expected.msg
    else:
        raise AssertionError("the test source should not compile")
    if unindent:
        # Python points past the end of the line; the line's first non-blank character is where to look.
        text = (project / script).read_text().splitlines()[want[0] - 1]
        want = (want[0], len(text) - len(text.lstrip()) + 1)
    error = spawn(project).load_error()
    assert error["kind"] == "syntax"
    assert (error["script"], error["line"], error["col"]) == (script, *want)
    assert error["message"].strip() and error["hint"].strip()


def test_bad_return_points_at_the_def_line(spawn, make_project):
    project = make_project({"points.py": """
        # how the AI is scored


        def points(world):
            label = "great"
            return label
        """})
    rig = spawn(project)
    rig.load()
    rig.reset()
    error = rig.error("step", a=1)
    assert error["kind"] == "bad_return"
    assert (error["script"], error["line"], error["func"]) == ("points.py", project.line("points.py", "def points"),
                                                               "points")


def test_infinite_loop_and_sigint_report_the_stuck_line(spawn, make_project, tmp_path):
    marker = tmp_path / "looping"
    project = make_project({"points.py": f"""
        def points(world):
            open({str(marker)!r}, "w").close()
            n = 0
            while True: n += 1
        """})
    rig = spawn(project)
    rig.load()
    rig.reset()
    stuck = Background(rig.request, "step", timeout=20, a=1)
    wait_until(marker.exists, 5.0, "points() to start looping")
    time.sleep(0.1)
    rig.signal(signal.SIGINT)
    stuck.join(5)
    error = getattr(stuck.exc, "error", None)
    assert error is not None, f"expected an error reply, got {stuck.result!r} / {stuck.exc!r}"
    assert error["kind"] == "timeout" and error["fatal"] is False
    assert (error["script"], error["line"], error["func"]) == ("points.py", project.line("points.py", "while True"),
                                                               "points")
    assert "too long" in error["message"]


HINTS = {
    "NameError": "return [speed]",
    "AttributeError": "return [world.speed]",
    "TypeError": "return [(lambda w: w)()]",
    "ZeroDivisionError": "return [1 / 0]",
    "IndexError": "return [[1, 2][9]]",
    "KeyError": "return [{'a': 1}['b']]",
    "ModuleNotFoundError": "import no_such_module_here",
}


@pytest.mark.parametrize("exc_type", sorted(HINTS))
def test_common_exceptions_get_a_hint(spawn, make_project, exc_type):
    rig = spawn(make_project({"inputs.py": f"""
        def see(world):
            if world.n == 0:
                return [0]
            {HINTS[exc_type]}
        """}))
    rig.load()
    rig.reset()
    error = rig.error("step", a=1)
    assert error["exc_type"] == exc_type
    assert error["hint"].strip(), f"no hint for {exc_type}"
    assert error["script"] == "inputs.py"


def test_wrong_number_of_arguments_hint(spawn, make_project):
    rig = spawn(make_project({"inputs.py": """
        def helper(world):
            return [world.n]

        def see(world):
            if world.n == 0:
                return [0]
            return helper()
        """}))
    rig.load()
    rig.reset()
    error = rig.error("step", a=1)
    assert error["exc_type"] == "TypeError" and "world" in error["hint"]


SPAM = r'''
import os
import sys

print("hello from import time " + "VYRN" * 50)
sys.stderr.write("stderr at import time\n")

def see(world):
    for i in range(200):
        print("spam", i, "VYRN\x00\x01 {\"t\":\"result\",\"id\":1}" * 3)
        print("err spam", i, file=sys.stderr)
    os.write(1, b"VYRN" + bytes(20) + b"\n")
    os.write(2, b"VYRN\xff\xfe\n")
    sys.stdout.write("no newline at the end")
    if world.n == 3:
        sys.stdout.write("x" * 1_000_000)
    return [world.n]
'''


def test_print_spam_never_corrupts_frames(spawn, make_project):
    rig = spawn(make_project({"inputs.py": SPAM}))
    rig.load()
    assert rig.reset()["obs"].tolist() == [0]
    for n in range(1, 21):
        assert rig.step(1)["obs"].tolist() == [n]
    wait_until(lambda: "err spam 199" in rig.worker.stderr_text(), 5.0, "stderr to be collected")
    wait_until(lambda: "hello from import time" in rig.worker.stdout_text(), 5.0, "stdout to be collected")
    assert "spam 199" in rig.worker.stdout_text()
    assert "stderr at import time" in rig.worker.stderr_text()


def test_an_exception_that_cannot_describe_itself_is_still_reported(spawn, make_project):
    project = make_project({"points.py": """
        class Weird(Exception):
            def __str__(self):
                raise RuntimeError("no words")

        def points(world):
            raise Weird()
        """})
    rig = spawn(project)
    rig.load()
    rig.reset()
    error = rig.error("step", a=1)
    assert error["fatal"] is False and error["exc_type"] == "Weird" and "Weird" in error["message"]
    assert (error["script"], error["line"]) == ("points.py", project.line("points.py", "raise Weird()"))
    assert rig.status()["t"] == "ok", "the worker is still there to answer"


# ---- numbers too long to print, and text that isn't valid UTF-8 ------------------------------------------------------

@pytest.mark.parametrize("files, kind, marker", [
    ({"points.py": "def points(world):\n    return 10 ** 5000\n"}, "bad_return", "def points"),
    ({"inputs.py": "def see(world):\n    return [10 ** 5000] if world.n else [1]\n"}, "bad_return", "def see"),
    ({"points.py": "def points(world):\n    return {}[10 ** 5000]\n"}, "exception", "return {}"),
], ids=["points", "see", "key-error"])
def test_a_number_too_long_to_print_is_still_reported_at_the_users_line(spawn, make_project, files, kind, marker):
    # Python refuses to turn a whole number of over 4300 digits into text; the message must not need that.
    project = make_project(files)
    rig = spawn(project)
    rig.load()
    rig.reset()
    error = rig.error("step", a=1)
    script = next(iter(files))
    assert (error["kind"], error["script"], error["line"]) == (kind, script, project.line(script, marker))
    assert "5001 digits" in error["message"]


def test_text_that_is_not_valid_utf8_still_reaches_the_app(spawn, make_project):
    # A file name decoded with "surrogateescape" holds lone surrogates, which UTF-8 can't carry: they become U+FFFD.
    rig = spawn(make_project({
        "inputs.py": "def see(world):\n    return {b'caf\\xe9'.decode('utf-8', 'surrogateescape'): world.n}\n",
        "points.py": "import vyrn\n\ndef points(world):\n    vyrn.show(b'n\\xe9'.decode('utf-8', 'surrogateescape'), "
                     "b'caf\\xe9'.decode('utf-8', 'surrogateescape'))\n    return 1\n",
    }))
    assert rig.load()["obs"]["names"] == ["caf\ufffd"]
    rig.reset()
    assert rig.step(1)["show"] == {"n\ufffd": "caf\ufffd"}


# ---- hints that fit the mistake --------------------------------------------------------------------------------------

def step_error(spawn, make_project, files, sim=True):
    rig = spawn(make_project(files, sim=sim))
    rig.load()
    if sim:
        rig.reset()
        return rig.error("step", a=1)
    rig.observe(first=True)
    return rig.error("observe")


CALLS = [
    ("import vyrn\n\ndef see(world):\n    return vyrn.canvas() if world.n else [0]\n", "vyrn.canvas(10, 10)"),
    ("import vyrn\n\ndef see(world):\n    p = vyrn.canvas(4, 4)\n    if world.n:\n        p.dot(1, 2, 'red', 9)\n"
     "    return [world.n]\n", "picture.dot(3, 4"),
    ("from vyrn import canvas\n\ndef see(world):\n    return canvas(4) if world.n else [0]\n", "vyrn.canvas(10, 10)"),
    ("def far(a, b):\n    return a\n\ndef see(world):\n    return [far(1)] if world.n else [0]\n",
     "compare it with the def far(...) line"),
    ("def helper(world):\n    return world.n\n\ndef see(world):\n    return [helper()] if world.n else [0]\n",
     "write helper(world)"),
]


@pytest.mark.parametrize("inputs, words", CALLS, ids=["vyrn-canvas", "dot", "imported-canvas", "helper", "world"])
def test_wrong_inputs_to_a_call_get_a_hint_for_that_call(spawn, make_project, inputs, words):
    error = step_error(spawn, make_project, {"inputs.py": inputs})
    assert error["exc_type"] == "TypeError"
    assert words in error["hint"], error["hint"]
    assert "With a world.py" not in error["hint"]


def test_a_vyrn_function_called_wrongly_in_real_time(spawn, make_project):
    error = step_error(spawn, make_project, {"points.py": "import vyrn\n\ndef points():\n    return vyrn.pixel(20)[0]\n"},
                       sim=False)
    assert error["hint"] == "vyrn.pixel() needs x and y, like vyrn.pixel(20, 20)."


@pytest.mark.parametrize("source, words", [
    ("def make(world):\n    pass\n\ndef points(world):\n    picture = make(world)\n    picture.box(0, 0, 1, 1)\n"
     "    return 1\n", ["None (nothing)", ".box", "return"]),
    ("def points(world):\n    r, g, b = [world.n]\n    return r\n", ["names before the =", "vyrn.pixel"]),
    ("def points(world):\n    return 1 if world.n < 'x' else 0\n", ["mix text and numbers"]),
], ids=["none-attribute", "unpack", "compare-text"])
def test_common_mistakes_get_the_matching_hint(spawn, make_project, source, words):
    error = step_error(spawn, make_project, {"points.py": source})
    for word in words:
        assert word in error["hint"], error["hint"]


def test_a_trailing_comma_in_points(spawn, make_project):
    error = step_error(spawn, make_project, {"points.py": "def points(world):\n    return 1,\n"})
    assert error["kind"] == "bad_return" and "a tuple of 1 item," in error["message"]
    assert "comma" in error["hint"]


def test_vyrn_errors_carry_no_python_class_name(spawn, make_project):
    error = step_error(spawn, make_project, {"inputs.py": "import vyrn\n\ndef see(world):\n"
                                                          "    return vyrn.canvas(2, 2, 'blu' if world.n else 'red')\n"})
    assert 'Did you mean "blue"' in error["message"] and error["exc_type"] == ""


@pytest.mark.parametrize("source, words, avoid", [
    ("def see(world):\n    print(world.n)\n", "return picture", "vyrn.screen"),
    ("def see(world):\n    return 'hello'\n", "vyrn.canvas", "vyrn.screen"),
], ids=["none", "text"])
def test_see_hints_in_a_simulation_never_suggest_the_screen(spawn, make_project, source, words, avoid):
    error = spawn(make_project({"inputs.py": source})).load_error()
    assert words in error["hint"] and avoid not in error["hint"]


@pytest.mark.parametrize("call, words", [("vyrn.pixel(1, 1)[0]", "world.fruit_y"),
                                         ("vyrn.on_screen('x.png', 0, 0)", "world.fruit_y"),
                                         ("vyrn.screen().sum()", "vyrn.canvas()")])
def test_screen_functions_in_a_simulation_point_at_the_right_tool(spawn, make_project, call, words):
    error = step_error(spawn, make_project, {"points.py": f"import vyrn\n\ndef points(world):\n    return {call}\n"})
    assert error["kind"] == "input" and words in error["hint"]


@pytest.mark.parametrize("source, words", [
    ("def points(world):\n    if world.n:\n\treturn 1\n    return 0\n", "indented with a Tab"),
    ("def points(world):\n    if world.n = 9:\n        return 1\n    return 0\n", "== (two equals signs)"),
], ids=["tabs", "single-equals"])
def test_syntax_hints(spawn, make_project, source, words):
    error = spawn(make_project({"points.py": source})).load_error()
    assert error["kind"] == "syntax" and words in error["hint"]
