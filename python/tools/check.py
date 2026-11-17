"""Check a Vyrn project without the app:  python3 python/tools/check.py <project> [--steps N]

Starts the project's worker in check mode (keys are never pressed), shows what the AI sees and
which choices it has, then plays a few random steps (a simulation) or, for a real game, runs restart()
and a few real-time ticks, and reports points, timings, warnings and errors.

Exit status: 0 = ok, 2 = usage problem, 3 = a problem in the scripts, 4 = Python stopped.
"""

import argparse
import json
import math
import os
import random
import sys
import time

sys.dont_write_bytecode = True       # no __pycache__ next to engine.py or in the vyrn package
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np  # noqa: E402

import engine  # noqa: E402
from vyrn import _platform  # noqa: E402  (engine put python/ on sys.path)

RAMP = " .:-=+*#%@"           # dark to bright
MAX_COLUMNS = 32              # picture preview width in pixels (each pixel is 2 characters wide)
MAX_ROWS = 24


def read_settings(project):
    """vyrn.json as a dict ({} when missing). Returns (settings, problem or None)."""
    path = os.path.join(project, "vyrn.json")
    if not os.path.exists(path):
        return {}, None
    try:
        with open(path, encoding="utf-8") as file:
            settings = json.load(file)
    except (OSError, ValueError) as err:
        return {}, f"vyrn.json can't be read ({err}); using the default settings."
    if not isinstance(settings, dict):
        return {}, "vyrn.json should hold a {...} object; using the default settings."
    return settings, None


def _number(value, default):
    return value if isinstance(value, (int, float)) and not isinstance(value, bool) and value > 0 else default


# -- printing ---------------------------------------------------------------------------------------

def ascii_picture(picture):
    """Lines of text that sketch a uint8 [H, W, 3] picture (brighter = denser character)."""
    height, width = picture.shape[:2]
    block = max(1, math.ceil(width / MAX_COLUMNS), math.ceil(height / MAX_ROWS))
    gray = picture.astype(np.float32) @ np.array([0.299, 0.587, 0.114], dtype=np.float32)
    rows = []
    for y in range(0, height, block):
        line = []
        for x in range(0, width, block):
            level = float(gray[y:y + block, x:x + block].mean())
            line.append(RAMP[min(len(RAMP) - 1, int(level / 256 * len(RAMP)))] * 2)
        rows.append("".join(line))
    border = "+" + "-" * len(rows[0]) + "+"
    scale = f" (1 character pair = {block}×{block} pixels)" if block > 1 else ""
    return [border] + [f"|{row}|" for row in rows] + [border + scale]


def number_lines(values, names):
    names = names or [f"input {i + 1}" for i in range(len(values))]
    width = max(len(name) for name in names)
    return [f"{name.ljust(width)}  {value:g}" for name, value in zip(names, values)]


def print_error(error, project):
    print()
    print(engine.format_error(error))
    script, line, col = error.get("script"), error.get("line") or 0, error.get("col") or 0
    if script and line:
        try:
            with open(os.path.join(project, script), encoding="utf-8", errors="replace") as file:
                code = file.read().splitlines()[line - 1]
            # Tabs are expanded here and for the caret alike, so the caret lines up on tab-indented lines.
            print(f"  {line:>4} | {code.expandtabs(8)}")
            if col:
                print(f"       | {' ' * len(code[:col - 1].expandtabs(8))}^")
        except (OSError, IndexError):
            pass
    if error.get("kind") == "exception" and error.get("exc_type"):
        print(f"  ({error['exc_type']})")
    if error.get("hint"):
        hint = "\n".join(("        " + row) if row else "" for row in error["hint"].splitlines())
        print(f"  Hint: {hint.lstrip()}")
    trace = error.get("trace") or []
    if len(trace) > 1:
        print("  Called from: " + " <- ".join(trace))


def print_spec(spec, hello):
    sim = spec["mode"] == "sim"
    print("  Mode: " + ("simulation (world.py found): runs as fast as possible" if sim else
                        "real time (no world.py): plays a real program"))
    obs = spec["obs"]
    if obs["kind"] == "picture":
        height, width = obs["shape"][:2]
        print(f"  The AI sees: a picture, {width}×{height} pixels")
    else:
        names = ", ".join(obs["names"]) if obs["names"] else "no names"
        print(f"  The AI sees: {obs['shape'][0]} numbers ({names})")
    print(f"  Choices ({len(spec['choices'])}), in order:")
    width = max(len(label) for label in spec["labels"])
    for index, (label, effect) in enumerate(zip(spec["labels"], spec["effects"]), 1):
        print(f"    {index}. {label.ljust(width)}  {effect}")
    hooks = "  ".join(f"{name}() {where}" for name, where in spec["hooks"].items())
    print(f"  Hooks: {hooks}")
    uses = [name for name in ("screen", "keys") if spec["uses"].get(name)]
    if spec["uses"].get("game"):
        uses.append("a game connected with Vyrn Link")
    if not sim:
        print("  Uses: " + (", ".join(uses) if uses else "neither the screen nor keys"))
        for name, label in (("screen", "Screen capture"), ("keys", "Keyboard"), ("focus", "Focus guard")):
            print("  " + backend_line(label, hello["backends"].get(name), hello.get("problems", {}).get(name)))


def backend_line(label, backend, problem, system=None):
    """ "Screen capture: grim", or what is missing on this system, e.g. "Screen capture: needs the Screen Recording
    permission (...)" on macOS."""
    if backend:
        return f"{label}: {backend}"
    text = (problem or "not available").strip()
    if (system or _platform.SYSTEM) == "macos" and "permission" in text.lower():
        # macOS gives the permission to the app that runs Python: here the terminal, not Vyrn.
        text += " (check.py: give it to your terminal app, like Terminal or iTerm.)"
    return f"{label}: {text}"


# -- running ----------------------------------------------------------------------------------------

class Report:
    def __init__(self):
        self.steps = 0
        self.points = []
        self.rounds_over = 0
        self.rounds_truncated = 0
        self.ms = {}
        self.observations = []
        self.last_obs = None
        self.warnings = []
        self.did = []
        self.shown = {}
        self.restart_ran = False
        self.restart_did = []

    def add_ms(self, ms):
        for name, value in (ms or {}).items():
            self.ms.setdefault(name, []).append(value)

    def add_result(self, result):
        self.add_ms(result.get("ms"))
        self.warnings += result.get("warnings") or []
        self.did += result.get("did") or []
        self.shown.update(result.get("show") or {})
        for key in ("final_obs", "obs"):
            if result.get(key) is not None:
                self.observations.append(result[key])
        self.last_obs = result["obs"]
        if result.get("points") is not None:
            self.points.append(result["points"])
        self.rounds_over += bool(result.get("game_over"))
        self.rounds_truncated += bool(result.get("truncated"))


def run_sim(worker, spec, steps, rng):
    report = Report()
    report.add_result(worker.request("reset", timeout=30.0))
    for _ in range(steps):
        report.add_result(worker.request("step", timeout=5.0, a=rng.randrange(len(spec["choices"]))))
        report.steps += 1
    return report


def run_realtime(worker, spec, ticks, rng, decisions_per_second):
    report = Report()
    if "restart" in spec["hooks"]:
        # Like Train, start with restart(): it may set things up that points() needs. Keys are only described.
        reply = worker.request("restart", timeout=30.0)
        report.add_ms(reply.get("ms"))
        report.restart_ran = True
        report.restart_did = reply.get("did") or []
    period = 1.0 / decisions_per_second
    timeout = max(1.0, 5 * period)
    next_tick = time.monotonic()
    for tick in range(ticks):
        report.add_result(worker.request("observe", timeout=timeout, first=(tick == 0)))
        reply = worker.request("act", timeout=timeout, a=rng.randrange(len(spec["choices"])))
        report.add_ms(reply.get("ms"))
        report.did += reply.get("did") or []
        report.steps += 1
        next_tick += period
        time.sleep(max(0.0, next_tick - time.monotonic()))
    return report


def check_warnings(spec, report, decisions_per_second):
    """(warnings, notes) Check adds from what happened during the run (PLAN.md section 2, Check mode)."""
    out, notes = [], []
    sim = spec["mode"] == "sim"
    if "game_over" in spec["hooks"] and not report.rounds_over:
        if sim:
            out.append(f"game_over() was never True in {report.steps} steps. "
                       "Make sure it becomes True when a round really is over.")
        else:
            # A real game rarely ends within a check this short: a note, not a warning.
            notes.append(f"game_over() stayed False during this short check ({report.steps / decisions_per_second:g} s). "
                         "That is normal if the game didn't end. If your game-over screen was showing, "
                         "game_over() needs fixing.")
    observations = report.observations
    if spec["uses"].get("game") and not sim:
        # Check never connects a game: vyrn.game was {} the whole time, so unchanging inputs say nothing.
        notes.append("No game is connected during Check, so vyrn.game was {} and see() used its values for "
                     "\"no game yet\". The game's real numbers arrive when the AI plays with the game connected.")
        observations = []
    if observations and spec["obs"]["kind"] == "picture":
        brightest = max(int(o.max()) for o in observations)
        if brightest < 16:
            out.append(f"The picture is almost black (brightest value {brightest} of 255): the AI can hardly "
                       "see anything.")
    if len(observations) > 1:
        if spec["obs"]["kind"] == "picture":
            first = observations[0]
            if all(o.shape == first.shape and np.array_equal(o, first) for o in observations[1:]):
                out.append("The picture never changed during the check: the AI can't learn from a picture "
                           "that always looks the same.")
        else:
            stacked = np.stack(observations)
            names = spec["obs"]["names"] or [f"input {i + 1}" for i in range(stacked.shape[1])]
            for index, name in enumerate(names):
                if np.all(stacked[:, index] == stacked[0, index]):
                    out.append(f'"{name}" never changed during the check, so the AI would ignore it.')
    see_ms = report.ms.get("see")
    if not sim and see_ms:
        tick_ms = 1000.0 / decisions_per_second
        mean = sum(see_ms) / len(see_ms)
        if mean > 0.8 * tick_ms:
            out.append(f"see() takes {mean:.0f} ms, over 80% of a tick ({tick_ms:.0f} ms at "
                       f"{decisions_per_second:g} decisions per second). Use a smaller screen area, "
                       "or fewer decisions per second.")
    return out, notes


def print_report(spec, report, decisions_per_second, extra_warnings, notes=()):
    sim = spec["mode"] == "sim"
    print()
    if sim:
        ended = report.rounds_over + report.rounds_truncated
        print(f"Played {report.steps} random steps: {ended} round(s) ended "
              f"({report.rounds_over} game over, {report.rounds_truncated} at the round limit).")
    else:
        print(f"Ran {report.steps} real-time ticks at {decisions_per_second:g} per second (no keys pressed).")
        if report.restart_ran:
            done = ", ".join(dict.fromkeys(report.restart_did)) or "ran (it has no keys to press)"
            print(f"  restart(): {done}")
    if report.points:
        values = report.points
        counts = {}
        for value in values:
            if value:
                counts[value] = counts.get(value, 0) + 1
        detail = ", ".join(f"{value:+g} ×{count}" for value, count in sorted(counts.items()))
        zeros = len(values) - sum(counts.values())
        if detail and zeros:
            detail += f", 0 ×{zeros}"
        print(f"  Points: average {sum(values) / len(values):+.3f} per tick ({detail or 'always 0'})")
    timings = "  ".join(f"{name} {sum(v) / len(v):.3f}" for name, v in report.ms.items() if any(v))
    print(f"  Average time per call (ms): {timings or 'all under 0.001'}")
    if report.did:
        recent = list(dict.fromkeys(report.did))[:6]
        print("  Effects (described, never pressed): " + ", ".join(recent))
    if report.shown:
        print("  vyrn.show(): " + ", ".join(
            f"{name} = {'a picture' if isinstance(value, np.ndarray) else value!r}"
            for name, value in report.shown.items()))

    print()
    print("What the AI saw last:")
    if spec["obs"]["kind"] == "picture":
        for row in ascii_picture(report.last_obs):
            print("  " + row)
    else:
        for row in number_lines(report.last_obs.tolist(), spec["obs"]["names"]):
            print("  " + row)

    warnings = [f"{w['script']}:{w['line']}: {w['message']}" if w.get("script") and w.get("line")
                else (f"{w['script']}: {w['message']}" if w.get("script") else w["message"])
                for w in spec.get("warnings", []) + report.warnings]
    warnings += extra_warnings
    print()
    if warnings:
        print("Warnings:")
        for warning in dict.fromkeys(warnings):
            print(f"  - {warning}")
    else:
        print("No warnings.")
    if notes:
        print("Notes:")
        for note in notes:
            print(f"  - {note}")


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")   # a Windows console code page can't show every character
        except (AttributeError, ValueError):
            pass
    parser = argparse.ArgumentParser(description="Check a Vyrn project without the app.")
    parser.add_argument("project", help="the project folder (with inputs.py, responses.py, points.py)")
    parser.add_argument("--steps", type=int, default=None,
                        help="random steps to play (default: 20 for a simulation, 5 ticks for real time)")
    parser.add_argument("--seed", type=int, default=0, help="random seed (default 0)")
    parser.add_argument("--python", default=None, help="Python to run the scripts with")
    args = parser.parse_args(argv)

    project = os.path.abspath(args.project)
    if not os.path.isdir(project):
        print(f"check.py: there is no project folder at {project}", file=sys.stderr)
        return 2
    settings, problem = read_settings(project)
    python = args.python or settings.get("python") or sys.executable
    decisions_per_second = float(_number(settings.get("decisions_per_second"), 10.0))
    round_limit = int(_number(settings.get("round_ticks"), 1000))

    print(f"Checking {project}")
    if problem:
        print(f"  Note: {problem}")
    console = {"stdout": [], "stderr": []}
    try:
        worker = engine.Worker(project, python=python,
                               on_output=lambda stream, text: console[stream].append(text))
    except (engine.WorkerDied, OSError) as err:
        print(f"\nPython stopped before it could start.\n{err}")
        return 4

    status = 0
    try:
        spec = worker.load(check=True, seed=args.seed, round_limit=round_limit,
                           decisions_per_second=decisions_per_second)
        print_spec(spec, worker.hello)
        rng = random.Random(args.seed)
        if spec["mode"] == "sim":
            report = run_sim(worker, spec, 20 if args.steps is None else args.steps, rng)
        else:
            report = run_realtime(worker, spec, 5 if args.steps is None else max(1, args.steps), rng,
                                  decisions_per_second)
        warnings, notes = check_warnings(spec, report, decisions_per_second)
        if spec["uses"].get("game") and spec["mode"] != "sim" and not settings.get("link"):
            warnings.append('This project talks to a game with Vyrn Link (vyrn.game, vyrn.send or "game ..." '
                            'choices), but vyrn.json has no "link", so Vyrn won\'t wait for the game. Add '
                            '"link": {"port": 7457}.')
        print_report(spec, report, decisions_per_second, warnings, notes)
    except engine.WorkerError as err:
        print_error(err.error, project)
        status = 3
    except engine.WorkerDied as err:
        print(f"\n{err}")          # already ends with Python's last error lines
        status = 4
    finally:
        worker.close()

    streams = [("stdout", "Your print() output")]
    if status != 4:
        streams.append(("stderr", "Python's error output"))
    for stream, title in streams:
        output = "".join(console[stream]).splitlines()
        if output:
            print()
            print(f"{title} (last {min(10, len(output))} of {len(output)} lines):")
            for line in output[-10:]:
                print(f"  | {line}")
    if status == 0:
        print()
        print("Check passed: no errors.")
    return status


if __name__ == "__main__":
    sys.exit(main())
