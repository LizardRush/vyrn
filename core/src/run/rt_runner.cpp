// RealtimeRunner: projects without a world.py, played in real time (docs/dev/core-internals.md section 4,
// PLAN.md sections 2-4).
//
// Check: restart() once, then 5 observe ticks at decisions/s with described acts (nothing is pressed).
// Train and Watch: a state machine on the actor thread (the session's run thread):
//   countdown -> lock_focus (both only when keys are allowed) -> restart() + check_over polling -> a round:
//   ticks on absolute deadlines (observe -> act on a Policy snapshot) until a game over (restart again) or the
//   round limit (a truncated end, no restart()), holding whenever the game window isn't in front, the app
//   holds (set_hold) or the run is paused.
// Train adds a learner thread (2 torch threads, nice +10): asynchronous DQN updates, at most updates_per_tick
// per tick on average, a new snapshot for the actor every 50 updates, and every checkpoint (it owns the
// network). A guard thread makes sure keys never stay down while the actor waits for Python.
// A project whose vyrn.json has "link" plays a game connected with Vyrn Link (docs/dev/link.md): the run listens
// for it (GameLink), waits for it ("Waiting for the game"), hands its state to Python each tick and sends it the
// AI's buttons; in lockstep the game sets the pace (one tick per state it sends).
#include "run/rt_runner.h"

#include "brain/dqn.h"
#include "brain/threads.h"
#include "run/brain_host.h"
#include "run/check.h"
#include "run/game_link.h"
#include "run/pacing.h"
#include "run/rounds.h"
#include "run/runner.h"
#include "util/clock.h"
#include "platform/system.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <random>
#include <thread>

namespace vyrn::run {

namespace test_hooks {
RtTimings rt_timings;
std::function<void(const std::string&)> rt_trace;
std::function<void()> keys_released;
}  // namespace test_hooks

namespace {

namespace fs = std::filesystem;

constexpr int kCheckTicks = 5;
constexpr int kActorThreads = 1;            // the actor only runs forward passes
constexpr int kLearnerThreads = 2;
constexpr int kLearnerNice = 10;
constexpr int kSnapshotEvery = 50;          // updates between the snapshots the actor plays with
constexpr double kLiveEvery = 0.1;          // live view: at most 10 refreshes a second
constexpr double kTelemetryEvery = 2.0;     // timing and update events, CSV flushes
constexpr double kSaveLatestEvery = 600.0;  // every 10 minutes
constexpr double kSaveBestEvery = 60.0;     // the best brain is written at most once a minute
constexpr double kGuardPoll = 0.02;         // the key guard looks every 20 ms
constexpr double kGameWait = 0.25;          // Vyrn Link: waits for the game in slices (Stop also ends them at once)
constexpr double kCheckGameWait = 3.0;      // Check: how long after listening it waits for a game to connect
constexpr double kStaleState = 1.0;         // real time: a game whose newest state is older than this is paused
constexpr double kSlowGameNote = 2.0;       // lockstep: say that Vyrn waits for the game after this long
constexpr int64_t kLockstepSlack = 4;       // lockstep Train: updates the learner may be behind before Vyrn waits
constexpr double kLearnerWait = 1.0;        // ...and at most this long per tick

double number_or(const json& j, const char* key, double fallback) {
  auto it = j.find(key);
  return it != j.end() && it->is_number() ? it->get<double>() : fallback;
}

bool flag(const json& j, const char* key) {
  auto it = j.find(key);
  return it != j.end() && it->is_boolean() && it->get<bool>();
}

std::string text_or(const json& j, const char* key) {
  auto it = j.find(key);
  return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// A result or status reply says the locked game window isn't in front ("focused": null = no focus lock).
bool unfocused(const json& reply) {
  auto it = reply.find("focused");
  return it != reply.end() && it->is_boolean() && !it->get<bool>();
}

void trace(const std::string& step) {
  if (test_hooks::rt_trace) test_hooks::rt_trace(step);
}

// Leaves the phase being played: stop() or a limit (apply = false), or apply_changes() (apply = true).
struct Interrupt {
  bool apply = false;
};

// restart() didn't clear game_over() in time.
struct RestartStuck {};

// ---- KeyGuard ------------------------------------------------------------------------------------------------
// Keys must never stay down (PLAN.md section 4, "Input safety"). While the actor waits for Python, stop() -- and
// a pause or a hold during a tick request (observe, act, check_over) -- freezes the worker at once (SIGUSR1, or
// 'F' on Windows): its watchdog lets go of every key even while user code runs. If that request still hasn't come
// back kill_after seconds later, the worker is killed with everything it started (Linux: the kernel releases the
// keys of a virtual keyboard whose process died; macOS and Windows: close_worker() calls
// platform::release_all_keys()). Timeouts escalate the same way inside WorkerProcess::request().
class KeyGuard {
 public:
  explicit KeyGuard(Controls& controls) : controls_(controls) {
    thread_ = std::thread([this] { main(); });
  }
  ~KeyGuard() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      quit_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }
  KeyGuard(const KeyGuard&) = delete;
  KeyGuard& operator=(const KeyGuard&) = delete;

  void set_kill_after(double seconds) {
    std::lock_guard<std::mutex> lock(mu_);
    kill_after_ = seconds;
  }
  // The worker to watch; nullptr before it is closed or replaced.
  void attach(ipc::WorkerProcess* proc) {
    std::lock_guard<std::mutex> lock(mu_);
    proc_ = proc;
    busy_ = false;
    killed_ = false;
  }
  void begin(const std::string& type) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      busy_ = true;
      tick_request_ = type == "observe" || type == "act" || type == "check_over";
      ++seq_;
    }
    cv_.notify_all();
  }
  void end() {
    std::lock_guard<std::mutex> lock(mu_);
    busy_ = false;
  }
  // True once after a freeze: the worker refuses key actions until it gets "resume".
  bool take_froze() {
    std::lock_guard<std::mutex> lock(mu_);
    const bool froze = froze_;
    froze_ = false;
    return froze;
  }
  bool killed() const {
    std::lock_guard<std::mutex> lock(mu_);
    return killed_;
  }

 private:
  void main() {
    std::unique_lock<std::mutex> lock(mu_);
    while (!quit_) {
      if (proc_ == nullptr || !busy_) {
        cv_.wait(lock);   // begin() and the destructor wake it
        continue;
      }
      cv_.wait_for(lock, std::chrono::duration<double>(kGuardPoll));
      if (quit_ || proc_ == nullptr || !busy_) continue;
      const double now = util::now_seconds();
      if (frozen_seq_ != seq_) {
        const bool stop = controls_.stop_requested();
        const bool hold = tick_request_ && (controls_.paused() || controls_.hold());
        if (stop || hold) {
          proc_->freeze();
          frozen_seq_ = seq_;
          frozen_at_ = now;
          froze_ = true;
        }
      } else if (!killed_ && now - frozen_at_ >= kill_after_) {
        proc_->kill_group();   // the request then throws WorkerDied
        killed_ = true;
      }
    }
  }

  Controls& controls_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::thread thread_;
  bool quit_ = false;
  ipc::WorkerProcess* proc_ = nullptr;
  bool busy_ = false, tick_request_ = false, froze_ = false, killed_ = false;
  uint64_t seq_ = 0, frozen_seq_ = 0;
  double frozen_at_ = 0, kill_after_ = 0.5;
};

// Sums for one telemetry window (about 2 seconds).
struct Window {
  double start = 0;
  int64_t ticks = 0;
  double see = 0, points = 0, respond = 0, brain = 0, ipc = 0;
  std::vector<double> late_ms;
  int64_t skipped_before = 0;
};

// What the actor tells the learner about the run (for saves and update events).
struct Progress {
  Counters counters;                 // the brain's totals
  std::optional<double> best_mean20;
  double session_seconds = 0;        // this run's playing time
  double explore = 0, ticks_per_s = 0;
};

class RealtimeRunner final : public Runner {
 public:
  RealtimeRunner(RunContext& ctx, RunSetup setup)
      : ctx_(ctx), cfg_(ctx.config()), setup_(std::move(setup)), guard_(ctx.controls()) {}
  ~RealtimeRunner() override {
    ctx_.controls().set_waker(nullptr);   // before game_ goes
    stop_learner(false);
  }

  std::string run() override;

 private:
  enum class Phase { Countdown, Lock, Restart, Round };
  enum class RoundEnd { GameOver, Truncated };

  bool training() const { return cfg_.action == RunConfig::Action::Train; }
  double dps() const { return setup_.settings.decisions_per_second; }
  Phase first_phase() const { return cfg_.allow_input ? Phase::Countdown : Phase::Restart; }
  LoadOptions load_options(bool check) const;
  std::string check();
  std::string play();

  // ---- Python
  ipc::Frame call(json req, double timeout_s);
  void close_worker();
  bool respawn();
  bool recover_after_error();
  void unfreeze();

  // ---- the state machine (actor thread). Each throws Interrupt (stop, limits, apply_changes) and what the
  // worker calls throw.
  void countdown();
  void lock_focus();
  void restart_round();
  bool try_restart_round();   // false: a hold came in between (start over)
  RoundEnd play_round(bool on_schedule);   // on_schedule: the previous round was cut by the round limit
  void hold(std::string reason);
  void late_cut();
  std::string hold_reason(bool unfocused_now) const;   // "" when nothing holds the run
  void check_interrupts();
  bool sleep_until(double until);   // false: a pause or a hold came first
  void set_play_state(RunState state, const std::string& detail);

  // ---- Vyrn Link (game_ is set for projects whose vyrn.json has "link")
  bool start_game_link(bool check);   // false: Train/Watch can't run (the error was emitted)
  void stop_game_link();
  bool game_ready() const;            // connected, and in real time its newest state is fresh
  void wait_for_game();               // holds ("game") until game_ready(), a pause or a hold
  link::MessagePtr next_state();      // lockstep: the next state to answer (null: none yet, look at holds first)
  void wait_for_learning();           // lockstep Train: lets the learner keep up with the game

  // ---- the brain side (actor thread)
  void start_segment(const ObsView& obs);
  void end_segment(const ObsView& obs, bool terminal);
  void cut();
  void record(int action, double points);
  void finish_round(bool game_over, bool truncated);
  void abandon_round();
  float epsilon() const;
  Counters counters_now() const;
  bool limits_reached(std::string* why) const;
  void periodic(bool force);
  void reset_window();
  void publish_live(int chosen, std::vector<float> probs);
  void share_progress();
  Progress progress() const;

  // ---- the learner thread (Train)
  void start_learner();
  void stop_learner(bool final_save);
  void learner_main();
  void learner_report(double span);
  void save_latest_now(const Progress& p);
  void write_best_now(const Progress& p);
  void note_brain_in_settings();

  RunContext& ctx_;
  const RunConfig& cfg_;
  RunSetup setup_;
  RtTimings timings_;
  std::unique_ptr<GameLink> game_;
  uint64_t answered_seq_ = 0;     // lockstep: the newest state answered with an input
  uint64_t game_connection_ = 0;  // the connection whose states the open segment saw
  link::MessagePtr last_state_;   // the state the last observe saw
  double game_wait_since_ = -1;   // lockstep: when Vyrn started waiting for the next state (-1: not waiting)
  bool game_slow_ = false;        // lockstep: the state says Vyrn waits for the game
  std::unique_ptr<WorkerLink> link_;
  KeyGuard guard_;
  int generation_ = 0;   // worker respawns (each gets its own seed)
  bool locked_ = false;  // lock_focus succeeded for this worker

  double period_ = 0.1, tick_timeout_ = 1.0;
  int64_t round_limit_ = 600;
  brain::Spec spec_;
  brain::DqnConfig dcfg_;
  std::unique_ptr<BrainHost> brain_;
  BrainPlan plan_;

  // actor state
  RoundTracker rounds_;
  Counters counters_;            // this brain's totals (training) or this Watch's (watching)
  std::atomic<int64_t> session_ticks_{0};
  int64_t round_ticks_ = 0;
  double round_points_ = 0, points_sum_ = 0;
  int prev_ = -1;                // the choice whose points the next observe brings (-1: none)
  bool segment_open_ = false;
  util::Stopwatch running_;
  Pacer pacer_{0.1};
  ShowValues show_;
  Window window_;
  double ticks_per_s_ = 0, last_live_ = -1e9;
  RunState play_state_ = RunState::Running;
  std::string play_detail_;
  std::string stop_reason_;
  util::CsvWriter rounds_csv_;

  // shared with the learner
  mutable std::mutex progress_mu_;
  Progress progress_;
  std::thread learner_;
  std::mutex learner_mu_;
  std::condition_variable learner_cv_;
  bool learner_quit_ = false, final_save_ = false;
  std::atomic<bool> learner_failed_{false};
  std::string learner_error_;
  std::atomic<int64_t> updates_done_{0};    // this run's updates (the learner counts them)
  std::atomic<bool> learner_idle_{false};   // the replay had nothing to learn from yet
  std::mutex updates_mu_;
  std::condition_variable updates_cv_;
  // learner-only (read by the actor after the join)
  util::CsvWriter updates_csv_;
  bool brain_recorded_ = false, saved_any_ = false, final_saved_ = false;
};

LoadOptions RealtimeRunner::load_options(bool check) const {
  LoadOptions o;
  o.check = check;   // check: effects are described, never pressed
  o.allow_input = !check && cfg_.allow_input;
  o.allow_screen = cfg_.allow_screen;
  o.seed = setup_.seed + static_cast<uint64_t>(generation_);
  o.round_limit = std::max<int64_t>(1, std::llround(setup_.settings.round_seconds * dps()));
  o.decisions_per_second = dps();
  o.focus_helper = cfg_.focus_helper;
  o.grace_s = kRealtimeGrace;
  return o;
}

std::string RealtimeRunner::run() {
  timings_ = test_hooks::rt_timings;
  guard_.set_kill_after(timings_.freeze_kill_s);
  return cfg_.action == RunConfig::Action::Check ? check() : play();
}

// ---- Check ---------------------------------------------------------------------------------------------------

std::string RealtimeRunner::check() {
  ctx_.set_state(RunState::Starting, "Starting Python and loading your scripts...");
  start_game_link(true);   // a game connected with Vyrn Link shows see() real numbers; nothing is sent to it
  WorkerLink link(ctx_, setup_.python, setup_.data_dir, setup_.project_dir);
  ipc::Frame spec_frame;
  try {
    spec_frame = link.start(load_options(true));
  } catch (const StopRequested&) {
    stop_game_link();
    return "Stopped before the scripts were loaded.";
  } catch (...) {
    ctx_.emit(describe_failure(std::current_exception(), true).event);
    ctx_.emit(CheckReport(json::object()).event(false, true, dps()));
    stop_game_link();
    return "Stopped: the scripts didn't load.";
  }
  brain::Spec spec;
  try {
    spec = brain::parse_spec(spec_frame.j);
  } catch (const brain::SpecError& e) {
    ctx_.emit(app_error_event("protocol", e.what(), "Vyrn's app and its Python library may not match: reinstall Vyrn.",
                              true));
    stop_game_link();
    return "Stopped: Python described the project in a way Vyrn doesn't understand.";
  }
  if (game_) game_->set_buttons(spec_frame.j);
  brain::DqnConfig dcfg = brain::default_config(spec, true);
  for (const auto& w : brain::apply_overrides(dcfg, setup_.settings.advanced, spec.obs.kind)) {
    emit_settings_warning(ctx_, w);
  }
  const BrainPlan plan = plan_check(setup_.project_dir, setup_.settings.brain, spec, dcfg.frames);
  ctx_.emit(spec_event(spec_frame.j, cfg_.action, plan.status, plan.why, plan.id,
                       brain::describe(brain::net_config_for(plan.shape)), plan.shape, setup_.seed));
  if (plan.status == "incompatible") ctx_.emit(warning_event("", 0, plan.why));
  ObsView obs;
  if (read_obs(spec_frame, "preview", obs)) ctx_.publish(check_live_view(obs, spec));

  // Training will need the screen and the keyboard this project uses: say now if they aren't there.
  const json backends = link.hello().value("backends", json::object());
  const json problems = link.hello().value("problems", json::object());
  for (const auto& [use, what] : {std::pair<const char*, const char*>{"screen", "Screen capture"},
                                  std::pair<const char*, const char*>{"keys", "Pressing keys"}}) {
    const bool used = spec.raw.value("uses", json::object()).value(use, false);
    if (used && backends.value(use, json()).is_null()) {
      ctx_.emit(warning_event("", 0, std::string(what) + " isn't available: " +
                                         problems.value(use, std::string("Vyrn's setup check can tell you why.")) +
                                         " Check works anyway; training will need it."));
    }
  }

  CheckReport report(spec_frame.j);
  if (game_ && !game_->latest_state()) {
    // A mod reconnects every couple of seconds: give it a moment, then go on without it. A game that has just
    // connected sends its first state right after the welcome: wait for that too, or see() would read no state.
    if (!game_->connected()) {
      ctx_.set_state(RunState::Running, "Checking: waiting a moment for your game to connect...");
    }
    const double until = game_->started_at() + kCheckGameWait;
    for (double left = until - util::now_seconds(); left > 0 && !ctx_.controls().stop_requested();
         left = until - util::now_seconds()) {
      if (!game_->connected()) {
        game_->server().wait_connected(left);
        continue;
      }
      const link::Wait w = game_->server().wait_state(0, left).status;
      if (w == link::Wait::Ok || w == link::Wait::Stopped) break;
    }
  }
  bool game_seen = false;   // a state from the game reached see()
  auto with_game = [&](json req) {
    if (game_) {
      link::MessagePtr state = game_->latest_state();
      game_seen = game_seen || state != nullptr;
      game_->attach(req, state, true);
    }
    return req;
  };
  const double period = 1.0 / dps();
  const double timeout = std::max(1.0, 5 * period);
  Pacer pacer(period);
  std::mt19937_64 rng(setup_.seed);
  std::uniform_int_distribution<int> any_choice(0, static_cast<int>(spec.choices.size()) - 1);
  const json hooks = spec_frame.j.value("hooks", json::object());
  bool ok = true;
  try {
    if (hooks.is_object() && hooks.contains("restart")) {
      // Like Train, start with restart(): it may set up what points() needs. Its keys are only described.
      ctx_.set_state(RunState::Running, "Checking: restart() first; keys are only described, never pressed...");
      ipc::Frame r = link.request(with_game(json{{"t", "restart"}}), kLoadTimeout, kRealtimeGrace);
      report.add_restart(r.j);
      ctx_.app_log(report.restart_summary());
    }
    ctx_.set_state(RunState::Running, "Checking: " + std::to_string(kCheckTicks) +
                                          " ticks in real time; keys are only described, never pressed...");
    for (int i = 0; i < kCheckTicks && !ctx_.controls().stop_requested(); ++i) {
      const PaceStep step = pacer.next(util::now_seconds());
      if (step.wait_s > 0) ctx_.controls().wait_for(step.wait_s);
      if (ctx_.controls().stop_requested()) break;
      ipc::Frame r = link.request(with_game(json{{"t", "observe"}, {"first", i == 0}}), timeout, kRealtimeGrace);
      report.count_step();
      report.add_result(r);
      for (const auto& w : report.take_new_warnings()) ctx_.emit(warning_event(w.script, w.line, w.message));
      if (read_obs(r, "obs", obs)) ctx_.publish(check_live_view(obs, spec));
      ipc::Frame done = link.request(json{{"t", "act"}, {"a", any_choice(rng)}}, timeout, kRealtimeGrace);
      report.add_ok(done.j);
    }
  } catch (...) {
    ctx_.emit(describe_failure(std::current_exception(), true).event);
    ok = false;
  }
  for (const auto& w : report.take_new_warnings()) ctx_.emit(warning_event(w.script, w.line, w.message));
  const bool uses_game = spec.raw.value("uses", json::object()).value("game", false);
  if (game_ && !game_seen) {
    report.ignore_constant_inputs();
    report.add_note("No game was connected, so see() read an empty vyrn.game. Start your game, connect it to " +
                    game_->url() + " with the pairing code from Settings \u2192 Vyrn Link, then Check again to test "
                    "see() with its numbers.");
  } else if (game_ && game_->lockstep()) {
    report.ignore_constant_inputs();   // the game waits for an input that Check never sends: its state stays put
  } else if (!game_ && uses_game && !link::link_settings(json(setup_.settings.raw))) {
    ctx_.emit(warning_event("vyrn.json", 0,
                            "This project talks to a game with Vyrn Link (vyrn.game, vyrn.send or \"game ...\" "
                            "choices), but vyrn.json has no \"link\", so Vyrn won't wait for the game. Add \"link\": "
                            "{\"port\": 7457} (or turn on Vyrn Link in Settings)."));
  }
  if (ok) {
    for (const auto& w : report.findings(true, dps())) ctx_.emit(warning_event(w.script, w.line, w.message));
    for (const auto& note : report.notes(true, dps())) ctx_.app_log("Note: " + note);
  }
  ctx_.emit(report.event(ok, true, dps()));
  ctx_.set_state(RunState::Stopping, "Closing Python...");
  link.close();
  stop_game_link();
  return ok ? "Check finished: no errors." : "Check found a problem in the scripts.";
}

// ---- Train and Watch -----------------------------------------------------------------------------------------

std::string RealtimeRunner::play() {
  brain::init_torch_process();
  brain::set_torch_threads(kActorThreads);
  const bool learn = training();
  period_ = 1.0 / dps();
  tick_timeout_ = std::max(1.0, 5 * period_);
  round_limit_ = std::max<int64_t>(1, std::llround(setup_.settings.round_seconds * dps()));
  pacer_ = Pacer(period_);

  ctx_.set_state(RunState::Starting, "Starting Python and loading your scripts...");
  // Vyrn Link listens before Python loads, so the game can connect meanwhile; it stays across worker restarts.
  if (!start_game_link(false)) return "Stopped: Vyrn Link couldn't start.";
  link_ = std::make_unique<WorkerLink>(ctx_, setup_.python, setup_.data_dir, setup_.project_dir);
  ipc::Frame spec_frame;
  try {
    spec_frame = link_->start(load_options(false));
  } catch (const StopRequested&) {
    return "Stopped before the scripts were loaded.";
  } catch (...) {
    ctx_.emit(describe_failure(std::current_exception(), true).event);
    link_->close();
    return "Stopped: the scripts didn't load.";
  }
  try {
    spec_ = brain::parse_spec(spec_frame.j);
  } catch (const brain::SpecError& e) {
    ctx_.emit(app_error_event("protocol", e.what(), "Vyrn's app and its Python library may not match: reinstall Vyrn.",
                              true));
    link_->close();
    return "Stopped: Python described the project in a way Vyrn doesn't understand.";
  }
  if (spec_.uses_keys && !cfg_.allow_input) {
    ctx_.emit(app_error_event(
        "input_not_allowed", "This project presses keys, so it only plays when you allow that.",
        "Use Train or Watch in the Vyrn app: it counts down and asks you to click your game first. With vyrn-cli, "
        "add --allow-input.",
        true));
    link_->close();
    return "Nothing was run: this project presses keys, and keys weren't allowed.";
  }
  if (game_) {
    game_->set_buttons(spec_frame.j);
  } else if (spec_.raw.value("uses", json::object()).value("game", false)) {
    ctx_.emit(warning_event("vyrn.json", 0,
                            "This project talks to a game with Vyrn Link, but vyrn.json has no \"link\", so no game "
                            "can connect and vyrn.game stays empty. Add \"link\": {\"port\": 7457} (or turn on "
                            "Vyrn Link in Settings)."));
  }
  dcfg_ = brain::default_config(spec_, true);
  for (const auto& w : brain::apply_overrides(dcfg_, setup_.settings.advanced, spec_.obs.kind)) {
    emit_settings_warning(ctx_, w);
  }
  try {
    plan_ = learn ? plan_training(setup_.project_dir, setup_.settings.brain, cfg_.new_brain, spec_, dcfg_.frames)
                  : plan_watching(setup_.project_dir, setup_.settings.brain, cfg_.watch_brain, spec_);
    brain_ = std::make_unique<BrainHost>(setup_.project_dir, spec_, plan_, dcfg_, learn, setup_.seed);
  } catch (const BrainUnavailable& e) {
    ctx_.emit(app_error_event(e.reason, e.what(), e.hint, true));
    link_->close();
    return "Nothing to watch yet.";
  } catch (const std::exception& e) {
    ctx_.emit(app_error_event("brain", std::string("Vyrn couldn't set up the brain: ") + e.what(), "", true));
    link_->close();
    return "Stopped: the brain couldn't be set up.";
  }
  dcfg_ = brain_->config();
  brain_recorded_ = plan_.status == "resumed";
  ojson spec_ev = spec_event(spec_frame.j, cfg_.action, plan_.status, plan_.why, plan_.id, brain_->net_description(),
                             brain_->shape(), setup_.seed);
  if (!learn) spec_ev["checkpoint"] = plan_.which;   // "best" or "latest"
  ctx_.emit(spec_ev);
  if (learn) {
    counters_ = brain_->saved_counters();
    rounds_ = RoundTracker(brain_->saved_best());
    open_run_logs(ctx_, setup_.project_dir, plan_.id, cfg_.action, rounds_csv_, updates_csv_);
  }
  running_ = util::Stopwatch();
  reset_window();
  share_progress();
  if (learn) {
    brain_->share_with_learner();   // the actor plays on snapshots from now on
    start_learner();
  }
  guard_.attach(link_->process());

  Phase phase = first_phase();
  bool truncated = false;   // the last round ended at the round limit: the next one keeps the tick schedule
  for (;;) {
    if (ctx_.controls().stop_requested() || limits_reached(&stop_reason_) || learner_failed_.load()) break;
    try {
      switch (phase) {
        case Phase::Countdown:
          countdown();
          phase = Phase::Lock;
          break;
        case Phase::Lock:
          lock_focus();
          phase = Phase::Restart;
          break;
        case Phase::Restart:
          restart_round();
          phase = Phase::Round;
          truncated = false;
          break;
        case Phase::Round:
          truncated = play_round(truncated) == RoundEnd::Truncated;
          phase = truncated ? Phase::Round : Phase::Restart;
          break;
      }
    } catch (const Interrupt& interrupt) {
      if (!interrupt.apply) continue;   // stop() or a limit: the loop ends at its top
      cut();
      abandon_round();
      ctx_.set_state(RunState::Starting, "Restarting Python with your changes (the brain is kept)...");
      if (respawn()) {
        running_.resume();   // a hold may have paused it
        reset_window();
        phase = first_phase();
      } else if (recover_after_error()) {
        phase = first_phase();
      } else {
        break;
      }
    } catch (const RestartStuck&) {
      const json hooks = spec_.raw.value("hooks", json::object());
      const bool has_restart = hooks.is_object() && hooks.contains("restart");
      const auto where = split_location(hooks.is_object() ? text_or(hooks, has_restart ? "restart" : "game_over") : "");
      ojson e = app_error_event(
          "restart_stuck",
          has_restart ? "restart() didn't clear game_over(): it was still True " +
                            short_number(timings_.restart_give_up_s) + " s later."
                      : "game_over() stayed True for " + short_number(timings_.restart_give_up_s) +
                            " s, and there is no restart() to start a new round.",
          "Make restart() start a new round (for example, press the key that restarts your game), and make "
          "game_over() True only while the game-over screen shows.",
          false);
      e["script"] = where.first;
      e["line"] = where.second;
      ctx_.emit(e);
      close_worker();
      if (!recover_after_error()) break;
      phase = first_phase();
    } catch (...) {
      const bool killed = guard_.killed();
      if (ctx_.controls().stop_requested() && (killed || !link_->running() || !link_->process()->alive())) {
        break;   // stop() froze Python, and it didn't answer in time: it was stopped on purpose
      }
      Failure failure = describe_failure(std::current_exception(), false);
      if (failure.kind == FailureKind::Internal) {   // a problem inside Vyrn: resuming wouldn't help
        failure.event["fatal"] = true;
        ctx_.emit(failure.event);
        stop_reason_ = "Stopped because of a problem inside Vyrn.";
        break;
      }
      if (killed) {
        failure.event = app_error_event(
            "unresponsive",
            "Python didn't answer within " + short_number(timings_.freeze_kill_s) +
                " s after Vyrn released the keys, so Vyrn stopped it to be safe.",
            "Is see(), points() or one of your choices stuck in a long loop? Resume restarts Python.", false);
      }
      ctx_.emit(failure.event);
      cut();
      abandon_round();
      close_worker();
      if (!recover_after_error()) break;
      phase = first_phase();
    }
  }

  // ---- the end: keys up and Python closed first, then the learner saves the brain
  if (learner_failed_.load()) {
    ctx_.emit(app_error_event("internal", "Vyrn's learning stopped: " + learner_error_, "", true));
    stop_reason_ = "Stopped because of a problem inside Vyrn.";
  }
  running_.pause();
  periodic(true);
  const int64_t ticks = session_ticks_.load();
  if (learn && ticks > 0) {
    ctx_.set_state(RunState::Stopping,
                   stop_reason_.empty() ? "Saving the brain..." : stop_reason_ + " Saving the brain...");
  } else {
    ctx_.set_state(RunState::Stopping, stop_reason_.empty() ? "Stopping..." : stop_reason_);
  }
  close_worker();
  stop_game_link();   // the game plays on by itself while the brain is saved
  share_progress();
  stop_learner(learn && ticks > 0);
  std::string detail;
  if (learn) {
    if (ticks == 0) {
      detail = "Stopped before the first tick: nothing to save.";
    } else if (final_saved_) {
      detail = "Stopped after " + std::to_string(ticks) + " ticks; the brain is saved in brains/" + plan_.id + ".";
    } else {
      detail = "Stopped, but the brain couldn't be saved.";
    }
    if (!saved_any_ && plan_.status != "resumed") {
      std::error_code ec;
      fs::remove(brain_->dir(), ec);   // the folder reserved for a new brain stays only if something was saved
    }
  } else {
    detail = "Watched " + std::to_string(rounds_.rounds()) + " rounds (" + std::to_string(ticks) + " ticks)";
    detail += rounds_.rounds() > 0
                  ? ": " + short_number(points_sum_ / static_cast<double>(rounds_.rounds())) + " points per round."
                  : ".";
  }
  rounds_csv_.close();
  updates_csv_.close();
  ctx_.flush_dropped_lines();
  return detail;
}

// ---- Python --------------------------------------------------------------------------------------------------

ipc::Frame RealtimeRunner::call(json req, double timeout_s) {
  guard_.begin(text_or(req, "t"));
  struct End {
    KeyGuard& guard;
    ~End() { guard.end(); }
  } end{guard_};
  return link_->request(std::move(req), timeout_s, kRealtimeGrace);
}

void RealtimeRunner::close_worker() {
  guard_.attach(nullptr);
  locked_ = false;
  if (game_) game_->send_stop();   // the AI stops: the game lets go of its buttons and leaves lockstep
  // "close" releases the keys; then the worker is killed with what it started. A worker that couldn't answer it
  // (killed by the key guard or a timeout, frozen without an answer, dead) may have left keys down, and on macOS
  // and Windows synthetic keys stay down after their process ends: let go of all of them (Linux: no-op, the kernel
  // already did).
  if (link_ && !link_->close() && cfg_.allow_input && spec_.uses_keys) {
    platform::release_all_keys();
    if (test_hooks::keys_released) test_hooks::keys_released();
  }
}

void RealtimeRunner::unfreeze() {
  if (guard_.take_froze() && link_->running()) call(json{{"t", "resume"}}, kQuickTimeout);
}

bool RealtimeRunner::respawn() {
  ++generation_;
  guard_.attach(nullptr);
  locked_ = false;
  if (game_) game_->send_stop();   // Python restarts: the game plays on by itself meanwhile
  try {
    ipc::Frame spec_frame = link_->start(load_options(false));
    guard_.attach(link_->process());
    brain::Spec spec = brain::parse_spec(spec_frame.j);
    std::string why;
    if (brain::compare(brain_->shape(), spec, &why) != brain::Compat::Same) {
      ctx_.emit(app_error_event("brain_mismatch", "This brain can't follow that change: " + why,
                                "Undo the change, or Stop and Train again to start a new brain.", false));
      close_worker();
      return false;
    }
    if (spec.uses_keys && !cfg_.allow_input) {
      ctx_.emit(app_error_event("input_not_allowed", "The project presses keys now, and this run wasn't allowed to.",
                                "Stop, then Train again: Vyrn counts down and asks you to click your game first.",
                                false));
      close_worker();
      return false;
    }
    spec_ = spec;
    if (game_) game_->set_buttons(spec_frame.j);
    ctx_.emit(spec_event(spec_frame.j, cfg_.action, "resumed", why, plan_.id, brain_->net_description(),
                         brain_->shape(), setup_.seed));
    return true;
  } catch (const StopRequested&) {
    return false;
  } catch (const brain::SpecError& e) {
    ctx_.emit(app_error_event("protocol", e.what(), "", false));
  } catch (...) {
    ctx_.emit(describe_failure(std::current_exception(), false).event);
  }
  close_worker();
  return false;
}

bool RealtimeRunner::recover_after_error() {
  running_.pause();
  ctx_.set_state(RunState::Error,
                 "Paused because of the error above. Fix the script, then Resume: Python restarts and the brain "
                 "is kept.");
  ctx_.controls().take_resume();   // only a resume() from now on counts
  for (;;) {
    if (ctx_.controls().stop_requested() || learner_failed_.load()) return false;
    const bool resume = ctx_.controls().take_resume();
    const bool apply = ctx_.controls().take_apply();
    if (resume || apply) {
      ctx_.set_state(RunState::Starting, "Restarting Python (the brain is kept)...");
      if (respawn()) {
        running_.resume();
        reset_window();
        return true;
      }
      if (ctx_.controls().stop_requested()) return false;
      ctx_.set_state(RunState::Error, "Still not working: fix the script, then Resume.");
      continue;
    }
    ctx_.controls().wait_for(0.5);
  }
}

// ---- the state machine ---------------------------------------------------------------------------------------

void RealtimeRunner::check_interrupts() {
  if (ctx_.controls().stop_requested()) throw Interrupt{false};
  if (limits_reached(&stop_reason_)) throw Interrupt{false};
  if (ctx_.controls().take_apply()) throw Interrupt{true};
}

std::string RealtimeRunner::hold_reason(bool unfocused_now) const {
  if (ctx_.controls().paused()) return "paused";
  if (ctx_.controls().hold()) return "user";
  if (unfocused_now) return "unfocused";
  return "";
}

bool RealtimeRunner::sleep_until(double until) {
  for (;;) {
    check_interrupts();
    if (!hold_reason(false).empty()) return false;
    const double left = until - util::now_seconds();
    if (left <= 0) return true;
    ctx_.controls().wait_for(left);   // wakes early for stop(), pause(), set_hold()...
  }
}

void RealtimeRunner::set_play_state(RunState state, const std::string& detail) {
  play_state_ = state;
  play_detail_ = detail;
  ctx_.set_state(state, detail);
}

// ---- Vyrn Link -----------------------------------------------------------------------------------------------

bool RealtimeRunner::start_game_link(bool check) {
  game_ = GameLink::for_project(ctx_, setup_.project_dir, setup_.settings, dps());
  if (!game_) return true;
  try {
    game_->start();
  } catch (const link::LinkError& e) {
    game_.reset();
    if (check) {
      ctx_.emit(warning_event("vyrn.json", 0, std::string(e.what()) + " Check goes on without the game."));
      return true;
    }
    ctx_.emit(app_error_event("link", e.what(), "", true));
    return false;
  }
  ctx_.controls().set_waker([game = game_.get()] { game->interrupt(); });   // Stop, pause... end a wait at once
  return true;
}

void RealtimeRunner::stop_game_link() {
  if (!game_) return;
  ctx_.controls().set_waker(nullptr);
  game_->stop();
}

bool RealtimeRunner::game_ready() const {
  if (!game_->connected()) return false;
  if (game_->lockstep()) return true;   // a game in lockstep waits for Vyrn: no news is fine
  const link::MessagePtr state = game_->latest_state();
  return state && util::now_seconds() - state->received_s <= std::max(kStaleState, 3 * period_);
}

void RealtimeRunner::wait_for_game() {
  // Like a hold: the open move is never learned from, Python lets go of any keys, the clock stops.
  cut();
  ctx_.emit(hold_event(true, "game"));
  running_.pause();
  if (game_->connected()) game_->send_stop();   // there but quiet: it lets go of its buttons
  if (link_ && link_->running()) call(json{{"t", "pause"}}, kQuickTimeout);
  std::string shown;
  for (;;) {
    check_interrupts();
    if (!hold_reason(false).empty() || game_ready()) break;   // a pause or a hold: hold() takes over
    const bool connected = game_->connected();
    const std::string detail =
        !connected ? game_->waiting_text()
        : !game_->latest_state()
            ? std::string("Waiting for the game to send its state...")
            : std::string("Waiting for the game: it stopped sending its state (is it paused, or in a menu?).");
    if (detail != shown) {
      ctx_.set_state(RunState::Holding, detail);
      shown = detail;
    }
    if (!connected) {
      game_->server().wait_connected(kGameWait);
    } else {
      const link::MessagePtr state = game_->latest_state();
      game_->server().wait_state(state ? state->seq : 0, kGameWait);
    }
  }
  ctx_.emit(hold_event(false, "game"));
  running_.resume();
  pacer_.restart();
  reset_window();
  ctx_.set_state(play_state_, play_detail_);
}

void RealtimeRunner::wait_for_learning() {
  // The game waits for Vyrn at every step, so a fast game would outrun the learner and the brain would learn
  // little per tick. Like a simulation, each tick gets its updates (updates_per_tick) before the next one is
  // played. Never for long: a learner with nothing to sample yet, or a stop, pause or hold, ends the wait.
  if (!training() || !learner_.joinable()) return;
  const int64_t ticks = session_ticks_.load();
  if (ticks < dcfg_.learning_starts) return;
  const auto owed = static_cast<int64_t>(
      std::floor(static_cast<double>(ticks - dcfg_.learning_starts + 1) * dcfg_.updates_per_tick));
  const double until = util::now_seconds() + kLearnerWait;
  std::unique_lock<std::mutex> lock(updates_mu_);
  while (updates_done_.load() + kLockstepSlack < owed && !learner_idle_.load() && !learner_failed_.load() &&
         util::now_seconds() < until) {
    if (ctx_.controls().stop_requested() || ctx_.controls().paused() || ctx_.controls().hold()) return;
    updates_cv_.wait_for(lock, std::chrono::milliseconds(20));
  }
}

link::MessagePtr RealtimeRunner::next_state() {
  // Every state is answered exactly once: the next one is newer than the last answered.
  const link::StepResult w = game_->server().wait_state(answered_seq_, kGameWait);
  const double now = util::now_seconds();
  if (w.status == link::Wait::Ok) {
    if (game_slow_) ctx_.set_state(play_state_, play_detail_);
    game_slow_ = false;
    game_wait_since_ = -1;
    return w.state;
  }
  if (w.status == link::Wait::Timeout) {   // the game may be in a menu: keep waiting
    if (game_wait_since_ < 0) game_wait_since_ = now - kGameWait;
    if (!game_slow_ && now - game_wait_since_ >= kSlowGameNote) {
      game_slow_ = true;
      ctx_.set_state(RunState::Running, "Lockstep: waiting for the game's next state (is it paused, or in a menu?)...");
    }
  }
  return nullptr;   // also Interrupted, Disconnected, Stopped: the caller looks at stop, holds and the connection
}

void RealtimeRunner::countdown() {
  // Hands off: the person clicks the game window, which the worker then locks onto (PLAN.md section 4).
  const double total = std::max(0.0, timings_.countdown_s);
  ctx_.set_state(RunState::Countdown, "Hands off: click your game window. The AI starts pressing keys in " +
                                          short_number(std::ceil(total)) + " s.");
  const double end = util::now_seconds() + total;
  for (double left = total; left > 0; left = end - util::now_seconds()) {
    ctx_.emit(countdown_event(std::ceil(left - 1e-6)));
    const double next = end - (std::ceil(left - 1e-6) - 1.0);   // the next whole second left
    while (util::now_seconds() < next) {
      check_interrupts();
      ctx_.controls().wait_for(next - util::now_seconds());
    }
  }
  ctx_.emit(countdown_event(0));
  // The app holds while Vyrn is the active window: wait until the person clicks their game (or resumes).
  std::string reason = hold_reason(false);
  if (reason.empty()) return;
  ctx_.emit(hold_event(true, reason));
  ctx_.set_state(reason == "paused" ? RunState::Paused : RunState::Holding,
                 reason == "paused" ? "Paused." : "Waiting for you to click your game window...");
  for (std::string now = reason; !now.empty(); now = hold_reason(false)) {
    reason = now;
    check_interrupts();
    ctx_.controls().wait_for(timings_.hold_poll_s);
  }
  ctx_.controls().take_resume();
  ctx_.emit(hold_event(false, reason));
}

void RealtimeRunner::lock_focus() {
  check_interrupts();
  ctx_.set_state(RunState::Starting, "Locking onto the active window...");
  ipc::Frame r = call(json{{"t", "lock_focus"}}, kLoadTimeout);
  locked_ = true;
  std::string title = text_or(r.j, "title"), app_id = text_or(r.j, "app_id");
  ctx_.app_log("The AI plays in the window \"" + title + "\"" + (app_id.empty() ? "" : " (" + app_id + ")") +
               ". Click Vyrn, press Stop or press Ctrl+Alt+Esc to stop it.");
}

void RealtimeRunner::restart_round() {
  while (!try_restart_round()) {
  }
}

bool RealtimeRunner::try_restart_round() {
  check_interrupts();
  if (const std::string why = hold_reason(false); !why.empty()) {
    hold(why);
    return false;
  }
  if (game_ && !game_ready()) {   // restart() talks to the game: wait until it is there and playing
    wait_for_game();
    return false;
  }
  unfreeze();
  set_play_state(RunState::Restarting, "Starting a new round: restart()...");
  // Lockstep: the game waits for an input at every step. Let it play on by itself so it can restart; the first
  // input of the round puts it back into lockstep.
  if (game_ && game_->lockstep()) game_->send_stop();
  // restart() may press keys: only while the game window is in front.
  auto restart = [&] {
    if (locked_ && unfocused(call(json{{"t", "status"}}, kQuickTimeout).j)) {
      hold("unfocused");
      return false;
    }
    json req = {{"t", "restart"}};
    if (game_) game_->attach(req, game_->latest_state(), true);
    ipc::Frame r = call(std::move(req), kLoadTimeout);
    if (game_) game_->forward_sends(r.j);   // e.g. vyrn.send("restart")
    pacer_.restart();
    return true;
  };
  if (!restart()) return false;
  const int64_t every = std::max<int64_t>(1, std::llround(timings_.restart_every_s / period_));
  const int64_t give_up = std::max<int64_t>(1, std::llround(timings_.restart_give_up_s / period_));
  int64_t since_restart = 0, total = 0;
  bool fresh = true;   // the first poll after restart() comes at once
  link::MessagePtr polled = game_ ? game_->latest_state() : nullptr;
  double polled_at = util::now_seconds();
  bool quiet_said = false;
  for (;;) {
    const PaceStep step = pacer_.next(util::now_seconds());
    if (!sleep_until(step.due)) return false;   // a pause or hold: start over once it is over
    const int64_t passed = fresh ? 0 : 1 + step.skipped;
    if (game_) {
      if (!game_->connected()) return false;    // the top waits for the game again
      // Only ticks with news from the game count: a game that sends nothing (a menu, a loading screen) is waited
      // for, without restart() again and without giving up.
      link::MessagePtr state = game_->latest_state();
      if (!fresh && state == polled) {
        if (!quiet_said && util::now_seconds() - polled_at >= kSlowGameNote) {
          quiet_said = true;
          ctx_.set_state(RunState::Restarting,
                         "Waiting for the game: it hasn't sent its state since restart() (is it in a menu?)...");
        }
        periodic(false);
        continue;
      }
      polled = std::move(state);
      polled_at = util::now_seconds();
    }
    fresh = false;
    since_restart += passed;
    total += passed;
    if (total >= give_up) throw RestartStuck{};
    if (since_restart >= every) {
      if (!restart()) return false;
      since_restart = 0;
      fresh = true;
      continue;
    }
    json req = {{"t", "check_over"}};
    if (game_) game_->attach(req, polled, true);
    ipc::Frame r = call(std::move(req), kStepTimeout);
    if (game_) game_->forward_sends(r.j);
    if (!flag(r.j, "game_over")) return true;
    periodic(false);
  }
}

RealtimeRunner::RoundEnd RealtimeRunner::play_round(bool on_schedule) {
  round_ticks_ = 0;
  round_points_ = 0;
  prev_ = -1;
  bool first = true;   // the next observe starts the round
  const bool lockstep = game_ && game_->lockstep();   // the game sets the pace: one tick per state it sends
  if (!on_schedule) pacer_.restart();   // else the first observe waits for the next tick (no burst)
  set_play_state(RunState::Running, training() ? "Training: the AI plays your game and learns."
                                               : "Watching: the AI plays without learning.");
  game_wait_since_ = -1;
  game_slow_ = false;
  for (;;) {
    check_interrupts();
    if (const std::string why = hold_reason(false); !why.empty()) {
      hold(why);
      continue;
    }
    if (game_ && !game_ready()) {
      wait_for_game();
      continue;
    }
    unfreeze();
    link::MessagePtr state;   // Vyrn Link: the game's state this tick answers
    double t0 = 0;
    if (lockstep) {
      wait_for_learning();
      state = next_state();
      if (!state) continue;   // none yet: look at stop, holds and the connection again, then wait on
      t0 = util::now_seconds();
    } else {
      const PaceStep step = pacer_.next(util::now_seconds());
      if (!sleep_until(step.due)) continue;   // a pause or hold came: the top of the loop holds
      // Behind by more than late_cut_periods: the last choice was held much too long. Skipped ticks count as
      // overruns; the lateness reported is when this tick really starts, after its deadline.
      const double behind_s = static_cast<double>(step.skipped) * period_ + step.late_s;
      if (behind_s > timings_.late_cut_periods * period_ && segment_open_) late_cut();
      t0 = util::now_seconds();
      window_.late_ms.push_back(std::max(0.0, t0 - step.due) * 1000.0);
      if (game_ && !(state = game_->latest_state())) continue;   // it left just now: the top waits for it
    }
    if (state && state->connection != game_connection_) {
      cut();   // another connection: the open move doesn't continue in it
      game_connection_ = state->connection;
    }
    json observe = {{"t", "observe"}, {"first", first}};
    if (game_) game_->attach(observe, state, true);
    ipc::Frame r = call(std::move(observe), tick_timeout_);
    if (game_) game_->forward_sends(r.j);
    last_state_ = state;
    const double t1 = util::now_seconds();
    show_.update(r);
    emit_result_warnings(ctx_, r.j);
    const bool round_start = first;
    first = false;   // Python counts this round from here
    // Seen while the game wasn't in front: the open move is never learned from and the brain doesn't get
    // this observation, but its points and a game over still count for the round.
    const bool away = unfocused(r.j);
    if (away) cut();
    ObsView obs;
    if (!read_obs(r, "obs", obs)) throw ipc::ProtocolError("the reply to observe has no observation");
    if (!round_start) {
      const double points = number_or(r.j, "points", 0.0);
      ++round_ticks_;
      round_points_ += points;
      if (prev_ >= 0) record(prev_, points);
      prev_ = -1;
      if (flag(r.j, "game_over")) {   // Python has let go of the keys
        end_segment(obs, true);
        finish_round(true, false);
        if (lockstep) {
          game_->send_stop();   // the game plays on by itself so it can restart
        } else if (game_) {
          game_->release(state);   // and the game lets go of its buttons
        }
        return RoundEnd::GameOver;
      }
      if (round_ticks_ >= round_limit_) {   // the next round starts without restart()
        end_segment(obs, false);
        finish_round(false, true);
        return RoundEnd::Truncated;
      }
    }
    if (away) {
      hold("unfocused");
      continue;
    }
    if (segment_open_) {
      brain_->continue_segment(obs);
    } else {
      start_segment(obs);
    }
    const bool want_live = t1 - last_live_ >= kLiveEvery;
    std::vector<float> probs;
    const int action = brain_->act(epsilon(), want_live ? &probs : nullptr);
    const double t2 = util::now_seconds();
    ipc::Frame done = call(json{{"t", "act"}, {"a", action}}, tick_timeout_);
    if (game_) {
      game_->forward_sends(done.j);    // vyrn.send() messages first, then the tick's input
      game_->send_input(state, done.j);
      if (state) answered_seq_ = state->seq;
    }
    const double t3 = util::now_seconds();
    prev_ = action;

    // Telemetry: the user's code (as the worker measured it), the brain, and the rest of the round trips.
    double worker_ms = 0;
    for (const json* reply : {&r.j, &done.j}) {
      auto ms = reply->find("ms");
      if (ms == reply->end() || !ms->is_object()) continue;
      const double see = number_or(*ms, "see", 0), pts = number_or(*ms, "points", 0) + number_or(*ms, "game_over", 0);
      const double respond = number_or(*ms, "respond", 0);
      window_.see += see;
      window_.points += pts;
      window_.respond += respond;
      worker_ms += see + pts + respond;
    }
    ++window_.ticks;
    window_.brain += (t2 - t1) * 1000.0;
    window_.ipc += std::max(0.0, ((t1 - t0) + (t3 - t2)) * 1000.0 - worker_ms);
    if (want_live) publish_live(action, std::move(probs));
    periodic(false);
  }
}

void RealtimeRunner::hold(std::string reason) {
  cut();   // the open transition is never sampled; the next observation starts a segment
  ctx_.emit(hold_event(true, reason));
  auto show = [&] {
    if (reason == "paused") {
      ctx_.set_state(RunState::Paused, "Paused: the keys are released.");
    } else if (reason == "user") {
      ctx_.set_state(RunState::Holding, "On hold while you use Vyrn: the keys are released. Click your game to "
                                        "let the AI continue.");
    } else {
      ctx_.set_state(RunState::Holding, "On hold: your game window isn't the active window, so the keys are "
                                        "released. Click your game to let the AI continue.");
    }
  };
  show();
  running_.pause();
  if (game_) game_->send_stop();               // the game lets go of its buttons (and leaves lockstep)
  call(json{{"t", "pause"}}, kQuickTimeout);   // keys up: Python can't see a hold by the app or a pause
  for (;;) {
    check_interrupts();
    ctx_.controls().wait_for(timings_.hold_poll_s);   // wakes early when the person resumes or clicks the game
    check_interrupts();
    const std::string now = hold_reason(unfocused(call(json{{"t", "status"}}, kQuickTimeout).j));
    if (now.empty()) break;
    if (now != reason) {
      reason = now;
      ctx_.emit(hold_event(true, reason));
      show();
    }
  }
  ctx_.controls().take_resume();   // this resume() ended the pause
  unfreeze();
  ctx_.emit(hold_event(false, reason));
  running_.resume();
  pacer_.restart();
  reset_window();
  ctx_.set_state(play_state_, play_detail_);
}

void RealtimeRunner::late_cut() {
  // This tick comes much too late: the last choice was held for too long, so its move doesn't teach anything.
  cut();
  ctx_.emit(hold_event(true, "late"));
  call(json{{"t", "pause"}}, kQuickTimeout);   // keys up; the next act presses again
  ctx_.emit(hold_event(false, "late"));
}

// ---- the brain side ------------------------------------------------------------------------------------------

void RealtimeRunner::start_segment(const ObsView& obs) {
  brain_->start_segment(obs);
  segment_open_ = true;
  trace("start");
}

void RealtimeRunner::end_segment(const ObsView& obs, bool terminal) {
  if (!segment_open_) return;   // nothing open: the round ended right after a hold
  brain_->end_segment(&obs, terminal);
  segment_open_ = false;
  trace(terminal ? "end terminal" : "end truncated");
}

void RealtimeRunner::cut() {
  prev_ = -1;
  if (!segment_open_) return;
  brain_->end_segment(nullptr, false);   // ends at the last observation; its open move is dropped
  segment_open_ = false;
  trace("cut");
}

void RealtimeRunner::record(int action, double points) {
  brain_->record(action, static_cast<float>(points));
  trace("record " + std::to_string(action) + " " + short_number(points));
  ++counters_.ticks;
  session_ticks_.fetch_add(1);
  learner_cv_.notify_one();
}

void RealtimeRunner::finish_round(bool game_over, bool truncated) {
  rounds_.add(round_points_);
  points_sum_ += round_points_;
  ++counters_.rounds;
  RoundRecord rec;
  rec.round = training() ? counters_.rounds : rounds_.rounds();
  rec.points = round_points_;
  rec.ticks = round_ticks_;
  rec.truncated = truncated && !game_over;
  rec.game_over = game_over;
  rec.mean20 = rounds_.mean20();
  rec.best_mean20 = rounds_.best_mean20();
  rec.total_ticks = counters_.ticks;
  rec.explore = epsilon();
  rec.seconds = running_.seconds();
  ctx_.emit(round_event(rec));
  if (training()) {
    write_round_row(rounds_csv_, rec);
    if (rounds_.improved()) brain_->keep_best(counters_now(), *rounds_.best_mean20());
  }
  round_ticks_ = 0;
  round_points_ = 0;
  share_progress();
}

void RealtimeRunner::abandon_round() {
  // Python restarts: the round in progress doesn't count (like a simulation's).
  round_ticks_ = 0;
  round_points_ = 0;
  prev_ = -1;
}

float RealtimeRunner::epsilon() const {
  return training() ? brain::epsilon_at(dcfg_, counters_.ticks) : dcfg_.eps_watch;
}

Counters RealtimeRunner::counters_now() const {
  Counters c = counters_;
  c.seconds = brain_->saved_counters().seconds + running_.seconds();
  return c;
}

bool RealtimeRunner::limits_reached(std::string* why) const {
  if (cfg_.max_ticks >= 0 && session_ticks_.load() >= cfg_.max_ticks) {
    *why = "Reached " + std::to_string(cfg_.max_ticks) + " ticks.";
    return true;
  }
  if (cfg_.max_seconds >= 0 && running_.seconds() >= cfg_.max_seconds) {
    *why = "Reached the time limit.";
    return true;
  }
  return false;
}

void RealtimeRunner::reset_window() {
  window_ = Window{};
  window_.start = util::now_seconds();
  window_.skipped_before = pacer_.skipped();
}

void RealtimeRunner::periodic(bool force) {
  const double now = util::now_seconds();
  const double span = now - window_.start;
  if ((force && window_.ticks > 0) || span >= kTelemetryEvery) {
    const double ticks = static_cast<double>(std::max<int64_t>(window_.ticks, 1));
    TimingRecord t;
    t.see_ms = window_.see / ticks;
    t.points_ms = window_.points / ticks;
    t.respond_ms = window_.respond / ticks;
    t.brain_ms = window_.brain / ticks;
    t.ipc_ms = window_.ipc / ticks;
    t.ticks_per_s = span > 0 ? static_cast<double>(window_.ticks) / span : 0.0;
    t.late_p99_ms = percentile(window_.late_ms, 99);
    t.overruns = pacer_.skipped() - window_.skipped_before;
    t.explore = epsilon();
    t.total_ticks = counters_.ticks;
    ctx_.emit(timing_event(t));
    ticks_per_s_ = t.ticks_per_s;
    if (training()) rounds_csv_.flush();
    ctx_.flush_dropped_lines();
    reset_window();
    share_progress();
  }
}

void RealtimeRunner::publish_live(int chosen, std::vector<float> probs) {
  LiveView v;
  brain_->fill_live(v);
  v.choices = spec_.labels;
  v.probs = std::move(probs);
  v.chosen = chosen;
  show_.fill(v);
  v.round = (training() ? counters_.rounds : rounds_.rounds()) + 1;
  v.tick = round_ticks_;
  v.total_ticks = counters_.ticks;
  if (rounds_.best_mean20()) {
    v.best_mean = *rounds_.best_mean20();
    v.has_best = true;
  }
  v.explore = epsilon();
  v.ticks_per_s = ticks_per_s_;
  ctx_.publish(std::move(v));
  last_live_ = util::now_seconds();
}

void RealtimeRunner::share_progress() {
  if (!brain_) return;
  Progress p;
  p.counters = counters_now();
  p.best_mean20 = rounds_.best_mean20();
  p.session_seconds = running_.seconds();
  p.explore = epsilon();
  p.ticks_per_s = ticks_per_s_;
  std::lock_guard<std::mutex> lock(progress_mu_);
  progress_ = p;
}

Progress RealtimeRunner::progress() const {
  std::lock_guard<std::mutex> lock(progress_mu_);
  return progress_;
}

// ---- the learner thread --------------------------------------------------------------------------------------

void RealtimeRunner::start_learner() {
  learner_quit_ = false;
  final_save_ = false;
  learner_ = std::thread([this] { learner_main(); });
}

void RealtimeRunner::stop_learner(bool final_save) {
  if (!learner_.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(learner_mu_);
    learner_quit_ = true;
    final_save_ = final_save;
  }
  learner_cv_.notify_all();
  learner_.join();
}

void RealtimeRunner::learner_main() {
  // Below the actor (Linux: nice is per thread), then the torch threads; threads torch starts here inherit both.
  platform::lower_thread_priority(kLearnerNice);
  try {
    brain::set_torch_threads(kLearnerThreads);
    const brain::DqnConfig& cfg = brain_->config();
    int64_t done = 0;   // updates this run
    int since_snapshot = 0;
    double last_report = util::now_seconds(), last_latest = last_report, last_best = -1e9;
    bool final_save = false;
    for (;;) {
      {
        std::lock_guard<std::mutex> lock(learner_mu_);
        if (learner_quit_) {
          final_save = final_save_;
          break;
        }
      }
      const double now = util::now_seconds();
      if (now - last_report >= kTelemetryEvery) {
        learner_report(now - last_report);
        last_report = now;
      }
      // Saves happen here, between updates: this thread owns the network.
      if ((brain_->best_pending() && now - last_best >= kSaveBestEvery) || now - last_latest >= kSaveLatestEvery) {
        const Progress p = progress();
        try {
          if (brain_->best_pending() && now - last_best >= kSaveBestEvery) {
            write_best_now(p);
            last_best = now;
          }
          if (now - last_latest >= kSaveLatestEvery) {
            save_latest_now(p);
            last_latest = now;
          }
        } catch (const std::exception& e) {
          ctx_.emit(warning_event("", 0, std::string("Vyrn couldn't save the brain (it tries again later): ") +
                                             e.what()));
          last_latest = now;
          last_best = now;
        }
      }
      // At most updates_per_tick updates per tick played (on average), once the warm-up is over.
      const int64_t ticks = session_ticks_.load();
      const double owed = ticks >= cfg.learning_starts
                              ? std::floor(static_cast<double>(ticks - cfg.learning_starts + 1) * cfg.updates_per_tick)
                              : 0.0;
      if (static_cast<double>(done) < owed && brain_->learn_once()) {
        ++done;
        learner_idle_.store(false);
        updates_done_.store(done);
        updates_cv_.notify_all();
        if (++since_snapshot >= kSnapshotEvery) {
          brain_->publish_snapshot();
          since_snapshot = 0;
        }
        continue;
      }
      if (static_cast<double>(done) < owed) learner_idle_.store(true);   // owed, but nothing to sample yet
      std::unique_lock<std::mutex> lock(learner_mu_);
      learner_cv_.wait_for(lock, std::chrono::milliseconds(50),
                           [&] { return learner_quit_ || session_ticks_.load() != ticks; });
    }
    learner_report(std::max(1e-3, util::now_seconds() - last_report));
    if (!final_save) return;
    const Progress p = progress();
    try {
      write_best_now(p);
      save_latest_now(p);
      final_saved_ = true;
    } catch (const std::exception& e) {
      ctx_.emit(app_error_event("save_failed", std::string("Vyrn couldn't save the brain: ") + e.what(),
                                "Check that the disk isn't full and that the project folder can be written.", true));
    }
  } catch (const std::exception& e) {
    learner_error_ = e.what();
    learner_failed_.store(true);
  }
}

void RealtimeRunner::learner_report(double span) {
  int64_t n = 0;
  double q_sum = 0;
  const double loss_sum = brain_->take_loss_sum(&n, &q_sum);
  if (n <= 0) return;
  const Progress p = progress();
  UpdateRecord u;
  u.updates = brain_->updates();
  u.loss = loss_sum / static_cast<double>(n);
  u.q = q_sum / static_cast<double>(n);
  u.explore = p.explore;
  u.lr = brain_->config().lr;
  u.updates_per_s = span > 0 ? static_cast<double>(n) / span : 0.0;
  ctx_.emit(update_event(u));
  write_update_row(updates_csv_, p.session_seconds, p.counters.ticks, u, p.ticks_per_s);
  updates_csv_.flush();
}

void RealtimeRunner::save_latest_now(const Progress& p) {
  const std::string path = brain_->save_latest(p.counters, p.best_mean20);
  saved_any_ = true;
  note_brain_in_settings();
  ctx_.emit(checkpoint_event("latest", path, p.best_mean20));
}

void RealtimeRunner::write_best_now(const Progress& p) {
  const std::string path = brain_->write_best();
  if (path.empty()) return;
  saved_any_ = true;
  note_brain_in_settings();
  ctx_.emit(checkpoint_event("best", path, p.best_mean20));
}

void RealtimeRunner::note_brain_in_settings() {
  if (brain_recorded_) return;
  brain_recorded_ = true;
  util::ordered_json change = util::ordered_json::object();
  change["brain"] = plan_.id;
  std::string why;
  if (!util::update_settings(setup_.project_dir, change, &why)) {
    ctx_.emit(warning_event("vyrn.json", 0, "Vyrn couldn't note the new brain " + plan_.id + " in vyrn.json (" +
                                                why + "). It is saved in brains/" + plan_.id + "."));
  }
}

}  // namespace

std::unique_ptr<Runner> make_realtime_runner(RunContext& ctx, RunSetup setup) {
  return std::make_unique<RealtimeRunner>(ctx, std::move(setup));
}

}  // namespace vyrn::run
