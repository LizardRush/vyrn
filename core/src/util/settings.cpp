#include "util/settings.h"

#include "util/files.h"

#include <cmath>
#include <filesystem>
#include <sstream>

namespace vyrn::util {

namespace fs = std::filesystem;

namespace {

std::string number_text(double v) {
  if (v == std::floor(v) && std::fabs(v) < 1e15) return std::to_string(static_cast<long long>(v));
  std::ostringstream os;
  os << v;
  return os.str();
}

// A number setting: kept when `valid`, else the default stays and a warning says what was expected.
template <typename T, typename Valid>
void read_number(const ordered_json& j, const char* key, const char* expected, Valid valid, T& field,
                 std::vector<std::string>& warnings) {
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return;
  const bool number = it->is_number() && !it->is_boolean();
  const double v = number ? it->template get<double>() : 0.0;
  if (!number || !std::isfinite(v) || !valid(v)) {
    warnings.push_back(std::string("vyrn.json: \"") + key + "\" should be " + expected + ", so Vyrn uses " +
                       number_text(static_cast<double>(field)) + ".");
    return;
  }
  field = static_cast<T>(v);
}

void read_text(const ordered_json& j, const char* key, std::string& field, std::vector<std::string>& warnings) {
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return;
  if (!it->is_string()) {
    warnings.push_back(std::string("vyrn.json: \"") + key + "\" should be text in quotes, so Vyrn ignores it.");
    return;
  }
  field = it->get<std::string>();
}

}  // namespace

std::string settings_path(const std::string& project_dir) { return (fs::path(project_dir) / "vyrn.json").string(); }

ProjectSettings parse_settings(const std::string& text) {
  ProjectSettings s;
  s.exists = true;
  ordered_json j;
  try {
    j = ordered_json::parse(text);
  } catch (const nlohmann::json::exception& e) {
    s.readable = false;
    std::string detail = e.what();
    const auto colon = detail.rfind(": ");
    if (colon != std::string::npos) detail = detail.substr(colon + 2);
    s.warnings.push_back("vyrn.json isn't valid JSON (" + detail + "), so Vyrn uses the default settings.");
    return s;
  }
  if (!j.is_object()) {
    s.readable = false;
    s.warnings.push_back("vyrn.json should hold one {...} object, so Vyrn uses the default settings.");
    return s;
  }
  s.raw = j;
  auto& w = s.warnings;
  read_text(j, "name", s.name, w);
  read_text(j, "template", s.template_id, w);
  read_text(j, "python", s.python, w);
  read_text(j, "start_program", s.start_program, w);
  read_text(j, "game_window", s.game_window, w);
  read_text(j, "brain", s.brain, w);
  if (!s.brain.empty() && !is_safe_name(s.brain)) {
    w.push_back("vyrn.json: \"brain\" should be the name of a folder in brains/ (letters, digits, - and _), "
                "so Vyrn ignores it.");
    s.brain.clear();
  }
  read_number(j, "decisions_per_second", "a number above 0, up to 1000",
              [](double v) { return v > 0 && v <= 1000; }, s.decisions_per_second, w);
  read_number(j, "round_seconds", "a number of seconds above 0", [](double v) { return v > 0 && v <= 86400; },
              s.round_seconds, w);
  read_number(j, "round_ticks", "a whole number of ticks, at least 1",
              [](double v) { return v >= 1 && v <= 1e12 && v == std::floor(v); }, s.round_ticks, w);
  auto adv = j.find("advanced");
  if (adv != j.end() && !adv->is_null()) s.advanced = nlohmann::json::parse(adv->dump());
  return s;
}

ProjectSettings read_settings(const std::string& project_dir) {
  const std::string path = settings_path(project_dir);
  std::error_code ec;
  if (!fs::exists(path, ec)) return ProjectSettings{};
  std::string text, why;
  if (!read_text_file(path, text, &why)) {
    ProjectSettings s;
    s.exists = true;
    s.readable = false;
    s.warnings.push_back("Vyrn can't read vyrn.json (" + why + "), so it uses the default settings.");
    return s;
  }
  return parse_settings(text);
}

bool update_settings(const std::string& project_dir, const ordered_json& changes, std::string* why) {
  const std::string path = settings_path(project_dir);
  ordered_json j = ordered_json::object();
  std::error_code ec;
  if (fs::exists(path, ec)) {
    std::string text, problem;
    if (!read_text_file(path, text, &problem)) {
      if (why) *why = "Vyrn can't read vyrn.json (" + problem + ")";
      return false;
    }
    try {
      j = ordered_json::parse(text);
    } catch (const nlohmann::json::exception&) {
      if (why) *why = "vyrn.json isn't valid JSON, so Vyrn won't change it";
      return false;
    }
    if (!j.is_object()) {
      if (why) *why = "vyrn.json doesn't hold a {...} object, so Vyrn won't change it";
      return false;
    }
  }
  for (auto it = changes.begin(); it != changes.end(); ++it) j[it.key()] = it.value();
  try {
    write_text_file_atomic(path, dump_settings(j));
  } catch (const std::exception& e) {
    if (why) *why = e.what();
    return false;
  }
  return true;
}

std::string dump_settings(const ordered_json& j) { return j.dump(2) + "\n"; }

}  // namespace vyrn::util
