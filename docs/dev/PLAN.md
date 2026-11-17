# Vyrn: Reinforcement-Learning Launcher (C++/Qt6 app + `vyrn` Python library)

## Context

`/home/lizard/vyrn-nn` is empty: this is a new project. You asked for a native desktop **RL launcher** with a built-in IDE. Inside it you write three tiny Python scripts using a `vyrn` library that ships with the app:

- `inputs.py`: what the neural network **sees** (a screen image or a list of numbers)
- `responses.py`: what each of the network's choices **does** (e.g. `left = "a"` holds the A key)
- `points.py`: how the AI is **scored** (a number each tick)

Your words: "Python is the microcontroller, the app is the chip." So:

- **The app (the chip)** builds, trains, saves and runs the neural network: a CNN for pictures, an MLP for numbers. It also owns all the plumbing between Python and the brain: processes, IPC, the tick clock and safety.
- **Your Python (the microcontroller)** only touches the world. It reads the screen, presses keys, or runs a small Python-simulated game.
- User Python never contains neural-network, training or socket code.
- I read "the app handles the networking" as **the neural network plus that plumbing**. Nothing goes over the internet. Tell me if you meant something else.

Your decisions: C++ / Qt6; inputs can be pictures as well as numbers; three scripts per project, each shown as an IDE tab.

**Target machine:** Raspberry Pi 5 (4×A76, 16 GB, aarch64), Debian 13 trixie, labwc 0.9.2 Wayland, viewed over Raspberry Pi Connect (wayvnc).
- Already installed: g++ 14, ninja, Python 3.13 with numpy and Pillow, grim, slurp.
- From apt: Qt 6.8, QScintilla 2.14, libtorch 2.6 (Debian build, CMake config at `/usr/lib/aarch64-linux-gnu/cmake/Torch`) and cmake.
- There is no passwordless sudo, so you run the install script once yourself.

This plan came from a read-only design panel (3 researchers, 3 independent designs, 1 synthesis, 2 adversarial reviews). Their fixes are already folded in.

---

## 1. The user-facing contract (what a beginner writes)

**Project** = a folder `~/Vyrn/<name>/` containing `vyrn.json`, `inputs.py`, `responses.py`, `points.py`, an optional `world.py`, and the folders `pictures/` (snapshots), `brains/` and `runs/`.

**Mode rule: is there a `world.py`?**
- **Yes → simulation.** Runs as fast as possible while training. "Watch" shows it at human speed.
- **No → real-time.** Plays a real program at *decisions per second* (default 10).
- There is no other mode switch.

**Hook names.** Vyrn finds these in whichever script defines them; defining one twice is an error.

| Hook | Where (template) | Required | Meaning |
|---|---|---|---|
| `see()` | inputs.py | yes | Returns a **picture** (uint8 H×W, H×W×3 or H×W×4 array, PIL image, or `vyrn.canvas`) or **numbers** (a number, list, 1-D array, or dict name→number) |
| `points()` | points.py | yes | Returns a number. `None` counts as 0 (warned once); a bool counts as 1/0; NaN or inf is an error |
| `game_over()` | points.py | no | A truthy value ends the round; anything falsy, `None` included, means "not over". Without it, rounds end at the time limit |
| `restart()` | points.py / world.py | no | Real-time: restart the real game (e.g. `vyrn.press("space")`). Sim: set up a new round |
| `step()` | world.py | no | Sim: advance the world one tick after the AI's choice |

- **The `world` object:** with a `world.py`, Vyrn creates one blank `world` object and passes it to every hook and choice function that takes a parameter. No classes are needed.
- **Each tick:** `see()` → `points()` (scores the previous choice; skipped on a round's first tick) → `game_over()` → the AI picks → the choice's effect → `step()` (sim only).

**Choices (responses.py)**
- Every top-level name that doesn't start with `_` is a choice, **in file order**, if its value is a string, `None`, or a function defined in that file. Hook names don't count.
- An optional `RESPONSES = {...}` dict overrides this, for advanced use.
- The effect grammar:

| Effect | Meaning |
|---|---|
| `"a"`, `"space"`, `"left"`, `"shift+d"`, `"a+space"` | Held while the AI keeps choosing it. Released when it switches, when Vyrn holds, and at round end. The library diffs the held set each tick |
| `"tap space"`, `"tap ctrl+c"` | Pressed and released once on each tick it is chosen |
| `None` | Does nothing |
| a function | Called (it receives `world` in a sim) |

- **Allowed keys:** letters, digits, punctuation, arrows, space, enter, tab, esc, backspace, left shift/ctrl, F1–F12.
- **Refused when the scripts load**, with a did-you-mean suggestion: Alt, Super, lock keys, power and media keys, and unknown names.
- **No mouse in v1.** That keeps your own mouse free to click Vyrn and stop the AI.

**The whole public `vyrn` module**
```python
vyrn.screen(x, y, width, height) # picture of that area; no args = whole screen ("Pick area" fills them in)
vyrn.pixel(x, y)                 # (r, g, b)
vyrn.on_screen("name.png", x, y) # True if that Snapshot is showing there; missing file → False + Check warning
vyrn.press("space")              # tap a key right now (for restart())
vyrn.wait(seconds)
vyrn.canvas(width, height)       # blank picture: .dot(x, y, color), .box(x, y, w, h, color)
vyrn.show(name, value)           # number, text or picture on the dashboard
vyrn.choice, vyrn.tick, vyrn.round   # last choice, tick in this round, round number
```
- **Screen capture:** one `grim` capture per tick, covering the union of every area used. It is cropped per call, and `pixel`/`on_screen` read from it.
- **In a simulation**, screen and key calls raise a plain-English error.

### Templates (shipped; New Project copies one)

**(a) `demo-game`.** Zero edits needed. *Start program* launches the bundled fullscreen Tk "Dodge" game.
```python
# inputs.py
import vyrn

def see():
    return vyrn.screen()          # the whole screen
```
```python
# responses.py — a key is held while the AI keeps choosing it
left = "a"
right = "d"
wait = None
```
```python
# points.py
import vyrn

def points():
    if game_over():
        return -10                # got hit
    return 1                      # still alive

def game_over():
    r, g, b = vyrn.pixel(20, 20)  # the demo game turns red when you're hit
    return r > 200 and g < 80

def restart():
    vyrn.press("space")
```

**(b) `my-game`.** Same shape for your own game:
- `see()` returns `vyrn.screen(x=0, y=0, width=640, height=480)`, with the comment `# "Pick area" fills these in`.
- `game_over()` returns `vyrn.on_screen("game_over.png", x=0, y=0)`, with the comment `# take a Snapshot of your game-over screen`.
- Choices: `left` / `right` / `jump = "tap space"` / `wait = None`.

**(c) `catch-pictures`** (sim)
```python
# world.py — a tiny game Vyrn can run very fast
import random

def restart(world):
    world.basket = 5
    world.fruit_x = random.randint(0, 9)
    world.fruit_y = 0

def step(world):
    world.fruit_y += 1
```
```python
# inputs.py
import vyrn

def see(world):
    picture = vyrn.canvas(10, 10)
    picture.box(world.basket - 1, 9, 3, 1, "gray")
    picture.dot(world.fruit_x, world.fruit_y, "white")
    return picture
```
```python
# responses.py
def left(world):
    world.basket = max(1, world.basket - 1)

def right(world):
    world.basket = min(8, world.basket + 1)

stay = None
```
```python
# points.py
def points(world):
    if world.fruit_y < 9:
        return 0          # still falling
    if abs(world.fruit_x - world.basket) <= 1:
        return 1          # caught it
    return -1             # missed

def game_over(world):
    return world.fruit_y == 9
```

**(d) `catch-numbers`.** Same as (c); only `see` changes:
```python
def see(world):
    return {"fruit x": world.fruit_x, "fruit y": world.fruit_y, "basket": world.basket}
```

The docs also include a GPIO example: `gpiozero` in `see()` and in a choice function. A project that sends no keys gets no focus hold or countdown.

---

## 2. Architecture

```
vyrn (Qt GUI) / vyrn-cli ── link ── vyrn_core (Qt-free, libtorch, std-only pimpl API)
  UI thread      Qt event loop; 10 Hz QTimer drains Session events + live view; never touches torch/IPC
  brain thread   SIM: step → act → replay → inline DQN updates (3 torch threads)
  actor thread   RT: absolute-deadline ticks, forward on policy snapshot (1 torch thread)
  learner thread RT: async DQN updates (2 torch threads, nice +10), publishes snapshot every 50 updates
  io thread      drains worker stdout/stderr at full speed → console (display throttled, "N lines dropped")
   └─ worker: python3 -u <share>/vyrn/python/vyrn_worker.py <project>   (own process group)
        fd3 = AF_UNIX socketpair (first statement: os.set_inheritable(3, False)); fd1/fd2 = pipes
        main thread: request → user code → exactly one reply
        watchdog:    POLLRDHUP on fd3 (app died) + RT dead-man timer → release keys → os._exit
        SIGUSR1:     out-of-band freeze, handled on the watchdog thread (wakeup fd), so it works
                     even while user code is stuck: release all keys, block input until "resume"
```

- **Subprocess, not embedded CPython.** An infinite loop or crash in user code can be killed, Stop always works, and the interpreter is fresh each run.
- **Interpreter:** `/usr/bin/python3` (the apt numpy, Pillow and evdev are enough); an optional per-project Python path.
- **Spawning:** `posix_spawn` with dup2 onto fd 3, `addclosefrom_np(4)` and `SETPGROUP`. The worker gets `OMP_NUM_THREADS=1`.
- **Panic support:** the pid plus its start time (from `/proc/pid/stat`) is written to `$XDG_RUNTIME_DIR/vyrn/` so `vyrn-panic` can verify a process before killing it. Stale files are swept at startup.
- **Torch threads:**
  - `main()` re-execs itself once with `OMP_WAIT_POLICY=PASSIVE`.
  - `at::set_num_interop_threads(1)`.
  - Each brain thread calls the exported `at::get_num_threads()` first and *then* `at::set_num_threads(k)`. The inline lazy-init otherwise resets it; a unit test checks this.
- **Qt vs torch macros:** no translation unit includes both. A lint ctest enforces it. `vyrn_core` compiles as C++17 (libtorch's tested standard); the app and CLI compile as C++20.

**Protocol** (little-endian): `"VYRN" | u32 json_len | u32 bin_len | JSON | bin`.
- Arrays are written as `{"$a": [offset, "u8"|"f32", shape]}`, 64-byte aligned, C order. Pictures travel raw as uint8.
- Limits: JSON ≤ 1 MiB, bin ≤ 64 MiB. One request is outstanding per worker. `proto: 1`; unknown keys are ignored.

| t | Dir | Purpose / reply |
|---|---|---|
| hello | W→A | proto, python version, pid, backends (sent first, unprompted) |
| load | A→W | project, mode, `allow_input`, check, seed, round_limit, focus_target → **spec** |
| spec | W→A | obs {kind, shape, names}, choices[] (ordered), effects described in plain English, uses {screen, keys}, warnings[] |
| reset / step | A→W (sim) | → **result** (step auto-restarts at round end; includes `final_obs` on truncation) |
| observe / act | A→W (RT) | see+points+game_over → **result**; act(a) → ok{ms} |
| status | A→W (RT) | focus and held keys, no user code; polled at 4–10 Hz while held |
| pause / resume / close | A→W | ok after keys are released / bye |
| result | W→A | obs, points, game_over, truncated, final_obs?, focused, show{}, timings |
| error | W→A | script, line, col, func, kind, message, hint, trace[], fatal; sent instead of a reply |

**Error handling**
- An error points at the deepest stack frame inside the project. A bad return points at the function's `def` line.
- The run pauses with keys already released, and the IDE marks the line. Resume respawns the worker and keeps the brain.

**Timeouts**
- hello 15 s, load/reset 30 s, close 2 s.
- step 5 s (sim); observe/act max(1 s, 5 ticks) (RT).
- On timeout: SIGINT to the **worker pid only** (it raises inside user code and reports the stuck line), then SIGKILL of the whole group 2 s later (sim) or 0.5 s later (RT).
- EOF or a bad frame → SIGKILL the group and show "Python stopped" with the last 20 stderr lines.

**Check mode** (`load{check:true}`)
- Effects are *described* ("would hold A"), never executed.
- Real-time Check runs `restart()` once, then 5 observe ticks with no input. Choice functions are described, never
  run, and the spec warns about this. A literal `vyrn.press("…")` key is checked at load (fatal `load` error with a
  did-you-mean). A sim runs 20 random steps.
- It shows previews and timings, and warns about:
  - no do-nothing choice;
  - `game_over()` never true (in real time this is a note, not a warning: a real game rarely ends in 5 ticks);
  - a picture that is almost black;
  - `see()` taking over 80% of a tick;
  - a missing snapshot file;
  - an input that never changes (it would be ignored).

---

## 3. The brain (DQN only in v1; PPO is v1.1 if sims prove slow)

**Spec.** Saved in `brain.json`:
- observation kind and canonical shape;
- the **ordered** choice and input names;
- the network type.

A reorder, rename or removal of choices refuses to resume, and Check warns "this change starts a new brain". **Appending** a choice grows the output layer and keeps the brain (from M5; before that, it also starts a new brain).

**Preprocessing** (C++)

| Input | Steps |
|---|---|
| Pictures | Drop alpha and **keep RGB** (grayscale would merge red and green into ~75/76 gray). If the max side is ≤ 32, keep the native size: the brain remembers it, and later pictures of another size are nearest-resized to it, with a Check warning. Otherwise area-resize to 84×84. Stack 4 frames into 12 channels as uint8, repeating the first frame of each segment. Divide by 255 inside the net |
| Numbers | float64 running mean/std; input = clip((x−μ)/√(σ²+1e-8), ±10); frozen in Watch |

**Networks.** All three end in a Q-head over the choices.

| Network | Use | Layers |
|---|---|---|
| MLP | numbers | 128-128 ReLU |
| Small CNN | pictures ≤ 32 px, native size | 16@3×3 s1 p1 → 32@3×3 s2 p1 → FC 256 (~0.5M multiply-adds for 10×10; the padding means even 1×N pictures work) |
| DER CNN | 84×84 pictures | 32@5×5 s5 → 64@5×5 s5 → FC 256 (~3M multiply-adds per frame with RGB, vs 9.4M for the Nature CNN) |

**DQN.** Double DQN, 3-step returns, Huber loss, Adam, grad-norm 10, γ 0.99, hard target copy.
- **Rewards:** multiplied by 1/max(1, the largest |points| seen during warm-up). That scale is then frozen and saved, and rewards are clipped to ±10.
- **Replay:** a uint8 ring with one frame per slot. Stacks are rebuilt by index and never cross a segment start. Each round's final frame is kept for bootstrapping. A truncated end bootstraps; a game over does not.

| | Numbers | Pictures |
|---|---|---|
| buffer | 100k | 50k (RGB 84² ≈ 1 GB) |
| batch / lr | 128 / 2.5e-4 | 32 / 1e-4 |
| updates per tick | sim 0.25 · RT ≤ 1 (async) | sim 0.25 · RT ≤ 1 |
| learning starts | 1k | 2k sim / 1k RT |
| target copy | every 500 updates | every 1000 updates |
| ε | 1 → 0.05 over 20k ticks (`explore_ticks`); 0.01 in Watch | same |

These are starting points. The **e2e thresholds are set from measured runs** over 3 seeds in M2, then frozen.

**SimRunner** (one worker, one brain thread)
- Training is unpaced.
- **Watch** uses one copy at decisions/s × a speed slider (0.5×–Max) with the Best brain, and draws the picture large.

**RealtimeRunner.** A state machine, unit-tested against a scripted fake worker. Its states:
1. **Countdown and lock:** a 3 s countdown ("Hands off — click your game window"), then it locks onto the active window (app_id + title).
2. **Restart:** calls `restart()`, then polls `game_over()` each tick with no choices and nothing recorded. It re-runs `restart()` every 3 s, and after 30 s pauses with "restart() didn't clear game_over()".
3. **Round:** ticks on absolute deadlines. Missed ticks are skipped, never bursted; a tick more than 3 periods late is a segment cut.
4. **Hold:** whenever the locked window isn't active (clicking Vyrn counts). Keys are released and the open transition is marked never-sampled. `prev` is cleared and one `segment_start` flag goes to both the frame stack and the replay. While held, focus is polled with `status`.
5. **Round limit** (default 60 s): ends a truncated segment without calling `restart()`.

**Checkpoints**
- Layout: `brains/<id>/{latest,best}/`, each holding `model.pt` (online + target), `optim.pt` and `brain.json` (spec, hyperparameters, counters, normalizer stats, reward scale, best score, version).
- **Atomic writes:** each save goes into a fresh `save-<n>/` folder, is fsynced, and then the small `current` pointer file is replaced. Renaming onto a non-empty folder fails, so the folder itself is never renamed over an old one.
- **When:**
  - Saves run on the thread that owns the model, between updates.
  - Every 10 min, on Stop, and when the window closes.
  - Best weights are kept in RAM and written at most once a minute.
- The replay buffer is not saved; DQN re-warms it on resume.
- **Start over** creates a new `<id>`; old ones stay in the Brain dropdown.

**Telemetry**
- `Session::drain()` returns bounded JSON events: state, log, error, spec, round, update, timing, checkpoint.
- `Session::live()` gives, at ≤ 10 fps: the network-input image or the normalized numbers, choice probabilities, the current pick, and `show` values.
- CSV logs go to `runs/<ts>/{rounds,updates}.csv`.

**Speed**
- oneDNN defaults **off**: this A76 has no SVE, and Debian's oneDNN has no Arm Compute Library.
- `libopenblas0-openmp` replaces reference BLAS.
- `vyrn-cli bench` (a real Double-DQN update at the real-time thread counts) confirms the default during M2, and the result is hardcoded.

---

## 4. The Qt app

- **LauncherWindow:** project cards from `~/Vyrn/*` (name, mode, time trained, best score; rename / duplicate / delete), and **New project** (name + template).
- **ProjectWindow** (the IDE and the dashboard)
  - **Toolbar:** Check · Train · Watch · Stop · Pick area · Snapshot · Brain ▾ (Best/Latest/older, Start over) · Settings.
  - **Train** does: autosave → automatic Check (errors stop it; warnings don't) → the countdown if keys are used.
  - **Editor tabs** with captions: "inputs.py — what the AI sees", "responses.py — what its choices do", "points.py — how it scores", "world.py".
  - **QScintilla editor** with the Python lexer:
    - `vyrn.` completion and call tips from a generated `vyrn.api` (QsciAPIs).
    - Errors shown as a margin marker, a squiggle and an inline annotation. Tracebacks are 1-based; QScintilla lines are 0-based.
    - Ctrl+S saves. Saving during a run offers **Apply changes**: respawn the worker and keep the brain.
  - **Right dock:**
    - **What the AI sees:** the exact network input, scaled up, or labelled number bars.
    - **Choice bars:** probabilities and the current pick.
    - `vyrn.show` panels.
  - **Bottom tabs:**
    - Console: clicking `points.py:7` jumps to that line.
    - Points chart: a custom QPainter plot of points per round plus a 20-round mean (no Qt Charts: it's deprecated and GPL-only).
    - Problems.
    - Docs: F1 on a `vyrn.` name opens its entry.
  - **Status bar** in plain words: "Round 312 · Best 14.2 · Exploring 35% · 10 decisions/s". Timings and update rates go in a tooltip.
  - **Red banner** inside the window while keys are live: "AI is pressing keys — click here or press Ctrl+Alt+Esc to stop".
- **SettingsDialog:** decisions per second, round length, game window, **Start program**, and an advanced Python path. Everything else lives in `vyrn.json`.
  - Start program runs a command when Check, Train or Watch begins, if it isn't already running (e.g. the demo game or `chromium chrome://dino`). Vyrn terminates it on Stop if Vyrn started it.
- **Pick area / Snapshot:** `slurp` already draws above every window, so Vyrn doesn't minimize.
  - **Pick area** rewrites the `vyrn.screen(...)` call (undoable).
  - **Snapshot** grabs the region with `grim`, saves `pictures/<name>.png`, and inserts `vyrn.on_screen("<name>.png", x=…, y=…)` at the cursor.
- **SetupDialog:** the checks depend on the project.
  - Sims need only python3, numpy and Pillow.
  - Real-game checks appear the first time a real-game project trains: grim test grab with a thumbnail, `/dev/uinput` writable plus evdev, the focus helper sees windows, and the panic keybinds are present.
  - Each failing row has a copyable fix (usually `./install.sh --keyboard`).

**Input safety** (only for projects whose effects send keys)
1. **Gated at runtime:** the worker opens `/dev/uinput` only when `load.allow_input` is true. The app sets it only after Check, the countdown and the focus lock. The CLI needs `--allow-input`, and sims never create devices.
2. **Allow-list device:** the virtual keyboard declares only the allowed keys: no Alt, Super, locks, power or sleep.
3. **Focus guard:** `vyrn-focus` is a ~200-line C helper. It uses libwayland-client and the vendored MIT `wlr-foreign-toplevel-management` XML, which labwc supports, and prints a line on every focus change. Input is sent only while the locked window is active, re-checked right before every key event and inside `vyrn.press()`.
4. **Stop layers:**
   - Clicking Vyrn (always holds).
   - The Stop button.
   - The labwc keybinds **`C-A-Escape` and `S-C-A-Escape`** → `vyrn-panic`. The second bind is needed because the AI's held Shift merges into a physical keyboard's modifiers.
   - SIGUSR1 freeze.
   - Real-time call timeouts.
   - The watchdog and dead-man timer.
   - Kernel key-ups when the uinput fd closes (covers SIGKILL).

---

## 5. Repo layout (key files)

```
install.sh                      idempotent; asks for sudo once. Flags: --deps, --keyboard, --app (default: all)
CMakeLists.txt                  targets: vyrn_ipc, vyrn_core, vyrn-cli, vyrn, vyrn-focus, hello, tests, install
cmake/FindQScintilla.cmake      /usr/include/aarch64-linux-gnu/qt6/Qsci + libqscintilla2_qt6 (no cmake/pc files)
core/include/vyrn/{session,project}.h   std-only pimpl API: Session{start,pause,resume,stop,set_hold,drain,live}
core/src/ipc/{proto,worker}.*   frame codec, posix_spawn, timeouts, SIGINT→SIGKILL, io thread, pid files
core/src/brain/{spec,preprocess,nets,replay,dqn,checkpoint}.*
core/src/run/{sim_runner,rt_runner,session,bench}.cpp
cli/main.cpp                    vyrn-cli check|train|watch|bench, JSONL output, exit 0 / 2 usage / 3 script error / 4 crash
app/main.cpp + {LauncherWindow,ProjectWindow,Editor,SessionBridge,PlotWidget,SeesView,ChoiceBars,
                SetupDialog,PickTool,SettingsDialog}.{h,cpp}
app/resources/                  icon, dark palette/qss, generated vyrn.api + docs
focus/vyrn-focus.c + focus/protocols/wlr-foreign-toplevel-management-unstable-v1.xml
python/vyrn_worker.py           entry: fix sys.path (library first, then project), run _host
python/vyrn/__init__.py         the public API (the only thing users import)
python/vyrn/_host.py            request loop, hook/choice discovery, world injection, validation, errors, watchdog
python/vyrn/{_proto,_screen,_keys,_effects,_canvas,_focus}.py
python/tools/{check.py,gen_api.py}     standalone Check; docstrings → vyrn.api + docs HTML (CMake step)
templates/{demo-game,my-game,catch-pictures,catch-numbers}/   demo-game includes demo_game.py (Tk Dodge)
docs/                           "First AI in 5 minutes", "The three files", "Teaching it a real game",
                                "Why isn't it learning / how long does it take", GPIO example
scripts/vyrn-panic              POSIX sh: write marker, verify pid+start time, SIGKILL worker groups
packaging/{vyrn.desktop,vyrn.png,60-vyrn-uinput.rules}
tests/cpp (GTest) · tests/python (pytest, fake engine, recording key backend)
tests/e2e/worlds/{cartpole,catch-pictures,catch-numbers,rt-catch}/ + tests/e2e/test_learning.py
```

## 6. One-time setup (`./install.sh` does this; you run it because it needs sudo)

```bash
sudo apt install cmake qt6-base-dev qt6-qpa-plugins qt6-wayland libqscintilla2-qt6-dev libtorch-dev \
  libopenblas0-openmp nlohmann-json3-dev libgtest-dev python3-evdev python3-pytest libwayland-dev libwayland-bin
```

- About 700 MB. libtorch pulls in MPI/ROCm libraries.
- **`--keyboard`** (real games only):
  - the udev rule `KERNEL=="uinput", SUBSYSTEM=="misc", OPTIONS+="static_node=uinput", GROUP="input", MODE="0660"`;
  - uinput autoload in `/etc/modules-load.d`;
  - `modprobe` and `udevadm trigger`;
  - installs `vyrn-panic` to `~/.local/bin`, so the panic key works before the full app install.
  - Note: after this, any program run by a member of the `input` group can inject keys.
- **Panic keybinds:** merged into `~/.config/labwc/rc.xml`.
  - The file is backed up first and edited XML-aware in the `openbox_config` namespace.
  - labwc runs with `-m`, so a small user file merges with the system one; then `labwc -r`.
- **`--app`:** builds (Ninja, `-j3`) and installs to `~/.local`, including the `.desktop` entry and the hicolor icon, so Vyrn appears in the Pi menu.

---

## 7. Milestones (vertical slices; something visible early)

| M | Deliverable | Acceptance (checkable by command) |
|---|---|---|
| **M0** | `install.sh`; you run `./install.sh --deps`; a `hello` target linking Torch + Qt6 + QScintilla (torch and Qt in separate .cpp files) | `build/hello` prints the torch/Qt versions and runs a tiny conv |
| **M1** | The Python side alone: library, worker host, protocol, effects, canvas, `check.py`; pytest with a fake engine | `python3 -m pytest tests/python -q` passes; `python3 python/tools/check.py templates/catch-pictures` prints the choices, the spec and an ASCII view of what the AI sees |
| **M2** | The chip: C++ IPC, preprocessing, nets, replay, DQN, checkpoints, SimRunner, Session, `vyrn-cli`, bench (oneDNN default chosen) | `ctest -L unit`; `build/vyrn-cli train templates/catch-numbers --minutes 3` prints rising round means (**first learning AI**); e2e catch-numbers, catch-pictures and cartpole pass at their measured thresholds |
| **M3** | The app for sims: launcher, IDE, Check, Train, Watch, chart, sees-view, choice bars, Brain ▾, Apply changes, Docs tab, desktop entry | `ctest -L gui` (offscreen smoke test); by hand: new Catch (pictures) → Train → chart rises → Watch catches |
| **M3b** | MCP server `vyrn-cli mcp` (docs/dev/mcp.md), shared `vyrn/project.h`, AGENTS.md in templates, "Connect an AI coding agent" dialog | `ctest -L e2e -R mcp` (legacy + modern flows, refusals, PNG); `claude mcp add` line works by hand |
| **M3c** | Sharing: `.vrl` (one trained brain) and `.vrlp` (project + its brain) files, docs/dev/package.md: strict ZIP subset + safetensors weights (no pickle), sanitized settings, untrusted imports (Session refuses until the person trusts), `vyrn-cli pack/export-brain/inspect/import/trust`, MIME + double-click, app Share/Import dialogs, MCP share/inspect/import | `ctest -R package` (round trips for all three nets, every refusal); double-clicking a `.vrlp` opens the import dialog; an imported sim project Watches after Trust |
| **M4** | Real games: grim, uinput keys, vyrn-focus, safety flow, freeze/dead-man, vyrn-panic + keybinds, RealtimeRunner + async learner, restart polling, Pick area/Snapshot, demo-game + my-game templates, Start program, `--keyboard` | `ctest -L slow -R rt_catch`; the manual Dodge test below |
| **M4b** | Vyrn Link WebSocket server in the core + `vyrn.game`/`vyrn.events`/`vyrn.send`/`"game …"` effects + geometry-dash and link-runner templates (the user writes the Geode mod; protocol frozen in docs/user/vyrn-link.md, test tools in tools/link/) | ctest link tests with tools/link/link_fake_game.py (real time + lockstep, pairing code, reconnect, stop); the user's mod connects |
| **M5** | Polish: hand-written docs, card actions, growing the output layer on an appended choice, perf tuning, full `./install.sh`, README | A fresh `./install.sh` → Vyrn in the menu, Setup all green, demo-game trains with zero edits |

## 8. Verification

**C++ unit tests** (GTest, `ctest -L unit`)

| Area | Checks |
|---|---|
| Codec | golden bytes shared with pytest; malformed and oversize frames rejected |
| Spawn | only fd 3 is inherited; a group kill reaches grandchildren; SIGINT→SIGKILL escalation |
| Preprocess + nets | shapes 1×1, 1×10, 4×4, 10×10, 32×32, 33×33, 640×480 and 1920×1080 all preprocess and run a forward/backward pass |
| Frame stack | AAAA→AAAB→AABC, then DDDD at a segment start |
| Replay | (C=8, K=3, n=2) no stack crosses a segment; overwritten or end slots never sampled; n-step correct for terminal and truncated endings |
| Double-DQN target | Q_on(s′)=[1,3], Q_tgt(s′)=[5,2], r=1, γ=.9 → 2.8 |
| Normalizer | matches numpy to 1e-6 |
| Checkpoints | round-trip, plus kill-during-save (the old checkpoint stays loadable) |
| Torch threads | two threads with k=1 and k=2 keep their own counts after conv and matmul |
| Snapshot stress | 30 s of actor forwards during learner publishes; every output finite |
| RealtimeRunner | scripted fake worker through hold → refocus → game over → restart polling → round limit |

**pytest** (`tests/python`)
- Spec inference for every return type.
- Errors carry the exact file:line for exceptions, SyntaxError, bad returns and an infinite loop.
- `print()` spam never corrupts frames.
- Effect grammar, did-you-mean and blocked keys.
- Hold/tap diffing against a recording key backend.
- Keys are released on pause, close, exception, SIGTERM, SIGUSR1 and parent death, all in under 1 s.
- `vyrn.api` matches the public API.

**End-to-end learning** (`vyrn-cli train <world> --json`)
- A test passes in ≥ 2 of 3 seeds, with explicit CTest timeouts.
- Expected targets, confirmed in M2: catch-numbers and catch-pictures last-100 mean ≥ 0.8 within minutes; cartpole best 20-round mean ≥ 400.
- rt-catch: a self-running Python game thread at 20 Hz with no desktop. After 20 min the last-100 mean is ≥ 0.7, p99 tick lateness < 5 ms, and overruns < 1% (measured with the remote viewer connected).

**GUI smoke test**
```bash
QT_QPA_PLATFORM=offscreen build/vyrn --smoke-test templates/catch-pictures --seconds 60 --screenshot build/smoke.png
```
- Checks: the tabs exist, ≥ 5 rounds are charted, an injected NameError is marked on the right line, and the PNG isn't blank.
- Also run on the real desktop and capture with `grim`.

**Manual real-game test** (demo-game template)
1. Open the template and press Train with zero edits.
2. After 30–60 min the median survival time should be ≥ 2× the random baseline.
3. `pkill -9 -f vyrn_worker`: no KeyPress events for ≥ 2 s afterwards (autorepeat has stopped), and the uinput device is gone from `/proc/bus/input/devices`.
4. Press Ctrl+Alt+Esc, including while the AI holds Shift: the worker is gone in under 0.5 s and Vyrn says "Panic stop".
5. Click Vyrn while it trains: it holds and releases all keys.
6. Alt-tab to a terminal: it holds, and nothing is typed into the terminal.

## 8b. AI coding agents (MCP), added after plan approval

`vyrn-cli mcp` is an MCP server on stdio. It is dual-era: the stateless MCP `2026-07-28` with `server/discover`, plus
the legacy `initialize` handshake for `2025-11-25` and older. With it, Claude Code, Codex, Gemini CLI and others can
create projects, write the three scripts, run Check, train and watch simulations, and read progress and what the AI
sees, as PNG.
- **Safety:** agents never start real-game training or press keys. Screen capture requires the user's opt-in per
  project. A lock file stops the app and an agent training the same project at once.
- **Setup:** one copyable line per agent, from the launcher's **Connect an AI agent** button.
- **Agent instructions:** templates ship `AGENTS.md` and `CLAUDE.md`.
- **Spec:** `docs/dev/mcp.md`.
- **Milestone M3b:** built in parallel with M3, after M2. The shared `vyrn/project.h` core API comes first.

## 8c. Vyrn Link: WebSocket games (added after plan approval)

The app hosts a WebSocket server (default port 7457, pairing code) so modded games can stream their state and receive
the AI's choices. Geometry Dash connects through a Geode mod running on the user's PC; the user writes the mod.
- **Python side:** `vyrn.game`, `vyrn.events`, `vyrn.send()` and `"game <button>"` effects.
- **Safety:** no desktop keys, so no focus guard.
- **Lockstep (optional):** the game waits for the AI each frame.
- **Spec:** `docs/dev/link.md`; the protocol for mod authors is `docs/user/vyrn-link.md`, frozen at v1.
- **Test tools:** `tools/link/`.
- **Milestone M4b:** scheduled with M4.

## 8d. Sharing trained AIs: `.vrl` and `.vrlp` (added after plan approval)

The person asked for trained models to be shareable: a **`.vrl`** holds one trained brain (weights, what it sees,
its ordered choices, training stats, the points chart) and a **`.vrlp`** holds a whole project (scripts, pictures,
settings) plus its brain as an embedded `.vrl`. Binding spec: docs/dev/package.md; API: core/include/vyrn/package.h.

- Both are ZIP files (a `mimetype` first entry, like EPUB, so the desktop recognizes them; renaming to `.zip` shows
  the contents). Weights are safetensors: opening a brain file never runs code.
- A `.vrl` is only accepted by a project whose input and choices fit it (`brain::compare`), with a plain reason.
- A `.vrlp` contains someone else's Python, so it imports as **untrusted**: Session refuses Check/Train/Watch until
  the person trusts it in the app (or `vyrn-cli trust`). Agents (MCP) can import and read packages, never trust them.
- Exports drop machine-specific and permission settings (`python`, `start_program` → a hint, `agents`), hidden
  files, brains/runs folders, agent instruction files and anything that isn't a script, picture, sound or text file.

## 9. Risks / out of scope

| Risk | Mitigation |
|---|---|
| Debian libtorch convolutions are slow on this A76 (no SVE/ACL) | oneDNN off by default, OpenBLAS-openmp, the light DER net, measured at M2. Fallback: the pip wheel's libtorch |
| Unverified uinput behaviour on labwc: key-up when the device is removed, Xwayland autorepeat | Explicit M4 tests |
| Ctrl+Alt+Esc may not pass through the Pi Connect browser | Clicking Vyrn always holds; the Stop button |
| Real-time learning is limited by data (~36k ticks per hour at 10 Hz) | The demo game is designed to be learnable; the docs set expectations; status shows progress in plain words |
| Keys follow the active keyboard layout (punctuation can differ from US) | Letters, arrows and space are safe; docs note it |

**Out of scope for v1:**
- **Other platforms:** Windows/macOS, X11-only desktops, GNOME/KDE.
- **Input devices:** mouse control, gamepads.
- **Screen:** multiple monitors, per-window capture, OCR.
- **What the AI sees and does:** mixing pictures and numbers in one `see()`; continuous or independent simultaneous actions (key combos are single choices).
- **Training:** PPO (v1.1), parallel sim copies, GPU, learning from demonstrations.
- **Tooling:** a venv/pip manager, debugger, git, `.deb` packaging, running brains outside Vyrn.
