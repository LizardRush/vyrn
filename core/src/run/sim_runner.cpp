// SimRunner: projects with a world.py (docs/dev/core-internals.md section 4, PLAN.md section 3).
//
// One worker and one brain thread (the session's run thread) that does everything: reset -> loop act -> step
// -> replay -> updates_per_tick updates. Train is unpaced; Watch plays one copy at decisions/s x speed with the
// chosen brain, eps_watch, no learning and frozen number statistics; Check plays check_ticks random steps.
#include "brain/dqn.h"
#include "brain/threads.h"
#include "run/brain_host.h"
#include "run/check.h"
#include "run/pacing.h"
#include "run/rounds.h"
#include "run/runner.h"
#include "util/clock.h"
#include "util/csv.h"
#include "util/files.h"

#include <filesystem>
#include <random>

namespace vyrn::run {

namespace {

namespace fs = std::filesystem;

constexpr int kTorchThreads = 3;             // the brain thread does all torch work for a simulation
constexpr double kLiveEvery = 0.1;           // live view: at most 10 refreshes a second
constexpr double kTelemetryEvery = 2.0;      // timing and update events, CSV flushes
constexpr double kSaveLatestEvery = 600.0;   // every 10 minutes
constexpr double kSaveBestEvery = 60.0;      // the best brain is written at most once a minute

double number_or(const json& j, const char* key, double fallback) {
  auto it = j.find(key);
  return it != j.end() && it->is_number() ? it->get<double>() : fallback;
}

bool flag(const json& j, const char* key) {
  auto it = j.find(key);
  return it != j.end() && it->is_boolean() && it->get<bool>();
}

// Sums for one telemetry window (about 2 seconds).
struct Window {
  double start = 0;
  int64_t ticks = 0;
  double see = 0, points = 0, respond = 0, step = 0, brain = 0, ipc = 0;
  std::vector<double> late_ms;
  int64_t skipped_before = 0;
};

class SimRunner final : public Runner {
 public:
  SimRunner(RunContext& ctx, RunSetup setup)
      : ctx_(ctx), cfg_(ctx.config()), setup_(std::move(setup)), rng_(setup_.seed) {}

  std::string run() override;

 private:
  bool training() const { return cfg_.action == RunConfig::Action::Train; }
  LoadOptions load_options(bool check) const;
  std::string check(const ipc::Frame& spec_frame, const brain::Spec& spec, const brain::DqnConfig& dcfg);
  std::string play(const ipc::Frame& spec_frame, const brain::Spec& spec, const brain::DqnConfig& dcfg);

  // One tick: act, step, record, learn. Throws worker failures.
  void reset();
  void tick();
  void finish_round(bool game_over, bool truncated);
  void periodic(bool force_telemetry);
  void publish_live(int chosen, std::vector<float> probs);
  bool limits_reached(std::string* why) const;
  bool wait_while_paused();
  bool recover_after_error();   // Error state until resume/apply (respawn) or stop. False = stop.
  bool respawn();
  float epsilon() const;
  Counters counters_now() const;
  void save_latest();
  void write_best();
  void note_brain_in_settings();
  void open_logs();

  RunContext& ctx_;
  const RunConfig& cfg_;
  RunSetup setup_;
  std::mt19937_64 rng_;
  std::unique_ptr<WorkerLink> link_;
  int generation_ = 0;   // worker respawns (each gets its own seed)

  brain::Spec spec_;
  brain::DqnConfig dcfg_;
  std::unique_ptr<BrainHost> brain_;
  BrainPlan plan_;
  bool brain_recorded_ = false;   // vyrn.json names this brain
  bool saved_any_ = false;

  RoundTracker rounds_;
  Counters counters_;          // this brain's totals (training) or this Watch's (watching)
  int64_t session_ticks_ = 0;
  int64_t round_ticks_ = 0;
  double round_points_ = 0;
  double points_sum_ = 0;      // all rounds of this session
  util::Stopwatch running_;
  Pacer pacer_{0};
  ShowValues show_;
  Window window_;
  double ticks_per_s_ = 0;
  double last_live_ = -1e9, last_latest_ = 0, last_best_write_ = -1e9;
  util::CsvWriter rounds_csv_, updates_csv_;
};

LoadOptions SimRunner::load_options(bool check) const {
  LoadOptions o;
  o.check = check;
  o.allow_input = false;   // simulations never press keys
  o.seed = setup_.seed + static_cast<uint64_t>(generation_);
  o.round_limit = setup_.settings.round_ticks;
  o.decisions_per_second = setup_.settings.decisions_per_second;
  o.focus_helper = cfg_.focus_helper;
  return o;
}

std::string SimRunner::run() {
  brain::init_torch_process();
  brain::set_torch_threads(kTorchThreads);
  const bool checking = cfg_.action == RunConfig::Action::Check;

  ctx_.set_state(RunState::Starting, "Starting Python and loading your scripts...");
  link_ = std::make_unique<WorkerLink>(ctx_, setup_.python, setup_.data_dir, setup_.project_dir);
  ipc::Frame spec_frame;
  try {
    spec_frame = link_->start(load_options(checking));
  } catch (const StopRequested&) {
    return "Stopped before the scripts were loaded.";
  } catch (...) {
    ctx_.emit(describe_failure(std::current_exception(), true).event);
    if (checking) ctx_.emit(CheckReport(json::object()).event(false, false, setup_.settings.decisions_per_second));
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
  dcfg_ = brain::default_config(spec_, false);
  for (const auto& w : brain::apply_overrides(dcfg_, setup_.settings.advanced, spec_.obs.kind)) {
    emit_settings_warning(ctx_, w);
  }
  return checking ? check(spec_frame, spec_, dcfg_) : play(spec_frame, spec_, dcfg_);
}

// ---- Check -------------------------------------------------------------------------------------------------

std::string SimRunner::check(const ipc::Frame& spec_frame, const brain::Spec& spec, const brain::DqnConfig& dcfg) {
  const BrainPlan plan = plan_check(setup_.project_dir, setup_.settings.brain, spec, dcfg.frames);
  ctx_.emit(spec_event(spec_frame.j, cfg_.action, plan.status, plan.why, plan.id,
                       brain::describe(brain::net_config_for(plan.shape)), plan.shape, setup_.seed));
  if (plan.status == "incompatible") ctx_.emit(warning_event("", 0, plan.why));

  ObsView obs;
  if (read_obs(spec_frame, "preview", obs)) ctx_.publish(check_live_view(obs, spec));

  const int steps = std::max(0, cfg_.check_ticks);
  ctx_.set_state(RunState::Running, "Checking: " + std::to_string(steps) + " random steps...");
  CheckReport report(spec_frame.j);
  bool ok = true;
  std::uniform_int_distribution<int> any_choice(0, static_cast<int>(spec.choices.size()) - 1);
  try {
    ipc::Frame r = link_->request(json{{"t", "reset"}}, kLoadTimeout, kSimGrace);
    report.add_result(r);
    for (int i = 0; i < steps && !ctx_.controls().stop_requested(); ++i) {
      for (const auto& w : report.take_new_warnings()) ctx_.emit(warning_event(w.script, w.line, w.message));
      if (read_obs(r, "obs", obs)) ctx_.publish(check_live_view(obs, spec));
      r = link_->request(json{{"t", "step"}, {"a", any_choice(rng_)}}, kStepTimeout, kSimGrace);
      report.count_step();
      report.add_result(r);
    }
    if (read_obs(r, "obs", obs)) ctx_.publish(check_live_view(obs, spec));
  } catch (...) {
    ctx_.emit(describe_failure(std::current_exception(), true).event);
    ok = false;
  }
  for (const auto& w : report.take_new_warnings()) ctx_.emit(warning_event(w.script, w.line, w.message));
  if (ok) {
    for (const auto& w : report.findings(false, setup_.settings.decisions_per_second)) {
      ctx_.emit(warning_event(w.script, w.line, w.message));
    }
  }
  ctx_.emit(report.event(ok, false, setup_.settings.decisions_per_second));
  ctx_.set_state(RunState::Stopping, "Closing Python...");
  link_->close();
  return ok ? "Check finished: no errors." : "Check found a problem in the scripts.";
}

// ---- Train and Watch ----------------------------------------------------------------------------------------

std::string SimRunner::play(const ipc::Frame& spec_frame, const brain::Spec& spec, const brain::DqnConfig& dcfg) {
  const bool learn = training();
  try {
    plan_ = learn ? plan_training(setup_.project_dir, setup_.settings.brain, cfg_.new_brain, spec, dcfg.frames)
                  : plan_watching(setup_.project_dir, setup_.settings.brain, cfg_.watch_brain, spec);
    brain_ = std::make_unique<BrainHost>(setup_.project_dir, spec, plan_, dcfg, learn, setup_.seed);
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
    open_logs();
  }
  const double speed = cfg_.watch_speed;
  if (!learn && speed > 0) pacer_ = Pacer(1.0 / (setup_.settings.decisions_per_second * speed));

  const double now = util::now_seconds();
  window_ = Window{};
  window_.start = now;
  last_latest_ = now;
  running_ = util::Stopwatch();
  ctx_.set_state(RunState::Running, learn ? "Training: the AI plays as fast as it can and learns."
                                          : "Watching: the AI plays without learning.");

  std::string stop_reason;
  bool need_reset = true;
  for (;;) {
    if (ctx_.controls().stop_requested() || limits_reached(&stop_reason)) break;
    if (ctx_.controls().paused()) {
      if (!wait_while_paused()) break;
      continue;
    }
    if (ctx_.controls().take_apply()) {
      ctx_.set_state(RunState::Starting, "Restarting Python with your changes (the brain is kept)...");
      brain_->end_segment(nullptr, false);   // the round in progress ends here
      need_reset = true;
      if (respawn()) {
        ctx_.set_state(RunState::Running, learn ? "Training." : "Watching.");
      } else if (!recover_after_error()) {
        break;
      }
    }
    try {
      if (need_reset) {
        reset();
        need_reset = false;
      }
      tick();
    } catch (...) {
      Failure failure = describe_failure(std::current_exception(), false);
      if (failure.kind == FailureKind::Internal) {   // a problem inside Vyrn: resuming wouldn't help
        failure.event["fatal"] = true;
        ctx_.emit(failure.event);
        stop_reason = "Stopped because of a problem inside Vyrn.";
        break;
      }
      ctx_.emit(failure.event);
      brain_->end_segment(nullptr, false);   // the segment ends at the last observation; an open move is dropped
      link_->close();
      if (!recover_after_error()) break;
      need_reset = true;
    }
    periodic(false);
  }

  // ---- the end: telemetry, saves, Python
  running_.pause();
  periodic(true);
  std::string detail;
  if (learn && session_ticks_ > 0) {
    ctx_.set_state(RunState::Stopping, stop_reason.empty() ? "Saving the brain..." : stop_reason + " Saving the brain...");
    try {
      write_best();
      save_latest();
      detail = "Stopped after " + std::to_string(session_ticks_) + " ticks; the brain is saved in brains/" +
               plan_.id + ".";
    } catch (const std::exception& e) {
      ctx_.emit(app_error_event("save_failed", std::string("Vyrn couldn't save the brain: ") + e.what(),
                                "Check that the disk isn't full and that the project folder can be written.", true));
      detail = "Stopped, but the brain couldn't be saved.";
    }
  } else {
    ctx_.set_state(RunState::Stopping, stop_reason.empty() ? "Stopping..." : stop_reason);
    detail = "Stopped before the first tick: nothing to save.";
    if (!learn) {
      detail = "Watched " + std::to_string(rounds_.rounds()) + " rounds (" + std::to_string(session_ticks_) + " ticks)";
      detail += rounds_.rounds() > 0
                    ? ": " + short_number(points_sum_ / static_cast<double>(rounds_.rounds())) + " points per round."
                    : ".";
    }
  }
  if (learn && !saved_any_ && plan_.status != "resumed") {
    std::error_code ec;
    fs::remove(brain_->dir(), ec);   // the folder reserved for a new brain stays only if something was saved
  }
  rounds_csv_.close();
  updates_csv_.close();
  link_->close();
  ctx_.flush_dropped_lines();
  return detail;
}

void SimRunner::open_logs() {
  open_run_logs(ctx_, setup_.project_dir, plan_.id, cfg_.action, rounds_csv_, updates_csv_);
}

bool SimRunner::limits_reached(std::string* why) const {
  if (cfg_.max_ticks >= 0 && session_ticks_ >= cfg_.max_ticks) {
    *why = "Reached " + std::to_string(cfg_.max_ticks) + " ticks.";
    return true;
  }
  if (cfg_.max_seconds >= 0 && running_.seconds() >= cfg_.max_seconds) {
    *why = "Reached the time limit.";
    return true;
  }
  return false;
}

float SimRunner::epsilon() const {
  return training() ? brain::epsilon_at(dcfg_, counters_.ticks) : dcfg_.eps_watch;
}

Counters SimRunner::counters_now() const {
  Counters c = counters_;
  c.seconds = brain_->saved_counters().seconds + running_.seconds();
  return c;
}

void SimRunner::reset() {
  ipc::Frame r = link_->request(json{{"t", "reset"}}, kLoadTimeout, kSimGrace);
  show_.update(r);
  emit_result_warnings(ctx_, r.j);
  ObsView obs;
  if (!read_obs(r, "obs", obs)) throw ipc::ProtocolError("the reply to reset has no observation");
  brain_->start_segment(obs);
  round_ticks_ = 0;
  round_points_ = 0;
}

void SimRunner::tick() {
  if (pacer_.paced()) {
    const PaceStep step = pacer_.next(util::now_seconds());
    const double due = util::now_seconds() + step.wait_s;
    for (double left = step.wait_s; left > 0; left = due - util::now_seconds()) {
      ctx_.controls().wait_for(left);   // wakes early for stop() / pause()
      if (ctx_.controls().stop_requested() || ctx_.controls().paused()) return;
    }
    window_.late_ms.push_back(step.late_s * 1000.0);
  }
  const double t0 = util::now_seconds();
  const bool want_live = t0 - last_live_ >= kLiveEvery;
  const float eps = epsilon();
  std::vector<float> probs;
  const int action = brain_->act(eps, want_live ? &probs : nullptr);
  const double t1 = util::now_seconds();
  ipc::Frame r = link_->request(json{{"t", "step"}, {"a", action}}, kStepTimeout, kSimGrace);
  const double t2 = util::now_seconds();

  show_.update(r);
  emit_result_warnings(ctx_, r.j);
  const double points = number_or(r.j, "points", 0.0);
  brain_->record(action, static_cast<float>(points));
  ++counters_.ticks;
  ++session_ticks_;
  ++round_ticks_;
  round_points_ += points;
  ObsView obs;
  if (flag(r.j, "first")) {   // the round ended and Python already started the next one
    const bool over = flag(r.j, "game_over");
    ObsView final_obs;
    const bool has_final = read_obs(r, "final_obs", final_obs);
    brain_->end_segment(has_final ? &final_obs : nullptr, over);
    finish_round(over, flag(r.j, "truncated"));
    if (!read_obs(r, "obs", obs)) throw ipc::ProtocolError("the reply to step has no observation");
    brain_->start_segment(obs);
  } else {
    if (!read_obs(r, "obs", obs)) throw ipc::ProtocolError("the reply to step has no observation");
    brain_->continue_segment(obs);
  }
  brain_->learn(session_ticks_);
  const double t3 = util::now_seconds();

  // Telemetry: the user's code (as the worker measured it), the brain, and the rest of the round trip.
  double worker_ms = 0;
  auto ms = r.j.find("ms");
  if (ms != r.j.end() && ms->is_object()) {
    const double see = number_or(*ms, "see", 0), pts = number_or(*ms, "points", 0) + number_or(*ms, "game_over", 0);
    const double respond = number_or(*ms, "respond", 0), step = number_or(*ms, "step", 0);
    window_.see += see;
    window_.points += pts;
    window_.respond += respond;
    window_.step += step;
    worker_ms = see + pts + respond + step;
  }
  ++window_.ticks;
  window_.brain += ((t1 - t0) + (t3 - t2)) * 1000.0;
  window_.ipc += std::max(0.0, (t2 - t1) * 1000.0 - worker_ms);
  if (want_live) publish_live(action, std::move(probs));
}

void SimRunner::finish_round(bool game_over, bool truncated) {
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
}

void SimRunner::periodic(bool force_telemetry) {
  const double now = util::now_seconds();
  const double span = now - window_.start;
  if ((force_telemetry && window_.ticks > 0) || span >= kTelemetryEvery) {
    const double ticks = static_cast<double>(std::max<int64_t>(window_.ticks, 1));
    TimingRecord t;
    t.see_ms = window_.see / ticks;
    t.points_ms = window_.points / ticks;
    t.respond_ms = window_.respond / ticks;
    t.step_ms = window_.step / ticks;
    t.brain_ms = window_.brain / ticks;
    t.ipc_ms = window_.ipc / ticks;
    t.ticks_per_s = span > 0 ? static_cast<double>(window_.ticks) / span : 0.0;
    t.late_p99_ms = percentile(window_.late_ms, 99);
    t.overruns = pacer_.skipped() - window_.skipped_before;
    t.explore = epsilon();
    t.total_ticks = counters_.ticks;
    ctx_.emit(timing_event(t));
    ticks_per_s_ = t.ticks_per_s;
    if (training()) {
      int64_t n = 0;
      double q_sum = 0;
      const double loss_sum = brain_->take_loss_sum(&n, &q_sum);
      if (n > 0) {
        UpdateRecord u;
        u.updates = brain_->updates();
        u.loss = loss_sum / static_cast<double>(n);
        u.q = q_sum / static_cast<double>(n);
        u.explore = t.explore;
        u.lr = dcfg_.lr;
        u.updates_per_s = span > 0 ? static_cast<double>(n) / span : 0.0;
        ctx_.emit(update_event(u));
        write_update_row(updates_csv_, running_.seconds(), counters_.ticks, u, t.ticks_per_s);
      }
      rounds_csv_.flush();
      updates_csv_.flush();
    }
    ctx_.flush_dropped_lines();
    window_ = Window{};
    window_.start = now;
    window_.skipped_before = pacer_.skipped();
  }
  if (!training() || force_telemetry) return;
  try {
    if (brain_->best_pending() && now - last_best_write_ >= kSaveBestEvery) write_best();
    if (now - last_latest_ >= kSaveLatestEvery) save_latest();
  } catch (const std::exception& e) {
    ctx_.emit(warning_event("", 0, std::string("Vyrn couldn't save the brain (it tries again later): ") + e.what()));
    last_latest_ = now;
    last_best_write_ = now;
  }
}

void SimRunner::publish_live(int chosen, std::vector<float> probs) {
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

bool SimRunner::wait_while_paused() {
  running_.pause();
  ctx_.set_state(RunState::Paused, "Paused.");
  while (ctx_.controls().paused() && !ctx_.controls().stop_requested()) ctx_.controls().wait_for(0.5);
  if (ctx_.controls().stop_requested()) return false;
  ctx_.controls().take_resume();   // this resume() ended the pause
  running_.resume();
  pacer_.restart();
  window_ = Window{};
  window_.start = util::now_seconds();
  window_.skipped_before = pacer_.skipped();
  ctx_.set_state(RunState::Running, training() ? "Training." : "Watching.");
  return true;
}

bool SimRunner::recover_after_error() {
  running_.pause();
  ctx_.set_state(RunState::Error,
                 "Paused because of the error above. Fix the script, then Resume: Python restarts and the brain "
                 "is kept.");
  ctx_.controls().take_resume();   // only a resume() from now on counts
  for (;;) {
    if (ctx_.controls().stop_requested()) return false;
    const bool resume = ctx_.controls().take_resume();
    const bool apply = ctx_.controls().take_apply();
    if (resume || apply) {
      ctx_.set_state(RunState::Starting, "Restarting Python (the brain is kept)...");
      if (respawn()) {
        running_.resume();
        pacer_.restart();
        window_ = Window{};
        window_.start = util::now_seconds();
        window_.skipped_before = pacer_.skipped();
        ctx_.set_state(RunState::Running, training() ? "Training." : "Watching.");
        return true;
      }
      if (ctx_.controls().stop_requested()) return false;
      ctx_.set_state(RunState::Error, "Still not working: fix the script, then Resume.");
      continue;
    }
    ctx_.controls().wait_for(0.5);
  }
}

bool SimRunner::respawn() {
  ++generation_;
  try {
    ipc::Frame spec_frame = link_->start(load_options(false));
    brain::Spec spec = brain::parse_spec(spec_frame.j);
    std::string why;
    if (brain::compare(brain_->shape(), spec, &why) != brain::Compat::Same) {
      ctx_.emit(app_error_event("brain_mismatch", "This brain can't follow that change: " + why,
                                "Undo the change, or Stop and Train again to start a new brain.", false));
      link_->close();
      return false;
    }
    spec_ = spec;
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
  link_->close();
  return false;
}

void SimRunner::save_latest() {
  const std::string path = brain_->save_latest(counters_now(), rounds_.best_mean20());
  last_latest_ = util::now_seconds();
  saved_any_ = true;
  note_brain_in_settings();
  ctx_.emit(checkpoint_event("latest", path, rounds_.best_mean20()));
}

void SimRunner::write_best() {
  if (!brain_->best_pending()) return;
  const std::string path = brain_->write_best();
  last_best_write_ = util::now_seconds();
  saved_any_ = true;
  note_brain_in_settings();
  ctx_.emit(checkpoint_event("best", path, rounds_.best_mean20()));
}

void SimRunner::note_brain_in_settings() {
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

std::unique_ptr<Runner> make_sim_runner(RunContext& ctx, RunSetup setup) {
  return std::make_unique<SimRunner>(ctx, std::move(setup));
}

}  // namespace vyrn::run
