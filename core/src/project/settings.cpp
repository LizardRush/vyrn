// vyrn.json as text, and merging changes into it (the core's util/settings keeps unknown keys and their order).
#include "util/files.h"
#include "util/settings.h"
#include "vyrn/project.h"

#include <filesystem>
#include <system_error>

namespace vyrn {

std::string read_settings_text(const std::string& project) {
  const std::string path = util::settings_path(project);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) return "{}";
  std::string text;
  if (!util::read_text_file(path, text)) return "{}";
  return text;
}

bool merge_settings(const std::string& project, const std::string& json_object_text, std::string* why) {
  util::ordered_json changes;
  try {
    changes = util::ordered_json::parse(json_object_text);
  } catch (const nlohmann::json::exception&) {
    if (why) *why = "The new settings aren't valid JSON, so vyrn.json was not changed.";
    return false;
  }
  if (!changes.is_object()) {
    if (why) *why = "The new settings should be one {...} object, such as {\"round_ticks\": 500}, so vyrn.json was "
                    "not changed.";
    return false;
  }
  std::string reason;
  if (!util::update_settings(project, changes, &reason)) {
    if (reason.empty()) reason = "Vyrn couldn't save vyrn.json";
    if (reason.back() != '.') reason += '.';
    if (why) *why = reason;
    return false;
  }
  if (why) why->clear();
  return true;
}

}  // namespace vyrn
