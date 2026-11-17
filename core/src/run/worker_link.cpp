#include "run/worker_link.h"

#include <utility>

namespace vyrn::run {

using nlohmann::json;

WorkerLink::WorkerLink(RunContext& ctx, std::string python, std::string data_dir, std::string project_dir)
    : ctx_(ctx), python_(std::move(python)), data_dir_(std::move(data_dir)), project_dir_(std::move(project_dir)) {}

WorkerLink::~WorkerLink() { close(); }

ipc::Frame WorkerLink::start(const LoadOptions& load) {
  close();
  ipc::WorkerOptions options;
  options.python = python_;
  options.data_dir = data_dir_;
  options.project_dir = project_dir_;
  RunContext* ctx = &ctx_;
  options.on_line = [ctx](ipc::Stream stream, const std::string& line) { ctx->worker_line(stream, line); };
  proc_ = std::make_unique<ipc::WorkerProcess>(std::move(options));

  // Spawning, the hello and the load may take seconds: let stop() kill the worker meanwhile.
  ctx_.set_interruptible(proc_.get());
  struct Clear {
    RunContext& ctx;
    ~Clear() { ctx.set_interruptible(nullptr); }
  } clear{ctx_};
  try {
    if (ctx_.controls().stop_requested()) throw StopRequested();
    try {
      proc_->spawn();
    } catch (const std::logic_error&) {
      throw;
    } catch (const std::runtime_error& e) {
      throw SpawnFailed(e.what());
    }
    if (ctx_.controls().stop_requested()) throw StopRequested();
    hello_ = proc_->hello(kHelloTimeout);
    json req = {{"t", "load"},
                {"project", project_dir_},
                {"check", load.check},
                {"allow_input", load.allow_input},
                {"seed", load.seed},
                {"round_limit", load.round_limit},
                {"decisions_per_second", load.decisions_per_second},
                {"focus_helper", load.focus_helper.empty() ? json(nullptr) : json(load.focus_helper)}};
    if (!load.allow_screen) req["screen"] = false;   // only sent when off: older workers ignore unknown keys
    if (ctx_.controls().stop_requested()) throw StopRequested();
    return proc_->request(std::move(req), kLoadTimeout, load.grace_s);
  } catch (const ipc::WorkerDied&) {
    if (ctx_.controls().stop_requested()) throw StopRequested();   // we killed it on purpose
    throw;
  } catch (const ipc::WorkerTimeout&) {
    if (ctx_.controls().stop_requested()) throw StopRequested();
    throw;
  }
}

ipc::Frame WorkerLink::request(json req, double timeout_s, double grace_s) {
  if (!proc_) throw ipc::WorkerDied("Python is not running.", {}, false);
  return proc_->request(std::move(req), timeout_s, grace_s);
}

bool WorkerLink::close() {
  if (!proc_) return true;
  bool answered = false;
  if (proc_->alive()) {
    try {
      proc_->request(json{{"t", "close"}}, kQuickTimeout, 0.0);
      answered = true;
    } catch (const std::exception&) {
      // It is going away either way: the destructor kills it with what it started.
    }
  }
  proc_.reset();
  return answered;
}

Failure describe_failure(std::exception_ptr error, bool fatal) {
  Failure f;
  try {
    std::rethrow_exception(error);
  } catch (const ipc::WorkerError& e) {
    f.kind = FailureKind::Script;
    f.event = worker_error_event(e.error, fatal);
    if (f.event.value("kind", "") == "protocol") f.kind = FailureKind::Died;
  } catch (const ipc::WorkerTimeout& e) {
    f.kind = FailureKind::Timeout;
    if (e.error.is_object()) {
      f.event = worker_error_event(e.error, fatal);
      f.event["kind"] = "timeout";
    } else {
      f.event = app_error_event("timeout", e.what(),
                                "Is there a loop in your code that never ends? Vyrn stopped Python to be safe.",
                                fatal);
      f.event["kind"] = "timeout";
    }
  } catch (const ipc::WorkerDied& e) {
    f.kind = FailureKind::Died;
    f.event = app_error_event(e.panic ? "panic" : "worker_died", e.what(),
                              e.panic ? "" : "Python's last error lines are in the console.", fatal);
    f.event["panic"] = e.panic;
    f.event["stderr_tail"] = e.stderr_tail;
  } catch (const SpawnFailed& e) {
    f.kind = FailureKind::Died;
    f.event = app_error_event("spawn_failed", e.what(), "", fatal);
  } catch (const ipc::ProtocolError& e) {
    f.kind = FailureKind::Died;
    f.event = app_error_event("protocol", std::string("Python sent a message Vyrn couldn't use: ") + e.what(),
                              "Vyrn's app and its Python library may not match: reinstall Vyrn.", fatal);
  } catch (const std::exception& e) {
    f.kind = FailureKind::Internal;
    f.event = app_error_event("internal", e.what(), "", fatal);
  } catch (...) {
    f.kind = FailureKind::Internal;
    f.event = app_error_event("internal", "Something unexpected went wrong inside Vyrn.", "", fatal);
  }
  return f;
}

}  // namespace vyrn::run
