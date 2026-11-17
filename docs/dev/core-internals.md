# C++ core ("the chip"): layout and internal interfaces

Binding inputs: `docs/dev/PLAN.md` (§2–3, §7–8), `docs/dev/protocol.md` (the wire contract; the Python worker
already implements it). This file pins the C++ module boundaries so parts can be built in parallel.

## 0. Build rules

- CMake ≥ 3.25 + Ninja. Libraries: Debian `libtorch-dev` 2.6 (`find_package(Torch REQUIRED)`; config in
  `/usr/lib/aarch64-linux-gnu/cmake/Torch`), `nlohmann_json` (`find_package(nlohmann_json REQUIRED)`), GTest,
  Qt 6.8 Widgets, QScintilla (`cmake/FindQScintilla.cmake`: header dir `/usr/include/aarch64-linux-gnu/qt6`
  (`Qsci/qsciscintilla.h`), library `qscintilla2_qt6`; no CMake/pkg-config files exist).
- **No translation unit includes both Qt and torch headers.** `vyrn_core`'s public headers
  (`core/include/vyrn/*.h`) use only the standard library. A ctest `lint_no_torch_in_app` greps `app/` and
  `cli/` for `torch/` / `ATen/` includes and the core's public headers for Qt/torch includes.
- `vyrn_ipc` and `vyrn_core` compile as **C++17** (libtorch's tested standard); `app/`, `cli/` as C++20.
- Warnings: `-Wall -Wextra` on our code (not on third-party headers: include them as SYSTEM).
- Build: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j3`.
  Sources are globbed (`core/CMakeLists.txt`): add files under `core/src/{ipc,brain,run,util}/` without editing
  CMake. `vyrn_core` precompiles `<torch/torch.h>`.
- C++ tests: `tests/cpp/<area>/*.cpp` → one GTest binary `test_<area>` per area (`ipc` links only `vyrn_ipc`;
  `brain`, `run` link `vyrn_core` + torch); ctest names are `<area>.<Suite>.<Test>`, label `unit`.
  `tests/e2e/test_learning.py` (pytest, env `VYRN_CLI`, `VYRN_DATA`) → ctest `e2e_<world>`, label `e2e`;
  `tests/e2e/test_realtime.py` (real time, `tests/e2e/worlds/rt-catch`) → ctest `e2e_rt_catch`, label `slow`.
- Measured on this Pi (M0): DER net, batch 32, RGB×4 frames (12×84×84), one Adam training step ≈ 32 ms with
  oneDNN off, 4 threads, OpenBLAS-openmp, `OMP_WAIT_POLICY=PASSIVE`.

## 1. Targets

| Target | Kind | Contents | Links |
|---|---|---|---|
| `hello` | exe | `tools/hello/hello_torch.cpp` + `hello_qt.cpp` + `main.cpp` (M0 toolchain proof) | Torch, Qt6::Widgets, QScintilla |
| `vyrn_ipc` | static | `core/src/ipc/*` frame codec, worker process | nlohmann_json, pthread |
| `vyrn_core` | static | `core/src/{brain,run,util}/*`, public API `core/include/vyrn/*` | PUBLIC vyrn_ipc; PRIVATE torch |
| `vyrn_project` | static | `core/src/project/*`: projects, templates, settings, brains list, `.vyrn-lock`, PNG (`vyrn/project.h`, docs/dev/mcp.md §6) | PUBLIC vyrn_core; zlib |
| `vyrn_package` | static | `core/src/package/*`: `.vrl`/`.vrlp` sharing files (`vyrn/package.h`, docs/dev/package.md) | PUBLIC vyrn_project, vyrn_core; PRIVATE torch, zlib |
| `vyrn_mcp` / `vyrn-mcp` | static / exe | `cli/mcp/*`: the MCP server (docs/dev/mcp.md) | PUBLIC vyrn_package |
| `vyrn-cli` | exe | `cli/main.cpp` (+ `cli/package_cmd.cpp`) | vyrn_core (+ vyrn_project, vyrn_package, vyrn_mcp when built: `VYRN_HAVE_PROJECT`, `VYRN_HAVE_PACKAGE`, `VYRN_HAVE_MCP`) |
| `vyrn` | exe | `app/*` (M3) | vyrn_core, Qt6::Widgets, QScintilla |
| `vyrn-focus` | exe (C) | `focus/vyrn-focus.c` + wayland-scanner output (M4) | wayland-client |
| `vyrn_tests` | exe | `tests/cpp/*` (GTest), labels `unit`; e2e via `tests/e2e/*.cmake`/python, labels `e2e`, `slow`, `gui` | vyrn_core (+ torch for brain tests) |

`vyrn_project`, `vyrn_package`, `vyrn_mcp` and the app are built with `VYRN_WITH_APP_PARTS` (default ON; OFF builds
the core, `vyrn-cli`'s check/train/watch/bench and the core's tests only). The top-level CMakeLists.txt adds
`cli/mcp` before `cli`, so `vyrn-cli` can link `vyrn_mcp`.

Install layout (`cmake --install`, prefix `~/.local`): `bin/vyrn`, `bin/vyrn-cli`, `bin/vyrn-mcp`, `bin/vyrn-focus`,
`bin/vyrn-panic`, `share/vyrn/python/**`, `share/vyrn/templates/**`, `share/vyrn/docs/**`,
`share/applications/vyrn.desktop`, `share/icons/hicolor/256x256/apps/vyrn.png`,
`share/mime/packages/vyrn.xml`, `share/icons/hicolor/256x256/mimetypes/application-x-vyrn-{project,brain}.png`
(`install.sh --app`/`--desktop` then runs `update-mime-database`, `update-desktop-database`).
**Data dir lookup** (`vyrn::find_data_dir`): `$VYRN_DATA` → `<exe>/../share/vyrn` (installed) → the source tree
(`VYRN_SOURCE_DIR` compiled in) — the folder that contains `python/vyrn_worker.py`.

## 2. `vyrn_ipc` (`core/src/ipc/`, namespace `vyrn::ipc`)

```cpp
// proto.h
using json = nlohmann::json;
struct ArrayView { std::string dtype; std::vector<int64_t> shape; const uint8_t* data; size_t bytes; };
struct Frame {
  json j;                         // the JSON object (array refs left as {"$a":[off,dtype,shape]})
  std::vector<uint8_t> bin;       // the blob
  ArrayView array(const json& ref) const;   // validates bounds; throws ProtocolError
  bool has_array(const char* key) const;    // j[key] is an array ref
};
struct ProtocolError : std::runtime_error { using runtime_error::runtime_error; };
std::vector<uint8_t> encode(const json& j, const std::vector<uint8_t>& bin = {});
// Incremental decoder for a byte stream:
class Decoder { public: void feed(const uint8_t*, size_t); std::optional<Frame> next(); /* throws ProtocolError */ };

// worker.h
enum class Stream { Stdout, Stderr };
struct WorkerOptions {
  std::string python, data_dir, project_dir;
  std::vector<std::pair<std::string, std::string>> env;   // extra/override env vars
  std::function<void(Stream, const std::string& line)> on_line;   // called from the io thread, full speed
};
struct WorkerError  : std::runtime_error { json error; };          // the worker replied "error"
struct WorkerTimeout: std::runtime_error { json error; };          // timeout (error = the worker's timeout report if any)
struct WorkerDied   : std::runtime_error { std::vector<std::string> stderr_tail; bool panic; };
class WorkerProcess {
 public:
  explicit WorkerProcess(WorkerOptions);
  ~WorkerProcess();                         // kill_group() if still running, join io thread, remove pid file
  void spawn();                             // socketpair → fd 3, pipes, posix_spawn (own process group), pid file
  json hello(double timeout_s = 15);        // waits for the unprompted hello
  Frame request(json req, double timeout_s, double grace_s);  // adds "id"; returns the matching reply;
        // error reply → WorkerError; timeout → SIGINT pid, wait grace_s for the timeout error, SIGKILL group,
        // throw WorkerTimeout; EOF/protocol error → SIGKILL group, throw WorkerDied (panic = marker file seen)
  void freeze();                            // SIGUSR1 to pid (out-of-band key release)
  void kill_group();                        // SIGKILL the group, waitpid
  bool alive() const;  pid_t pid() const;
  std::vector<std::string> stderr_tail(size_t n) const;
};
void sweep_stale_pid_files();               // at app/CLI start
```
Requirements: `posix_spawn` with `posix_spawn_file_actions_adddup2` (child socket → 3, pipes → 1/2, `/dev/null` → 0),
`posix_spawn_file_actions_addclosefrom_np(…, 4)`, `POSIX_SPAWN_SETPGROUP` (pgid 0), `POSIX_SPAWN_SETSIGMASK` (empty);
parent ends `CLOEXEC`; sends use `MSG_NOSIGNAL`; io thread `poll()`s stdout/stderr and splits lines (lines longer than
8 KiB are split); pid file `$XDG_RUNTIME_DIR/vyrn/<pid>.pid` = `"<pid> <starttime> <pgid>\n"`; the panic marker
`$XDG_RUNTIME_DIR/vyrn/panic` is checked and removed when the worker dies. Worker env per protocol.md §1.

## 3. Brain (`core/src/brain/`, namespace `vyrn::brain`; torch allowed)

```cpp
// spec.h
struct ObsSpec { enum class Kind { Picture, Numbers } kind; int h = 0, w = 0; int d = 0; std::vector<std::string> names; };
struct Spec { std::string mode; ObsSpec obs; std::vector<std::string> choices, labels, effects;
              bool uses_keys = false, uses_screen = false; json raw; };
Spec parse_spec(const json& spec_msg);
// What a saved brain was built for (stored in brain.json):
struct BrainShape { ObsSpec::Kind kind; int in_c, in_h, in_w; int d; bool small_picture; int frames;
                    std::vector<std::string> choices, inputs; };
enum class Compat { Same, AppendedChoices, Incompatible };
Compat compare(const BrainShape& saved, const Spec& now, std::string* why_plain_english);

// preprocess.h  (pictures always arrive as uint8 [H,W,3])
struct Geometry { bool small; int h, w; };          // small (max side ≤ 32): native size; else 84x84
Geometry choose_geometry(int h, int w);
void to_frame(const uint8_t* rgb, int h, int w, const Geometry& g, uint8_t* out_chw); // area-resize down, bilinear
     // up for large; nearest for small (when the size differs from the saved native size); output CHW uint8, C=3
class FrameStack { public: FrameStack(int k, size_t frame_bytes); void reset(const uint8_t* f); void push(const uint8_t* f);
                   const std::vector<uint8_t>& stacked() const; /* oldest..newest, K*frame_bytes */ };
class RunningMeanStd { public: explicit RunningMeanStd(int d); void update(const float* x, int n_rows);
                       void normalize(const float* x, float* out) const; /* clip ±10, eps 1e-8 */
                       json to_json() const; void from_json(const json&); bool frozen = false; };

// nets.h
enum class NetKind { MLP, SmallCNN, DER };
struct NetConfig { NetKind kind; int in_c, in_h, in_w; int d; int n_actions; };
struct QNetImpl : torch::nn::Module { /* forward(x) -> [B, A]; pictures: uint8 → float /255 inside */ };
TORCH_MODULE(QNet);
QNet make_qnet(const NetConfig&);
void grow_actions(QNet&, int new_n);        // append output rows (small init), keep the rest (M5)

// replay.h  (ring buffer; one stored observation per slot; stacks rebuilt by index)
enum class EndKind { Terminal, Truncated };
struct Batch { torch::Tensor obs, actions, returns, next_obs, discounts; };  // discounts = γ^m or 0 if terminal
class Replay {
 public:
  Replay(int64_t capacity, size_t obs_bytes, bool obs_is_float, int stack_k, int n_step, float gamma);
  int64_t add_obs(const void* obs, bool segment_start);        // store o_t; returns its slot
  void set_transition(int64_t slot, int action, float reward);  // a_t taken at slot, r_t received after it
  void end_segment(int64_t last_slot, EndKind kind);            // last_slot holds the final obs (no action)
  int64_t size() const;
  Batch sample(int batch_size, std::mt19937_64& rng) const;     // only fully valid transitions
};
// Validity: t is sampleable iff a_t is set and o_{t+1} exists in the same segment and nothing in the stack of
// o_t / o_{t+m} or in the n-step window has been overwritten. n-step return stops at the segment end:
// m = min(n, steps to the end); discount = 0 if the segment ended Terminal within the window, else γ^m.
// A hold/late cut is recorded as end_segment(last fully observed slot, Truncated) WITHOUT a transition on it.

// dqn.h
struct DqnConfig { /* from PLAN.md §3 table + vyrn.json "advanced" overrides */ int batch; float lr, adam_eps;
                   int target_every; int n_step; float gamma; float grad_clip; int64_t learning_starts;
                   double updates_per_tick; int64_t explore_ticks; float eps_start, eps_end, eps_watch;
                   int64_t buffer; int frames; };
DqnConfig default_config(const Spec&, bool realtime);
struct UpdateStats { double loss, q_mean; int64_t updates; };
class Policy {        // immutable snapshot for acting (RT actor) — eval mode, no grad
 public: int act(const torch::Tensor& obs1, float epsilon, std::mt19937_64&, std::vector<float>* probs) const;
         uint64_t version; };
class Dqn {
 public:
  Dqn(const DqnConfig&, const NetConfig&, torch::Device = torch::kCPU);
  int act(const torch::Tensor& obs1, float epsilon, std::mt19937_64&, std::vector<float>* probs);
  UpdateStats update(const Batch&);                 // Double DQN + Huber + Adam + grad clip; hard target copy
  std::shared_ptr<const Policy> snapshot() const;   // deep copy of the online net
  float reward_scale() const; void observe_warmup_reward(float r); void freeze_reward_scale();
  // persistence
  void save(const std::string& dir) const; void load(const std::string& dir);   // model.pt, optim.pt
};

// checkpoint.h  — atomic: write <brain>/<which>/save-<n>/ (model.pt, optim.pt, brain.json), fsync files + dir,
// then atomically replace <brain>/<which>/current (a file containing "save-<n>"), then delete older save-* dirs.
struct BrainMeta { json j; };   // spec/shape, config, counters, normalizer stats, reward scale, best score, version
void save_checkpoint(const std::string& brain_dir, const std::string& which, const Dqn&, const BrainMeta&);
bool load_checkpoint(const std::string& brain_dir, const std::string& which, Dqn&, BrainMeta&);   // false if none
```

Thread counts: every thread that runs torch ops first calls `at::get_num_threads()` (exported, runs libtorch's own
lazy init) and **then** `at::set_num_threads(k)`; `at::set_num_interop_threads(1)` once per process; oneDNN off by
default (`at::globalContext().setUserEnabledMkldnn(false)`) unless `VYRN_ONEDNN=1` / bench says otherwise.

## 4. Runners and session (`core/src/run/`, public API `core/include/vyrn/session.h`)

```cpp
namespace vyrn {
struct RunConfig {
  enum class Action { Check, Train, Watch } action = Action::Train;
  std::string project_dir, data_dir, python = "/usr/bin/python3", focus_helper;
  std::string watch_brain = "best";          // Watch: "best" | "latest"
  double watch_speed = 1.0;                  // sims: × decisions/s; 0 = as fast as possible
  bool allow_input = false;                  // real time only, set by the app after its safety flow / CLI --allow-input
  bool allow_screen = true;                  // false: load "screen": false (an agent's Check without screen permission)
  bool new_brain = false;                    // "Start over"
  uint64_t seed = 0;                         // 0 → random
  int64_t max_ticks = -1; double max_seconds = -1;   // stop conditions (CLI/tests)
  int check_ticks = 20;
};
enum class RunState { Starting, Countdown, Restarting, Running, Holding, Paused, Error, Stopping, Finished };
struct Picture { int w = 0, h = 0; std::vector<uint8_t> rgb; };
struct LiveView {
  uint64_t version = 0; std::string kind;             // "picture" | "numbers"
  Picture input;                                      // network input as seen (newest frame, RGB)
  std::vector<float> numbers; std::vector<std::string> number_names;
  std::vector<std::string> choices; std::vector<float> probs; int chosen = -1;
  std::vector<std::pair<std::string, std::string>> show_text; std::vector<std::pair<std::string, Picture>> show_pictures;
  int64_t round = 0, tick = 0, total_ticks = 0; double best_mean = 0; bool has_best = false;
  double explore = 0, ticks_per_s = 0; RunState state = RunState::Starting;
};
class Session {
 public:
  explicit Session(RunConfig);
  ~Session();                                   // stop() + join
  void start();                                 // non-blocking
  void pause(); void resume(); void stop();     // stop saves the brain (Train) and closes the worker
  void set_hold(bool);                          // app-side hold request (real time)
  void apply_changes();                         // respawn the worker, keep the brain
  std::vector<std::string> drain(size_t max = 1000);   // JSON event strings (§5), oldest first
  LiveView live() const;
  RunState state() const; bool finished() const;
};
std::string find_data_dir(const char* argv0);
std::string not_trusted_message(const std::string& project_dir);   // "" = Session runs it (see "Trust" below)
inline constexpr const char* kStopMarkerFile = ".vyrn-stop";       // see "Stop marker" below
}
```

- **SimRunner** (`sim_runner.cpp`): one worker; brain thread: `reset` → loop `act` → `step` → replay → updates per
  `updates_per_tick`; Train is unpaced; Watch paces at decisions/s × speed with the chosen brain, ε_watch, no learning,
  frozen normalizers. Check: `check_ticks` random steps.
- **RealtimeRunner** (`rt_runner.cpp`): states per PLAN.md §3 (countdown → `lock_focus` → `restart` + `check_over`
  polling → rounds on absolute deadlines → hold/cut → round limit); actor thread (1 torch thread) acts on a
  `Policy` snapshot; learner thread (2 torch threads, `nice +10`) samples/updates and publishes a snapshot every 50
  updates (mutex + `shared_ptr` swap); `updates ≤ ticks × updates_per_tick`. Check: `restart` once (if the project
  has one; its keys are described), then 5 `observe` ticks + described acts. Details of Train and Watch:
  - A project whose spec says `uses.keys` doesn't play without `RunConfig::allow_input` (fatal error, reason
    `input_not_allowed`). The countdown (3 s of `countdown` events, then `seconds_left: 0`) and `lock_focus` happen
    only with `allow_input`; after the countdown it waits (a `hold`, reason `user`) while the app holds, so the lock
    never lands on Vyrn. While locked, a `status` before every `restart` checks the game is in front.
  - Restart: `restart`, then `check_over` every tick (nothing chosen or recorded); `restart` again every 3 s of
    polling; after 30 s an error (reason `restart_stuck`, at the `restart` hook; not fatal: Resume respawns).
  - Round: `observe{first}` → `act` per tick; a tick more than 3 periods late cuts the segment and sends `pause`
    (`hold` on/off, reason `late`). A result with `focused: false` (or `set_hold(true)`, or `pause()`) holds: the
    open transition is never sampled (`end_segment` on the last observation), `pause` (keys up), `status` at 5 Hz
    until the game is in front and nobody holds, then the round goes on with a new segment. Its points still count
    for the round. At round_seconds × decisions/s ticks the segment ends truncated and the next round starts with
    `observe{first:true}` on the tick schedule, without `restart`.
  - Keys never stay down: while the actor waits for a reply, `stop()` (and a pause or hold during `observe`, `act`,
    `check_over`) sends SIGUSR1 (freeze) at once; if that request hasn't come back 0.5 s later the worker's group is
    SIGKILLed (error reason `unresponsive` unless stopping). After a freeze the actor sends `resume` before it acts
    again. Timeouts use the 0.5 s real-time grace.
  - Vyrn Link (`"link"` in vyrn.json): `GameLink` (`game_link.{h,cpp}`) listens before Python loads, the runner waits
    for the game (a `hold` with reason `game`), hands its state to every request and sends it the buttons; lockstep
    takes one tick per state the game sends. A port in use is the fatal error reason `link`. Details:
    docs/dev/link.md, "Real-time runner and app".
  - `max_ticks` counts recorded transitions; `max_seconds` and the brain's training time leave holds out.
  - Timings and a trace of the brain-side steps are test hooks (`run/rt_runner.h`).
- Saves happen on the model-owning thread between updates (request flag), every 10 min, on stop, and best (kept in
  RAM) at most once a minute. CSV logs in `runs/<YYYYmmdd-HHMMSS>/{rounds,updates}.csv`, plus
  `runs/<ts>/run.json` = `{"brain": "<id>", "action": "train", "started": "<local ISO time>"}` (written by
  `SimRunner::open_logs`, so only Train makes a run folder; `action` is `"train"|"watch"|"check"` should another
  action log one). Sharing reads it: a `.vrl`'s points chart is the `rounds.csv` of the runs whose `run.json` names
  that brain (docs/dev/package.md §4).
- **Trust** (docs/dev/package.md §7): `run_session` checks `util::project_trusted(dir)` (`core/src/util/trust.h`)
  right after the project folder. A project with `<project>/.vyrn-imported` whose `"trusted"` isn't `true` (a
  damaged or unreadable marker, a symlink or a folder counts as untrusted) gets one `error` event of kind
  `not_trusted` with `util::not_trusted_message(dir)`: "This project came from Catch.vrlp. Its Python code hasn't run
  on this computer yet: read its scripts, then trust it in Vyrn (or run: vyrn-cli trust <folder>)." Python is never
  spawned and the session finishes ("Nothing was run: this project isn't trusted yet."). `vyrn_package`'s
  `is_trusted()` calls the same function; the public `vyrn::not_trusted_message()` lets the CLI and the MCP server ask
  first.
- **Stop marker** (`core/src/run/stop_marker.{h,cpp}`): another program stops a running Session by creating the empty
  file `<project>/.vyrn-stop` (the app's Stop button for a training that `vyrn-cli` or an agent's MCP server runs; it
  replaces sending SIGINT). From just before the runner starts until it returns, a `StopMarkerWatch` thread polls
  for it every 0.5 s; when it is there, the thread deletes it, logs one `log` line (stream `app`, mentioning
  `.vyrn-stop`) and calls `RunContext::request_stop()`, exactly what `Session::stop()` does (Train saves the brain).
  It works for Check, Train and Watch, sims and real time. A marker already there at the start is stale: it is
  deleted (one that can't be deleted, e.g. in a read-only folder, is ignored as long as it is the same file).

## 5. Session events (`drain()`), one JSON object per string

| `type` | Fields |
|---|---|
| `state` | `state` (RunState name, lowercase), `detail` (plain English) |
| `log` | `stream` (`stdout`/`stderr`/`app`), `text` (one line). Display throttled to ≤ 200 lines/s; drops counted in a `log` line "(N lines dropped)" |
| `spec` | the worker's spec fields + `brain` (`new`/`resumed`/`incompatible`), `why`, `brain_id`, `net`, `frames` (stacked frames of the brain's input: the brain shape's `frames` for pictures — the saved brain's when it resumes, else the config's, default 4 — and 1 for numbers), `seed`, `action`; Watch adds `checkpoint` |
| `error` | the worker's error fields (protocol.md §3.7), `{kind:"app", reason, message, hint}`, or `{kind:"not_trusted", message}` (an imported project nobody trusted yet: nothing ran); `script`, `line`, `fatal` |
| `warning` | `script`, `line`, `message` |
| `round` | `round`, `points`, `ticks`, `truncated`, `mean20`, `best_mean20` |
| `update` | `updates`, `loss`, `q`, `explore`, `lr`, `updates_per_s` |
| `timing` | `see_ms`, `points_ms`, `respond_ms`, `brain_ms`, `ipc_ms`, `ticks_per_s`, `late_p99_ms`, `overruns` (every ~2 s) |
| `countdown` | `seconds_left` |
| `hold` | `on` (bool), `reason` (`unfocused`/`user`/`paused`/`late`/`game`: Vyrn Link waits for the game) |
| `checkpoint` | `which`, `path`, `best_mean20` |
| `link` | Vyrn Link (docs/dev/link.md): `url`, `address`, `port`, `code`, `lockstep`, `frames_per_step`, `connected`; with a game also `game`, `client`, `from`. At the start and whenever a game connects or leaves |
| `check` | `ok`, `warnings[]`, `notes[]` (real time: `game_over()` stayed False; also a `log` line), `ms{}`, `mean_points`, `game_over_seen`, `constant_inputs[]`, `restart_did[]` (real time, when `restart` ran) |

## 6. CLI (`vyrn-cli`)

```
vyrn-cli check <project> [--json]
vyrn-cli train <project> [--minutes M | --ticks N] [--seed S] [--new-brain] [--allow-input] [--json]
vyrn-cli watch <project> [--brain best|latest] [--speed X] [--ticks N] [--allow-input] [--json]
vyrn-cli bench
vyrn-cli pack | export-brain | inspect | import | trust ...     (docs/dev/package.md §8; VYRN_HAVE_PACKAGE)
vyrn-cli mcp [--projects-dir DIR] [--allow-dir DIR]... ...      (docs/dev/mcp.md; VYRN_HAVE_MCP)
```
`--json` prints every event as one JSON line (JSONL); otherwise a friendly progress line per ~2 s and per round
summary. `main()` re-executes itself once with `OMP_WAIT_POLICY=PASSIVE`, then hands `mcp` to
`run_mcp_server(argc - 1, argv + 1)` and the sharing commands to `run_package_command()`. `train` and `watch` hold
the project's `.vyrn-lock` (`by: "cli"`, `VYRN_HAVE_PROJECT`) for the whole run; while another live program holds
it they refuse ("The Vyrn app is already training this project (since 2026-11-13 17:05).", exit 2). An untrusted
project prints the `not_trusted` sentence and exits 2. Exit codes: 0 ok, 2 usage / not trusted / another trainer,
3 script error (worker `error`; also the app reasons `timeout`, `restart_stuck` and `unresponsive`), 4
crash/protocol/worker died, 5 a sharing file was refused or couldn't be written/read. Reasons `input_not_allowed`,
`no_brain`, `brain_mismatch`, `no_project` and `link` (the Vyrn Link port is taken) exit 2. The text output says
what to do in terminal words (e.g. "Add --allow-input"), and prints the Vyrn Link address and pairing code once.
