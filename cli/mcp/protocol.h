// JSON-RPC 2.0 + MCP for one client (docs/dev/mcp.md section 1): dual-era.
//
//   Modern (2026-07-28): no handshake. A request carries params._meta["io.modelcontextprotocol/protocolVersion"];
//   results get "resultType" and _meta serverInfo; list and read results get "ttlMs" and "cacheScope".
//   Legacy (2025-11-25, 2025-06-18, 2025-03-26, 2024-11-05): initialize negotiates the version for the process.
//
// C++17 (the unit tests include this header). No I/O here: the stdio loop (stdio.cpp) feeds lines in.
#pragma once

#include "tooling.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace vyrn::mcp {

constexpr const char* kModernVersion = "2026-07-28";
constexpr const char* kLatestLegacyVersion = "2025-11-25";
const std::vector<std::string>& legacy_versions();      // newest first
const std::vector<std::string>& supported_versions();   // the modern one first, then the legacy ones

constexpr const char* kMetaProtocolVersion = "io.modelcontextprotocol/protocolVersion";
constexpr const char* kMetaServerInfo = "io.modelcontextprotocol/serverInfo";

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;
constexpr int kResourceNotFound = -32002;
constexpr int kUnsupportedVersion = -32022;

struct Identity {
  std::string name = "vyrn";
  std::string title = "Vyrn";
  std::string version;
  std::string instructions;
};

struct ResourceInfo {
  std::string uri, name, title, description, mime_type;
};

struct ResourceTemplate {
  std::string uri_template, name, title, description, mime_type;
};

struct ResourceText {
  std::string uri, mime_type, text;
  int64_t ttl_ms = 0;   // how long a modern client may keep it
};

struct ResourceError : std::runtime_error {
  ResourceError(int code_, const std::string& message) : std::runtime_error(message), code(code_) {}
  int code;
};

class Resources {
 public:
  virtual ~Resources() = default;
  virtual std::vector<ResourceInfo> list() = 0;
  virtual std::vector<ResourceTemplate> templates() = 0;
  virtual ResourceText read(const std::string& uri) = 0;   // throws ResourceError
};

class Protocol {
 public:
  Protocol(Identity identity, const ToolTable& tools, Resources& resources);

  // One line from the client: a message or a batch. Returns the reply to send (one line, no newline), or "" when
  // nothing is sent (notifications, responses, cancelled requests).
  std::string handle_line(const std::string& line, Waiter& waiter);
  std::string handle(const json& message, Waiter& waiter);

  // The version initialize negotiated ("" before it).
  const std::string& negotiated_version() const { return negotiated_; }
  // The id (compact JSON) of the tools/call running right now, "" between calls.
  const std::string& current_request() const { return current_; }
  // The client cancelled the running call: its reply is dropped.
  void cancel_current() { current_cancelled_ = !current_.empty(); }
  bool current_cancelled() const { return current_cancelled_; }

  // Compact JSON of a request id ("" for anything that isn't a string or a number).
  static std::string id_text(const json& id);

 private:
  struct Era {
    bool modern = false;
    std::string version;   // kModernVersion or a legacy version
  };

  json handle_one(const json& message, Waiter& waiter);
  json request(const std::string& method, const json& params, const json& id, Waiter& waiter);
  void notification(const std::string& method, const json& params);
  Era era_for(const std::string& method, const json& params) const;
  json initialize(const json& params);
  json capabilities() const;
  json server_info(const Era& era) const;
  json tools_list(const json& params, const Era& era) const;
  json tools_call(const json& params, const json& id, const Era& era, Waiter& waiter);
  json resources_list(const json& params, const Era& era);
  json resource_templates(const json& params, const Era& era);
  json resources_read(const json& params, const Era& era, int64_t* ttl_ms);
  void decorate(json& result, const Era& era, int64_t ttl_ms) const;

  Identity identity_;
  const ToolTable& tools_;
  Resources& resources_;
  std::string negotiated_;
  std::string current_;
  bool current_cancelled_ = false;
};

}  // namespace vyrn::mcp
