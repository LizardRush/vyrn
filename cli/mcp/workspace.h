// Where agents may work, and how (docs/dev/mcp.md section 4): the projects folder plus --allow-dir folders,
// project names and paths (symlinks resolved), script names, script writes and settings an agent may change.
//
// C++17 (the unit tests include this header).
#pragma once

#include "tooling.h"

#include <cstddef>
#include <string>
#include <vector>

namespace vyrn::mcp {

struct Options {
  std::string projects_dir;              // absolute, symlinks resolved; exists
  std::vector<std::string> allow_dirs;   // the same, from --allow-dir
  std::string data_dir;                  // the folder with python/ and templates/ ("" = let the core look)
  std::string python;                    // "" = vyrn::platform::default_python()
  int max_trainings = 2;                 // trainings at once in one server: 1 or 2 (a Pi 5 has 4 cores)
  double call_seconds = 60;              // how long vyrn_check, vyrn_watch and vyrn_stop may take
};

// Parses run_mcp_server()'s arguments; argv[0] is the command's name ("vyrn-mcp", or "mcp" from vyrn-cli).
// Creates the projects folder when it is missing. Returns 0 = run; 1 = done, print `message` (help, version) on
// stdout; 2 = usage error, print `message` on stderr.
int parse_options(int argc, char** argv, Options& out, std::string& message);

constexpr size_t kMaxScriptBytes = 100 * 1024;   // vyrn_write_script

struct Project {
  std::string path;     // absolute, symlinks resolved
  std::string folder;   // the folder's own name, e.g. "Catch-numbers"
  bool sim = false;     // it has world.py: a simulation (else a real game on the screen)
};

class Workspace {
 public:
  explicit Workspace(Options options);
  const Options& options() const { return options_; }

  // True when `path`, symlinks resolved, is inside the projects folder or an --allow-dir folder.
  bool allowed(const std::string& path) const;
  // "/home/u/Vyrn" or "/home/u/Vyrn and /tmp/x", for messages.
  std::string allowed_text() const;

  // A `project` argument: a folder name in the projects folder (or an --allow-dir folder), a project's name from
  // its vyrn.json, or an absolute path ("~/" works) inside the allowed folders. False + a plain reason otherwise.
  bool resolve(const std::string& arg, Project& out, std::string& why) const;

  // `file` must be a plain *.py name directly inside the project, not a link, not hidden.
  bool script_target(const Project& p, const std::string& file, std::string& path, std::string& why) const;
  // Writes atomically (temporary file + rename). Refuses links, folders, read-only files and > 100 KB.
  bool write_script(const Project& p, const std::string& file, const std::string& content, std::string& why) const;
  // The project's *.py files: inputs.py, responses.py, points.py, world.py first, then the rest by name.
  std::vector<std::string> script_names(const Project& p) const;
  bool read_script(const Project& p, const std::string& file, std::string& text, std::string& why) const;

 private:
  Options options_;
};

// A folder with vyrn.json or inputs.py.
bool is_project_dir(const std::string& path);

// vyrn.json "agents": {"screen": true}: the user lets agents' Checks read the screen in this real-game project.
// Without it, Check runs with screen reading switched off in the worker. Only the user sets it, in the app.
bool screen_allowed(const std::string& project);
// What an agent is told when a Check stopped because the scripts read the screen without that permission.
std::string screen_opt_in_help();
// Part of the worker's error when a run that may not read the screen tries to (python/vyrn/_screen.py NO_SCREEN).
inline constexpr const char* kNoScreen = "can't read the screen in this run";
// What an agent is told when it tries to train (watch = false) or watch a real game.
std::string real_game_refusal(bool watch);

// vyrn_settings: turns the agent's `values` into top-level vyrn.json changes, given vyrn.json as it is (an object).
// "advanced" is merged key by key (null removes a key). Refuses unknown keys and agents.* with a plain reason.
bool settings_changes(const json& current, const json& values, json& changes, std::string& why);

}  // namespace vyrn::mcp
