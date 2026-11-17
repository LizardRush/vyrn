"""End to end: Ctrl+C (SIGINT) and SIGTERM stop vyrn-cli gracefully, whenever they come.

vyrn-cli takes the stop signals in its main loop, between printing batches of events. A thread that a shared library
starts before main() doesn't block them, so a signal that comes while the main loop is busy printing can land on
that thread. With the default action it killed vyrn-cli (exit -2, death by SIGINT) instead of stopping it. Now a
handler catches it there and the main loop reacts as usual.

Each run trains a fresh copy of templates/catch-numbers (about 170 rounds a second, so vyrn-cli prints all the
time) and sends one signal right after reading a "round" line, while vyrn-cli is still printing the rest of that
batch: the timing that failed. The run must stop, save the brain ("latest") and exit 0. There are 10 SIGINT runs
and 3 SIGTERM runs, each signal after a different round.

Environment: VYRN_CLI = the vyrn-cli program, VYRN_DATA = the repo. ctest: e2e_cli_signals (label e2e). By hand:
    VYRN_CLI=build/cli/vyrn-cli VYRN_DATA=$PWD python3 -B -m pytest tests/e2e/test_cli_signals.py -q -p no:cacheprovider
"""

import json
import os
import shutil
import signal
import subprocess
import threading
from pathlib import Path

import pytest

TEMPLATE = "templates/catch-numbers"
RUN_TIMEOUT_S = 300   # one run takes a few seconds; this is for a very busy machine
CASES = [(signal.SIGINT, run) for run in range(10)] + [(signal.SIGTERM, run) for run in range(3)]


def tool_paths():
    cli = os.environ.get("VYRN_CLI")
    data = os.environ.get("VYRN_DATA")
    if not cli or not data:
        pytest.skip("set VYRN_CLI (the vyrn-cli program) and VYRN_DATA (the repo folder) to run the e2e tests")
    cli, data = Path(cli), Path(data)
    assert cli.is_file(), f"VYRN_CLI={cli} is not a file: build vyrn-cli first"
    assert (data / TEMPLATE / "vyrn.json").is_file(), f"VYRN_DATA={data} has no {TEMPLATE}"
    return cli, data


def train_then_signal(cli, project, signum, after_round, seed):
    """Trains `project` with --json and sends `signum` right after reading the line of round `after_round`, then
    reads the rest. Returns (exit status, events, index of the first event read after the signal, stderr)."""
    stderr_path = project.with_name(project.name + "-stderr.txt")
    with open(stderr_path, "wb") as stderr:
        proc = subprocess.Popen([str(cli), "train", str(project), "--minutes", "10", "--seed", str(seed), "--json"],
                                stdout=subprocess.PIPE, stderr=stderr)
    watchdog = threading.Timer(RUN_TIMEOUT_S, proc.kill)
    watchdog.start()
    events, signalled_at = [], None
    try:
        for line in proc.stdout:
            events.append(json.loads(line))
            if signalled_at is None and events[-1].get("type") == "round" and events[-1]["round"] >= after_round:
                proc.send_signal(signum)
                signalled_at = len(events)
        proc.wait()
    finally:
        watchdog.cancel()
        if proc.poll() is None:
            proc.kill()
            proc.wait()
    return proc.returncode, events, signalled_at, stderr_path.read_text(errors="replace")


@pytest.mark.parametrize("signum,run", CASES, ids=[f"{s.name}-{r + 1}" for s, r in CASES])
def test_a_stop_signal_saves_the_brain_and_exits_0(tmp_path, signum, run):
    cli, data = tool_paths()
    project = shutil.copytree(data / TEMPLATE, tmp_path / "catch",
                              ignore=shutil.ignore_patterns("brains", "runs", "__pycache__"))
    after_round = 5 + 23 * run   # a different moment in each run (round 5 comes about 1 s after the start)
    code, events, signalled_at, stderr = train_then_signal(cli, project, signum, after_round, seed=run + 1)
    assert signalled_at is not None, f"the training ended before round {after_round}:\n{stderr[-3000:]}"
    errors = [e for e in events if e.get("type") == "error"]
    assert code == 0 and not errors, (
        f"{signum.name} after round {after_round}: exit status {code} (-{int(signum)}: killed by the signal)\n"
        + "\n".join(json.dumps(e) for e in errors) + "\n" + stderr[-3000:])
    assert "Stopping: saving the brain" in stderr, stderr[-3000:]
    saved = [e for e in events[signalled_at:] if e.get("type") == "checkpoint" and e.get("which") == "latest"]
    assert saved and Path(saved[-1]["path"]).is_dir(), (
        f"no brain saved after {signum.name}: {[e for e in events[signalled_at:] if e.get('type') != 'round']}")
    assert list(Path(saved[-1]["path"]).iterdir()), f"{saved[-1]['path']} is empty"
