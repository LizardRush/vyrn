"""End-to-end Vyrn Link: vyrn-cli trains templates/link-runner while tools/link/link_fake_game.py plays the game
(docs/dev/link.md), in real time and in lockstep.

For each mode it copies templates/link-runner to a temporary folder, sets "link" to a free high port (lockstep:
"lockstep": true, 2 frames per step) and, so that a short test can see learning, "advanced": {"explore_ticks": ...},
then starts

    vyrn-cli train <copy> --ticks N --seed 1 --json

and reads its first "link" event (the address and the pairing code). The real-time test first checks that the
pairing code is needed: the fake game connects to the LAN address (as a game on another computer would) without a
code and with a wrong one, and Vyrn refuses both. Then the fake game connects with the code and plays until the
training ends. The run must end without errors, and the AI must improve: the mean points of the last LAST rounds
beat those of the first FIRST rounds (the first rounds play almost at random) by the mode's "improve". Then

    vyrn-cli watch <copy> --brain latest --ticks WATCH_TICKS --json

plays the trained brain with the fake game connected again, and its mean points per round must reach the mode's
"watch".

Environment: VYRN_CLI = the vyrn-cli program, VYRN_DATA = the repo. ctest: e2e_link (label e2e). By hand:
    VYRN_CLI=build/cli/vyrn-cli VYRN_DATA=$PWD python3 -B -m pytest tests/e2e/test_link_e2e.py -s
Every server is Vyrn's own Link server on a random high port, closed when vyrn-cli ends.
"""

import json
import os
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
TEMPLATE = REPO / "tests" / "fixtures" / "link-runner"
FAKE_GAME = REPO / "tools" / "link" / "link_fake_game.py"

# Set from measured runs (2026-11-14, see tests/e2e/README.md "Vyrn Link"). Per mode: training ticks, exploration
# ticks (the template's 20000 would still explore most of the time in a short test), the improvement the last rounds
# must show over the first ones, and the mean a watched round must reach.
MODES = {   # measured: real time +70 and +210, Watch 467; lockstep +270 and +290, Watch 552
    "realtime": {"lockstep": False, "ticks": 12000, "explore": 4000, "improve": 30.0, "watch": 120.0},
    "lockstep": {"lockstep": True, "ticks": 30000, "explore": 5000, "improve": 120.0, "watch": 150.0},
}
FIRST = 20                # rounds at the start (exploring: about random play)
LAST = 20                 # rounds at the end
WATCH_TICKS = 1500


def tool_paths():
    cli = os.environ.get("VYRN_CLI")
    data = os.environ.get("VYRN_DATA")
    if not cli or not data:
        pytest.skip("set VYRN_CLI (the vyrn-cli program) and VYRN_DATA (the repo folder) to run the e2e tests")
    cli, data = Path(cli), Path(data)
    assert cli.is_file(), f"VYRN_CLI={cli} is not a file: build vyrn-cli first"
    assert (data / "python" / "vyrn_worker.py").is_file(), f"VYRN_DATA={data} has no python/vyrn_worker.py"
    return cli


def free_port():
    with socket.socket() as s:
        s.bind(("0.0.0.0", 0))
        return s.getsockname()[1]


def make_project(tmp_path, name, lockstep, explore_ticks):
    project = tmp_path / name
    shutil.copytree(TEMPLATE, project, ignore=shutil.ignore_patterns("brains", "runs", "__pycache__", ".vyrn-*"))
    settings = json.loads((project / "vyrn.json").read_text())
    settings["link"] = {"port": free_port(), "lockstep": lockstep, "frames_per_step": 2 if lockstep else 1}
    settings["advanced"] = {"explore_ticks": explore_ticks}
    (project / "vyrn.json").write_text(json.dumps(settings, indent=2) + "\n")
    return project


class Cli:
    """vyrn-cli ... --json, its events read by a thread as they come."""

    def __init__(self, cli, args):
        self.command = [str(cli), *map(str, args), "--json"]
        self.proc = subprocess.Popen(self.command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.events = []
        self.cond = threading.Condition()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.proc.stdout:
            try:
                event = json.loads(line)
            except ValueError:
                continue
            with self.cond:
                self.events.append(event)
                self.cond.notify_all()
        with self.cond:
            self.cond.notify_all()

    def wait_for(self, matches, timeout):
        """The first event that matches, waiting up to timeout seconds (None if vyrn-cli ended first)."""
        def found():
            return next((e for e in self.events if matches(e)), None)
        with self.cond:
            self.cond.wait_for(lambda: found() is not None or not self.reader.is_alive(), timeout)
            return found()

    def finish(self, timeout):
        try:
            self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.send_signal(signal.SIGINT)   # stops and saves; the assertion below reports it
            self.proc.wait(timeout=120)
        self.reader.join(timeout=10)
        errors = [e for e in self.events if e.get("type") == "error"]
        assert self.proc.returncode == 0 and not errors, (
            f"{' '.join(self.command)} failed (exit status {self.proc.returncode}):\n"
            + "\n".join(json.dumps(e) for e in errors) + "\n" + self.proc.stderr.read()[-3000:])
        return self.events

    def kill(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(timeout=30)


def fake_game(url, *args):
    return subprocess.Popen([sys.executable, "-B", str(FAKE_GAME), url, "--quiet", *args],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def end_game(game):
    """Stops the fake game (Ctrl+C) and returns its summary line."""
    if game.poll() is None:
        game.send_signal(signal.SIGINT)
    out, err = game.communicate(timeout=30)
    lines = [line for line in out.splitlines() if line.startswith("{")]
    return json.loads(lines[-1]) if lines else {"error": err[-500:]}


def round_points(events):
    return [e["points"] for e in events if e.get("type") == "round"]


def speed(events):
    timing = [e["ticks_per_s"] for e in events if e.get("type") == "timing" and e.get("ticks_per_s", 0) > 0]
    return statistics.mean(timing) if timing else 0.0


def check_code_is_needed(link):
    """A game on another computer must give the pairing code: no code, then a wrong one, are refused."""
    if link["address"] == "127.0.0.1":
        print("link: this computer has no network address; the pairing code test needs one")
        return
    url = link["url"]
    wrong = "123456" if link["code"] != "123456" else "654321"
    for args in ([], ["--code", wrong]):
        game = fake_game(url, *args)
        out, err = game.communicate(timeout=30)
        assert game.returncode == 2 and "bad_code" in err, (
            f"the fake game connected to {url} with {args or 'no code'}: exit {game.returncode}\n{out}\n{err}")


@pytest.mark.parametrize("mode", list(MODES))
def test_link_runner_learns(tmp_path, mode):
    cli = tool_paths()
    m = MODES[mode]
    lockstep, ticks = m["lockstep"], m["ticks"]
    project = make_project(tmp_path, f"link-runner-{mode}", lockstep, m["explore"])

    train = Cli(cli, ["train", project, "--ticks", ticks, "--seed", 1])
    game = None
    try:
        link = train.wait_for(lambda e: e.get("type") == "link", 60)
        assert link, f"vyrn-cli train printed no link event:\n{train.events}"
        assert link["port"] == json.loads((project / "vyrn.json").read_text())["link"]["port"]
        assert link["code"] == (project / ".vyrn-link-code").read_text().strip()
        assert link["lockstep"] is lockstep
        if not lockstep:
            check_code_is_needed(link)
        started = time.monotonic()
        game = fake_game(link["url"], "--code", link["code"], "--reconnect")
        assert train.wait_for(lambda e: e.get("type") == "link" and e.get("connected"), 30), "the game never connected"
        events = train.finish(timeout=ticks / 20 + 600)
        trained_s = time.monotonic() - started
    finally:
        train.kill()
        summary = end_game(game) if game else {}

    points = round_points(events)
    assert len(points) >= FIRST + LAST, f"{mode}: only {len(points)} rounds in {ticks} ticks"
    first = statistics.mean(points[:FIRST])
    last = statistics.mean(points[-LAST:])
    print(f"link {mode} train: {len(points)} rounds in {trained_s / 60:.1f} min ({speed(events):.0f} ticks/s); "
          f"first {FIRST} rounds {first:.1f}, last {LAST} {last:.1f}, best {max(points):.0f}; game {summary}")
    assert summary.get("connections", 0) >= 1 and summary.get("inputs", 0) >= ticks // 2, summary
    assert last >= first + m["improve"], (
        f"{mode}: the AI didn't improve enough: first {FIRST} rounds {first:.1f}, last {LAST} {last:.1f} "
        f"(needs +{m['improve']})")

    watch = Cli(cli, ["watch", project, "--brain", "latest", "--ticks", WATCH_TICKS])
    game = None
    try:
        link = watch.wait_for(lambda e: e.get("type") == "link", 60)
        assert link, f"vyrn-cli watch printed no link event:\n{watch.events}"
        game = fake_game(link["url"], "--code", link["code"], "--reconnect")
        events = watch.finish(timeout=WATCH_TICKS / 20 + 300)
    finally:
        watch.kill()
        summary = end_game(game) if game else {}
    points = round_points(events)
    assert points, f"{mode}: watching for {WATCH_TICKS} ticks finished no round"
    watched = statistics.mean(points)
    print(f"link {mode} watch: mean {watched:.1f} over {len(points)} rounds; game {summary}")
    assert watched >= m["watch"], (
        f"{mode}: the trained brain scored {watched:.1f} per round while watched (needs {m['watch']})")
