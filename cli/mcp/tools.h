// The Vyrn tools (docs/dev/mcp.md section 3) and what they share.
//
// Adding a tool:
//   1. write `ToolDef some_tool(Context&)` (name, title, description, object_schema(...), run),
//   2. declare it below,
//   3. add it to tool_makers() in tools.cpp, at the place it should have in tools/list.
// Refusals are ToolResult::error() with plain English that says what to do next.
//
// C++17 (the unit tests include this header).
#pragma once

#include "guide.h"
#include "runs.h"
#include "tooling.h"
#include "workspace.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace vyrn::mcp {

// What the tools share; one per server.
struct Context {
  explicit Context(const Options& options);
  Workspace workspace;
  Runs runs;
  Guide guide;
  std::map<std::string, json> checks;   // project path -> the last vyrn_check result
  std::map<std::string, Look> looks;    // project path -> what the AI saw in the last Check or Watch
};

using ToolMaker = ToolDef (*)(Context&);

// Every tool, in the fixed order tools/list shows them.
const std::vector<ToolMaker>& tool_makers();
ToolTable make_tool_table(Context& ctx);

// tools_projects.cpp
ToolDef guide_tool(Context& ctx);
ToolDef list_templates_tool(Context& ctx);
ToolDef list_projects_tool(Context& ctx);
ToolDef create_project_tool(Context& ctx);
ToolDef read_project_tool(Context& ctx);
ToolDef write_script_tool(Context& ctx);
ToolDef settings_tool(Context& ctx);

// tools_runs.cpp
ToolDef check_tool(Context& ctx);
ToolDef train_tool(Context& ctx);
ToolDef status_tool(Context& ctx);
ToolDef stop_tool(Context& ctx);
ToolDef watch_tool(Context& ctx);
ToolDef see_tool(Context& ctx);

// tools_share.cpp: .vrlp / .vrl sharing files (docs/dev/package.md section 10). There is no tool that trusts an
// imported project, by design: only the person does that, in the app or in their own terminal.
ToolDef share_tool(Context& ctx);
ToolDef inspect_package_tool(Context& ctx);
ToolDef import_package_tool(Context& ctx);

// ---- shared by the tool files (tools.cpp) ----

// The "project" argument.
Arg project_arg();
// Resolves args["project"]; on failure `refusal` is the error to return.
bool project_from(Context& ctx, const json& args, Project& p, ToolResult& refusal);

struct CheckReport {
  json data;                    // vyrn_check's structured result
  std::string summary;          // readable lines
  std::vector<uint8_t> png;     // what the AI saw (pictures only)
  bool screen_refused = false;  // the scripts tried to read the screen, which the user hasn't allowed agents
};
// Runs Check (at most Options::call_seconds) and keeps its result for vyrn_read_project and vyrn_see. A real game
// without the user's screen permission (screen_allowed) runs with screen reading switched off in the worker.
CheckReport run_check(Context& ctx, const Project& p, Waiter& waiter);

// "text\n\n{json}": readable text followed by the same data as JSON (for clients without structuredContent).
std::string with_json(const std::string& text, const json& data);
double rounded(double v, int decimals);
json number_or_null(const std::optional<double>& v, int decimals);
// Who holds the project's .vyrn-lock right now: "", "app", "agent" or "cli".
std::string lock_holder(const std::string& project);
// Why someone else's lock stops this server ("The Vyrn app is training this project ...").
std::string holder_text(const ProjectLock::Holder& h, bool to_stop);
// "a 10x10 picture", "3 numbers (fruit x, fruit y, basket)"
std::string sees_text(const json& spec);

// "" for a project Vyrn runs; for an imported project nobody trusted yet, the core's "not_trusted" sentence plus
// what an agent should do (ask the person). vyrn_check, vyrn_train, vyrn_watch and vyrn_write_script's Check use it.
std::string untrusted_refusal(const std::string& project);
// The note vyrn_read_project (and the script resources) put first for an untrusted project: its files were
// written by someone else, so they are data, not instructions. "" for a trusted project.
std::string untrusted_note(const std::string& project);

}  // namespace vyrn::mcp
