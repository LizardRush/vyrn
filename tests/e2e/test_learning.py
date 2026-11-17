"""End-to-end learning: vyrn-cli trains small simulations and they must really learn (PLAN.md section 8).

Each world is trained with up to three seeds, one after the other, from a fresh copy of its folder:
    vyrn-cli train <copy> --ticks N --seed S --json
A world passes when its metric reaches the threshold for at least 2 of the 3 seeds (it stops as soon as that is
decided). Then the brain of a passing copy is watched without learning, as fast as possible:
    vyrn-cli watch <copy> --ticks M --speed 0 --json
and the mean points per round while watching must reach the watch threshold.

Environment: VYRN_CLI = the vyrn-cli program, VYRN_DATA = the folder with python/ and templates/ (the repo).
ctest runs one world per test (e2e_catch_numbers, e2e_catch_pictures, e2e_cartpole; label e2e). By hand:
    VYRN_CLI=build/cli/vyrn-cli VYRN_DATA=$PWD python3 -m pytest tests/e2e/test_learning.py -k catch_numbers -s
See tests/e2e/README.md.
"""

import json
import math
import os
import shutil
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path

import pytest


@dataclass(frozen=True)
class World:
    folder: str              # the project, relative to VYRN_DATA
    train_ticks: int         # training budget per seed
    metric: str              # "last100": mean points of the last 100 rounds; "best20": best mean of 20 rounds in a row
    threshold: float         # a seed passes when its metric reaches this
    watch_ticks: int         # how long the trained brain is watched (unpaced)
    watch_threshold: float   # the mean points per round it must reach while watched


# Budgets and thresholds: set from measured runs (M2, 3 seeds per world, 2026-11-13). All 9 seeds passed; each
# budget is at least 2.2x the latest tick at which a seed first reached its threshold. The thresholds are PLAN.md's
# targets. The measurements are in tests/e2e/README.md.
WORLDS = {
    #                          folder                       train    metric     threshold  watch   watch threshold
    "catch_numbers": World("templates/catch-numbers", 30_000, "last100", 0.8, 2_000, 0.8),
    "catch_pictures": World("templates/catch-pictures", 50_000, "last100", 0.8, 2_000, 0.8),
    "cartpole": World("tests/e2e/worlds/cartpole", 150_000, "best20", 400.0, 5_000, 300.0),
}
SEEDS = (1, 2, 3)
PASSES_NEEDED = 2
RUN_TIMEOUT_S = 1800         # one vyrn-cli run; ctest's own timeout for the whole world is 3600 s


def tool_paths():
    cli = os.environ.get("VYRN_CLI")
    data = os.environ.get("VYRN_DATA")
    if not cli or not data:
        pytest.skip("set VYRN_CLI (the vyrn-cli program) and VYRN_DATA (the repo folder) to run the e2e tests")
    cli, data = Path(cli), Path(data)
    assert cli.is_file(), f"VYRN_CLI={cli} is not a file: build vyrn-cli first"
    assert (data / "python" / "vyrn_worker.py").is_file(), f"VYRN_DATA={data} has no python/vyrn_worker.py"
    return cli, data


def copy_world(source, target):
    shutil.copytree(source, target, ignore=shutil.ignore_patterns("brains", "runs", "__pycache__"))
    return target


def run_cli(cli, *args):
    """Runs vyrn-cli with --json and returns its events. Fails on a non-zero exit or an error event."""
    command = [str(cli), *map(str, args), "--json"]
    started = time.monotonic()
    proc = subprocess.run(command, capture_output=True, text=True, timeout=RUN_TIMEOUT_S)
    events = []
    for line in proc.stdout.splitlines():
        try:
            events.append(json.loads(line))
        except ValueError:
            pass
    errors = [e for e in events if e.get("type") == "error"]
    assert proc.returncode == 0 and not errors, (
        f"{' '.join(command)} failed (exit status {proc.returncode}):\n"
        + "\n".join(json.dumps(e) for e in errors) + "\n" + proc.stderr[-3000:])
    return events, time.monotonic() - started


def round_points(events):
    return [e["points"] for e in events if e.get("type") == "round"]


def mean_of_last(points, n):
    tail = points[-n:]
    return sum(tail) / len(tail) if tail else -math.inf


def best_mean(points, n):
    if len(points) < n:
        return -math.inf
    window = sum(points[:n])
    best = window
    for i in range(n, len(points)):
        window += points[i] - points[i - n]
        best = max(best, window)
    return best / n


def metric(name, points):
    if name == "last100":
        return mean_of_last(points, 100)
    if name == "best20":
        return best_mean(points, 20)
    raise ValueError(name)


def first_reached(events, name, threshold):
    """The brain's total ticks when a full window (100 or 20 rounds) first had a mean >= threshold, or None."""
    window = {"last100": 100, "best20": 20}[name]
    rounds = [e for e in events if e.get("type") == "round"]
    total = 0.0
    for i, r in enumerate(rounds):
        total += r["points"]
        if i >= window:
            total -= rounds[i - window]["points"]
        if i + 1 >= window and total / window >= threshold:
            return r.get("total_ticks")
    return None


def ticks_per_second(events):
    speeds = [e["ticks_per_s"] for e in events if e.get("type") == "timing" and e.get("ticks_per_s")]
    return sum(speeds) / len(speeds) if speeds else 0.0


@pytest.mark.parametrize("name", list(WORLDS))
def test_learning(name, tmp_path):
    cli, data = tool_paths()
    world = WORLDS[name]
    results, passing = [], []
    for seed in SEEDS:
        project = copy_world(data / world.folder, tmp_path / f"{name}-seed{seed}")
        events, seconds = run_cli(cli, "train", project, "--ticks", world.train_ticks, "--seed", seed)
        points = round_points(events)
        value = metric(world.metric, points)
        reached = first_reached(events, world.metric, world.threshold)
        when = f"first reached at {reached} ticks" if reached is not None else "never reached"
        results.append(f"seed {seed}: {world.metric} = {value:.3f} after {len(points)} rounds, {world.threshold} "
                       f"{when} ({seconds:.0f} s, {ticks_per_second(events):.0f} ticks/s)")
        print(f"{name} {results[-1]}")
        if value >= world.threshold:
            passing.append(project)
        failed = len(results) - len(passing)
        if len(passing) >= PASSES_NEEDED or failed > len(SEEDS) - PASSES_NEEDED:
            break
    assert len(passing) >= PASSES_NEEDED, (
        f"{name} learned in {len(passing)} of {len(results)} seeds (needs {PASSES_NEEDED}, "
        f"{world.metric} >= {world.threshold}):\n  " + "\n  ".join(results))

    events, seconds = run_cli(cli, "watch", passing[-1], "--ticks", world.watch_ticks, "--speed", 0)
    points = round_points(events)
    assert points, f"{name}: watching for {world.watch_ticks} ticks finished no round"
    watched = sum(points) / len(points)
    print(f"{name} watch: mean {watched:.3f} over {len(points)} rounds ({seconds:.0f} s)")
    assert watched >= world.watch_threshold, (
        f"{name}: the trained brain scored {watched:.3f} per round while watched "
        f"(needs {world.watch_threshold}); training: " + "; ".join(results))
