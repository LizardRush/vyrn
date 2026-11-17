// The tool table, argument checking and result helpers (docs/dev/mcp.md section 3).
#include "tooling.h"

#include <chrono>
#include <cmath>
#include <stdexcept>
#include <thread>

namespace vyrn::mcp {

namespace {

std::string list_names(const json& properties) {
  std::string out;
  size_t i = 0;
  for (auto it = properties.begin(); it != properties.end(); ++it, ++i) {
    if (i > 0) out += i + 1 == properties.size() ? " and " : ", ";
    out += it.key();
  }
  return out;
}

std::string shown_value(const json& v) {
  std::string text = dump_line(v);
  if (text.size() > 60) text = text.substr(0, 57) + "...";
  return text;
}

std::string number_text(double v) {
  if (v == std::floor(v) && std::fabs(v) < 1e15) return std::to_string(static_cast<long long>(v));
  std::string s = std::to_string(v);
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s;
}

const char* type_words(const std::string& type) {
  if (type == "string") return "text";
  if (type == "integer") return "a whole number";
  if (type == "number") return "a number";
  if (type == "boolean") return "true or false";
  if (type == "object") return "an object {...}";
  if (type == "array") return "a list [...]";
  return type.c_str();
}

// Checks one value against its schema; may normalise it (5.0 -> 5 for integers). "" when fine.
std::string check_value(const std::string& tool, const std::string& name, const json& schema, json& v) {
  const std::string where = tool + " argument \"" + name + "\"";
  const std::string type = schema.value("type", "");
  bool type_ok = true;
  if (type == "string") {
    type_ok = v.is_string();
  } else if (type == "integer") {
    if (v.is_number_float()) {
      const double d = v.get<double>();
      if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 9e15) v = static_cast<int64_t>(d);
    }
    type_ok = v.is_number_integer();
  } else if (type == "number") {
    type_ok = v.is_number();
  } else if (type == "boolean") {
    type_ok = v.is_boolean();
  } else if (type == "object") {
    type_ok = v.is_object();
  } else if (type == "array") {
    type_ok = v.is_array();
  }
  if (!type_ok) return where + " must be " + type_words(type) + ", not " + shown_value(v) + ".";

  if (auto e = schema.find("enum"); e != schema.end() && e->is_array()) {
    bool found = false;
    std::string options;
    for (const auto& option : *e) {
      if (option == v) found = true;
      options += (options.empty() ? "" : ", ") + dump_line(option);
    }
    if (!found) return where + " must be one of " + options + ", not " + shown_value(v) + ".";
  }
  if (v.is_number()) {
    const double d = v.get<double>();
    if (!std::isfinite(d)) return where + " must be a finite number.";
    const bool has_min = schema.contains("minimum"), has_max = schema.contains("maximum");
    const double lo = has_min ? schema["minimum"].get<double>() : 0, hi = has_max ? schema["maximum"].get<double>() : 0;
    if ((has_min && d < lo) || (has_max && d > hi)) {
      if (has_min && has_max) return where + " must be between " + number_text(lo) + " and " + number_text(hi) + ".";
      if (has_min) return where + " must be at least " + number_text(lo) + ".";
      return where + " must be at most " + number_text(hi) + ".";
    }
  }
  if (v.is_string()) {
    const size_t n = v.get_ref<const std::string&>().size();
    if (schema.contains("minLength") && n < schema["minLength"].get<size_t>()) {
      return n == 0 ? where + " can't be empty." : where + " is too short.";
    }
    if (schema.contains("maxLength") && n > schema["maxLength"].get<size_t>()) {
      return where + " is too long (at most " + std::to_string(schema["maxLength"].get<size_t>()) + " characters).";
    }
  }
  return "";
}

}  // namespace

bool SleepWaiter::wait(double seconds) {
  if (seconds > 0) std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
  return true;
}

ToolResult ToolResult::ok(std::string text, json data) {
  ToolResult r;
  r.text = std::move(text);
  r.data = std::move(data);
  return r;
}

ToolResult ToolResult::error(std::string text, json data) {
  ToolResult r;
  r.is_error = true;
  r.text = std::move(text);
  r.data = std::move(data);
  return r;
}

void ToolTable::add(ToolDef tool) {
  if (tool.name.empty()) throw std::logic_error("a tool needs a name");
  if (find(tool.name)) throw std::logic_error("two tools are called " + tool.name);
  tools_.push_back(std::move(tool));
}

const ToolDef* ToolTable::find(const std::string& name) const {
  for (const auto& t : tools_) {
    if (t.name == name) return &t;
  }
  return nullptr;
}

json object_schema(const std::vector<Arg>& args) {
  json properties = json::object();
  json required = json::array();
  for (const auto& a : args) {
    properties[a.name] = a.schema;
    if (a.required) required.push_back(a.name);
  }
  json schema = json::object();
  schema["type"] = "object";
  schema["properties"] = properties;
  if (!required.empty()) schema["required"] = required;
  schema["additionalProperties"] = false;
  return schema;
}

std::string validate_arguments(const ToolDef& tool, json& args) {
  if (args.is_null()) args = json::object();
  if (!args.is_object()) return tool.name + " needs its arguments as an object {...}.";
  const json& schema = tool.input_schema;
  const json properties = schema.value("properties", json::object());
  // Unknown names first: a typo such as "minute" should not silently fall back to a default.
  for (auto it = args.begin(); it != args.end(); ++it) {
    if (properties.contains(it.key())) continue;
    if (properties.empty()) return tool.name + " takes no arguments, so \"" + it.key() + "\" can't be used.";
    return tool.name + " has no argument \"" + it.key() + "\". It takes " + list_names(properties) + ".";
  }
  // Null means "not given".
  for (auto it = args.begin(); it != args.end();) {
    if (it.value().is_null()) {
      it = args.erase(it);
    } else {
      ++it;
    }
  }
  for (const auto& name : schema.value("required", json::array())) {
    const std::string key = name.get<std::string>();
    if (!args.contains(key)) {
      const std::string about = properties.contains(key) ? properties[key].value("description", "") : "";
      return tool.name + " needs \"" + key + "\"" + (about.empty() ? "." : ": " + about);
    }
  }
  for (auto it = properties.begin(); it != properties.end(); ++it) {
    if (args.contains(it.key())) {
      std::string problem = check_value(tool.name, it.key(), it.value(), args[it.key()]);
      if (!problem.empty()) return problem;
    } else if (it.value().contains("default")) {
      args[it.key()] = it.value()["default"];
    }
  }
  return "";
}

std::string dump_line(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

namespace {

bool scalar(const json& j) { return !j.is_object() && !j.is_array(); }

// Indented, but short containers and lists of numbers stay on one line: compact for the agent, readable still.
void pretty(const json& j, int indent, bool top, std::string& out) {
  if (scalar(j)) {
    out += dump_line(j);
    return;
  }
  const std::string flat = dump_line(j);
  bool numbers = j.is_array();
  for (const auto& item : j) numbers = numbers && (item.is_number() || item.is_boolean() || item.is_null());
  if (j.empty() || numbers || (!top && flat.size() <= 100)) {
    out += flat;
    return;
  }
  const bool object = j.is_object();
  out += object ? "{\n" : "[\n";
  size_t i = 0;
  for (auto it = j.begin(); it != j.end(); ++it, ++i) {
    out.append(static_cast<size_t>(indent + 2), ' ');
    if (object) out += dump_line(json(it.key())) + ": ";
    pretty(it.value(), indent + 2, false, out);
    if (i + 1 < j.size()) out += ',';
    out += '\n';
  }
  out.append(static_cast<size_t>(indent), ' ');
  out += object ? '}' : ']';
}

}  // namespace

std::string dump_pretty(const json& j) {
  std::string out;
  pretty(j, 0, true, out);
  return out;
}

}  // namespace vyrn::mcp
