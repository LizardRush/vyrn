// Sessions the MCP server runs (docs/dev/mcp.md sections 3-4): trainings in the background (at most 2, each
// holding the project's .vyrn-lock), and Checks and Watches that a tool call runs to the end.
//
// C++17 (the unit tests include this header). Everything here runs on the server's one thread; a Session runs
// on its own threads and is only asked for events and its live view.
#pragma once

#include "tooling.h"
#include "vyrn/project.h"
#include "vyrn/session.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vyrn::mcp {

// Seconds on a steady clock.
double now_s();

// "points.py:7:12 NameError: name 'scor' is not defined — Is it spelled the same as where you made it?"
std::string format_error(const json& event);
// "points.py:9: game_over() was never True in 20 steps. ..."
std::string format_warning(const json& event);

// What a Session's events said (docs/dev/core-internals.md section 5), kept for the tools.
struct Digest {
  json spec;                          // the last "spec" event, or null
  json check;                         // the "check" event, or null
  json last_error;                    // the last "error" event, or null
  std::vector<std::string> errors;    // formatted errors, each once (at most 20)
  std::vector<std::string> warnings;  // formatted warnings, each once (at most 50)
  std::deque<std::string> console;    // the last 30 lines the scripts (or Vyrn) printed
  std::deque<double> recent_points;   // points of the last 50 rounds
  std::vector<double> round_points;   // points of every round (at most 1000)
  int64_t rounds = 0;                 // "round" events seen
  int64_t last_round = 0;             // the round number of the last one
  int64_t total_ticks = 0;
  std::optional<double> mean20, best_mean20, explore, ticks_per_s;
  std::string state, detail;          // the last "state" event
  int checkpoints = 0;                // brain saves
  bool save_failed = false;

  void add(const std::string& event_text);   // one string from Session::drain()
};

// What the AI saw, kept after a run for vyrn_see.
struct Look {
  LiveView view;
  std::string source;   // "check", "watch" or "training"
  double when = 0;      // now_s()
};

struct Training {
  std::string path, folder;
  double minutes = 5;
  bool new_brain = false;
  std::optional<ProjectLock> lock;     // declared before `session`: the Session ends (and saves) first
  std::unique_ptr<Session> session;
  Digest digest;
  double started = 0, last_pump = 0;
  double running_s = 0;                // time spent running (pauses for errors don't count)
  bool was_running = false;
  bool stop_requested = false;
  int checkpoints_at_stop = 0;
  bool finished = false;
  double finished_at = 0;
  LiveView final_view;                 // live() when it finished

  // The brain was saved after the stop request (or, without one, at all) and no save failed.
  bool saved() const { return !digest.save_failed && digest.checkpoints > checkpoints_at_stop; }
  double minutes_left() const;
};

class Runs {
 public:
  Runs(std::string data_dir, std::string python, int max_trainings);
  ~Runs();   // stops every session and waits for them (trainings save their brains)
  Runs(const Runs&) = delete;
  Runs& operator=(const Runs&) = delete;

  // A RunConfig for this server: never allow_input (agents never press keys).
  RunConfig config(RunConfig::Action action, const std::string& project) const;
  int max_trainings() const { return max_; }
  int running() const;                         // trainings that haven't finished
  std::vector<std::string> running_folders() const;
  Training* find(const std::string& path);     // this server's latest training of that project, or null
  // Starts a training; `lock` is the project's .vyrn-lock, held until the training ends.
  Training& start(const std::string& path, const std::string& folder, double minutes, bool new_brain,
                  ProjectLock lock);
  void stop(Training& t);                      // asks it to stop; it saves and finishes a little later
  void pump();                                 // reads events; frees the lock of trainings that ended
  void stop_all();
  bool all_finished() const;

 private:
  std::string data_dir_, python_;
  int max_;
  std::vector<std::unique_ptr<Training>> trainings_;
};

struct Outcome {
  Digest digest;
  LiveView last_view;    // the last view that showed something
  bool finished = false, timed_out = false, interrupted = false;
};

// Runs one Check or Watch until it finishes, `enough` says so, `seconds` pass or the waiter says stop (then it
// stops the Session and waits a little for it to end).
Outcome run_to_end(const RunConfig& config, Waiter& waiter, double seconds,
                   const std::function<bool(const Digest&)>& enough = nullptr);

// PNG of a picture, enlarged (nearest neighbour) so a tiny picture is easy to see. Empty when there is none.
std::vector<uint8_t> picture_png(const Picture& picture);

}  // namespace vyrn::mcp
