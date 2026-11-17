// vyrn-cli: Check, Train and Watch Vyrn projects from a terminal (docs/dev/core-internals.md section 6).
//
//   vyrn-cli check <project> [--json]
//   vyrn-cli train <project> [--minutes M | --ticks N] [--seed S] [--new-brain] [--allow-input] [--json]
//   vyrn-cli watch <project> [--brain best|latest] [--speed X] [--ticks N] [--allow-input] [--json]
//   vyrn-cli bench
//   vyrn-cli pack|export-brain|inspect|import|trust ...   sharing files (package_cmd.cpp; VYRN_HAVE_PACKAGE)
//   vyrn-cli compile <project> ...                        the trained AI as a Python program (package_cmd.cpp)
//   vyrn-cli mcp ...                                      the MCP server for coding agents (VYRN_HAVE_MCP)
//
// Exit status: 0 ok, 2 usage (also: the project isn't trusted yet, or another program is training it), 3 script
// error (the worker reported an error), 4 crash / protocol / Python died, 5 a sharing file was refused.
// Only the core's public headers are used here (no torch: a ctest checks that).
#include "vyrn/bench.h"
#include "vyrn/platform.h"
#include "vyrn/session.h"

#ifdef VYRN_HAVE_PROJECT
#include "vyrn/project.h"
#endif
#ifdef VYRN_HAVE_PACKAGE
#include "package_cmd.h"
#endif
#ifdef VYRN_HAVE_MCP
#include "mcp_server.h"
#endif

#include <nlohmann/json.hpp>

#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifndef VYRN_VERSION
#define VYRN_VERSION "0.0.0"
#endif

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitScript = 3;
constexpr int kExitCrash = 4;

constexpr const char* kUsage =
    "Usage:\n"
    "  vyrn-cli check <project> [--json]\n"
    "  vyrn-cli train <project> [--minutes M | --ticks N] [--seed S] [--new-brain] [--allow-input] [--json]\n"
    "  vyrn-cli watch <project> [--brain best|latest] [--speed X] [--ticks N] [--allow-input] [--json]\n"
    "  vyrn-cli bench\n"
#ifdef VYRN_HAVE_MCP
    "  vyrn-cli mcp [--projects-dir DIR] [--allow-dir DIR]... [--data-dir DIR] [--max-trainings N]\n"
#endif
    "\n"
    "  check        load the scripts and play a few random steps (nothing is learned or pressed)\n"
    "  train        teach the project's brain; Ctrl+C stops and saves it\n"
    "  watch        play the trained brain without learning\n"
    "  bench        time the neural network on this computer\n"
#ifdef VYRN_HAVE_MCP
    "  mcp          the MCP server that lets AI coding agents use Vyrn (on stdin/stdout)\n"
#endif
    "  --minutes M  stop training after M minutes        --ticks N   stop after N ticks\n"
    "  --seed S     repeatable randomness (0 = random)   --new-brain start over with a new brain\n"
    "  --brain      which brain Watch plays (best)       --speed X   x decisions per second (0 = max)\n"
    "  --json       print every event as one JSON line\n";

constexpr const char* kExitStatus =
    "\n"
    "Exit status: 0 ok, 2 usage (also: the project isn't trusted yet, or another program is training it),\n"
    "3 a problem in the scripts, 4 Python stopped or Vyrn failed"
#ifdef VYRN_HAVE_PACKAGE
    ", 5 a sharing file was refused or couldn't be\n"
    "written or read"
#endif
    ".\n";

// The whole --help text: the commands above, the sharing commands, then the exit status once.
std::string usage_text() {
  std::string text = kUsage;
#ifdef VYRN_HAVE_PACKAGE
  std::string sharing = vyrn::cli::package_usage();
  if (const size_t exit = sharing.find("\nExit status:"); exit != std::string::npos) sharing.resize(exit + 1);
  text += "\n" + sharing;
  while (text.size() > 1 && text[text.size() - 1] == '\n' && text[text.size() - 2] == '\n') text.pop_back();
#endif
  return text + kExitStatus;
}

// ---- command line --------------------------------------------------------------------------------------

struct Options {
  std::string command;
  std::string project;
  bool json = false;
  std::optional<double> minutes;
  std::optional<int64_t> ticks;
  std::optional<uint64_t> seed;
  bool new_brain = false;
  bool allow_input = false;
  std::string brain = "best";
  double speed = 1.0;
  bool watch_options = false;   // --brain or --speed was given
};

struct UsageError {
  std::string message;
};

template <typename T>
T parse_integer(std::string_view text, std::string_view option) {
  T value{};
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc() || end != text.data() + text.size()) {
    throw UsageError{std::string(option) + " needs a whole number, not \"" + std::string(text) + "\""};
  }
  return value;
}

double parse_number(const std::string& text, std::string_view option) {
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (text.empty() || end != text.c_str() + text.size() || !std::isfinite(value)) {
    throw UsageError{std::string(option) + " needs a number, not \"" + text + "\""};
  }
  return value;
}

Options parse_args(int argc, char** argv) {
  Options o;
  std::vector<std::string> positional;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    std::string value;
    bool has_value = false;
    if (arg.rfind("--", 0) == 0) {
      const auto eq = arg.find('=');
      if (eq != std::string::npos) {
        value = arg.substr(eq + 1);
        arg = arg.substr(0, eq);
        has_value = true;
      }
    }
    auto take_value = [&]() -> std::string {
      if (has_value) return value;
      if (i + 1 >= argc) throw UsageError{arg + " needs a value"};
      return argv[++i];
    };
    if (arg == "--json") {
      o.json = true;
    } else if (arg == "--new-brain") {
      o.new_brain = true;
    } else if (arg == "--allow-input") {
      o.allow_input = true;
    } else if (arg == "--minutes") {
      o.minutes = parse_number(take_value(), arg);
      if (*o.minutes <= 0) throw UsageError{"--minutes must be above 0"};
    } else if (arg == "--ticks") {
      o.ticks = parse_integer<int64_t>(take_value(), arg);
      if (*o.ticks < 1) throw UsageError{"--ticks must be at least 1"};
    } else if (arg == "--seed") {
      o.seed = parse_integer<uint64_t>(take_value(), arg);
    } else if (arg == "--brain") {
      o.watch_options = true;
      o.brain = take_value();
      if (o.brain != "best" && o.brain != "latest") throw UsageError{"--brain must be best or latest"};
    } else if (arg == "--speed") {
      o.watch_options = true;
      o.speed = parse_number(take_value(), arg);
      if (o.speed < 0) throw UsageError{"--speed can't be negative (0 means as fast as possible)"};
    } else if (arg == "-h" || arg == "--help" || arg == "--version") {
      positional.insert(positional.begin(), arg);
    } else if (arg.size() > 1 && arg[0] == '-') {
      throw UsageError{"unknown option " + arg};
    } else {
      positional.push_back(arg);
    }
  }
  if (positional.empty()) throw UsageError{"tell vyrn-cli what to do: check, train, watch or bench (--help lists all)"};
  o.command = positional[0];
  if (o.command == "-h" || o.command == "--help" || o.command == "help" || o.command == "--version") return o;
  if (o.command == "bench") {
    if (positional.size() > 1) throw UsageError{"bench takes no project"};
    return o;
  }
  if (o.command != "check" && o.command != "train" && o.command != "watch") {
    throw UsageError{"unknown command \"" + o.command + "\""};
  }
  if (positional.size() < 2) throw UsageError{o.command + " needs a project folder"};
  if (positional.size() > 2) throw UsageError{"unexpected argument \"" + positional[2] + "\""};
  o.project = positional[1];
  const bool train = o.command == "train", watch = o.command == "watch";
  if (o.minutes && o.ticks) throw UsageError{"use --minutes or --ticks, not both"};
  if (o.minutes && !train) throw UsageError{"--minutes is for train"};
  if (o.ticks && !train && !watch) throw UsageError{"--ticks is for train and watch"};
  if (o.new_brain && !train) throw UsageError{"--new-brain is for train"};
  if (o.watch_options && !watch) throw UsageError{"--brain and --speed are for watch"};
  if (o.allow_input && !train && !watch) throw UsageError{"--allow-input is for train and watch"};
  return o;
}

// ---- printing events -------------------------------------------------------------------------------------

std::string fixed(double v, int decimals) {
  char text[64];
  std::snprintf(text, sizeof text, "%.*f", decimals, v);
  return text;
}

// "3.5", "0.85", "412": short, for progress lines.
std::string brief(double v) {
  const double a = std::fabs(v);
  return fixed(v, a >= 100 ? 0 : (a >= 10 ? 1 : 2));
}

std::string location(const json& e) {
  const std::string script = e.value("script", "");
  const int64_t line = e.value("line", int64_t{0});
  if (script.empty()) return "";
  return line > 0 ? script + ":" + std::to_string(line) : script;
}

// The exit status an error event asks for.
int exit_code_for(const json& e) {
  const std::string kind = e.value("kind", "");
  if (kind == "not_trusted") return kExitUsage;   // nothing ran: the person has to trust the project first
  if (kind == "protocol") return kExitCrash;
  if (kind != "app") return kExitScript;   // syntax, load, exception, bad_return, timeout, input, focus
  const std::string reason = e.value("reason", "");
  // The scripts: too slow, restart() never started a new round, or stuck so long that Vyrn stopped Python.
  if (reason == "timeout" || reason == "restart_stuck" || reason == "unresponsive") return kExitScript;
  // Nothing (more) could run as asked: keys not allowed, no brain, the Vyrn Link port is taken...
  if (reason == "input_not_allowed" || reason == "no_brain" || reason == "brain_mismatch" ||
      reason == "no_project" || reason == "link") {
    return kExitUsage;
  }
  return kExitCrash;
}

// What to do about an app error, said for a terminal (the event's hint may name the app's buttons).
std::string cli_hint(const json& e) {
  const std::string reason = e.value("reason", "");
  if (reason == "input_not_allowed") {
    return "This project presses keys. Add --allow-input to let it (it counts down first, so you can click your "
           "game), or use Train in the Vyrn app.";
  }
  if (reason == "unresponsive") {
    return "Is see(), points() or one of your choices stuck in a long loop? Fix it, then run vyrn-cli again.";
  }
  if (reason == "restart_stuck") {
    return "Make restart() start a new round, and game_over() True only while the game-over screen shows; then "
           "run vyrn-cli again.";
  }
  return e.value("hint", "");
}

class Printer {
 public:
  explicit Printer(bool json_lines) : json_(json_lines) {}

  // Handles one event. Returns true when the run should stop because of it (an error).
  bool event(const std::string& text) {
    json e;
    try {
      e = json::parse(text);
    } catch (const json::exception&) {
      if (json_) out(text);
      return false;
    }
    const std::string type = e.value("type", "");
    bool stop = false;
    if (type == "error") {
      if (exit_code_ == kExitOk) exit_code_ = exit_code_for(e);
      stop = e.value("kind", "") != "not_trusted";   // that run never started: nothing to stop
    }
    if (json_) {
      out(text);
    } else {
      print(type, e);
    }
    return stop;
  }

  // A line about what the CLI itself does (stderr with --json, so stdout stays pure JSON lines).
  void note(const std::string& line) {
    if (json_) {
      std::fprintf(stderr, "%s\n", line.c_str());
    } else {
      out(line);
    }
  }

  int exit_code() const { return exit_code_; }
  bool output_broken() const { return broken_; }

 private:
  void out(const std::string& line) {
    if (std::fprintf(stdout, "%s\n", line.c_str()) < 0 || std::fflush(stdout) != 0) broken_ = true;
  }

  void print(const std::string& type, const json& e) {
    if (type == "state") {
      const std::string state = e.value("state", ""), detail = e.value("detail", "");
      if (state == "error" || detail.empty() || detail == last_detail_) return;
      if (state == "restarting") {   // real time: before every round; say it once
        if (said_restarting_) return;
        said_restarting_ = true;
        out(detail);
        return;
      }
      last_detail_ = detail;
      out(detail);
    } else if (type == "log") {
      out("  | " + e.value("text", ""));
    } else if (type == "spec") {
      print_spec(e);
    } else if (type == "warning") {
      const std::string where = location(e), message = e.value("message", "");
      const bool named = !where.empty() && message.rfind(e.value("script", ""), 0) == 0;   // "vyrn.json isn't..."
      out("Warning: " + (where.empty() || named ? "" : where + ": ") + message);
    } else if (type == "error" && e.value("kind", "") == "not_trusted") {
      out(e.value("message", ""));
    } else if (type == "error") {
      const std::string where = location(e), hint = e.value("kind", "") == "app" ? cli_hint(e) : e.value("hint", "");
      out((where.empty() ? std::string("Error") : where) + ": " + e.value("message", "") +
          (hint.empty() ? "" : " (" + hint + ")"));
    } else if (type == "round") {
      round_ = e.value("round", int64_t{0});
      mean20_ = e.value("mean20", 0.0);
      if (e.contains("best_mean20") && e["best_mean20"].is_number()) best_ = e["best_mean20"].get<double>();
      explore_ = e.value("explore", explore_);
      ++rounds_seen_;
    } else if (type == "timing") {
      explore_ = e.value("explore", explore_);
      progress(e.value("ticks_per_s", 0.0));
    } else if (type == "checkpoint") {
      const std::string which = e.value("which", "");
      std::string line = which == "best" ? "Saved the best brain" : "Saved the brain";
      if (e.contains("best_mean20") && e["best_mean20"].is_number()) {
        line += " (best mean20 " + brief(e["best_mean20"].get<double>()) + ")";
      }
      out(line + ": " + e.value("path", ""));
    } else if (type == "check") {
      print_check(e);
    } else if (type == "countdown") {
      const double left = e.value("seconds_left", 0.0);
      if (left > 0) out("  " + brief(left) + "...");
    } else if (type == "link" && !said_link_) {   // Vyrn Link: where the game connects (once; then log lines)
      said_link_ = true;
      const std::string address = e.value("address", ""), port = std::to_string(e.value("port", 0));
      out("Vyrn Link: connect your game to " + e.value("url", "") + " with the pairing code " + e.value("code", "") +
          (address == "127.0.0.1" || address.empty() ? std::string(".")
                                                      : " (on this computer: ws://127.0.0.1:" + port + "/, no code)."));
    }
  }

  void progress(double ticks_per_s) {
    std::string line = "Round " + std::to_string(rounds_seen_ ? round_ : round_ + 1);
    if (rounds_seen_) {
      line += " · mean20 " + brief(mean20_);
      if (best_) line += " · best " + brief(*best_);
    } else {
      line += " · no round finished yet";
    }
    line += " · exploring " + std::to_string(static_cast<int>(std::lround(explore_ * 100))) + "%";
    line += " · " + std::to_string(static_cast<long long>(std::llround(ticks_per_s))) + " ticks/s";
    out(line);
  }

  void print_spec(const json& e) {
    const json obs = e.value("obs", json::object());
    const json shape = obs.value("shape", json::array());
    std::string sees;
    if (obs.value("kind", "") == "picture" && shape.size() >= 2) {
      sees = "a " + std::to_string(shape[1].get<int64_t>()) + "x" + std::to_string(shape[0].get<int64_t>()) +
             " picture";
    } else if (!shape.empty()) {
      sees = std::to_string(shape[0].get<int64_t>()) + " numbers";
      const json names = obs.value("names", json());
      if (names.is_array() && !names.empty()) {
        std::string list;
        for (const auto& n : names) list += (list.empty() ? "" : ", ") + n.get<std::string>();
        sees += " (" + list + ")";
      }
    }
    if (!sees.empty()) out("The AI sees " + sees + ".");
    const json labels = e.value("labels", json::array()), effects = e.value("effects", json::array());
    std::string choices;
    for (size_t i = 0; i < labels.size(); ++i) {
      choices += (i ? ", " : "") + labels[i].get<std::string>();
      if (i < effects.size() && effects[i].is_string()) choices += " (" + effects[i].get<std::string>() + ")";
    }
    out("Choices: " + choices);
    const std::string action = e.value("action", "");
    if (action != "check") {
      const std::string status = e.value("brain", ""), id = e.value("brain_id", "");
      std::string line = action == "watch" ? "Brain: " + id + " (" + e.value("checkpoint", "best") + ")"
                                           : "Brain: " + status + (id.empty() ? "" : " (" + id + ")");
      if (const std::string net = e.value("net", ""); !net.empty()) line += ", " + net;
      out(line);
      if (const std::string why = e.value("why", ""); !why.empty()) out("  " + why);   // Check warns instead
    }
    if (action == "check") return;   // Check repeats the spec's warnings as "warning" events: print them once
    for (const auto& w : e.value("warnings", json::array())) {
      const std::string where = location(w);
      out("Warning: " + (where.empty() ? "" : where + ": ") + w.value("message", ""));
    }
  }

  void print_check(const json& e) {
    const bool ok = e.value("ok", false);
    const size_t warnings = e.value("warnings", json::array()).size();
    if (!ok) {
      out("Check found a problem (see above).");
      return;
    }
    out(warnings == 0 ? "Check passed: no errors, no warnings."
                      : "Check passed with " + std::to_string(warnings) + (warnings == 1 ? " warning." : " warnings."));
    const std::string what = e.value("mode", "") == "realtime" ? " real-time ticks" : " random steps";
    out("  " + std::to_string(e.value("steps", int64_t{0})) + what + "; rounds ended: " +
        std::to_string(e.value("rounds_ended", int64_t{0})) +
        (e.value("game_over_seen", false) ? " (game_over() was seen)." : "."));
    out("  Points: " + fixed(e.value("mean_points", 0.0), 3) + " per tick on average.");
    std::string ms;
    const json timings = e.value("ms", json::object());
    for (const auto& [hook, value] : timings.items()) {
      if (value.is_number() && value.get<double>() > 0) ms += (ms.empty() ? "" : ", ") + hook + " " + fixed(value.get<double>(), 3);
    }
    if (!ms.empty()) out("  Time per call (ms): " + ms);
    std::string did;
    for (const auto& d : e.value("did", json::array())) did += (did.empty() ? "" : ", ") + d.get<std::string>();
    if (!did.empty()) out("  It would: " + did + " (nothing was pressed).");
  }

  bool json_;
  bool broken_ = false;
  bool said_restarting_ = false;
  bool said_link_ = false;
  int exit_code_ = kExitOk;
  std::string last_detail_;
  int64_t round_ = 0, rounds_seen_ = 0;
  double mean20_ = 0, explore_ = 0;
  std::optional<double> best_;
};

// ---- running ---------------------------------------------------------------------------------------------

#ifdef VYRN_HAVE_PROJECT
// "The Vyrn app is already training this project (since 2026-11-13 17:05)."
std::string holder_sentence(const vyrn::ProjectLock::Holder& h) {
  const std::string pid = " (pid " + std::to_string(h.pid) + ")";
  const std::string who = h.by == "app"     ? std::string("The Vyrn app")
                          : h.by == "agent" ? "An AI agent's Vyrn MCP server" + pid
                          : h.by == "cli"   ? "vyrn-cli" + pid
                                            : "Another program" + pid;
  std::string since = h.since;
  if (since.size() >= 16 && since[10] == 'T') since = since.substr(0, 10) + " " + since.substr(11, 5);
  return who + " is already training this project" + (since.empty() ? "" : " (since " + since + ")") +
         (h.by == "app" ? ". Stop it in the app first." : ". Wait until it finishes, or stop it first.");
}

// One trainer per project (docs/dev/mcp.md section 4): Train and Watch hold <project>/.vyrn-lock while they run.
// Returns false (after saying why) when another live program holds it.
bool take_lock(const fs::path& project, std::optional<vyrn::ProjectLock>& lock) {
  for (int attempt = 0; attempt < 2 && !lock; ++attempt) {
    lock = vyrn::ProjectLock::acquire(project.string(), "cli");
    if (lock) return true;
    if (const auto h = vyrn::ProjectLock::holder(project.string())) {
      std::fprintf(stderr, "vyrn-cli: %s\n", holder_sentence(*h).c_str());
      return false;
    }
  }   // no live holder, and still no lock: a folder Vyrn can't write in
  std::fprintf(stderr, "vyrn-cli: can't create the lock file .vyrn-lock in %s; carrying on without it.\n",
               project.string().c_str());
  return true;
}
#endif

int run_project(const Options& o, const char* argv0) {
  std::error_code ec;
  const fs::path project = fs::absolute(o.project, ec);
  if (ec || !fs::is_directory(project, ec)) {
    std::fprintf(stderr, "vyrn-cli: there is no project folder at %s\n", o.project.c_str());
    return kExitUsage;
  }
#ifdef VYRN_HAVE_PROJECT
  std::optional<vyrn::ProjectLock> lock;   // released when this function returns (after the Session has ended)
  if ((o.command == "train" || o.command == "watch") && !take_lock(project, lock)) return kExitUsage;
#endif
  vyrn::RunConfig cfg;
  cfg.action = o.command == "check"   ? vyrn::RunConfig::Action::Check
               : o.command == "train" ? vyrn::RunConfig::Action::Train
                                      : vyrn::RunConfig::Action::Watch;
  cfg.project_dir = project.string();
  cfg.data_dir = vyrn::find_data_dir(argv0);
  cfg.watch_brain = o.brain;
  cfg.watch_speed = o.speed;
  cfg.allow_input = o.allow_input;
  cfg.new_brain = o.new_brain;
  cfg.seed = o.seed.value_or(0);
  if (o.ticks) cfg.max_ticks = *o.ticks;
  if (o.minutes) cfg.max_seconds = *o.minutes * 60.0;

  // Ctrl+C (SIGINT/SIGTERM; Windows: the console's Ctrl+C, Ctrl+Break and close) no longer ends the process: the
  // loop below takes the stop requests. POSIX blocks the signals here, before the Session starts, so its threads
  // inherit the block (Python runs in its own process group with a clean signal mask); a thread that already
  // existed reports them through the same channel instead of the default action killing the process.
  vyrn::platform::install_stop_handlers(false);

  Printer printer(o.json);
  vyrn::Session session(cfg);   // declared after `lock`: it ends (and saves) before the lock goes
  session.start();
  bool stopping = false;
  auto handle = [&](const std::vector<std::string>& events) {
    for (const auto& text : events) {
      if (printer.event(text) && !stopping && o.command != "check") {
        stopping = true;
        printer.note("Stopping because of the error above.");
        session.stop();
      }
    }
    if (printer.output_broken() && !stopping) {   // nobody reads our output any more
      stopping = true;
      session.stop();
    }
  };
  int signals_taken = 0;     // stop requests that came
  int signals_handled = 0;   // the ones below reacted to
  for (;;) {
    handle(session.drain(1000));
    if (session.finished()) {
      handle(session.drain(1u << 30));
      break;
    }
    signals_taken += vyrn::platform::wait_stop_requests(100);
    for (const int seen = signals_taken; signals_handled < seen; ++signals_handled) {
      if (!stopping) {
        stopping = true;
        printer.note(o.command == "train" ? "Stopping: saving the brain... (press Ctrl+C again to quit at once)"
                                          : "Stopping... (press Ctrl+C again to quit at once)");
        session.stop();
      } else {
        std::fprintf(stderr, "vyrn-cli: quitting without waiting.\n");
        std::_Exit(130);   // Python notices that we are gone and exits on its own
      }
    }
  }
  return printer.exit_code();
}

}  // namespace

int main(int argc, char** argv) {
  vyrn::platform::ensure_passive_openmp(argv);
#ifdef VYRN_HAVE_MCP
  if (argc > 1 && std::string_view(argv[1]) == "mcp") return run_mcp_server(argc - 1, argv + 1);
#endif
#ifdef VYRN_HAVE_PACKAGE
  if (argc > 1 && vyrn::cli::is_package_command(argv[1])) return vyrn::cli::run_package_command(argc, argv);
#endif
  Options options;
  try {
    options = parse_args(argc, argv);
  } catch (const UsageError& e) {
    std::fprintf(stderr, "vyrn-cli: %s\n\n%s", e.message.c_str(), usage_text().c_str());
    return kExitUsage;
  }
  if (options.command == "-h" || options.command == "--help" || options.command == "help") {
    std::fputs(usage_text().c_str(), stdout);
    return kExitOk;
  }
  if (options.command == "--version") {
    std::printf("vyrn-cli %s\n", VYRN_VERSION);
    return kExitOk;
  }
  if (options.command == "bench") {
    std::printf("Timing the neural network on this computer (about half a minute)...\n");
    std::fflush(stdout);
    std::fputs(vyrn::run_bench().c_str(), stdout);
    return kExitOk;
  }
  return run_project(options, argc > 0 ? argv[0] : nullptr);
}
