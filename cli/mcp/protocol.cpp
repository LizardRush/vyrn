// JSON-RPC 2.0 + MCP, dual-era (docs/dev/mcp.md section 1).
#include "protocol.h"

#include <utility>

namespace vyrn::mcp {

namespace {

// A JSON-RPC error reply in the making.
struct RpcError : std::runtime_error {
  RpcError(int code_, const std::string& message, json data_ = nullptr)
      : std::runtime_error(message), code(code_), data(std::move(data_)) {}
  int code;
  json data;
};

constexpr int64_t kLongTtlMs = 3600000;   // server/discover, tools/list, resource templates: fixed for the process
constexpr int64_t kListTtlMs = 5000;      // resources/list: projects come and go

bool valid_id(const json& id) { return id.is_string() || id.is_number(); }

bool is_legacy(const std::string& version) {
  for (const auto& v : legacy_versions()) {
    if (v == version) return true;
  }
  return false;
}

json error_reply(const json& id, int code, const std::string& message, const json& data = nullptr) {
  json error = json::object();
  error["code"] = code;
  error["message"] = message;
  if (!data.is_null()) error["data"] = data;
  json reply = json::object();
  reply["jsonrpc"] = "2.0";
  reply["id"] = id;
  reply["error"] = std::move(error);
  return reply;
}

json result_reply(const json& id, json result) {
  json reply = json::object();
  reply["jsonrpc"] = "2.0";
  reply["id"] = id;
  reply["result"] = std::move(result);
  return reply;
}

std::string base64(const std::vector<uint8_t>& in) {
  static const char* const digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const uint32_t v = (uint32_t{in[i]} << 16) | (uint32_t{in[i + 1]} << 8) | uint32_t{in[i + 2]};
    out += digits[(v >> 18) & 63];
    out += digits[(v >> 12) & 63];
    out += digits[(v >> 6) & 63];
    out += digits[v & 63];
  }
  if (i + 1 == in.size()) {
    const uint32_t v = uint32_t{in[i]} << 16;
    out += digits[(v >> 18) & 63];
    out += digits[(v >> 12) & 63];
    out += "==";
  } else if (i + 2 == in.size()) {
    const uint32_t v = (uint32_t{in[i]} << 16) | (uint32_t{in[i + 1]} << 8);
    out += digits[(v >> 18) & 63];
    out += digits[(v >> 12) & 63];
    out += digits[(v >> 6) & 63];
    out += '=';
  }
  return out;
}

void refuse_cursor(const json& params) {
  auto c = params.find("cursor");
  if (c != params.end() && !c->is_null()) {
    throw RpcError(kInvalidParams, "Invalid params: unknown cursor (this server returns everything in one page).");
  }
}

}  // namespace

const std::vector<std::string>& legacy_versions() {
  static const std::vector<std::string> versions = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
  return versions;
}

const std::vector<std::string>& supported_versions() {
  static const std::vector<std::string> versions = [] {
    std::vector<std::string> v{kModernVersion};
    for (const auto& legacy : legacy_versions()) v.push_back(legacy);
    return v;
  }();
  return versions;
}

Protocol::Protocol(Identity identity, const ToolTable& tools, Resources& resources)
    : identity_(std::move(identity)), tools_(tools), resources_(resources) {}

std::string Protocol::id_text(const json& id) { return valid_id(id) ? dump_line(id) : std::string(); }

std::string Protocol::handle_line(const std::string& line, Waiter& waiter) {
  json message = json::parse(line, nullptr, false);
  if (message.is_discarded()) {
    return dump_line(error_reply(nullptr, kParseError, "Parse error: each line must hold one JSON-RPC message."));
  }
  return handle(message, waiter);
}

std::string Protocol::handle(const json& message, Waiter& waiter) {
  if (message.is_array()) {   // a JSON-RPC batch (MCP 2025-03-26 allows them)
    if (message.empty()) return dump_line(error_reply(nullptr, kInvalidRequest, "Invalid Request: an empty batch."));
    json replies = json::array();
    for (const auto& m : message) {
      json reply = handle_one(m, waiter);
      if (!reply.is_null()) replies.push_back(std::move(reply));
    }
    return replies.empty() ? std::string() : dump_line(replies);
  }
  json reply = handle_one(message, waiter);
  return reply.is_null() ? std::string() : dump_line(reply);
}

json Protocol::handle_one(const json& message, Waiter& waiter) {
  if (!message.is_object()) {
    return error_reply(nullptr, kInvalidRequest, "Invalid Request: a message must be a JSON object.");
  }
  const bool has_id = message.contains("id");
  const json id = has_id ? message["id"] : json();
  const json reply_id = valid_id(id) ? id : json();
  const auto method = message.find("method");
  if (method == message.end()) {
    if (message.contains("result") || message.contains("error")) return nullptr;   // a response: we ask nothing
    return error_reply(reply_id, kInvalidRequest, "Invalid Request: the message has no \"method\".");
  }
  const auto version = message.find("jsonrpc");
  if (version == message.end() || !version->is_string() || version->get<std::string>() != "2.0") {
    return error_reply(reply_id, kInvalidRequest, "Invalid Request: \"jsonrpc\" must be \"2.0\".");
  }
  if (!method->is_string()) return error_reply(reply_id, kInvalidRequest, "Invalid Request: \"method\" must be a string.");
  if (has_id && !valid_id(id)) {
    return error_reply(nullptr, kInvalidRequest, "Invalid Request: \"id\" must be a string or a number.");
  }
  json params = json::object();
  if (auto p = message.find("params"); p != message.end() && !p->is_null()) {
    if (!p->is_object()) {
      if (!has_id) return nullptr;
      return error_reply(id, kInvalidParams, "Invalid params: \"params\" must be an object.");
    }
    params = *p;
  }
  const std::string name = method->get<std::string>();
  if (!has_id) {
    notification(name, params);
    return nullptr;
  }
  try {
    return request(name, params, id, waiter);
  } catch (const RpcError& e) {
    return error_reply(id, e.code, e.what(), e.data);
  } catch (const std::exception& e) {
    return error_reply(id, kInternalError, std::string("Internal error: ") + e.what());
  }
}

void Protocol::notification(const std::string& method, const json& params) {
  // notifications/initialized needs nothing; unknown notifications are ignored (they never get a reply).
  if (method == "notifications/cancelled") {
    auto r = params.find("requestId");
    if (r != params.end() && !current_.empty() && id_text(*r) == current_) current_cancelled_ = true;
  }
}

json Protocol::request(const std::string& method, const json& params, const json& id, Waiter& waiter) {
  if (method == "initialize") return result_reply(id, initialize(params));
  const Era era = era_for(method, params);
  json result;
  int64_t ttl_ms = -1;   // -1: not a list or read result
  if (method == "ping") {
    result = json::object();
  } else if (method == "server/discover") {
    result = json::object();
    result["supportedVersions"] = supported_versions();
    result["capabilities"] = capabilities();
    result["serverInfo"] = server_info(era);
    result["instructions"] = identity_.instructions;
    ttl_ms = kLongTtlMs;
  } else if (method == "tools/list") {
    result = tools_list(params, era);
    ttl_ms = kLongTtlMs;
  } else if (method == "tools/call") {
    result = tools_call(params, id, era, waiter);
    if (result.is_null()) return nullptr;   // the client cancelled it: no reply
  } else if (method == "resources/list") {
    result = resources_list(params, era);
    ttl_ms = kListTtlMs;
  } else if (method == "resources/templates/list") {
    result = resource_templates(params, era);
    ttl_ms = kLongTtlMs;
  } else if (method == "resources/read") {
    result = resources_read(params, era, &ttl_ms);
  } else {
    throw RpcError(kMethodNotFound, "Method not found: " + method);
  }
  decorate(result, era, ttl_ms);
  return result_reply(id, std::move(result));
}

Protocol::Era Protocol::era_for(const std::string& method, const json& params) const {
  Era era;
  auto meta = params.find("_meta");
  if (meta != params.end() && meta->is_object()) {
    auto v = meta->find(kMetaProtocolVersion);
    if (v != meta->end()) {
      const std::string asked = v->is_string() ? v->get<std::string>() : std::string();
      if (asked == kModernVersion) {
        era.modern = true;
        era.version = asked;
        return era;
      }
      if (is_legacy(asked)) {   // a legacy version named per request: that version's rules, for this request
        era.version = asked;
        return era;
      }
      json data = json::object();
      data["supported"] = supported_versions();
      data["requested"] = *v;
      throw RpcError(kUnsupportedVersion, "Unsupported protocol version", data);
    }
  }
  if (method == "server/discover") {   // modern only; a client may call it before it knows a version
    era.modern = true;
    era.version = kModernVersion;
    return era;
  }
  era.version = negotiated_.empty() ? kLatestLegacyVersion : negotiated_;
  return era;
}

json Protocol::initialize(const json& params) {
  auto v = params.find("protocolVersion");
  if (v == params.end() || !v->is_string()) {
    throw RpcError(kInvalidParams, "Invalid params: initialize needs \"protocolVersion\", such as \"2025-11-25\".");
  }
  const std::string asked = v->get<std::string>();
  negotiated_ = is_legacy(asked) ? asked : kLatestLegacyVersion;
  Era era;
  era.version = negotiated_;
  json result = json::object();
  result["protocolVersion"] = negotiated_;
  result["capabilities"] = capabilities();
  result["serverInfo"] = server_info(era);
  result["instructions"] = identity_.instructions;
  return result;
}

json Protocol::capabilities() const {
  json caps = json::object();
  caps["tools"] = json{{"listChanged", false}};
  caps["resources"] = json{{"subscribe", false}, {"listChanged", false}};
  return caps;
}

namespace {

// Fields that appeared in a given legacy version (the modern era has all of them).
bool since(const std::string& version, bool modern, const char* first) { return modern || version >= first; }

}  // namespace

json Protocol::server_info(const Era& era) const {
  json info = json::object();
  info["name"] = identity_.name;
  if (since(era.version, era.modern, "2025-06-18")) info["title"] = identity_.title;
  info["version"] = identity_.version;
  return info;
}

json Protocol::tools_list(const json& params, const Era& era) const {
  refuse_cursor(params);
  const bool titles = since(era.version, era.modern, "2025-06-18");
  const bool annotations = since(era.version, era.modern, "2025-03-26");
  json list = json::array();
  for (const auto& t : tools_.all()) {
    json tool = json::object();
    tool["name"] = t.name;
    if (titles) tool["title"] = t.title;
    tool["description"] = t.description;
    tool["inputSchema"] = t.input_schema;
    if (annotations) {
      json a = json::object();
      a["title"] = t.title;
      a["readOnlyHint"] = t.read_only;
      a["destructiveHint"] = t.destructive;
      a["idempotentHint"] = t.idempotent;
      a["openWorldHint"] = false;
      tool["annotations"] = std::move(a);
    }
    list.push_back(std::move(tool));
  }
  json result = json::object();
  result["tools"] = std::move(list);
  return result;
}

json Protocol::tools_call(const json& params, const json& id, const Era& era, Waiter& waiter) {
  auto n = params.find("name");
  if (n == params.end() || !n->is_string()) {
    throw RpcError(kInvalidParams, "Invalid params: tools/call needs \"name\", the tool's name (tools/list shows them).");
  }
  const std::string name = n->get<std::string>();
  json args = json::object();
  if (auto a = params.find("arguments"); a != params.end() && !a->is_null()) {
    if (!a->is_object()) throw RpcError(kInvalidParams, "Invalid params: \"arguments\" must be an object.");
    args = *a;
  }
  const ToolDef* tool = tools_.find(name);
  if (!tool) throw RpcError(kInvalidParams, "Unknown tool \"" + name + "\": tools/list shows the tools of this server.");

  ToolResult r;
  const std::string problem = validate_arguments(*tool, args);
  if (!problem.empty()) {
    r = ToolResult::error(problem);   // the agent reads it and can fix its call
  } else {
    current_ = id_text(id);
    current_cancelled_ = false;
    try {
      r = tool->run(args, waiter);
    } catch (const std::exception& e) {
      r = ToolResult::error("Vyrn couldn't finish " + name + ": " + e.what());
    } catch (...) {
      r = ToolResult::error("Vyrn couldn't finish " + name + ".");
    }
    const bool cancelled = current_cancelled_;
    current_.clear();
    current_cancelled_ = false;
    if (cancelled) return nullptr;
  }
  json content = json::array();
  content.push_back(json{{"type", "text"}, {"text", r.text}});
  for (const auto& png : r.pngs) {
    content.push_back(json{{"type", "image"}, {"data", base64(png)}, {"mimeType", "image/png"}});
  }
  json result = json::object();
  result["content"] = std::move(content);
  if (r.data.is_object() && since(era.version, era.modern, "2025-06-18")) result["structuredContent"] = r.data;
  result["isError"] = r.is_error;
  return result;
}

json Protocol::resources_list(const json& params, const Era& era) {
  refuse_cursor(params);
  const bool titles = since(era.version, era.modern, "2025-06-18");
  json list = json::array();
  for (const auto& r : resources_.list()) {
    json j = json::object();
    j["uri"] = r.uri;
    j["name"] = r.name;
    if (titles && !r.title.empty()) j["title"] = r.title;
    if (!r.description.empty()) j["description"] = r.description;
    j["mimeType"] = r.mime_type;
    list.push_back(std::move(j));
  }
  json result = json::object();
  result["resources"] = std::move(list);
  return result;
}

json Protocol::resource_templates(const json& params, const Era& era) {
  refuse_cursor(params);
  const bool titles = since(era.version, era.modern, "2025-06-18");
  json list = json::array();
  for (const auto& t : resources_.templates()) {
    json j = json::object();
    j["uriTemplate"] = t.uri_template;
    j["name"] = t.name;
    if (titles && !t.title.empty()) j["title"] = t.title;
    if (!t.description.empty()) j["description"] = t.description;
    j["mimeType"] = t.mime_type;
    list.push_back(std::move(j));
  }
  json result = json::object();
  result["resourceTemplates"] = std::move(list);
  return result;
}

json Protocol::resources_read(const json& params, const Era& /*era*/, int64_t* ttl_ms) {
  auto u = params.find("uri");
  if (u == params.end() || !u->is_string()) {
    throw RpcError(kInvalidParams, "Invalid params: resources/read needs \"uri\", such as \"vyrn://guide\".");
  }
  const std::string uri = u->get<std::string>();
  ResourceText text;
  try {
    text = resources_.read(uri);
  } catch (const ResourceError& e) {
    throw RpcError(e.code, e.what(), json{{"uri", uri}});
  }
  json item = json::object();
  item["uri"] = text.uri;
  item["mimeType"] = text.mime_type;
  item["text"] = text.text;
  json contents = json::array();
  contents.push_back(std::move(item));
  json result = json::object();
  result["contents"] = std::move(contents);
  *ttl_ms = text.ttl_ms;
  return result;
}

void Protocol::decorate(json& result, const Era& era, int64_t ttl_ms) const {
  if (!era.modern) return;
  if (ttl_ms >= 0) {
    result["ttlMs"] = ttl_ms;
    result["cacheScope"] = "private";
  }
  result["resultType"] = "complete";
  json meta = result.contains("_meta") && result["_meta"].is_object() ? result["_meta"] : json::object();
  meta[kMetaServerInfo] = server_info(era);
  result["_meta"] = std::move(meta);
}

}  // namespace vyrn::mcp
