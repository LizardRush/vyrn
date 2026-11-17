// Where agents may work, and how (docs/dev/mcp.md section 4).
#include "workspace.h"

#include "vyrn/platform.h"
#include "vyrn/project.h"
#include "vyrn/session.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>

#ifndef VYRN_VERSION
#define VYRN_VERSION "0.0.0"
#endif

namespace vyrn::mcp {

namespace {

namespace fs = std::filesystem;

std::string usage(const std::string& prog) {
  return "Usage: " + prog +
         " [--projects-dir DIR] [--allow-dir DIR]... [--data-dir DIR] [--max-trainings N]\n"
         "\n"
         "The Vyrn MCP server for AI coding agents (Claude Code, Codex, Gemini CLI, ...): JSON-RPC 2.0 on\n"
         "stdin/stdout, one message per line. Logs go to stderr.\n"
         "\n"
         "  --projects-dir DIR   where projects live (default: $VYRN_PROJECTS, else ~/Vyrn)\n"
         "  --allow-dir DIR      also allow projects inside DIR (repeatable)\n"
         "  --data-dir DIR       the folder with python/ and templates/ (default: found automatically)\n"
         "  --max-trainings N    trainings at once: 1 or 2 (default 2)\n"
         "\n"
         "Agents can create projects, write their scripts (which runs Python as you), Check them, and train and\n"
         "watch simulations. They never train or watch real games, and never press keys.\n";
}

std::string expand_home(const std::string& p) {
  if (p == "~" || p.rfind("~/", 0) == 0) {
    const std::string home = vyrn::platform::home_dir();
    if (!home.empty()) return home + p.substr(1);
  }
  return p;
}

// An existing folder, absolute with symlinks resolved; "" when it isn't one.
std::string real_dir(const std::string& p, bool create) {
  std::error_code ec;
  fs::path path = fs::absolute(expand_home(p), ec);
  if (ec) return "";
  if (create) fs::create_directories(path, ec);
  if (!fs::is_directory(path, ec)) return "";
  const fs::path real = fs::canonical(path, ec);
  return ec ? "" : real.string();
}

bool ends_with(const std::string& s, const std::string& tail) {
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

std::string trim(const std::string& s) {
  const auto first = s.find_first_not_of(" \t");
  if (first == std::string::npos) return "";
  const auto last = s.find_last_not_of(" \t");
  return s.substr(first, last - first + 1);
}

// For messages: control characters as '?', at most 80 bytes.
std::string shown(const std::string& s) {
  std::string out;
  for (const char c : s) {
    if (out.size() >= 80) return out + "...";
    out += static_cast<unsigned char>(c) < 0x20 || c == 0x7f ? '?' : c;
  }
  return out;
}

int script_rank(const std::string& name) {
  static const char* const order[] = {"inputs.py", "responses.py", "points.py", "world.py"};
  for (int i = 0; i < 4; ++i) {
    if (name == order[i]) return i;
  }
  return 4;
}

std::string kb(size_t bytes) {
  char text[32];
  std::snprintf(text, sizeof text, "%.1f KB", static_cast<double>(bytes) / 1024.0);
  return text;
}

struct AdvancedField {
  const char* name;
  bool whole;
  double lo, hi;
};

// vyrn.json "advanced": the brain's settings and the ranges Vyrn accepts (core/src/brain/dqn.cpp visit_fields).
constexpr AdvancedField kAdvanced[] = {
    {"batch", true, 1, 65536},          {"lr", false, 1e-9, 1.0},         {"adam_eps", false, 1e-12, 1.0},
    {"target_every", true, 1, 1e9},     {"n_step", true, 1, 100},         {"gamma", false, 0, 1},
    {"grad_clip", false, 1e-6, 1e6},    {"learning_starts", true, 0, 1e12}, {"updates_per_tick", false, 0, 64},
    {"explore_ticks", true, 0, 1e12},   {"eps_start", false, 0, 1},       {"eps_end", false, 0, 1},
    {"eps_watch", false, 0, 1},         {"buffer", true, 100, 1e8},       {"frames", true, 1, 16},
};

std::string number_text(double v) {
  char text[32];
  std::snprintf(text, sizeof text, "%g", v);
  return text;
}

std::string advanced_names() {
  std::string out;
  for (const auto& f : kAdvanced) out += (out.empty() ? "" : ", ") + std::string(f.name);
  return out;
}

bool number_in(const json& v, double lo_exclusive, double hi, bool whole, double* out) {
  if (!v.is_number()) return false;
  const double d = v.get<double>();
  if (!std::isfinite(d) || !(d > lo_exclusive) || d > hi) return false;
  if (whole && d != std::floor(d)) return false;
  *out = d;
  return true;
}

}  // namespace

int parse_options(int argc, char** argv, Options& out, std::string& message) {
  std::string prog = "vyrn-mcp";
  if (argc > 0 && argv[0] && fs::path(argv[0]).filename() == "mcp") prog = "vyrn-cli mcp";
  std::string projects, data;
  std::vector<std::string> allow;
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i] ? argv[i] : "", value;
    bool has_value = false;
    if (arg.rfind("--", 0) == 0) {
      const auto eq = arg.find('=');
      if (eq != std::string::npos) {
        value = arg.substr(eq + 1);
        arg = arg.substr(0, eq);
        has_value = true;
      }
    }
    if (arg == "-h" || arg == "--help") {
      message = usage(prog);
      return 1;
    }
    if (arg == "--version") {
      message = prog + " " + VYRN_VERSION + "\n";
      return 1;
    }
    if (arg != "--projects-dir" && arg != "--allow-dir" && arg != "--data-dir" && arg != "--max-trainings") {
      message = "unknown option \"" + arg + "\"\n\n" + usage(prog);
      return 2;
    }
    if (!has_value) {
      if (i + 1 >= argc || !argv[i + 1]) {
        message = arg + " needs a value\n\n" + usage(prog);
        return 2;
      }
      value = argv[++i];
    }
    if (value.empty()) {
      message = arg + " needs a value\n\n" + usage(prog);
      return 2;
    }
    if (arg == "--projects-dir") {
      projects = value;
    } else if (arg == "--allow-dir") {
      allow.push_back(value);
    } else if (arg == "--data-dir") {
      data = value;
    } else {
      int n = 0;
      const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), n);
      if (ec != std::errc() || end != value.data() + value.size() || n < 1) {
        message = "--max-trainings needs a whole number of at least 1, not \"" + value + "\"\n";
        return 2;
      }
      if (n > 2) std::fprintf(stderr, "vyrn mcp: --max-trainings %d is more than this server allows; using 2.\n", n);
      o.max_trainings = std::min(n, 2);
    }
  }
  if (!projects.empty()) {
    o.projects_dir = real_dir(projects, true);
    if (o.projects_dir.empty()) {
      message = "--projects-dir " + projects + " is not a folder Vyrn can use (it couldn't be made either)\n";
      return 2;
    }
  } else {
    try {
      o.projects_dir = real_dir(projects_root(), true);
    } catch (const std::exception& e) {
      message = std::string("Vyrn can't use its projects folder: ") + e.what() + "\n";
      return 2;
    }
    if (o.projects_dir.empty()) {
      message = "Vyrn can't use its projects folder (~/Vyrn): pass --projects-dir DIR\n";
      return 2;
    }
  }
  for (const auto& a : allow) {
    const std::string dir = real_dir(a, false);
    if (dir.empty()) {
      message = "--allow-dir " + a + " is not a folder\n";
      return 2;
    }
    if (dir == "/") {
      message = "--allow-dir / would let agents work anywhere; give a smaller folder\n";
      return 2;
    }
    if (std::find(o.allow_dirs.begin(), o.allow_dirs.end(), dir) == o.allow_dirs.end()) o.allow_dirs.push_back(dir);
  }
  if (!data.empty()) {
    o.data_dir = real_dir(data, false);
    if (o.data_dir.empty()) {
      message = "--data-dir " + data + " is not a folder\n";
      return 2;
    }
  } else {
    o.data_dir = find_data_dir(argc > 0 ? argv[0] : nullptr);
  }
  out = std::move(o);
  return 0;
}

Workspace::Workspace(Options options) : options_(std::move(options)) {}

bool Workspace::allowed(const std::string& path) const {
  if (is_inside(options_.projects_dir, path)) return true;
  for (const auto& dir : options_.allow_dirs) {
    if (is_inside(dir, path)) return true;
  }
  return false;
}

std::string Workspace::allowed_text() const {
  std::string text = options_.projects_dir;
  for (size_t i = 0; i < options_.allow_dirs.size(); ++i) {
    text += (i + 1 == options_.allow_dirs.size() ? " and " : ", ") + options_.allow_dirs[i];
  }
  return text;
}

bool Workspace::resolve(const std::string& raw, Project& out, std::string& why) const {
  const std::string arg = trim(raw);
  if (arg.empty()) {
    why = "Please give a project: its folder name (vyrn_list_projects shows them) or its absolute path.";
    return false;
  }
  for (const char c : arg) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
      why = "A project name can't contain control characters such as line breaks.";
      return false;
    }
  }
  std::vector<fs::path> candidates;
  if (arg.find('/') != std::string::npos || arg == "~") {
    const std::string path = expand_home(arg);
    if (path.empty() || path[0] != '/') {
      why = "\"" + shown(arg) + "\" is a relative path. Give the project's folder name (for example Catch-numbers) "
            "or its absolute path inside " + allowed_text() + ".";
      return false;
    }
    if (!allowed(path)) {
      why = "\"" + shown(arg) + "\" is outside the folders Vyrn agents may use (" + allowed_text() +
            "), so Vyrn won't touch it.";
      return false;
    }
    candidates.emplace_back(path);
  } else {
    if (arg[0] == '.') {
      why = "\"" + shown(arg) + "\" isn't a project name: hidden folders and \"..\" can't be used. Give the "
            "project's folder name (vyrn_list_projects shows them).";
      return false;
    }
    std::vector<std::string> roots{options_.projects_dir};
    roots.insert(roots.end(), options_.allow_dirs.begin(), options_.allow_dirs.end());
    for (const auto& root : roots) candidates.push_back(fs::path(root) / arg);
    const std::string folder = folder_name_for(arg);
    if (folder != arg) {
      for (const auto& root : roots) candidates.push_back(fs::path(root) / folder);
    }
    for (const auto& dir : options_.allow_dirs) {   // an --allow-dir folder may be a project itself
      if (fs::path(dir).filename() == arg) candidates.emplace_back(dir);
    }
    for (const auto& root : roots) {   // the name in vyrn.json, e.g. "Catch (numbers)"
      try {
        for (const auto& info : list_projects(root)) {
          if (info.name == arg) candidates.emplace_back(info.path);
        }
      } catch (const std::exception&) {
      }
    }
  }
  for (const auto& c : candidates) {
    std::error_code ec;
    if (!fs::exists(c, ec)) continue;   // also false for a link that leads nowhere
    if (!allowed(c.string())) {
      why = "\"" + shown(arg) + "\" leads outside the folders Vyrn agents may use (" + allowed_text() +
            "), so Vyrn won't touch it.";
      return false;
    }
    const fs::path real = fs::canonical(c, ec);
    if (ec) continue;
    if (!fs::is_directory(real, ec)) {
      why = "\"" + shown(arg) + "\" is a file, not a project folder.";
      return false;
    }
    if (!is_project_dir(real.string())) {
      why = "\"" + shown(arg) + "\" is a folder but not a Vyrn project (it has no vyrn.json or inputs.py).";
      return false;
    }
    out.path = real.string();
    out.folder = real.filename().string();
    out.sim = fs::is_regular_file(real / "world.py", ec);
    return true;
  }
  why = "There is no project \"" + shown(arg) + "\" in " + allowed_text() +
        ". vyrn_list_projects shows the projects; vyrn_create_project makes a new one.";
  return false;
}

bool Workspace::script_target(const Project& p, const std::string& file, std::string& path, std::string& why) const {
  try {
    path = safe_join(p.path, file);
  } catch (const std::invalid_argument& e) {
    why = e.what();
    return false;
  }
  if (file.size() <= 3 || !ends_with(file, ".py")) {
    why = "\"" + shown(file) + "\" isn't a Python file. Agents can write inputs.py, responses.py, points.py, "
          "world.py or another *.py helper directly in the project folder.";
    return false;
  }
  if (!is_inside(p.path, path)) {
    why = file + " leads outside the project folder, so Vyrn won't use it.";
    return false;
  }
  std::error_code ec;
  const auto st = fs::symlink_status(path, ec);
  if (!ec && fs::is_symlink(st)) {
    why = file + " is a link to another file. Vyrn only writes plain files; ask the user to replace the link.";
    return false;
  }
  if (!ec && fs::exists(st) && !fs::is_regular_file(st)) {
    why = file + " is not a plain file.";
    return false;
  }
  return true;
}

bool Workspace::write_script(const Project& p, const std::string& file, const std::string& content,
                             std::string& why) const {
  std::string path;
  if (!script_target(p, file, path, why)) return false;
  if (content.size() > kMaxScriptBytes) {
    why = file + " would be " + kb(content.size()) + ", and a script can be at most 100 KB. Move code into a "
          "helper module (another *.py file) instead.";
    return false;
  }
  if (content.find('\0') != std::string::npos) {
    why = "The text contains a NUL character, which a Python file can't hold.";
    return false;
  }
  int mode = 0644;
  std::error_code ec;
  const fs::file_status status = fs::status(path, ec);   // follows a symlink, like stat()
  if (fs::exists(status)) {
    if (!vyrn::platform::is_writable(path)) {
      why = file + " is read-only, so Vyrn won't change it. Ask the user to make it writable.";
      return false;
    }
    mode = static_cast<int>(status.permissions() & fs::perms::mask) & 0777;
  }
  // A leftover of this pid is replaced; the new file goes in place with an atomic rename.
  const std::string temp = p.path + "/." + file + ".vyrn-new-" + std::to_string(vyrn::platform::current_pid());
  if (const int err = vyrn::platform::write_file_atomic(path, temp, content, mode); err != 0) {
    why = "Vyrn couldn't write " + file + ": " + std::strerror(err) + ".";
    return false;
  }
  return true;
}

std::vector<std::string> Workspace::script_names(const Project& p) const {
  std::vector<std::string> names;
  std::error_code ec;
  for (fs::directory_iterator it(p.path, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.empty() || name[0] == '.' || name.size() <= 3 || !ends_with(name, ".py")) continue;
    std::error_code ec2;
    if (!fs::is_regular_file(it->path(), ec2)) continue;   // follows links; is_inside() keeps them in the project
    if (!is_inside(p.path, it->path().string())) continue;
    names.push_back(name);
  }
  std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
    const int ra = script_rank(a), rb = script_rank(b);
    return ra != rb ? ra < rb : a < b;
  });
  return names;
}

bool Workspace::read_script(const Project& p, const std::string& file, std::string& text, std::string& why) const {
  std::string path;
  try {
    path = safe_join(p.path, file);
  } catch (const std::invalid_argument& e) {
    why = e.what();
    return false;
  }
  std::error_code ec;
  if (!ends_with(file, ".py") || !is_inside(p.path, path) || !fs::is_regular_file(path, ec)) {
    why = "There is no script " + shown(file) + " in " + p.folder + ".";
    return false;
  }
  const auto size = fs::file_size(path, ec);
  if (ec || size > 4 * kMaxScriptBytes) {
    why = file + " is too big to show (" + kb(ec ? 0 : size) + ").";
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    why = "Vyrn couldn't read " + file + ".";
    return false;
  }
  text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

bool is_project_dir(const std::string& path) {
  std::error_code ec;
  const fs::path dir(path);
  return fs::is_directory(dir, ec) &&
         (fs::is_regular_file(dir / "vyrn.json", ec) || fs::is_regular_file(dir / "inputs.py", ec));
}

bool screen_allowed(const std::string& project) {
  std::string text;
  try {
    text = read_settings_text(project);
  } catch (const std::exception&) {
    return false;
  }
  const json j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return false;
  const auto agents = j.find("agents");
  if (agents == j.end() || !agents->is_object()) return false;
  const auto screen = agents->find("screen");
  return screen != agents->end() && screen->is_boolean() && screen->get<bool>();
}

std::string screen_opt_in_help() {
  return "This is a real-game project (it has no world.py), and its scripts look at the screen. The user hasn't "
         "allowed AI agents to do that for this project. To allow it, the user opens the project in the Vyrn app, "
         "Settings -> AI coding agents, and turns on screen access (it sets \"agents\": {\"screen\": true} in "
         "vyrn.json; agents can't set it). I can still read and write the scripts.";
}

std::string real_game_refusal(bool watch) {
  if (watch) {
    return "Watching a real game presses keys on your desktop, so start it yourself in the Vyrn app (Watch). "
           "I can still help with the scripts and Check.";
  }
  return "Real-game training presses keys on your desktop, so start it yourself in the Vyrn app (Train). "
         "I can still help with the scripts and Check.";
}

bool settings_changes(const json& current, const json& values, json& changes, std::string& why) {
  changes = json::object();
  if (!values.is_object()) {
    why = "values must be an object such as {\"round_ticks\": 500}.";
    return false;
  }
  for (auto it = values.begin(); it != values.end(); ++it) {
    const std::string& key = it.key();
    const json& v = it.value();
    double d = 0;
    if (key == "agents" || key.rfind("agents.", 0) == 0) {
      why = "Only the user can change \"agents\" (whether AI agents may look at the screen): in the Vyrn app, "
            "Settings -> AI coding agents.";
      return false;
    }
    if (key == "decisions_per_second") {
      if (!number_in(v, 0, 1000, false, &d)) {
        why = "decisions_per_second must be a number above 0, up to 1000.";
        return false;
      }
      changes[key] = v;   // as written (10 stays 10)
    } else if (key == "round_seconds") {
      if (!number_in(v, 0, 86400, false, &d)) {
        why = "round_seconds must be a number of seconds above 0, up to 86400.";
        return false;
      }
      changes[key] = v;
    } else if (key == "round_ticks") {
      if (!number_in(v, 0, 1e12, true, &d)) {
        why = "round_ticks must be a whole number of ticks, at least 1.";
        return false;
      }
      changes[key] = static_cast<int64_t>(d);
    } else if (key == "advanced") {
      if (!v.is_object()) {
        why = "advanced must be an object such as {\"explore_ticks\": 10000}.";
        return false;
      }
      json merged = json::object();
      if (current.is_object() && current.contains("advanced") && current["advanced"].is_object()) {
        merged = current["advanced"];
      }
      for (auto a = v.begin(); a != v.end(); ++a) {
        const AdvancedField* field = nullptr;
        for (const auto& f : kAdvanced) {
          if (a.key() == f.name) field = &f;
        }
        if (!field) {
          why = "advanced." + shown(a.key()) + " isn't a setting Vyrn knows. The advanced settings are " +
                advanced_names() + ".";
          return false;
        }
        if (a.value().is_null()) {   // back to Vyrn's default
          merged.erase(a.key());
          continue;
        }
        const json& x = a.value();
        const double value = x.is_number() ? x.get<double>() : std::nan("");
        if (!x.is_number() || !std::isfinite(value)) {
          why = "advanced." + a.key() + " must be a number (or null for Vyrn's default).";
          return false;
        }
        if (field->whole && value != std::floor(value)) {
          why = "advanced." + a.key() + " must be a whole number.";
          return false;
        }
        if (value < field->lo || value > field->hi) {
          why = "advanced." + a.key() + " must be between " + number_text(field->lo) + " and " +
                number_text(field->hi) + ".";
          return false;
        }
        if (field->whole) {
          merged[a.key()] = static_cast<int64_t>(value);
        } else {
          merged[a.key()] = value;
        }
      }
      changes["advanced"] = merged;
    } else if (key.rfind("advanced.", 0) == 0) {
      why = "Write advanced settings as {\"advanced\": {\"" + shown(key.substr(9)) + "\": ...}}.";
      return false;
    } else if (key == "link") {
      // Vyrn Link (docs/dev/link.md): an object is merged into the current "link" (null puts one setting back to
      // its default), true turns it on as it was (port 7457 when it was off), false turns it off.
      if (v.is_boolean() && !v.get<bool>()) {
        changes["link"] = false;
        continue;
      }
      if (!v.is_boolean() && !v.is_object()) {
        why = "link must be an object such as {\"port\": 7457, \"lockstep\": true, \"frames_per_step\": 1}, or "
              "false to turn Vyrn Link off.";
        return false;
      }
      json merged = json::object();
      if (current.is_object() && current.contains("link") && current["link"].is_object()) merged = current["link"];
      if (merged.empty()) merged["port"] = 7457;
      if (v.is_object()) {
        for (auto l = v.begin(); l != v.end(); ++l) {
          const json& x = l.value();
          double n = 0;
          if (l.key() != "port" && l.key() != "lockstep" && l.key() != "frames_per_step") {
            why = "link." + shown(l.key()) + " isn't a Vyrn Link setting. They are port, lockstep and "
                  "frames_per_step.";
            return false;
          }
          if (x.is_null()) {   // back to Vyrn's default
            merged.erase(l.key());
          } else if (l.key() == "port") {
            if (!number_in(x, 1023, 65535, true, &n)) {
              why = "link.port must be a whole number from 1024 to 65535.";
              return false;
            }
            merged["port"] = static_cast<int>(n);
          } else if (l.key() == "lockstep") {
            if (!x.is_boolean()) {
              why = "link.lockstep must be true or false.";
              return false;
            }
            merged["lockstep"] = x;
          } else {
            if (!number_in(x, 0, 8, true, &n)) {
              why = "link.frames_per_step must be a whole number from 1 to 8.";
              return false;
            }
            merged["frames_per_step"] = static_cast<int>(n);
          }
        }
      }
      changes["link"] = merged;
    } else {
      why = "Agents can't change \"" + shown(key) + "\". They may change decisions_per_second, round_ticks, "
            "round_seconds, advanced{...} and link{...}; the user changes the rest in the Vyrn app.";
      return false;
    }
  }
  return true;
}

}  // namespace vyrn::mcp
