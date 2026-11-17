"""End-to-end real-time learning: vyrn-cli trains tests/e2e/worlds/rt-catch in real time (PLAN.md sections 3 and 8).

rt-catch has no world.py, so Vyrn plays it like a real program: a Catch game runs by itself in a Python thread that
the project's scripts start (about 20 steps a second), see() reads three numbers from it and the choices set its
input (no keys, no screen). Vyrn decides 20 times a second on absolute deadlines while a learner thread updates
the brain.

For each seed in 1, 2, 3 (one after the other) it trains a fresh copy for at most TRAIN_MINUTES:
    vyrn-cli train <copy> --minutes 20 --seed S --json
and stops it (Ctrl+C: it saves the brain) as soon as the mean points of the last 100 rounds reach THRESHOLD. The
world passes when 2 of the 3 seeds get there (it stops as soon as that is decided). Then it watches a passing
brain for WATCH_TICKS ticks:
    vyrn-cli watch <copy> --ticks 600 --json
and the mean points per round must reach WATCH_THRESHOLD. For every run it prints the tick lateness (the median
and the worst of the 2-second windows' 99th percentiles) and the overruns (ticks skipped because one ran late).

Environment: VYRN_CLI = the vyrn-cli program, VYRN_DATA = the repo. ctest: e2e_rt_catch (label slow). By hand:
    VYRN_CLI=build/cli/vyrn-cli VYRN_DATA=$PWD python3 -m pytest tests/e2e/test_realtime.py -s
Real time measures the machine too: run it on a Pi that isn't busy with builds. See tests/e2e/README.md.
"""

import json
import math
import os
import shutil
import signal
import statistics
import subprocess
import time
from pathlib import Path

import pytest

# Set from measured runs (2026-11-14, tests/e2e/README.md): every seed reached THRESHOLD within 3.9-5.2 minutes
# (PLAN's goal: 20), and the watched brains scored 1.00.
WORLD = "tests/e2e/worlds/rt-catch"
TRAIN_MINUTES = 20
THRESHOLD = 0.7          # last-100 mean (+1 caught, -1 missed)
WATCH_TICKS = 600        # 30 s at 20 decisions per second
WATCH_THRESHOLD = 0.8    # mean points per round while watched
SEEDS = (1, 2, 3)
PASSES_NEEDED = 2


def tool_paths():
    cli = os.environ.get("VYRN_CLI")
    data = os.environ.get("VYRN_DATA")
    if not cli or not data:
        pytest.skip("set VYRN_CLI (the vyrn-cli program) and VYRN_DATA (the repo folder) to run the e2e tests")
    cli, data = Path(cli), Path(data)
    assert cli.is_file(), f"VYRN_CLI={cli} is not a file: build vyrn-cli first"
    assert (data / "python" / "vyrn_worker.py").is_file(), f"VYRN_DATA={data} has no python/vyrn_worker.py"
    return cli, data


def last_mean(points, n=100):
    tail = points[-n:]
    return sum(tail) / len(tail) if len(tail) >= n else -math.inf


def lateness(events):
    """(median, worst) of the timing windows' 99th-percentile tick lateness in ms, total overruns, mean ticks/s."""
    timing = [e for e in events if e.get("type") == "timing" and e.get("ticks_per_s", 0) > 0]
    p99 = [e.get("late_p99_ms", 0.0) for e in timing]
    overruns = sum(e.get("overruns", 0) for e in timing)
    speed = statistics.mean(e["ticks_per_s"] for e in timing) if timing else 0.0
    return (statistics.median(p99) if p99 else 0.0), (max(p99) if p99 else 0.0), overruns, speed


def run_cli(cli, args, stop_when=None, timeout_s=None):
    """Runs vyrn-cli with --json, reading its events as they come. stop_when(events) -> True sends Ctrl+C (the
    run saves and stops). Returns (events, seconds, seconds when stop_when first held or None)."""
    command = [str(cli), *map(str, args), "--json"]
    started = time.monotonic()
    proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    events, reached = [], None
    try:
        for line in proc.stdout:
            try:
                events.append(json.loads(line))
            except ValueError:
                continue
            if stop_when and reached is None and events[-1].get("type") == "round" and stop_when(events):
                reached = time.monotonic() - started
                proc.send_signal(signal.SIGINT)
            if timeout_s and time.monotonic() - started > timeout_s:
                proc.kill()
                break
        proc.wait(timeout=120)
    finally:
        if proc.poll() is None:
            proc.kill()
    errors = [e for e in events if e.get("type") == "error"]
    assert proc.returncode == 0 and not errors, (
        f"{' '.join(command)} failed (exit status {proc.returncode}):\n"
        + "\n".join(json.dumps(e) for e in errors) + "\n" + proc.stderr.read()[-3000:])
    return events, time.monotonic() - started, reached


def round_points(events):
    return [e["points"] for e in events if e.get("type") == "round"]


def describe(events, seconds):
    median, worst, overruns, speed = lateness(events)
    return (f"{seconds / 60:.1f} min, {speed:.1f} ticks/s, tick lateness p99 {median:.1f} ms median / "
            f"{worst:.1f} ms worst window, {overruns} overruns")


def test_rt_catch(tmp_path):
    cli, data = tool_paths()
    results, passing = [], []
    for seed in SEEDS:
        project = tmp_path / f"rt-catch-seed{seed}"
        shutil.copytree(data / WORLD, project, ignore=shutil.ignore_patterns("brains", "runs", "__pycache__"))
        events, seconds, reached = run_cli(
            cli, ["train", project, "--minutes", TRAIN_MINUTES, "--seed", seed],
            stop_when=lambda ev: last_mean(round_points(ev)) >= THRESHOLD, timeout_s=TRAIN_MINUTES * 60 + 300)
        points = round_points(events)
        rounds = [e for e in events if e.get("type") == "round"]
        best = max((last_mean(points[:i]) for i in range(100, len(points) + 1)), default=-math.inf)
        when = (f"reached {THRESHOLD} after {reached / 60:.1f} min ({rounds[-1]['total_ticks']} ticks)"
                if reached is not None else f"never reached {THRESHOLD} (best last-100 {best:.2f})")
        results.append(f"seed {seed}: {when}, {len(points)} rounds; {describe(events, seconds)}")
        print(f"rt_catch {results[-1]}")
        if reached is not None:
            passing.append(project)
        if len(passing) >= PASSES_NEEDED or len(results) - len(passing) > len(SEEDS) - PASSES_NEEDED:
            break
    assert len(passing) >= PASSES_NEEDED, (
        f"rt_catch learned in {len(passing)} of {len(results)} seeds (needs {PASSES_NEEDED}, last-100 mean >= "
        f"{THRESHOLD} within {TRAIN_MINUTES} min):\n  " + "\n  ".join(results))

    events, seconds, _ = run_cli(cli, ["watch", passing[-1], "--ticks", WATCH_TICKS])
    points = round_points(events)
    assert points, f"rt_catch: watching for {WATCH_TICKS} ticks finished no round"
    watched = sum(points) / len(points)
    print(f"rt_catch watch: mean {watched:.3f} over {len(points)} rounds; {describe(events, seconds)}")
    assert watched >= WATCH_THRESHOLD, (
        f"rt_catch: the trained brain scored {watched:.3f} per round while watched (needs {WATCH_THRESHOLD}); "
        "training: " + "; ".join(results))
