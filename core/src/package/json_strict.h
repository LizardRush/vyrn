// Strict JSON parsing for package data: no repeated keys inside an object (two readers could disagree about which
// copy counts) and a bounded nesting depth.
#pragma once

#include <nlohmann/json.hpp>

#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vyrn::package_detail {

struct JsonError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// A key from the file, fit for a message: printable ASCII only (anything else, terminal escapes included, becomes
// '?') and at most 40 characters. The text came from whoever made the file.
inline std::string shown_json_key(const std::string& key) {
  std::string out;
  for (const char c : key) {
    if (out.size() >= 40) return out + "...";
    out += (c >= 0x20 && c < 0x7F) ? c : '?';
  }
  return out;
}

template <class Json>
Json parse_json_strict(std::string_view text, size_t max_depth = 32) {
  std::vector<std::set<std::string>> open;   // the keys seen in each open object (arrays get an empty set)
  auto callback = [&](int /*depth*/, typename Json::parse_event_t event, Json& parsed) -> bool {
    using Event = typename Json::parse_event_t;
    switch (event) {
      case Event::object_start:
      case Event::array_start:
        if (open.size() >= max_depth) throw JsonError("it is nested too deeply");
        open.emplace_back();
        break;
      case Event::object_end:
      case Event::array_end:
        if (!open.empty()) open.pop_back();
        break;
      case Event::key:
        if (!open.empty() && !open.back().insert(parsed.template get<std::string>()).second) {
          throw JsonError("it repeats the key \"" + shown_json_key(parsed.template get<std::string>()) + "\"");
        }
        break;
      default:
        break;
    }
    return true;
  };
  try {
    return Json::parse(text.begin(), text.end(), callback, /*allow_exceptions=*/true, /*ignore_comments=*/false);
  } catch (const typename Json::exception&) {
    throw JsonError("it isn't valid JSON");
  }
}

}  // namespace vyrn::package_detail
