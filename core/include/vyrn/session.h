// Running a project: one Session does a Check, a Train or a Watch (docs/dev/core-internals.md sections 4-5).
//
// Standard library only: the Qt app and vyrn-cli include this header (no torch, no Qt, no JSON library).
// A Session runs on its own threads. Every member function returns quickly and may be called from any one
// thread (normally the UI thread); none of them waits for Python or for the neural network.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vyrn {

struct RunConfig {
  enum class Action { Check, Train, Watch } action = Action::Train;
  std::string project_dir;
  std::string data_dir;                      // the folder with python/vyrn_worker.py; empty = find_data_dir()
  std::string python;   // "" = vyrn::platform::default_python(); a non-empty vyrn.json "python" wins over this
  std::string focus_helper;                  // vyrn-focus (real time); empty = the worker looks for it
  std::string watch_brain = "best";          // Watch: "best" | "latest"
  double watch_speed = 1.0;                  // sims: x decisions/s; 0 = as fast as possible
  bool allow_input = false;                  // real time only, set by the app after its safety flow / CLI --allow-input
  bool allow_screen = true;                  // false: the worker refuses to read the screen (an agent's Check that
                                             // the person hasn't allowed to see it)
  bool new_brain = false;                    // "Start over": Train a brand-new brain (the old one is kept)
  uint64_t seed = 0;                         // 0 = random
  int64_t max_ticks = -1;                    // stop after this many ticks (-1 = no limit)
  double max_seconds = -1;                   // stop after this much running time (-1 = no limit)
  int check_ticks = 20;                      // random steps a simulation's Check plays
};

enum class RunState { Starting, Countdown, Restarting, Running, Holding, Paused, Error, Stopping, Finished };

// "starting", "countdown", ... (the names used in "state" events).
const char* to_string(RunState state);

struct Picture {
  int w = 0, h = 0;
  std::vector<uint8_t> rgb;   // h * w * 3 bytes, row by row
};

// What the dashboard shows. Refreshed at most 10 times a second; `version` grows with every refresh.
struct LiveView {
  uint64_t version = 0;
  std::string kind;                                   // "picture" | "numbers" ("" before the first observation)
  Picture input;                                      // network input as seen (newest frame, RGB)
  std::vector<float> numbers;                         // network input as seen (normalized while training)
  std::vector<std::string> number_names;
  std::vector<std::string> choices;                   // labels, in order
  std::vector<float> probs;                           // chance of each choice at the last refresh
  int chosen = -1;                                    // the choice made at the last refresh
  std::vector<std::pair<std::string, std::string>> show_text;      // vyrn.show() numbers and text
  std::vector<std::pair<std::string, Picture>> show_pictures;      // vyrn.show() pictures
  int64_t round = 0, tick = 0, total_ticks = 0;
  double best_mean = 0;
  bool has_best = false;
  double explore = 0, ticks_per_s = 0;
  RunState state = RunState::Starting;
};

class Session {
 public:
  explicit Session(RunConfig config);
  ~Session();   // stop() + wait for the run to end (Train saves the brain first)
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  void start();                // non-blocking; a second call does nothing
  void pause();                // stop playing until resume()
  void resume();               // after pause(), or after an error: respawns Python and keeps the brain
  void stop();                 // stop saves the brain (Train) and closes the worker; finished() turns true later
  void set_hold(bool on);      // app-side hold request (real time)
  void apply_changes();        // respawn the worker with the edited scripts, keep the brain

  // Events (one JSON object per string, docs/dev/core-internals.md section 5), oldest first.
  std::vector<std::string> drain(size_t max = 1000);
  LiveView live() const;
  RunState state() const;
  bool finished() const;       // the run is over: everything is saved and Python is closed

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// The folder that contains python/vyrn_worker.py: $VYRN_DATA, else <exe>/../share/vyrn (installed), else the
// source tree this program was built from. "" when none of them has it. argv0 may be null.
std::string find_data_dir(const char* argv0);

// A project imported from a shared .vrlp file carries <project>/.vyrn-imported until a person trusts it
// (docs/dev/package.md section 7). Session runs none of its Python code until then: it emits one "error" event of
// kind "not_trusted" with this sentence and finishes. Returns "" for a project that runs (not imported, or trusted).
std::string not_trusted_message(const std::string& project_dir);

// While a Session runs, another program can stop it like stop() by creating the empty file <project>/.vyrn-stop
// (the Session deletes it; a marker already there when the run starts is deleted and ignored).
inline constexpr const char* kStopMarkerFile = ".vyrn-stop";

}  // namespace vyrn
