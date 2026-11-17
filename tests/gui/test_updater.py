"""Updater against a file:// URL (ctest gui_updater): never a real terminal, never the network, never ~/.config.

    VYRN_APP=build/app/vyrn python3 -B -m pytest tests/gui/test_updater.py -q -p no:cacheprovider
"""

import json
import os
import subprocess

import pytest

OLD = "a" * 40
NEW = "b" * 40


def run(tmp_path, stamp, latest):
    app = os.environ.get("VYRN_APP")
    if not app:
        pytest.skip("set VYRN_APP to the vyrn binary (ctest gui_updater does)")
    stamp_dir = tmp_path / "data"
    stamp_dir.mkdir(exist_ok=True)
    if stamp is not None:
        (stamp_dir / "commit").write_text(stamp + "\n")
    api = tmp_path / "latest"
    api.write_text(latest + "\n")
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", VYRN_UPDATE_URL=api.as_uri(),
               HOME=str(tmp_path / "home"), XDG_CONFIG_HOME=str(tmp_path / "cfg"))
    done = subprocess.run([app, "--update-test", str(stamp_dir)], env=env, capture_output=True, text=True, timeout=60)
    assert done.returncode == 0, done.stderr
    return json.loads(done.stdout.strip().splitlines()[-1])


def test_update_available(tmp_path):
    out = run(tmp_path, OLD, NEW)
    assert (out["result"], out["detail"]) == ("available", NEW)
    assert out["started"] and "get-" in out["command"] and "githubusercontent.com/LizardRush/vyrn/main" in out["command"]


def test_up_to_date(tmp_path):
    assert run(tmp_path, OLD, OLD)["result"] == "current"


def test_unstamped_copy_is_unknown(tmp_path):
    assert run(tmp_path, None, NEW)["result"] == "unknown"


def test_bad_answer_fails_plainly(tmp_path):
    out = run(tmp_path, OLD, "not a commit")
    assert out["result"] == "failed" and out["detail"]
