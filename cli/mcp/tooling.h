// Tools of the MCP server: the table tools/list shows, argument checking and results (docs/dev/mcp.md section 3).
//
// C++17 (the unit tests include this header). A tool is one ToolDef; the order of the table is the order of
// tools/list. tools.h lists the Vyrn tools; adding a tool is one maker function plus one line there.
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vyrn::mcp {

using json = nlohmann::ordered_json;

// Lets a long tool call wait while the server keeps working: it answers pings, follows trainings and notices
// cancel requests and shutdown signals.
class Waiter {
 public:
  virtual ~Waiter() = default;
  // Lets about `seconds` pass. Returns false when the running call should end now (the client cancelled it, or
  // the server is shutting down); the call then stops what it runs and returns what it has.
  virtual bool wait(double seconds) = 0;
};

// Just sleeps: for tests and for code that runs without a server loop.
class SleepWaiter : public Waiter {
 public:
  bool wait(double seconds) override;
};

struct ToolResult {
  bool is_error = false;
  std::string text;                         // readable text, always sent
  json data;                                // structuredContent (a JSON object), or null
  std::vector<std::vector<uint8_t>> pngs;   // sent as image content (image/png)

  static ToolResult ok(std::string text, json data = nullptr);
  static ToolResult error(std::string text, json data = nullptr);
};

using ToolFn = std::function<ToolResult(const json& args, Waiter& waiter)>;

struct ToolDef {
  std::string name;          // "vyrn_check"
  std::string title;         // "Check a project"
  std::string description;   // for the agent: what it does, what it returns, what to do next
  json input_schema;         // made with object_schema()
  bool read_only = false;    // tool annotations (MCP 2025-03-26 and later)
  bool destructive = false;
  bool idempotent = false;
  ToolFn run;
};

class ToolTable {
 public:
  void add(ToolDef tool);    // throws std::logic_error for a duplicate or empty name
  const std::vector<ToolDef>& all() const { return tools_; }
  const ToolDef* find(const std::string& name) const;

 private:
  std::vector<ToolDef> tools_;
};

// One argument of a tool. `schema` is its JSON Schema: "type" ("string", "integer", "number", "boolean",
// "object"), "description", and optionally "enum", "minimum", "maximum", "minLength", "maxLength", "default".
struct Arg {
  std::string name;
  json schema;
  bool required = false;
};

// {"type":"object","properties":{...},"required":[...],"additionalProperties":false}
json object_schema(const std::vector<Arg>& args);

// Checks `args` against the tool's input schema (the subset object_schema() makes) and fills in defaults; a null
// value counts as missing. Returns "" when they are fine, else a plain-English problem that names what the tool
// takes.
std::string validate_arguments(const ToolDef& tool, json& args);

// One line of JSON (no newlines inside), invalid UTF-8 replaced by U+FFFD.
std::string dump_line(const json& j);
// Indented JSON for readable text (short parts and lists of numbers on one line), invalid UTF-8 replaced.
std::string dump_pretty(const json& j);

}  // namespace vyrn::mcp
