"""GUI test for Vyrn Link (ctest gui_link): the vyrn app opens a copy of templates/link-runner offscreen, checks it and
trains for a short while, with tools/link/link_fake_game.py playing the game (docs/dev/link.md). The project
window's Vyrn Link bar must show the address and the pairing code, then the connected game, and rounds must be
played.

    VYRN_APP=build/app/vyrn python3 -B -m pytest tests/gui/test_link_gui.py -q -p no:cacheprovider

The app always runs with QT_QPA_PLATFORM=offscreen, and Vyrn's Link server listens on a random high port.
"""

import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
TEMPLATE = REPO / "tests" / "fixtures" / "link-runner"
FAKE_GAME = REPO / "tools" / "link" / "link_fake_game.py"
TRAIN_SECONDS = 25


def test_link_bar_shows_the_game_and_training_plays(tmp_path):
    app = os.environ.get("VYRN_APP")
    if not app:
        pytest.skip("set VYRN_APP to the vyrn binary (ctest gui_link does)")
    assert os.access(app, os.X_OK), f"VYRN_APP={app} is not an executable"

    project = tmp_path / "link-runner"
    shutil.copytree(TEMPLATE, project, ignore=shutil.ignore_patterns("brains", "runs", "__pycache__", ".vyrn-*"))
    with socket.socket() as s:
        s.bind(("0.0.0.0", 0))
        port = s.getsockname()[1]
    settings = json.loads((project / "vyrn.json").read_text())
    settings["link"] = {"port": port}
    (project / "vyrn.json").write_text(json.dumps(settings, indent=2) + "\n")

    # The fake game keeps connecting, like a mod: to the app's Check, then to its Train.
    game = subprocess.Popen([sys.executable, "-B", str(FAKE_GAME), f"ws://127.0.0.1:{port}/", "--reconnect",
                             "--quiet"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    env = dict(os.environ)
    env["QT_QPA_PLATFORM"] = "offscreen"          # never a window on the real desktop
    env.setdefault("VYRN_DATA", str(REPO))
    env["VYRN_PROJECTS"] = str(tmp_path / "projects")
    try:
        proc = subprocess.run([app, "--smoke-test", str(project), "--check", "--train-seconds", str(TRAIN_SECONDS)],
                              env=env, capture_output=True, text=True, timeout=TRAIN_SECONDS + 600)
    finally:
        game.send_signal(signal.SIGINT)
        out, _ = game.communicate(timeout=30)
    lines = [line for line in proc.stdout.splitlines() if line.startswith("{")]
    assert lines, f"no JSON line from vyrn --smoke-test (exit {proc.returncode})\n{proc.stdout}\n{proc.stderr[-3000:]}"
    result = json.loads(lines[-1])
    summary = json.loads(out.strip().splitlines()[-1]) if out.strip() else {}
    print(f"gui link: {result.get('rounds')} rounds charted; link bar {result.get('link_bar')}; game {summary}")

    assert result.get("ok"), result
    assert result.get("check_ok"), result
    assert result.get("rounds", 0) > 0, result
    texts = result.get("link_bar", [])
    shown = [m.group(1) for t in texts if f":{port}/" in t for m in [re.search(r"pairing code (\d{6})\b", t)] if m]
    assert shown, f"the Link bar never showed the address and the pairing code: {texts}"
    assert len(set(shown)) == 1, f"the Link bar showed different codes: {texts}"
    assert any("Fake Runner (link_fake_game 1.0) is connected from 127.0.0.1" in t for t in texts), texts
    # The smoke test plays a copy of the project: the code it made there is the one Settings shows.
    assert result.get("settings_link_code") == shown[0], f"Settings → Vyrn Link: {result.get('settings_link_code')}"
    assert summary.get("connections", 0) >= 2, f"the game connected to Check and to Train: {summary}"
    assert summary.get("inputs", 0) > 0, summary
