// One Python worker for a run: spawn + hello + load, requests with the protocol's timeouts, a polite close,
// and turning worker failures into "error" events. No torch in this header.
#pragma once

#include "ipc/worker.h"
#include "run/context.h"
#include "run/events.h"

#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>

namespace vyrn::run {

// Timeouts in seconds (docs/dev/protocol.md section 7).
constexpr double kHelloTimeout = 15;
constexpr double kLoadTimeout = 30;      // load, reset, restart, lock_focus
constexpr double kStepTimeout = 5;       // step, check_over
constexpr double kQuickTimeout = 2;      // status, pause, resume, close
constexpr double kSimGrace = 2;          // after SIGINT, before SIGKILL (simulations)
constexpr double kRealtimeGrace = 0.5;   // ... (real time)

// stop() interrupted the start-up: the worker was killed on purpose, nothing to report.
struct StopRequested : std::runtime_error {
  StopRequested() : std::runtime_error("stopped") {}
};

// Python couldn't be started at all (missing interpreter or worker script). The message is plain English.
struct SpawnFailed : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct LoadOptions {
  bool check = false;
  bool allow_input = false;
  bool allow_screen = true;   // false: "screen": false, the worker refuses to read the screen
  uint64_t seed = 0;
  int64_t round_limit = 1000;
  double decisions_per_second = 10;
  std::string focus_helper;   // "" = null (the worker looks in the install places)
  double grace_s = kSimGrace;  // after a load timeout: SIGINT, then SIGKILL this much later
};

class WorkerLink {
 public:
  WorkerLink(RunContext& ctx, std::string python, std::string data_dir, std::string project_dir);
  ~WorkerLink();
  WorkerLink(const WorkerLink&) = delete;
  WorkerLink& operator=(const WorkerLink&) = delete;

  // Spawns Python, waits for its hello and loads the scripts; returns the "spec" reply. Throws what
  // WorkerProcess throws (WorkerError for a script problem), SpawnFailed when Python can't start, or
  // StopRequested when stop() came meanwhile.
  ipc::Frame start(const LoadOptions& load);
  ipc::Frame request(nlohmann::json req, double timeout_s, double grace_s);
  // "close" (2 s), then the worker is killed with what it started; safe to call twice. True when it ended cleanly
  // (it answered the close, so it let go of its keys itself) or none was running; false when it couldn't answer
  // (it died, was killed, or was stuck).
  bool close();
  bool running() const { return proc_ != nullptr; }
  const nlohmann::json& hello() const { return hello_; }
  // The process while running (nullptr after close()), e.g. for another thread's freeze() or kill_group().
  // The owner makes sure that thread stops using it before close() or the next start().
  ipc::WorkerProcess* process() const { return proc_.get(); }

 private:
  RunContext& ctx_;
  std::string python_, data_dir_, project_dir_;
  std::unique_ptr<ipc::WorkerProcess> proc_;
  nlohmann::json hello_;
};

// How a run failed, for "error" events and the CLI's exit code.
enum class FailureKind {
  Script,     // the worker reported an error in the user's scripts (protocol.md 3.7)
  Timeout,    // the user's code took too long
  Died,       // Python stopped or broke the protocol
  Internal,   // anything else
};
struct Failure {
  FailureKind kind = FailureKind::Internal;
  ojson event;   // the "error" event
};

// Describes an exception from a worker call as an error event. `fatal` = the run ends because of it.
Failure describe_failure(std::exception_ptr error, bool fatal);

}  // namespace vyrn::run
