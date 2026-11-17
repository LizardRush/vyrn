// Check: what a few random steps (simulations) or observe ticks (real time) showed, as warnings and the
// "check" event (docs/dev/core-internals.md section 5; PLAN.md section 2 "Check mode"). No torch in this header.
#pragma once

#include "ipc/proto.h"
#include "run/events.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace vyrn::run {

struct CheckWarning {
  std::string script;
  int64_t line = 0;
  std::string message;
};

class CheckReport {
 public:
  explicit CheckReport(const json& spec);   // the worker's "spec" reply

  void add_result(const ipc::Frame& result);   // reset/step (sim) or observe (real time) results
  void add_ok(const json& reply);              // "ok" replies of act (real time): timings and described effects
  void add_restart(const json& reply);         // real time: the "ok" reply of the restart() Check runs first
  void count_step() { ++steps_; }
  // The inputs couldn't change during this Check (Vyrn Link without a game, or a game waiting in lockstep): no
  // "never changed" warnings.
  void ignore_constant_inputs() { constant_inputs_meaningful_ = false; }
  // A remark of the runner's own for notes() (e.g. that no game was connected).
  void add_note(std::string note) { extra_notes_.push_back(std::move(note)); }

  // Worker warnings (spec + results) not handed out yet, each warning once.
  std::vector<CheckWarning> take_new_warnings();
  // What Check concludes from the run: game_over() never true (a simulation), inputs that never change, an
  // almost black picture, a slow see() (real time).
  std::vector<CheckWarning> findings(bool realtime, double decisions_per_second) const;
  // Remarks that aren't warnings: real time, game_over() stayed False during the short check (python/tools/check.py).
  std::vector<std::string> notes(bool realtime, double decisions_per_second) const;
  // What the restart() run first would do, in plain English ("" when real-time Check didn't run one).
  std::string restart_summary() const;
  // The "check" event: ok, warnings (all of them), notes, ms, mean_points, game_over_seen, constant_inputs,
  // restart_did (real time, when restart() ran), ...
  ojson event(bool ok, bool realtime, double decisions_per_second) const;

  int64_t steps() const { return steps_; }
  bool game_over_seen() const { return game_over_seen_; }
  std::vector<std::string> constant_inputs() const;

 private:
  void add_warning(const json& w);
  void track_obs(const ipc::Frame& frame, const char* key);
  CheckWarning at_hook(const char* hook, const std::string& message) const;

  json spec_;
  std::vector<std::string> number_names_;
  std::vector<CheckWarning> warnings_;
  size_t handed_out_ = 0;
  int64_t steps_ = 0, rounds_ended_ = 0;
  bool game_over_seen_ = false;
  double points_sum_ = 0;
  int64_t points_count_ = 0;
  std::map<std::string, std::pair<double, int64_t>> ms_;   // hook -> (total ms, calls)
  std::vector<std::string> did_;
  // Inputs that changed: numbers per index; pictures as a whole.
  int64_t observations_ = 0;
  std::vector<float> first_numbers_;
  std::vector<bool> number_changed_;
  std::vector<uint8_t> first_picture_;
  int first_h_ = 0, first_w_ = 0;
  bool picture_changed_ = false;
  int brightest_ = -1;   // the brightest value in any picture (-1: no picture yet)
  bool restart_ran_ = false;
  bool constant_inputs_meaningful_ = true;
  std::vector<std::string> extra_notes_;
  std::vector<std::string> restart_did_;
};

// Pictures whose brightest value is below this are "almost black" (python/tools/check.py).
constexpr int kAlmostBlack = 16;

// "points.py:8" -> {"points.py", 8}; anything else -> {"", 0}.
std::pair<std::string, int64_t> split_location(const std::string& where);

}  // namespace vyrn::run
