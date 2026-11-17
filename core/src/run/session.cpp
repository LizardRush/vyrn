// vyrn::Session: owns the run thread and the RunContext it shares with the UI thread.
#include "vyrn/session.h"

#include "ipc/worker.h"
#include "platform/system.h"
#include "run/context.h"
#include "run/runner.h"
#include "run/stop_marker.h"
#include "util/settings.h"
#include "util/trust.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <random>
#include <thread>

namespace vyrn {

namespace fs = std::filesystem;

const char* to_string(RunState state) { return run::state_name(state); }

namespace run {

namespace {

uint64_t random_seed() {
  std::random_device device;
  const uint64_t seed = static_cast<uint64_t>(device()) & 0x7fffffffULL;   // fits every seed call in Python
  return seed == 0 ? 1 : seed;
}

}  // namespace

void run_session(RunContext& ctx) {
  try {
    const RunConfig& cfg = ctx.config();
    ctx.set_state(RunState::Starting, std::string("Getting ready to ") + action_name(cfg.action) + "...");
    static std::once_flag swept;
    std::call_once(swept, [] { ipc::sweep_stale_pid_files(); });

    std::error_code ec;
    if (cfg.project_dir.empty() || !fs::is_directory(cfg.project_dir, ec)) {
      ctx.emit(app_error_event("no_project", "There is no project folder at \"" + cfg.project_dir + "\".",
                               "Check the path: a project is a folder with inputs.py, responses.py and points.py.",
                               true));
      ctx.set_state(RunState::Finished, "Nothing was run.");
      ctx.mark_finished();
      return;
    }
    RunSetup setup;
    fs::path project = fs::absolute(cfg.project_dir, ec);
    const fs::path canonical = fs::weakly_canonical(project, ec);
    setup.project_dir = (ec ? project : canonical).string();
    // An imported project runs only once a person trusted it (docs/dev/package.md section 7): no Python at all.
    if (const std::string untrusted = util::not_trusted_message(setup.project_dir); !untrusted.empty()) {
      ctx.emit(not_trusted_event(untrusted));
      ctx.set_state(RunState::Finished, "Nothing was run: this project isn't trusted yet.");
      ctx.mark_finished();
      return;
    }
    setup.data_dir = cfg.data_dir.empty() ? find_data_dir(nullptr) : cfg.data_dir;
    if (setup.data_dir.empty()) {
      ctx.emit(app_error_event("no_data_dir", "Vyrn can't find its Python library (python/vyrn_worker.py).",
                               "Reinstall Vyrn, or set VYRN_DATA to the folder that contains python/.", true));
      ctx.set_state(RunState::Finished, "Nothing was run.");
      ctx.mark_finished();
      return;
    }
    setup.settings = util::read_settings(setup.project_dir);
    for (const auto& w : setup.settings.warnings) emit_settings_warning(ctx, w);
    setup.python = !setup.settings.python.empty() ? setup.settings.python
                                                  : (cfg.python.empty() ? platform::default_python() : cfg.python);
    setup.seed = cfg.seed != 0 ? cfg.seed : random_seed();

    const bool simulation = fs::is_regular_file(fs::path(setup.project_dir) / "world.py", ec);
    // <project>/.vyrn-stop from another program stops this run like stop() (a stale one is deleted first).
    StopMarkerWatch stop_marker(ctx, setup.project_dir);
    std::unique_ptr<Runner> runner =
        simulation ? make_sim_runner(ctx, std::move(setup)) : make_realtime_runner(ctx, std::move(setup));
    const std::string detail = runner->run();
    runner.reset();                  // Python is closed: every output line has arrived
    ctx.flush_dropped_lines(true);
    ctx.set_state(RunState::Finished, detail);
  } catch (const std::exception& e) {
    ctx.emit(app_error_event("internal", std::string("Something went wrong inside Vyrn: ") + e.what(), "", true));
    ctx.set_state(RunState::Finished, "Stopped because of a problem inside Vyrn.");
  } catch (...) {
    ctx.emit(app_error_event("internal", "Something went wrong inside Vyrn.", "", true));
    ctx.set_state(RunState::Finished, "Stopped because of a problem inside Vyrn.");
  }
  ctx.mark_finished();
}

}  // namespace run

struct Session::Impl {
  explicit Impl(RunConfig config) : ctx(std::move(config)) {}
  run::RunContext ctx;
  std::thread thread;
  std::atomic<bool> started{false};
};

Session::Session(RunConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

Session::~Session() {
  stop();
  if (impl_->thread.joinable()) impl_->thread.join();
}

void Session::start() {
  bool expected = false;
  if (!impl_->started.compare_exchange_strong(expected, true)) return;
  run::RunContext* ctx = &impl_->ctx;
  impl_->thread = std::thread([ctx] { run::run_session(*ctx); });
}

void Session::pause() { impl_->ctx.controls().set_paused(true); }
void Session::resume() { impl_->ctx.controls().request_resume(); }
void Session::stop() { impl_->ctx.request_stop(); }
void Session::set_hold(bool on) { impl_->ctx.controls().set_hold(on); }
void Session::apply_changes() { impl_->ctx.controls().request_apply(); }
std::vector<std::string> Session::drain(size_t max) { return impl_->ctx.drain(max); }
LiveView Session::live() const { return impl_->ctx.live(); }
RunState Session::state() const { return impl_->ctx.state(); }
bool Session::finished() const { return impl_->ctx.finished(); }

std::string not_trusted_message(const std::string& project_dir) { return util::not_trusted_message(project_dir); }

}  // namespace vyrn
