// A project's settings file, vyrn.json (docs/dev/protocol.md section 8). No torch in this header.
//
// Every key is optional. Vyrn keeps keys it doesn't know (and the order of all keys) when it writes the file.
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace vyrn::util {

using ordered_json = nlohmann::ordered_json;

struct ProjectSettings {
  ordered_json raw = ordered_json::object();   // the file as read, keys in file order
  bool exists = false;                         // there is a vyrn.json
  bool readable = true;                        // false: it isn't a JSON object (Vyrn never rewrites it then)
  // start_program: only the Qt app starts it (app/StartProgram), for a project the person trusts. The core,
  // vyrn-cli and the MCP server never run it: AI agents must never be able to launch a program this way.
  std::string name, template_id, python, brain, start_program, game_window;
  double decisions_per_second = 10.0;
  double round_seconds = 60.0;                 // real time: round length
  int64_t round_ticks = 1000;                  // simulations: round length
  nlohmann::json advanced;                     // the "advanced" value as written (null when missing)
  std::vector<std::string> warnings;           // plain English: values Vyrn replaced by a default
};

std::string settings_path(const std::string& project_dir);

// Never throws: problems become `warnings` and defaults.
ProjectSettings parse_settings(const std::string& text);
// A missing vyrn.json gives the defaults without a warning.
ProjectSettings read_settings(const std::string& project_dir);

// Sets top-level keys and keeps every other key, in order; creates vyrn.json when it is missing. Returns false
// with a plain-English reason when the existing file isn't a JSON object or can't be written: Vyrn never
// overwrites a file it can't understand, so the user's text is never lost.
bool update_settings(const std::string& project_dir, const ordered_json& changes, std::string* why);

// What Vyrn writes: 2-space indent and a final newline.
std::string dump_settings(const ordered_json& j);

}  // namespace vyrn::util
