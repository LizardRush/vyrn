# Brain speed on the Raspberry Pi 5 (`vyrn-cli bench`, M2)

PLAN.md §3 "Speed": oneDNN defaults off, and `vyrn-cli bench` confirms that during M2; the result is hardcoded.
This file records the measurement and the choice.

## What the bench measures

`vyrn-cli bench` (`core/src/brain/bench.cpp`) builds a real `Dqn` for each network and reports the median
milliseconds per call of

- **act**: one observation through the online network (what the brain thread / real-time actor does every tick);
- **update**: one full Double-DQN update at the default batch size (3 forward passes, backward, grad-norm clip, Adam),

for the three networks Vyrn uses, with oneDNN off and on, at 1, 2 and 3 torch threads:

| Network | Input | Batch |
|---|---|---|
| MLP 128-128 | 4 numbers | 128 |
| small CNN (16@3x3 s1 → 32@3x3 s2 → FC 256) | 4 stacked RGB frames of 10x10 (12x10x10) | 32 |
| DER CNN (32@5x5 s5 → 64@5x5 s5 → FC 256) | 4 stacked RGB frames of 84x84 (12x84x84) | 32 |

## Setup

- Raspberry Pi 5, 4x Cortex-A76 at 2.4 GHz (no SVE), 16 GB, Debian 13 (trixie), kernel 6.12, `ondemand` governor,
  48 °C, not throttled.
- Debian `libtorch2.6` 2.6.0+dfsg-7, `libopenblas0-openmp` 0.3.29, `libgomp1` 14.2, `libdnnl3.6` 3.7.2 (oneDNN,
  built without the Arm Compute Library).
- `build/` (RelWithDebInfo, `-O2`). `vyrn-cli` re-execs itself with `OMP_WAIT_POLICY=PASSIVE`; one inter-op
  thread.
- **The Pi was not idle.** Other programs kept two to three of the four cores busy during every run below (two
  Python game test harnesses at ~95 % each, plus parallel builds). The 1-thread rows are the reliable ones; the 2-
  and 3-thread rows are pessimistic (an idle Pi gave a DER update of ~32 ms at 4 threads in M0).

## Results (median ms per call)

Two runs of `build/cli/vyrn-cli bench` on 2026-11-13:

- **Run 1** (16:58): load average 4.1, three other processes at ~100 % CPU.
- **Run 2** (17:23): load average 7.8-8.2 (other test suites and builds on top). Its 2- and 3-thread rows are mostly
  scheduling noise: for example, the MLP update went from 2.7 ms on 2 threads to 26 ms on 3.

| Network | oneDNN | Threads | act, run 1 | act, run 2 | update, run 1 | update, run 2 |
|---|---|---|---|---|---|---|
| MLP (4 numbers, batch 128) | off | 1 | 0.03 | 0.04 | 2.1 | 2.8 |
| MLP (4 numbers, batch 128) | off | 2 | 0.04 | 0.04 | 2.3 | 2.7 |
| MLP (4 numbers, batch 128) | off | 3 | 0.04 | 0.04 | 2.4 | 26.3 |
| MLP (4 numbers, batch 128) | on | 1 | 0.04 | 0.04 | 2.0 | 2.6 |
| MLP (4 numbers, batch 128) | on | 2 | 0.04 | 0.04 | 2.2 | 8.5 |
| MLP (4 numbers, batch 128) | on | 3 | 0.04 | 0.04 | 2.4 | 23.9 |
| small CNN (12x10x10, batch 32) | off | 1 | 0.18 | 0.23 | 16.9 | 20.5 |
| small CNN (12x10x10, batch 32) | off | 2 | 0.20 | 0.28 | 18.1 | 25.5 |
| small CNN (12x10x10, batch 32) | off | 3 | 0.21 | 0.29 | 39.9 | 23.2 |
| small CNN (12x10x10, batch 32) | on | 1 | 0.18 | 0.23 | 32.4 | 76.5 |
| small CNN (12x10x10, batch 32) | on | 2 | 0.21 | 0.23 | 34.1 | 59.5 |
| small CNN (12x10x10, batch 32) | on | 3 | 0.22 | 0.24 | 52.5 | 80.8 |
| DER CNN (12x84x84, batch 32) | off | 1 | 0.75 | 0.92 | 110.6 | 121.6 |
| DER CNN (12x84x84, batch 32) | off | 2 | 0.84 | 1.42 | 114.8 | 311.6 |
| DER CNN (12x84x84, batch 32) | off | 3 | 3.99 | 1.29 | 127.7 | 126.3 |
| DER CNN (12x84x84, batch 32) | on | 1 | 1.99 | 2.48 | 248.0 | 275.2 |
| DER CNN (12x84x84, batch 32) | on | 2 | 2.08 | 1.79 | 251.1 | 363.2 |
| DER CNN (12x84x84, batch 32) | on | 3 | 2.09 | 1.98 | 228.5 | 341.9 |

What `vyrn-cli bench` itself concluded (it compares the DER net, act on 1 thread plus update on 2 or 3 threads):

| | Real time (act 1 thread + update 2 threads) | Simulation (3 threads) | Recommendation printed |
|---|---|---|---|
| Run 1 | off 115.5 ms, on 253.1 ms | off 128.5 ms, on 230.5 ms | keep oneDNN off: 2.2x faster without it |
| Run 2 | off 312.6 ms, on 365.7 ms | off 127.2 ms, on 344.4 ms | keep oneDNN off: 1.2x faster without it |

## Choice: oneDNN stays off

**oneDNN off is the hardcoded default** (`kOneDnnDefault = false` in `core/src/brain/threads.h`, applied once per
process by `init_torch_process()`). The environment can still override it: `VYRN_ONEDNN=1` turns it on,
`VYRN_ONEDNN=0` off.

Why: with oneDNN on, every network that matters is slower, at every thread count:

| Network, 1 thread | update, oneDNN off | update, oneDNN on | act, off | act, on |
|---|---|---|---|---|
| DER CNN (84x84) | 110.6 / 121.6 ms | 248.0 / 275.2 ms (2.2-2.3x slower) | 0.75 / 0.92 ms | 1.99 / 2.48 ms (2.7x slower) |
| small CNN (10x10) | 16.9 / 20.5 ms | 32.4 / 76.5 ms (1.9-3.7x slower) | 0.18 / 0.23 ms | 0.18 / 0.23 ms |
| MLP | 2.1 / 2.8 ms | 2.0 / 2.6 ms (the same) | 0.03 / 0.04 ms | 0.04 / 0.04 ms |

(run 1 / run 2)

That matches the plan's expectation: this A76 has no SVE and Debian's oneDNN has no Arm Compute Library, so its
convolutions are slower here than libtorch's own im2col + OpenBLAS path. The MLP costs about the same either way.
The gap for the two picture networks is large and points the same way in both runs, so the busy machine doesn't
change the decision.

## Other observations

- **Threads.** On this busy Pi, extra torch threads did not help any network: every parallel region waits for its
  slowest thread, and with the cores taken that thread is often not running. The small-CNN update took 17 ms on 1
  thread and 40 ms on 3 (run 1); under heavier load the MLP update went from 2.8 ms on 1 thread to 26 ms on 3
  (run 2). That is mostly the machine: M0 measured a DER update of ~32 ms on an idle Pi with 4 threads, against
  ~110 ms on 1 thread here. The simulation brain thread therefore keeps PLAN.md's 3 threads for now. To settle in
  M5 on an idle Pi: the MLP's tensors are too small to split (more threads made its update at most 4 % faster and
  up to 9x slower), so it should probably train on 1 thread everywhere; the small CNN should use 1 thread unless 3 threads
  clearly beat it on an idle Pi.
- **Training speed of the e2e worlds** (simulations, 0.25 updates per tick, this busy Pi): catch-numbers and cartpole
  (MLP, ~2 ms per update) train at 1000-1200 ticks/s; catch-pictures (small CNN, 17-40 ms per update) at 80-230
  ticks/s. Before learning starts (the first 1000-2000 ticks), all three run at 1200-1700 ticks/s, which is the
  cost of Python and the protocol alone. See tests/e2e/README.md.
- **Real time (M4).** The actor acts on 1 thread: 0.75 ms for the DER net, plenty for 10 decisions/s. One learner
  update per tick at 10 decisions/s needs a DER update under 100 ms on the learner's 2 threads; this busy Pi took
  ~115 ms, so a busy Pi will do a little less than one update per tick (the learner is asynchronous: ticks never
  wait for it).
- `vyrn-cli bench` takes about 15 s here (each case is timed for 0.4 s after one warm-up call).
