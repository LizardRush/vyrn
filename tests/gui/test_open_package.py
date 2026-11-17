"""Opening a shared project (.vrlp) in the app, offscreen (docs/dev/app-internals.md section 9, docs/dev/package.md).

A temporary copy of templates/catch-numbers is packed with the package CLI; `vyrn --smoke-test --open-package` then
imports it through the import dialog into a temporary projects folder ($VYRN_PROJECTS) and opens it. The project is
untrusted, so the trust banner shows. With --trust, pressing Check opens the trust dialog first; "Trust and run"
trusts the project and the Check passes. A last run trains it briefly, exports the brain as a .vrl and imports
that file again (a Check first, then the core's fit check).

    VYRN_APP=build-app/app/vyrn python3 -B -m pytest tests/gui -q -p no:cacheprovider

The package tool is $VYRN_PKG_TOOL, else build-pkg/driver/build/vyrn-pkg, else vyrn-cli next to the app's build.
"""

import json
import os
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
TEMPLATE = REPO / "templates" / "catch-numbers"
SCRIPTS = ["inputs.py", "responses.py", "points.py", "world.py"]
AUTHOR = "Smoke Test"


def pack_tool():
    candidates = [os.environ.get("VYRN_PKG_TOOL"), str(REPO / "build-pkg" / "driver" / "build" / "vyrn-pkg")]
    app = os.environ.get("VYRN_APP")
    if app:
        candidates.append(str(Path(app).resolve().parent.parent / "cli" / "vyrn-cli"))
    return next((c for c in candidates if c and os.access(c, os.X_OK)), None)


@pytest.fixture(scope="module")
def package(tmp_path_factory):
    app = os.environ.get("VYRN_APP")
    if not app:
        pytest.skip("set VYRN_APP to the vyrn binary")
    assert os.access(app, os.X_OK), f"VYRN_APP={app} is not an executable"
    tool = pack_tool()
    if not tool:
        pytest.skip("no program that writes .vrlp files: set VYRN_PKG_TOOL to vyrn-cli")

    work = tmp_path_factory.mktemp("open-package")
    project = work / "catch-numbers"
    shutil.copytree(TEMPLATE, project, ignore=shutil.ignore_patterns("brains", "runs", "__pycache__", ".vyrn-*"))
    out = work / "Catch.vrlp"
    proc = subprocess.run([tool, "pack", str(project), "--out", str(out), "--author", AUTHOR,
                           "--description", "Catch (numbers), shared for the GUI test."],
                          cwd=work, capture_output=True, text=True, timeout=300)
    assert proc.returncode == 0 and out.is_file(), f"{tool} pack failed:\n{proc.stdout}\n{proc.stderr}"
    return {"app": app, "file": out, "work": work}


def open_package(package, projects_name, *options):
    projects = package["work"] / projects_name
    env = dict(os.environ)
    env["QT_QPA_PLATFORM"] = "offscreen"      # never a window on the real desktop
    env.setdefault("VYRN_DATA", str(REPO))
    env["VYRN_PROJECTS"] = str(projects)       # ~/Vyrn stays untouched
    cmd = [package["app"], "--smoke-test", "--open-package", str(package["file"]), *options]
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=600)
    lines = [line for line in proc.stdout.splitlines() if line.startswith("{")]
    assert lines, (f"no JSON line from {' '.join(cmd[1:])} (exit {proc.returncode})\n"
                   f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr[-3000:]}")
    return {"result": json.loads(lines[-1]), "returncode": proc.returncode, "stderr": proc.stderr,
            "projects": projects}


@pytest.fixture(scope="module")
def opened(package):
    return open_package(package, "projects-opened")


@pytest.fixture(scope="module")
def trusted(package):
    return open_package(package, "projects-trusted", "--trust")


@pytest.fixture(scope="module")
def round_trip(package):
    # Trust, train a little, Brain > Export this brain (.vrl), then Brain > Import a brain (.vrl) with that file:
    # the import runs a Check first and passes its spec to the core's fit check.
    brain = package["work"] / "Catch-brain.vrl"
    run = open_package(package, "projects-round-trip", "--trust", "--train-seconds", "8",
                       "--export-brain", str(brain), "--import-brain", str(brain))
    run["brain"] = brain
    return run


def marker_of(run):
    return json.loads((Path(run["result"]["project"]) / ".vyrn-imported").read_text())


def test_import_opens_the_project_with_the_trust_banner(opened):
    result = opened["result"]
    assert result.get("ok") is True, f"failures: {result.get('failures')}\nstderr:\n{opened['stderr'][-3000:]}"
    assert opened["returncode"] == 0
    assert result["imported"] is True
    assert result["trust_banner"] is True
    assert result["tabs"] == SCRIPTS


def test_the_import_is_a_new_untrusted_project_in_the_projects_folder(opened):
    project = Path(opened["result"]["project"])
    assert project.parent == opened["projects"].resolve()
    for script in SCRIPTS:
        assert (project / script).read_text() == (TEMPLATE / script).read_text()
    marker = marker_of(opened)
    assert marker["trusted"] is False
    assert marker["file"] == "Catch.vrlp"
    assert marker["author"] == AUTHOR


def test_check_asks_for_trust_then_runs(trusted):
    result = trusted["result"]
    assert result.get("ok") is True, f"failures: {result.get('failures')}\nstderr:\n{trusted['stderr'][-3000:]}"
    assert result["trust_banner"] is True       # before trusting
    assert result["trust_dialog"] is True       # Check asked first instead of running
    assert result["trusted"] is True            # "Trust and run": trusted, banner gone
    assert result["check_ok"] is True           # and the Check went on and passed
    assert marker_of(trusted)["trusted"] is True


def test_a_trained_brain_goes_out_as_vrl_and_comes_back_in(round_trip):
    result = round_trip["result"]
    assert result.get("ok") is True, f"failures: {result.get('failures')}\nstderr:\n{round_trip['stderr'][-3000:]}"
    assert result["rounds"] > 0
    assert result["exported"] is True and round_trip["brain"].is_file()
    assert result["brain_imported"] is True
    project = Path(result["project"])
    brains = sorted(p.name for p in (project / "brains").iterdir() if p.is_dir())
    assert len(brains) == 2, brains
    assert json.loads((project / "vyrn.json").read_text())["brain"] == brains[-1]   # the imported one is active
