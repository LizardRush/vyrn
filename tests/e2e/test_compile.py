"""Compile end to end (docs/dev/standalone.md section 5): train catch-numbers briefly with vyrn-cli, turn it into a
standalone folder with `vyrn-cli compile`, and play it with plain Python and the vyrn package only.

    vyrn-cli train <copy of templates/catch-numbers> --ticks 12000 --seed 1
    vyrn-cli compile <copy> --out <folder> --no-wheel
    python/tools/make_wheel.py -> the wheel, unpacked into a temp dir that goes on PYTHONPATH
    python3 <folder>/play.py --rounds 100 --speed 0 --json     (mean points must match vyrn-cli watch's, within noise)

Environment: VYRN_CLI = the vyrn-cli program (needs `compile`), VYRN_DATA = the repo folder. Safe to run anywhere:
HOME, XDG_* and VYRN_PROJECTS point into pytest's temp dirs, and keys can only go to the record backend.
"""

import json
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
CLI = os.environ.get("VYRN_CLI", "")
DATA = Path(os.environ.get("VYRN_DATA", str(REPO)))

if not CLI or not Path(CLI).is_file():
    pytest.skip("set VYRN_CLI (the vyrn-cli program) and VYRN_DATA to run the e2e tests", allow_module_level=True)


def _has_compile():
    try:
        r = subprocess.run([CLI, "compile", "--help"], capture_output=True, text=True, timeout=60)
    except OSError:
        return False
    return "compile" in r.stdout and "--no-wheel" in r.stdout


if not _has_compile():
    pytest.skip("VYRN_CLI doesn't have the compile command yet", allow_module_level=True)


@pytest.fixture(scope="module")
def env(tmp_path_factory):
    home = tmp_path_factory.mktemp("home")
    e = dict(os.environ)
    e.update(VYRN_DATA=str(DATA), VYRN_PROJECTS=str(home / "Vyrn"), HOME=str(home),
             XDG_CONFIG_HOME=str(home / "config"), XDG_DATA_HOME=str(home / "data"),
             VYRN_KEYS_BACKEND=f"record:{home / 'keys.log'}")
    e.pop("PYTHONPATH", None)
    return e


def cli(env, *args, timeout=1800):
    r = subprocess.run([CLI, *map(str, args)], capture_output=True, text=True, env=env, timeout=timeout)
    assert r.returncode == 0, f"vyrn-cli {args[0]}: exit {r.returncode}\n{r.stdout[-1500:]}\n{r.stderr[-1500:]}"
    return r


def events(text):
    out = []
    for line in text.splitlines():
        try:
            out.append(json.loads(line))
        except ValueError:
            pass
    return out


@pytest.fixture(scope="module")
def trained(env, tmp_path_factory):
    project = tmp_path_factory.mktemp("trained") / "catch"
    shutil.copytree(DATA / "templates" / "catch-numbers", project,
                    ignore=shutil.ignore_patterns("brains", "runs", "__pycache__"))
    cli(env, "train", project, "--ticks", 12000, "--seed", 1, "--json")
    return project


@pytest.fixture(scope="module")
def site(tmp_path_factory):
    """The vyrn package, built by make_wheel.py and unpacked (what `pip install` would put in site-packages)."""
    out = tmp_path_factory.mktemp("wheel")
    subprocess.run([sys.executable, "-B", str(REPO / "python" / "tools" / "make_wheel.py"), "--out-dir", str(out)],
                   check=True, capture_output=True, timeout=120)
    wheels = list(out.glob("vyrn-*-py3-none-any.whl"))
    assert len(wheels) == 1
    folder = out / "site"
    with zipfile.ZipFile(wheels[0]) as zf:
        zf.extractall(folder)
    assert any(n.endswith("/RECORD") for n in zf.namelist())
    return folder


@pytest.fixture(scope="module")
def compiled(env, trained, tmp_path_factory):
    out = tmp_path_factory.mktemp("compiled") / "Catch-play"
    cli(env, "compile", trained, "--out", out, "--no-wheel", "--json")
    for name in ("play.py", "brain.vrl", "vyrn.json", "README.txt", "inputs.py", "responses.py", "points.py",
                 "world.py"):
        assert (out / name).is_file(), name
    return out


def play(env, site, folder, *args, timeout=300):
    e = dict(env, PYTHONPATH=str(site))
    return subprocess.run([sys.executable, "-B", str(Path(folder) / "play.py"), *map(str, args)],
                          capture_output=True, text=True, env=e, timeout=timeout, cwd=str(site))


def test_compiled_ai_plays_like_the_app(env, trained, compiled, site):
    r = play(env, site, compiled, "--rounds", 100, "--speed", 0, "--json", "--seed", 1)
    assert r.returncode == 0, r.stderr
    rounds = [e for e in events(r.stdout) if "round" in e]
    assert [e["round"] for e in rounds] == list(range(1, 101))
    mean = sum(e["points"] for e in rounds) / len(rounds)
    assert mean >= 0.7, f"the compiled AI averaged {mean:.2f} points"
    watched = [e["points"] for e in events(cli(env, "watch", trained, "--ticks", 3000, "--speed", 0, "--json").stdout)
               if e.get("type") == "round"]
    assert watched, "vyrn-cli watch played no rounds"
    assert abs(mean - sum(watched) / len(watched)) <= 0.3, (mean, sum(watched) / len(watched))


def test_the_runtime_never_imports_torch(env, site, compiled):
    code = ("import sys, vyrn.runtime as r; rc = r.main(sys.argv[1], ['--rounds', '3', '--speed', '0', '--quiet']);"
            " print(rc, 'torch' in sys.modules)")
    r = subprocess.run([sys.executable, "-B", "-c", code, str(compiled)], capture_output=True, text=True,
                       env=dict(env, PYTHONPATH=str(site)), timeout=120, cwd=str(site))
    assert r.stdout.split() == ["0", "False"], r.stdout + r.stderr


def test_python_dash_m_vyrn_play(env, site, compiled):
    r = subprocess.run([sys.executable, "-B", "-m", "vyrn", "play", str(compiled), "--rounds", "2", "--speed", "0"],
                       capture_output=True, text=True, env=dict(env, PYTHONPATH=str(site)), timeout=120,
                       cwd=str(site))
    assert r.returncode == 0 and "Round 2" in r.stdout, r.stdout + r.stderr


def test_a_brain_that_does_not_fit_is_explained(env, site, compiled, tmp_path):
    folder = tmp_path / "changed"
    shutil.copytree(compiled, folder)
    (folder / "responses.py").write_text((folder / "responses.py").read_text() + "\n\ndef jump(world):\n    pass\n")
    ok = play(env, site, folder, "--rounds", 1, "--speed", 0, "--quiet")
    assert ok.returncode == 0, ok.stderr                       # a new choice at the end is fine
    inputs = (folder / "inputs.py").read_text()
    (folder / "inputs.py").write_text(inputs.replace('"basket": world.basket}', '"basket": world.basket, "x": 1.0}'))
    bad = play(env, site, folder, "--rounds", 1, "--speed", 0)
    assert bad.returncode == 1 and "doesn't fit these scripts" in bad.stderr, bad.stderr


def test_a_missing_vyrn_package_says_how_to_get_it(env, compiled):
    r = subprocess.run([sys.executable, "-B", str(compiled / "play.py")], capture_output=True, text=True,
                       env=dict(env, PYTHONPATH=""), timeout=60, cwd=str(compiled.parent))
    assert r.returncode != 0 and "pip install" in r.stderr, r.stderr


def test_options_are_checked(env, site, compiled):
    assert play(env, site, compiled, "--rounds", 0).returncode == 2
    assert play(env, site, compiled, "--explore", 2).returncode == 2
    assert play(env, site, compiled, "--help").returncode == 0
