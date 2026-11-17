# End-to-end learning tests

`test_learning.py` checks that Vyrn really teaches an AI: `vyrn-cli` trains small simulations from scratch and
the trained brains must play well (PLAN.md section 8, "End-to-end learning").

| World | Project | What it learns |
|---|---|---|
| `catch_numbers` | `templates/catch-numbers` | catch a falling fruit, seeing 3 numbers |
| `catch_pictures` | `templates/catch-pictures` | the same game, seeing a 10x10 picture |
| `cartpole` | `tests/e2e/worlds/cartpole` | balance a pole on a cart (CartPole-v1 physics, at most 500 ticks per round) |

## What a test does

For each seed in 1, 2, 3 (one after the other) it copies the world into a temporary folder and runs

    vyrn-cli train <copy> --ticks N --seed S --json

then reads the `round` events. A seed passes when its metric reaches the threshold:

- `last100`: the mean points of the last 100 rounds (catch: +1 caught, -1 missed);
- `best20`: the best mean points of 20 rounds in a row (cartpole: 1 point per tick the pole stays up).

The world passes when 2 of the 3 seeds pass; it stops as soon as that is decided. Then it watches the brain of a
passing copy without learning, as fast as possible (`--speed 0`, so the test doesn't wait for the 10 decisions per
second a person would watch at):

    vyrn-cli watch <copy> --ticks M --speed 0 --json

and the mean points per round while watching must reach the watch threshold. Any `error` event or a non-zero exit
status fails the test.

## Running them

ctest runs one world per test (`e2e_catch_numbers`, `e2e_catch_pictures`, `e2e_cartpole`, label `e2e`, 1 hour
timeout each):

    ctest --test-dir build -L e2e --output-on-failure

By hand (add `-s` to see each seed's result as it finishes):

    VYRN_CLI=build/cli/vyrn-cli VYRN_DATA=$PWD python3 -m pytest tests/e2e/test_learning.py -k cartpole -s

`VYRN_CLI` is the `vyrn-cli` program, `VYRN_DATA` the folder that holds `python/` and `templates/` (the repo).
Without them the tests are skipped.

## Budgets and thresholds

The table at the top of `test_learning.py` ("set from measured runs (M2)") holds each world's training ticks, metric,
threshold, watch ticks and watch threshold. They were measured on the Raspberry Pi 5 on 2026-11-13 with `build/`
(RelWithDebInfo): three seeds per world, one run at a time, each followed by the watch run. Other programs kept two
to three of the four cores busy the whole time, so the speeds are on the slow side; learning is counted in ticks,
so a slower Pi takes longer but learns the same.

| World | Budget, pass when | Metric for seeds 1, 2, 3 | Threshold first reached at (ticks) | Time per seed | Watch, seeds 1, 2, 3 (threshold) |
|---|---|---|---|---|---|
| catch_numbers | 30k ticks, last-100 mean >= 0.8 | 0.98, 0.98, 1.00 | 12.9k, 13.5k, 11.2k | 27 s (1170 ticks/s) | 1.00, 1.00, 1.00 over 222 rounds (0.8) |
| catch_pictures | 50k ticks, last-100 mean >= 0.8 | 1.00, 0.98, 1.00 | 11.4k, 14.7k, 12.6k | 3.6-5.8 min (145-233 ticks/s) | 0.99, 1.00, 1.00 over 222 rounds (0.8) |
| cartpole | 150k ticks, best 20-round mean >= 400 | 500, 500, 500 | 20.7k, 20.6k, 20.1k | 2.2-2.6 min (980-1150 ticks/s) | 500, 500, 500 over 10 rounds (300) |

All nine seeds passed, so the "2 of 3 seeds" rule has room to spare. The margins:

- **Budgets** are 2.2x (catch_numbers), 3.4x (catch_pictures) and 7x (cartpole) the latest tick at which a seed
  first reached its threshold. Exploration falls to 5% at 20k ticks, so every budget also ends with at least 10k
  mostly-greedy ticks.
- **Thresholds** are PLAN.md's targets (0.8 for both catch worlds, 400 for cartpole); the measured finals are 0.98-1.0
  and 500.
- **cartpole** keeps a long budget for Watch's sake: its 20-round mean first passes 400 at ~20k ticks, but DQN on
  cartpole wobbles afterwards (last-100 means between 150 and 480 later in a run), and the Best brain that Watch
  plays reached a perfect 500 window at 140k, 142k and 25k ticks (seeds 1, 2, 3). The best-window metric and the watch threshold
  of 300 allow for that wobble.
- **Speed:** the catch worlds and cartpole run at ~1200-1700 ticks/s until learning starts; after that the brain's
  updates set the pace (MLP ~2 ms per update, small CNN 17-40 ms on this busy Pi, at 0.25 updates per tick), see
  `docs/dev/bench.md`.

`test_learning.py` prints, per seed, the metric, the tick at which the threshold was first reached, the time and the
ticks per second (run pytest with `-s`). Two seeds per world are enough when both pass: `ctest --test-dir build -L e2e`
took 28.5 minutes here with other builds running (catch_numbers 2 min, catch_pictures 8 min, cartpole 19 min);
on a quieter Pi expect about 15 minutes.

When a change to the brain makes learning faster or slower, re-measure with `-s` over the three seeds and update
both tables. Keep some margin: a threshold that passes only just will fail now and then.

## Real time: rt-catch (`test_realtime.py`)

`tests/e2e/worlds/rt-catch` has no `world.py`, so Vyrn plays it like a real program (PLAN.md section 3,
"RealtimeRunner"). Its `game.py`, imported by the three scripts, runs a Catch game by itself in a Python thread, 20
steps a second. `see()` reads three numbers from it; `left()` and `right()` set its input for the next step
(`wait = None`); `points()` hands out +1 for a catch and -1 for a miss; `game_over()` is True while the game waits
for `restart()`. No keys, no screen, no window. `vyrn.json`: 20 decisions per second, `"explore_ticks": 10000`.

For each seed (1, 2, 3; it stops once 2 pass) it runs

    vyrn-cli train <copy> --minutes 20 --seed S --json

and presses Ctrl+C (the brain is saved) as soon as the mean of the last 100 rounds reaches 0.7. Then it watches a
passing brain for 600 ticks (`vyrn-cli watch <copy> --ticks 600 --json`, 30 s): the mean points per round must
reach 0.8. Per run it prints the tick lateness (how late each tick really starts after its deadline: the median and
the worst of the 99th percentiles of the 2-second `timing` windows) and the overruns (ticks skipped because the one
before ran late).

ctest: `e2e_rt_catch`, label `slow`, run alone (real time measures the machine too):

    ctest --test-dir build -R e2e_rt_catch --output-on-failure

Measured on 2026-11-14 with `build-rt/` (RelWithDebInfo) on an otherwise quiet Pi. The table's seeds 1 and 2 are
the second run of the test; seed 3 ran separately (`--minutes 9`, alongside it: each run uses about a quarter of
one core).

| Seed | last-100 >= 0.7 after | Ticks then | Rounds then | Tick lateness p99, median / worst window | Overruns |
|---|---|---|---|---|---|
| 1 | 4.9 min (first run: 4.7) | 5789 | 644 | 0.1 ms / 5.6 ms | 0 |
| 2 | 4.8 min (first run: 5.2) | 5747 | 639 | 0.1 ms / 4.6 ms | 0 |
| 3 | 3.9 min | 4668 | 519 | 0.09 ms / 6.7 ms | 0 |
| watch (seed 2's brain) | mean 1.00 over 66 rounds (both runs) | 600 | 66 | 0.1 ms / 3.7 ms | 0 |

- A round is 9 ticks (0.45 s) plus one tick for `restart()` and `check_over`; the AI decides 20.0 times a second,
  the learner does 1 update per tick once 1000 ticks have warmed it up (20 updates/s; MLP, batch 128), and a tick
  costs about 0.6 ms of talking to Python and 0.1 ms of brain.
- Seed 3 kept learning after the threshold: last-100 0.76 at 5 min, 0.86 at 7 min, 1.00 at 9 min.
- Margins: the slowest seed needed 5.2 of the 20 minutes; Watch scored 1.00 against 0.8. Exploration falls to 5% at
  10k ticks (8.3 min), so the threshold is reached while the AI still explores 45-56% of the time.
- No late cuts and no overruns happened. On a Pi that is busy with builds the lateness grows; learning is counted in
  ticks, but a tick more than 3 periods late cuts the segment (`hold` events with reason `late`).


## Vyrn Link: link-runner (`test_link_e2e.py`)

`templates/link-runner` plays `tools/link/link_fake_game.py`, a tiny runner game that connects over Vyrn Link
(docs/dev/link.md): +1 per tick alive, -10 for a spike, +100 for the end of the level (about 600 points for a whole
level). For each mode it copies the template, puts its Link server on a free high port and sets
`"advanced": {"explore_ticks": ...}`, then runs

    vyrn-cli train <copy> --ticks N --seed 1 --json

and starts the fake game with the address and pairing code from the run's `link` event (`--reconnect`, like a mod).
The real-time test first connects the fake game to the LAN address without a code and with a wrong one: both must
be refused (`bad_code`). The run must end without an `error` event; the mean points of the last 20 rounds must beat
the first 20 rounds' by the mode's margin; then `vyrn-cli watch <copy> --brain latest --ticks 1500 --json` (the fake
game connected again) must reach the mode's mean points per round.

ctest: `e2e_link`, label `e2e`, run alone (real time measures the machine too):

    ctest --test-dir build -R e2e_link --output-on-failure

Measured on 2026-11-14 with `build-link/` (RelWithDebInfo) on an otherwise quiet Pi: one run by hand (the same
settings) and one `pytest -s` run of the test, which took 9.2 minutes in all. "First 20" plays at 100% down to about 60%
exploration, "last 20" at 5%; Watch plays with 1% (seed 1, the fake game's seed 1).

| Mode | Ticks (time) | Speed | First 20 → last 20 rounds | Whole levels | Watch, 1500 ticks |
|---|---|---|---|---|---|
| real time, 30 decisions/s | 12000 (6.8 min) | 30 ticks/s | 106 → 317 (by hand), 88.5 → 158.7 (test) | 4, 1 | 467 over 3 rounds (2 whole levels) |
| lockstep, 2 frames per step | 30000 (1.4 min) | 343-352 ticks/s | 91 → ~380 (by hand), 73.3 → 343.5 (test) | 24, 16 | 552 over 3 rounds (2 whole levels) |

- Random play scores about 70-110 per round (the runner dies at the first or second spike); a whole level is about
  600 (500 ticks + 100).
- Thresholds: real time +30 over the first 20 rounds and 120 while watched; lockstep +120 and 150. DQN's runs differ
  a lot (real time +70 and +210 here), so the margins are wide; Watch, without exploration, is the steadier measure.
- In lockstep the game waits for Vyrn and Vyrn waits for the learner (one update per tick, like a simulation), so the
  learner sets the pace: about 350 ticks a second here, 12 times real time.
- The pairing code check connects the fake game to this computer's LAN address (as another computer would) without a
  code and with a wrong one: both are refused with `bad_code`. Without a network address that check is skipped.


## Stopping: Ctrl+C and SIGTERM (`test_cli_signals.py`)

`vyrn-cli train` on a copy of `templates/catch-numbers` (about 170 rounds a second, so it prints all the time), and
one SIGINT (10 runs) or SIGTERM (3 runs) right after reading a `round` line, each after a different round. Every run
must stop gracefully: exit 0, "Stopping: saving the brain" on stderr and a `latest` checkpoint after the signal.
A signal that comes while vyrn-cli prints can land on a thread that a shared library started before main() (it
doesn't block the stop signals); without a handler that killed vyrn-cli (exit -2). About half a minute in all.

ctest: `e2e_cli_signals`, label `e2e`:

    ctest --test-dir build -R e2e_cli_signals --output-on-failure

`test_mcp.py` stops the MCP server with SIGTERM and with SIGINT during a training, as `vyrn-mcp` and as
`vyrn-cli mcp` (exit 128+n, brains saved, locks gone).
