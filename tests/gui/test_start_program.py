"""GUI test for Settings → Start program (ctest gui_start_program): the vyrn app opens a copy of
templates/catch-numbers offscreen whose vyrn.json "start_program" is a harmless stand-in for a game: a tiny Python
script that only notes its pid in a log and waits. It never opens a window, and demo_game.py is never started.

- Check starts it (once), Train's own Check finds it still running and doesn't start it again, and Stop ends it.
- A copy the person started themselves is never touched.
- A program that can't start gives a plain message, and the Check still runs.
- A project nobody trusted yet never starts it (the trust dialog comes first).

    VYRN_APP=build/app/vyrn python3 -B -m pytest tests/gui/test_start_program.py -q -p no:cacheprovider

The app always runs with QT_QPA_PLATFORM=offscreen, so nothing appears on the real desktop.
"""

import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
TEMPLATE = REPO / "templates" / "catch-numbers"

# The stand-in "game": "start <pid>" when it starts, "term <pid>" when asked to end (SIGTERM). It ends by itself
# after two minutes, whatever happens to the test.
FAKE_GAME = """\
import os, signal, sys, time

def note(what):
    with open(sys.argv[1], "a") as log:
        log.write(f"{what} {os.getpid()}\\n")

def ended(signum, frame):
    note("term")
    sys.exit(0)

signal.signal(signal.SIGTERM, ended)
note("start")
time.sleep(120)
note("timeout")
"""


@pytest.fixture
def app():
    path = os.environ.get("VYRN_APP")
    if not path:
        pytest.skip("set VYRN_APP to the vyrn binary (ctest gui_start_program does)")
    assert os.access(path, os.X_OK), f"VYRN_APP={path} is not an executable"
    return path


def make_project(tmp_path, start_program):
    project = tmp_path / "catch-numbers"
    shutil.copytree(TEMPLATE, project, ignore=shutil.ignore_patterns("brains", "runs", "__pycache__", ".vyrn-*"))
    settings = json.loads((project / "vyrn.json").read_text())
    settings["start_program"] = start_program
    (project / "vyrn.json").write_text(json.dumps(settings, indent=2) + "\n")
    return project


def fake_game(tmp_path):
    """The stand-in game's script, its log, and the Start program command line that runs it."""
    script = tmp_path / "fake_game.py"
    script.write_text(FAKE_GAME)
    log = tmp_path / "game.log"
    return script, log, f'"{sys.executable}" -B "{script}" "{log}"'


def notes(log):
    return [line.split() for line in log.read_text().splitlines()] if log.exists() else []


def smoke(app, tmp_path, project, *args, env_extra=None):
    env = dict(os.environ)
    env["QT_QPA_PLATFORM"] = "offscreen"          # never a window on the real desktop
    env.setdefault("VYRN_DATA", str(REPO))
    env["VYRN_PROJECTS"] = str(tmp_path / "projects")
    env.update(env_extra or {})
    proc = subprocess.run([app, "--smoke-test", str(project), *args], env=env, capture_output=True, text=True,
                          timeout=900)
    lines = [line for line in proc.stdout.splitlines() if line.startswith("{")]
    assert lines, f"no JSON line from vyrn --smoke-test (exit {proc.returncode})\n{proc.stdout}\n{proc.stderr[-3000:]}"
    result = json.loads(lines[-1])
    print(f"start program: {result.get('start_program')} failures: {result.get('failures')}")
    return result


@pytest.mark.skipif(sys.platform == "win32", reason="Windows has no SIGTERM: Stop closes the program's windows (the "
                                                  "stand-in has none), then ends it after 3 s, so it can't log 'term'")
def test_check_starts_it_once_and_stop_ends_it(app, tmp_path):
    script, log, command = fake_game(tmp_path)
    project = make_project(tmp_path, command)
    # The person's own copy, already running: Vyrn starts its own one and never touches this.
    own = subprocess.Popen([sys.executable, "-B", str(script), str(log)])
    try:
        deadline = time.monotonic() + 20
        while ["start", str(own.pid)] not in notes(log) and time.monotonic() < deadline:
            time.sleep(0.05)
        assert ["start", str(own.pid)] in notes(log), "the stand-in game didn't start"

        result = smoke(app, tmp_path, project, "--check", "--train-seconds", "3")
        assert result.get("ok"), result
        assert result.get("check_ok"), result
        game = result.get("start_program")
        assert game, result
        assert game["starts"] == 1, f"Check starts it, Train finds it still running: {game}"
        assert game["running_after_check"] is True, game
        assert game["running_after_stop"] is False, game
        assert any(line.startswith("Started ") for line in game["console"]), game

        seen = notes(log)
        started = [pid for what, pid in seen if what == "start" and pid != str(own.pid)]
        assert len(started) == 1, f"Vyrn started it {len(started)} times: {seen}"
        assert ["term", started[0]] in seen, f"Stop didn't end the copy Vyrn started: {seen}"
        assert own.poll() is None, "Vyrn ended the copy the person started themselves"
        assert ["term", str(own.pid)] not in seen, seen
    finally:
        own.terminate()
        own.wait(timeout=30)


def test_a_program_that_cant_start_gives_a_plain_message(app, tmp_path):
    project = make_project(tmp_path, "vyrn-no-such-game --fullscreen")
    result = smoke(app, tmp_path, project, "--check")
    assert result.get("ok"), result
    assert result.get("check_ok"), "the Check runs anyway"
    game = result["start_program"]
    assert game["starts"] == 0, game
    assert any("couldn't start" in line and "no program called “vyrn-no-such-game”" in line
               for line in game["console"]), game


def test_a_project_nobody_trusted_never_starts_it(app, tmp_path):
    _, log, command = fake_game(tmp_path)
    project = make_project(tmp_path, command)
    # As if imported from a shared file and not trusted yet (imports never set start_program, but even then).
    (project / ".vyrn-imported").write_text(json.dumps({"file": "Catch.vrlp", "name": "Catch", "trusted": False}))
    result = smoke(app, tmp_path, project, "--check")
    assert not result.get("ok"), "Check opens the trust dialog instead of running"
    assert any("Check didn't start" in f for f in result.get("failures", [])), result
    assert result["start_program"]["starts"] == 0, result
    assert notes(log) == [], notes(log)


# The variable vyrn::platform::runtime_dir() starts from (pid files, the panic marker): docs/dev/platforms.md.
RUNTIME_VARIABLE = {"darwin": "TMPDIR", "win32": "LOCALAPPDATA"}.get(sys.platform, "XDG_RUNTIME_DIR")


def test_the_panic_key_stops_the_run_and_leaves_the_game_running(app, tmp_path):
    """The panic stop behind the app's hotkey (macOS, Windows), called directly: the run ends at once, the app's own
    Python worker is killed, the console says "Panic stop", and the game Vyrn started keeps running."""
    _, _, command = fake_game(tmp_path)
    project = make_project(tmp_path, command)
    runtime = tmp_path / "runtime"              # pid files and the panic marker stay in this test
    runtime.mkdir(mode=0o700)
    result = smoke(app, tmp_path, project, "--train-seconds", "4", "--panic",
                   env_extra={RUNTIME_VARIABLE: str(runtime)})
    assert result.get("ok"), result
    panic = result.get("panic")
    assert panic and panic["stopped"] is True, result
    assert panic["said"] is True, "the console didn't say Panic stop"
    assert panic["killed"] >= 1, f"no Python worker of this app was killed: {panic}"
    game = result["start_program"]
    assert game["starts"] == 1, game
    assert game["running_after_stop"] is True, f"the panic stop ended the game: {game}"

