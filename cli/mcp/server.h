// The MCP server without its stdio loop: tools, resources, runs and the protocol (docs/dev/mcp.md).
// stdio.cpp wraps it in run_mcp_server(); the unit tests drive it directly.
//
// C++17 (the unit tests include this header).
#pragma once

#include "protocol.h"
#include "tools.h"

#include <string>
#include <vector>

namespace vyrn::mcp {

// docs/dev/mcp.md section 5: vyrn://guide, vyrn://api and vyrn://projects/<folder>/<file>.py (read-only).
class ProjectResources : public Resources {
 public:
  explicit ProjectResources(Context& ctx) : ctx_(ctx) {}
  std::vector<ResourceInfo> list() override;
  std::vector<ResourceTemplate> templates() override;
  ResourceText read(const std::string& uri) override;

 private:
  Context& ctx_;
};

// "My Game" -> "My%20Game" (everything but letters, digits and -._~ is escaped).
std::string percent_encode(const std::string& text);
// False for a broken escape or an encoded NUL.
bool percent_decode(const std::string& text, std::string& out);

// The server's identity and instructions (initialize, server/discover).
Identity server_identity();

class Server {
 public:
  explicit Server(const Options& options);
  ~Server();   // stops every session; trainings save their brains first
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  Protocol& protocol() { return protocol_; }
  Context& context() { return ctx_; }
  const ToolTable& tools() const { return tools_; }

  // Calls a tool the way tools/call does (arguments checked first). For tests.
  ToolResult call(const std::string& tool, json args, Waiter& waiter);

  void pump() { ctx_.runs.pump(); }                  // follows the trainings; frees the locks of finished ones
  void begin_shutdown() { ctx_.runs.stop_all(); }    // every training stops and saves
  bool all_finished() const { return ctx_.runs.all_finished(); }

 private:
  Context ctx_;
  ToolTable tools_;
  ProjectResources resources_;
  Protocol protocol_;
};

}  // namespace vyrn::mcp
