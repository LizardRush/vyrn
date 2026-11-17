"""The command-line tools: tools/check.py (python-internals.md section 12) and tools/gen_api.py (the editor's
autocompletion list and docs page must cover the whole public API)."""

import re
import subprocess
import sys

import pytest

from testkit import TOOLS_DIR

CHECK = TOOLS_DIR / "check.py"
GEN_API = TOOLS_DIR / "gen_api.py"
PUBLIC = ["screen", "pixel", "on_screen", "press", "wait", "canvas", "show", "choice", "tick", "round",
          "game", "events", "connected", "send"]


def run_check(fakes, project, *args, timeout=120):
    return subprocess.run([sys.executable, str(CHECK), str(project), *args], env=fakes.env(), capture_output=True,
                          text=True, timeout=timeout)


def output(result):
    return result.stdout + result.stderr


needs_check = pytest.mark.skipif(not CHECK.exists(), reason="python/tools/check.py is missing")


@needs_check
@pytest.mark.parametrize("template, words", [("catch-pictures", ["left", "right", "stay"]),
                                             ("catch-numbers", ["left", "right", "stay", "fruit x"])])
def test_check_passes_on_the_templates(fakes, copy_template, template, words):
    project = copy_template(template)
    result = run_check(fakes, project)
    assert result.returncode == 0, output(result)
    for word in words:
        assert word in result.stdout, result.stdout
    assert not (project / "__pycache__").exists()
    assert not fakes.keys.exists() or fakes.keys.actions() == []


@needs_check
def test_check_reports_a_script_error(fakes, copy_template):
    project = copy_template("catch-pictures")
    points = project / "points.py"
    points.write_text(points.read_text().replace("world.fruit_y < 9", "world.fruit_yy < 9"))
    result = run_check(fakes, project, "--steps", "5")
    assert result.returncode == 3, output(result)
    assert f"points.py:{project.line('points.py', 'fruit_yy')}" in output(result)


@needs_check
def test_check_reports_a_syntax_error(fakes, copy_template):
    project = copy_template("catch-numbers")
    inputs = project / "inputs.py"
    inputs.write_text(inputs.read_text().replace("def see(world):", "def see(world:"))
    with pytest.raises(SyntaxError) as expected:
        compile(inputs.read_text(), "inputs.py", "exec")
    result = run_check(fakes, project)
    assert result.returncode == 3, output(result)
    assert f"inputs.py:{expected.value.lineno}" in output(result)


@needs_check
def test_check_on_a_real_game_presses_nothing(fakes, make_project):
    project = make_project({"responses.py": 'left = "a"\njump = "tap space"\nwait = None\n'}, sim=False)
    result = run_check(fakes, project, "--steps", "5")
    assert result.returncode == 0, output(result)
    assert "left" in result.stdout and "jump" in result.stdout
    assert not fakes.keys.exists() or fakes.keys.actions() == []


@pytest.mark.skipif(not GEN_API.exists(), reason="python/tools/gen_api.py is not written yet")
def test_gen_api_covers_every_public_name(tmp_path):
    out = tmp_path / "resources"  # never the repo's app/resources
    result = subprocess.run([sys.executable, str(GEN_API), "--out-dir", str(out)], capture_output=True, text=True,
                            timeout=120)
    assert result.returncode == 0, output(result)

    api = (out / "vyrn.api").read_text(encoding="utf-8")
    entries = {match.group(1) for match in re.finditer(r"^vyrn\.(\w+)", api, re.MULTILINE)}
    assert entries == set(PUBLIC), f"vyrn.api must list exactly the public names, got {sorted(entries)}"
    for name in ["screen", "pixel", "on_screen", "press", "wait", "canvas", "show", "send"]:
        assert re.search(rf"^vyrn\.{name}\(", api, re.MULTILINE), f"no call tip for vyrn.{name}"

    html = (out / "docs" / "api.html").read_text(encoding="utf-8")
    for name in PUBLIC:
        assert re.search(rf"""id=["']{name}["']""", html), f"api.html has no #{name} anchor (F1 help)"


SHIPPED_API = TOOLS_DIR.parents[1] / "app" / "resources" / "vyrn.api"


@pytest.mark.skipif(not (GEN_API.exists() and SHIPPED_API.exists()), reason="no generated app/resources/vyrn.api yet")
def test_shipped_vyrn_api_is_up_to_date():
    """The editor's vyrn.api and api.html in app/resources match the library's docstrings (read-only check)."""
    result = subprocess.run([sys.executable, str(GEN_API), "--check"], capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, "run python3 python/tools/gen_api.py to regenerate:\n" + output(result)


@needs_check
def test_check_runs_restart_first_in_real_time(fakes, make_project):
    """restart(world) may set up what points() needs; Check runs it first, like Train (keys only described)."""
    project = make_project({"points.py": """
        import vyrn

        def restart(world):
            world.alive = 0
            vyrn.press("space")

        def points(world):
            world.alive += 1
            return world.alive
        """}, sim=False)
    result = run_check(fakes, project, "--steps", "3")
    assert result.returncode == 0, output(result)
    assert "restart(): would tap Space" in result.stdout
    assert not fakes.keys.exists() or fakes.keys.actions() == []


@needs_check
def test_real_time_check_notes_a_game_over_that_never_came(fakes, make_project):
    project = make_project({"inputs.py": "import vyrn\n\ndef see():\n    return [vyrn.tick]\n",
                            "points.py": "def points():\n    return 1\n\ndef game_over():\n    return False\n"},
                           sim=False)
    result = run_check(fakes, project, "--steps", "2")
    assert result.returncode == 0, output(result)
    assert "No warnings." in result.stdout and "Notes:" in result.stdout
    assert "game_over() stayed False during this short check" in result.stdout


@needs_check
def test_check_warns_about_an_almost_black_picture(fakes, make_project):
    project = make_project({"inputs.py": """
        import numpy as np

        def see(world):
            board = np.zeros((10, 10), dtype=np.uint8)
            board[world.n % 10, 3] = 1          # 0 and 1: what .astype('uint8') makes of a 0-1 picture
            return board
        """})
    result = run_check(fakes, project, "--steps", "3")
    assert result.returncode == 0, output(result)
    assert "almost black (brightest value 1 of 255)" in result.stdout


@needs_check
def test_the_caret_lines_up_on_tab_indented_lines(fakes, make_project):
    project = make_project({"points.py": "def points(world):\n\treturn world.n + scor\n"})
    result = run_check(fakes, project, "--steps", "2")
    assert result.returncode == 3, output(result)
    lines = result.stdout.splitlines()
    code = next(i for i, line in enumerate(lines) if "| " in line and "return world.n + scor" in line)
    assert lines[code + 1].index("^") == lines[code].index("scor"), "\n".join(lines[code:code + 2])
