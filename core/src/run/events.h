// Session events (docs/dev/core-internals.md section 5): builders for each event type and the queue that
// drain() reads. Events are JSON objects whose first key is "type". No torch in this header.
#pragma once

#include "vyrn/session.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vyrn::run {

using json = nlohmann::json;
using ojson = nlohmann::ordered_json;   // keeps "type" first in the printed event

const char* state_name(RunState state);

ojson state_event(RunState state, const std::string& detail);
ojson log_event(const std::string& stream, const std::string& text);   // stream: stdout | stderr | app
ojson warning_event(const std::string& script, int64_t line, const std::string& message);

struct RoundRecord {
  int64_t round = 0;                    // round number (counts on across resumed runs)
  double points = 0;                    // points scored in the round
  int64_t ticks = 0;                    // ticks the round lasted
  bool truncated = false;               // ended by the round limit
  bool game_over = false;               // ended by game_over()
  double mean20 = 0;                    // mean points of the last 20 rounds (fewer at the start)
  std::optional<double> best_mean20;    // best mean over 20 rounds so far (null before 20 rounds)
  int64_t total_ticks = 0;              // ticks this brain has played
  double explore = 0;                   // chance of a random choice (0..1)
  double seconds = 0;                   // running time of this session
};
ojson round_event(const RoundRecord& r);

struct UpdateRecord {
  int64_t updates = 0;   // learning updates this brain has done
  double loss = 0, q = 0, explore = 0, lr = 0, updates_per_s = 0;
};
ojson update_event(const UpdateRecord& u);

struct TimingRecord {
  double see_ms = 0, points_ms = 0, respond_ms = 0, step_ms = 0;   // the user's code, per tick
  double brain_ms = 0, ipc_ms = 0;                                 // the network / talking to Python, per tick
  double ticks_per_s = 0, late_p99_ms = 0;
  int64_t overruns = 0;
  double explore = 0;
  int64_t total_ticks = 0;
};
ojson timing_event(const TimingRecord& t);

ojson checkpoint_event(const std::string& which, const std::string& path, std::optional<double> best_mean20);
ojson countdown_event(double seconds_left);
ojson hold_event(bool on, const std::string& reason);

// The worker's error reply (protocol.md section 3.7) as an event; "t" and "id" are dropped.
ojson worker_error_event(const json& worker_error, bool fatal);
// A problem found by Vyrn itself. `reason` says what kind (e.g. "worker_died", "no_brain"), for programs.
ojson app_error_event(const std::string& reason, const std::string& message, const std::string& hint, bool fatal);
// An imported project nobody trusted yet (docs/dev/package.md section 7): kind "not_trusted", fatal, nothing ran.
ojson not_trusted_event(const std::string& message);

// What drain() hands out: bounded, oldest first, thread-safe. When nobody reads it, the oldest events are
// dropped and the next drain() starts with a "log" line that says how many.
class EventQueue {
 public:
  explicit EventQueue(size_t capacity = 50000) : capacity_(capacity > 0 ? capacity : 1) {}
  void push(const ojson& event);
  std::vector<std::string> drain(size_t max);
  size_t size() const;

 private:
  mutable std::mutex mu_;
  std::deque<std::string> items_;
  size_t capacity_;
  int64_t unreported_drops_ = 0;
};

// Serializes an event (invalid UTF-8 from user output is replaced, never thrown on).
std::string event_text(const ojson& event);

}  // namespace vyrn::run
