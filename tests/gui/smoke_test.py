"""GUI smoke test (ctest gui_smoke): the vyrn app opens a copy of catch-pictures offscreen, checks it, trains for
about a minute, marks an injected NameError on the right line and saves a screenshot that isn't blank
(docs/dev/app-internals.md section 9).

    VYRN_APP=build/app/vyrn python3 -B -m pytest tests/gui -q -p no:cacheprovider
    VYRN_APP=build/app/vyrn python3 -B tests/gui/smoke_test.py          # what ctest runs

The app always runs with QT_QPA_PLATFORM=offscreen: no window ever shows on the desktop.
"""

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
TEMPLATE = REPO / "templates" / "catch-pictures"
TRAIN_SECONDS = int(os.environ.get("VYRN_SMOKE_TRAIN_SECONDS", "60"))
MAIN_TABS = ["inputs.py", "responses.py", "points.py", "world.py"]
BOTTOM_TABS = ["Console", "Points", "Problems", "Docs"]


@pytest.fixture(scope="module")
def smoke(tmp_path_factory):
    app = os.environ.get("VYRN_APP")
    if not app:
        pytest.skip("set VYRN_APP to the vyrn binary (ctest gui_smoke does)")
    assert os.access(app, os.X_OK), f"VYRN_APP={app} is not an executable"

    work = tmp_path_factory.mktemp("gui-smoke")
    project = work / "catch-pictures"
    shutil.copytree(TEMPLATE, project, ignore=shutil.ignore_patterns("brains", "runs", "__pycache__", ".vyrn-lock"))
    shot = work / "window.png"
    env = dict(os.environ)
    env["QT_QPA_PLATFORM"] = "offscreen"          # never a window on the real desktop
    env.setdefault("VYRN_DATA", str(REPO))
    env["VYRN_PROJECTS"] = str(work / "projects")  # whatever happens, ~/Vyrn stays untouched
    cmd = [app, "--smoke-test", str(project), "--check", "--train-seconds", str(TRAIN_SECONDS),
           "--inject-error", "--screenshot", str(shot)]
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=TRAIN_SECONDS + 600)
    lines = [line for line in proc.stdout.splitlines() if line.startswith("{")]
    assert lines, (f"no JSON line from vyrn --smoke-test (exit {proc.returncode})\n"
                   f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr[-3000:]}")
    return {"result": json.loads(lines[-1]), "returncode": proc.returncode, "stderr": proc.stderr,
            "shot": shot, "project": project}


def test_every_step_succeeded(smoke):
    result = smoke["result"]
    assert result.get("ok") is True, f"failures: {result.get('failures')}\nstderr:\n{smoke['stderr'][-3000:]}"
    assert smoke["returncode"] == 0


def test_tabs_exist(smoke):
    assert smoke["result"]["tabs"] == MAIN_TABS
    assert smoke["result"]["bottom_tabs"] == BOTTOM_TABS


def test_check_passes_on_the_template(smoke):
    assert smoke["result"]["check_ok"] is True


def test_rounds_are_charted_while_training(smoke):
    assert smoke["result"]["rounds"] >= 5


def test_name_error_is_marked_on_its_line(smoke):
    # The app appends the NameError right after the last line of points.py.
    expected = {"file": "points.py", "line": len((TEMPLATE / "points.py").read_text().splitlines()) + 1}
    assert smoke["result"]["injected"] == expected
    assert smoke["result"]["error_marker"] == expected


def test_the_app_worked_on_its_own_copy(smoke):
    project = smoke["project"]
    assert (project / "points.py").read_text() == (TEMPLATE / "points.py").read_text()
    assert not (project / "brains").exists() and not (project / "runs").exists()


def test_screenshot_is_not_blank(smoke):
    from PIL import Image, ImageStat

    shot = smoke["shot"]
    assert shot.is_file() and shot.stat().st_size > 0, "no screenshot"
    with Image.open(shot) as image:
        assert image.width >= 800 and image.height >= 500, f"screenshot is only {image.size}"
        stddev = ImageStat.Stat(image.convert("RGB")).stddev
    assert max(stddev) > 5, f"the screenshot looks blank (stddev {stddev})"


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-q", "-p", "no:cacheprovider"]))
