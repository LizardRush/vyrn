// Runners: the code that plays a project for one Session, on the session's run thread.
//
//   SimRunner      (sim_runner.cpp)  projects with a world.py: Check, Train, Watch
//   RealtimeRunner (rt_runner.cpp)   projects without one (a real program, in real time): Check, Train, Watch
//
// A runner only talks to the outside through its RunContext (events, live view, state, controls), so new
// runners plug in without changing vyrn::Session's public API.
#pragma once

#include "brain/spec.h"
#include "run/context.h"
#include "run/observation.h"
#include "run/worker_link.h"
#include "util/csv.h"
#include "util/settings.h"

#include <cstdint>
#include <memory>
#include <string>

namespace vyrn::run {

// Everything a runner needs besides the context, resolved once by the session.
struct RunSetup {
  std::string project_dir;          // absolute
  std::string data_dir;             // contains python/vyrn_worker.py
  std::string python;               // vyrn.json "python", else RunConfig::python
  util::ProjectSettings settings;   // vyrn.json
  uint64_t seed = 1;                // never 0 (RunConfig::seed 0 is replaced by a random one)
};

class Runner {
 public:
  virtual ~Runner() = default;
  // Plays the whole Check/Train/Watch and returns when it is over (stopped, done or failed), with Python
  // closed and the brain saved. Returns the plain-English detail for the final "finished" state.
  virtual std::string run() = 0;
};

std::unique_ptr<Runner> make_sim_runner(RunContext& ctx, RunSetup setup);
std::unique_ptr<Runner> make_realtime_runner(RunContext& ctx, RunSetup setup);

// The session's run thread: resolves the setup, picks the runner, ends in the "finished" state. Never throws.
void run_session(RunContext& ctx);

// ---- shared by the runners

const char* action_name(RunConfig::Action action);   // "check" | "train" | "watch"

// The "spec" event: the worker's spec fields (without "preview", "t", "id") plus what the run does with the
// brain: brain ("new" | "resumed" | "incompatible"), why, brain_id, net, frames (stacked frames of the brain's
// input: its shape's frames for pictures, 1 for numbers), seed, action.
ojson spec_event(const json& spec_msg, RunConfig::Action action, const std::string& brain_status,
                 const std::string& why, const std::string& brain_id, const std::string& net,
                 const brain::BrainShape& shape, uint64_t seed);

// The live view for Check: the network input made from an observation (no brain needed).
LiveView check_live_view(const ObsView& obs, const brain::Spec& spec);

// "warning" events for a result's one-time warnings (results carry each warning once).
void emit_result_warnings(RunContext& ctx, const json& result);

// A "warning" event about vyrn.json (script "vyrn.json"; a leading "vyrn.json: " is dropped from the message).
void emit_settings_warning(RunContext& ctx, const std::string& message);

// The logs of a Train run: runs/<YYYYmmdd-HHMMSS>/{rounds,updates}.csv and run.json (which brain they belong
// to). Problems become warnings: training goes on without logs.
void open_run_logs(RunContext& ctx, const std::string& project_dir, const std::string& brain_id,
                   RunConfig::Action action, util::CsvWriter& rounds, util::CsvWriter& updates);
void write_round_row(util::CsvWriter& rounds, const RoundRecord& rec);
void write_update_row(util::CsvWriter& updates, double seconds, int64_t total_ticks, const UpdateRecord& u,
                      double ticks_per_s);

}  // namespace vyrn::run
