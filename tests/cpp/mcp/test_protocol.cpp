// Unit tests for the MCP server's protocol handling (docs/dev/mcp.md section 1): JSON-RPC 2.0, the legacy
// initialize flow, the modern per-request _meta flow, error codes, tool arguments, batches and cancellation.
// Fake tools and resources: no Python, no projects.
#include "protocol.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace {

using vyrn::mcp::Arg;
using vyrn::mcp::Identity;
using vyrn::mcp::json;
using vyrn::mcp::object_schema;
using vyrn::mcp::Protocol;
using vyrn::mcp::ResourceError;
using vyrn::mcp::ResourceInfo;
using vyrn::mcp::Resources;
using vyrn::mcp::ResourceTemplate;
using vyrn::mcp::ResourceText;
using vyrn::mcp::ToolDef;
using vyrn::mcp::ToolResult;
using vyrn::mcp::ToolTable;
using vyrn::mcp::Waiter;

struct FakeResources : Resources {
  std::vector<ResourceInfo> list() override {
    return {{"vyrn://guide", "guide", "Guide", "How it works", "text/markdown"}};
  }
  std::vector<ResourceTemplate> templates() override {
    return {{"vyrn://projects/{project}/{file}", "script", "Script", "A script", "text/x-python"}};
  }
  ResourceText read(const std::string& uri) override {
    if (uri == "vyrn://guide") return {uri, "text/markdown", "# Guide", 1234};
    throw ResourceError(vyrn::mcp::kResourceNotFound, "Resource not found: " + uri);
  }
};

// Counts waits; can cancel the running call the first time it waits (like notifications/cancelled would).
struct TestWaiter : Waiter {
  Protocol* protocol = nullptr;
  bool cancel = false;
  int waits = 0;
  bool wait(double) override {
    ++waits;
    if (cancel && protocol) protocol->cancel_current();
    return !(protocol && protocol->current_cancelled());
  }
};

ToolTable fake_tools() {
  ToolTable table;
  ToolDef echo;
  echo.name = "echo";
  echo.title = "Echo";
  echo.description = "Says the text back.";
  echo.input_schema = object_schema(
      {{"text", json{{"type", "string"}, {"minLength", 1}, {"description", "What to say."}}, true},
       {"times", json{{"type", "integer"}, {"minimum", 1}, {"maximum", 3}, {"default", 1}}, false},
       {"mode", json{{"type", "string"}, {"enum", json::array({"loud", "soft"})}, {"default", "soft"}}, false}});
  echo.read_only = true;
  echo.run = [](const json& args, Waiter&) {
    std::string said;
    for (int64_t i = 0; i < args["times"].get<int64_t>(); ++i) said += args["text"].get<std::string>();
    return ToolResult::ok(said, json{{"said", said}, {"times", args["times"]}, {"mode", args["mode"]}});
  };
  table.add(echo);

  ToolDef picture;
  picture.name = "picture";
  picture.title = "Picture";
  picture.description = "Returns an image.";
  picture.input_schema = object_schema({});
  picture.run = [](const json&, Waiter&) {
    ToolResult r = ToolResult::ok("a picture");
    r.pngs.push_back({0x89, 'P', 'N', 'G'});
    return r;
  };
  table.add(picture);

  ToolDef slow;
  slow.name = "slow";
  slow.title = "Slow";
  slow.description = "Waits until told to stop.";
  slow.input_schema = object_schema({});
  slow.run = [](const json&, Waiter& waiter) {
    for (int i = 0; i < 100 && waiter.wait(0.0); ++i) {
    }
    return ToolResult::ok("done");
  };
  table.add(slow);

  ToolDef broken;
  broken.name = "broken";
  broken.title = "Broken";
  broken.description = "Throws.";
  broken.input_schema = object_schema({});
  broken.run = [](const json&, Waiter&) -> ToolResult { throw std::runtime_error("boom"); };
  table.add(broken);

  ToolDef bytes;
  bytes.name = "bytes";
  bytes.title = "Bytes";
  bytes.description = "Text with a line break and a byte that isn't UTF-8.";
  bytes.input_schema = object_schema({});
  bytes.run = [](const json&, Waiter&) { return ToolResult::ok(std::string("a\nb\xff" "c"), json{{"raw", "x\xfey"}}); };
  table.add(bytes);
  return table;
}

Identity identity() {
  Identity id;
  id.version = "9.9.9";
  id.instructions = "Call vyrn_guide first.";
  return id;
}

class ProtocolTest : public ::testing::Test {
 protected:
  ProtocolTest() : tools(fake_tools()), protocol(identity(), tools, resources) { waiter.protocol = &protocol; }

  json send_line(const std::string& line) {
    const std::string reply = protocol.handle_line(line, waiter);
    EXPECT_EQ(reply.find('\n'), std::string::npos);
    return reply.empty() ? json() : json::parse(reply);
  }
  json send(const json& message) { return send_line(message.dump()); }
  json request(const std::string& method, json params = nullptr, json id = 1) {
    json m = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
    if (!params.is_null()) m["params"] = params;
    return send(m);
  }
  json modern(const std::string& method, json params = json::object()) {
    params["_meta"] = json{{vyrn::mcp::kMetaProtocolVersion, vyrn::mcp::kModernVersion},
                           {"io.modelcontextprotocol/clientInfo", json{{"name", "test"}, {"version", "1"}}},
                           {"io.modelcontextprotocol/clientCapabilities", json::object()}};
    return request(method, params);
  }
  json initialize(const std::string& version) {
    return request("initialize", json{{"protocolVersion", version},
                                      {"capabilities", json::object()},
                                      {"clientInfo", json{{"name", "test"}, {"version", "1"}}}});
  }
  json call(const std::string& tool, json args = json::object()) {
    return request("tools/call", json{{"name", tool}, {"arguments", args}});
  }
  int error_code(const json& reply) { return reply.contains("error") ? reply["error"]["code"].get<int>() : 0; }

  ToolTable tools;
  FakeResources resources;
  Protocol protocol;
  TestWaiter waiter;
};

TEST_F(ProtocolTest, InitializeAcceptsEveryLegacyVersion) {
  for (const std::string version : {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"}) {
    const json r = initialize(version)["result"];
    EXPECT_EQ(r["protocolVersion"], version);
    EXPECT_EQ(r["serverInfo"]["name"], "vyrn");
    EXPECT_EQ(r["serverInfo"]["version"], "9.9.9");
    EXPECT_TRUE(r["capabilities"].contains("tools"));
    EXPECT_TRUE(r["capabilities"].contains("resources"));
    EXPECT_EQ(r["instructions"], "Call vyrn_guide first.");
    EXPECT_EQ(protocol.negotiated_version(), version);
  }
}

TEST_F(ProtocolTest, InitializeOffersTheNewestLegacyVersionForOthers) {
  EXPECT_EQ(initialize("2099-01-01")["result"]["protocolVersion"], "2025-11-25");
  EXPECT_EQ(initialize("2026-07-28")["result"]["protocolVersion"], "2025-11-25");   // the modern era has no initialize
  EXPECT_EQ(error_code(request("initialize", json::object())), -32602);
  EXPECT_EQ(error_code(request("initialize", json{{"protocolVersion", 5}})), -32602);
}

TEST_F(ProtocolTest, FieldsFollowTheNegotiatedVersion) {
  initialize("2024-11-05");
  json tool = request("tools/list")["result"]["tools"][0];
  EXPECT_FALSE(tool.contains("title"));
  EXPECT_FALSE(tool.contains("annotations"));
  EXPECT_FALSE(call("echo", json{{"text", "hi"}})["result"].contains("structuredContent"));
  EXPECT_FALSE(initialize("2024-11-05")["result"]["serverInfo"].contains("title"));

  initialize("2025-03-26");
  tool = request("tools/list")["result"]["tools"][0];
  EXPECT_FALSE(tool.contains("title"));
  EXPECT_EQ(tool["annotations"]["readOnlyHint"], true);
  EXPECT_FALSE(call("echo", json{{"text", "hi"}})["result"].contains("structuredContent"));

  initialize("2025-06-18");
  tool = request("tools/list")["result"]["tools"][0];
  EXPECT_EQ(tool["title"], "Echo");
  const json r = call("echo", json{{"text", "hi"}})["result"];
  EXPECT_EQ(r["structuredContent"]["said"], "hi");
  EXPECT_EQ(r["content"][0]["text"], "hi");
  EXPECT_EQ(r["isError"], false);
  EXPECT_FALSE(r.contains("resultType"));   // legacy results stay plain
}

TEST_F(ProtocolTest, ModernRequestsNeedNoHandshake) {
  const json list = modern("tools/list")["result"];
  EXPECT_EQ(list["resultType"], "complete");
  EXPECT_EQ(list["cacheScope"], "private");
  EXPECT_TRUE(list["ttlMs"].is_number_integer());
  EXPECT_EQ(list["_meta"]["io.modelcontextprotocol/serverInfo"]["name"], "vyrn");
  EXPECT_EQ(list["tools"][0]["title"], "Echo");

  const json r = modern("tools/call", json{{"name", "echo"}, {"arguments", json{{"text", "yo"}}}})["result"];
  EXPECT_EQ(r["resultType"], "complete");
  EXPECT_EQ(r["structuredContent"]["said"], "yo");
  EXPECT_FALSE(r.contains("ttlMs"));   // only list and read results carry a cache time
  EXPECT_EQ(r["_meta"]["io.modelcontextprotocol/serverInfo"]["version"], "9.9.9");

  const json read = modern("resources/read", json{{"uri", "vyrn://guide"}})["result"];
  EXPECT_EQ(read["contents"][0]["text"], "# Guide");
  EXPECT_EQ(read["ttlMs"], 1234);
  EXPECT_EQ(read["cacheScope"], "private");
  EXPECT_EQ(modern("resources/list")["result"]["resultType"], "complete");
  EXPECT_EQ(modern("ping")["result"]["resultType"], "complete");
  EXPECT_EQ(protocol.negotiated_version(), "");
}

TEST_F(ProtocolTest, ServerDiscoverListsEveryVersion) {
  for (const json& reply : {request("server/discover"), modern("server/discover")}) {
    const json r = reply["result"];
    ASSERT_TRUE(r["supportedVersions"].is_array());
    EXPECT_EQ(r["supportedVersions"][0], "2026-07-28");
    EXPECT_EQ(r["supportedVersions"].size(), 5u);
    EXPECT_EQ(r["serverInfo"]["name"], "vyrn");
    EXPECT_TRUE(r["capabilities"].contains("tools"));
    EXPECT_EQ(r["instructions"], "Call vyrn_guide first.");
    EXPECT_EQ(r["resultType"], "complete");
    EXPECT_EQ(r["cacheScope"], "private");
  }
}

TEST_F(ProtocolTest, AnUnknownVersionIs32022) {
  for (const json& version : {json("2030-01-01"), json(""), json(5)}) {
    const json reply = request("tools/list", json{{"_meta", json{{vyrn::mcp::kMetaProtocolVersion, version}}}});
    EXPECT_EQ(error_code(reply), -32022);
    EXPECT_EQ(reply["error"]["message"], "Unsupported protocol version");
    EXPECT_EQ(reply["error"]["data"]["requested"], version);
    EXPECT_EQ(reply["error"]["data"]["supported"][0], "2026-07-28");
    EXPECT_EQ(reply["id"], 1);
  }
}

TEST_F(ProtocolTest, LegacyVersionInMetaUsesThatVersionsRules) {
  const json reply = request("tools/call", json{{"name", "echo"},
                                                {"arguments", json{{"text", "a"}}},
                                                {"_meta", json{{vyrn::mcp::kMetaProtocolVersion, "2025-03-26"}}}});
  EXPECT_FALSE(reply["result"].contains("structuredContent"));
  EXPECT_FALSE(reply["result"].contains("resultType"));
}

TEST_F(ProtocolTest, ErrorCodes) {
  EXPECT_EQ(error_code(request("no/such/method")), -32601);
  EXPECT_EQ(request("no/such/method", nullptr, "abc")["id"], "abc");
  EXPECT_EQ(error_code(request("tools/list", json::array({1}))), -32602);
  EXPECT_EQ(error_code(request("tools/list", json{{"cursor", "next"}})), -32602);
  EXPECT_EQ(error_code(request("tools/call", json::object())), -32602);
  EXPECT_EQ(error_code(request("tools/call", json{{"name", 3}})), -32602);
  EXPECT_EQ(error_code(request("tools/call", json{{"name", "echo"}, {"arguments", "text"}})), -32602);
  EXPECT_EQ(error_code(request("tools/call", json{{"name", "nope"}})), -32602);
  EXPECT_EQ(error_code(request("resources/read", json::object())), -32602);
  EXPECT_EQ(error_code(request("resources/read", json{{"uri", "vyrn://nothing"}})), -32002);
  EXPECT_EQ(error_code(request("resources/subscribe", json{{"uri", "vyrn://guide"}})), -32601);

  json parse = send_line("{\"jsonrpc\": \"2.0\", \"id\": 1, \"method\": ");
  EXPECT_EQ(error_code(parse), -32700);
  EXPECT_TRUE(parse["id"].is_null());
  EXPECT_EQ(error_code(send_line("\"just a string\"")), -32600);
  EXPECT_EQ(error_code(send(json{{"id", 1}, {"method", "ping"}})), -32600);                        // no jsonrpc
  EXPECT_EQ(error_code(send(json{{"jsonrpc", "1.0"}, {"id", 1}, {"method", "ping"}})), -32600);
  EXPECT_EQ(error_code(send(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", 7}})), -32600);
  EXPECT_EQ(error_code(send(json{{"jsonrpc", "2.0"}, {"id", true}, {"method", "ping"}})), -32600);
  EXPECT_EQ(error_code(send(json{{"jsonrpc", "2.0"}, {"id", 1}})), -32600);
  EXPECT_EQ(error_code(send_line("[]")), -32600);
}

TEST_F(ProtocolTest, NotificationsAndResponsesGetNoReply) {
  EXPECT_EQ(protocol.handle(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, waiter), "");
  EXPECT_EQ(protocol.handle(json{{"jsonrpc", "2.0"},
                                 {"method", "notifications/cancelled"},
                                 {"params", json{{"requestId", 42}, {"reason", "no longer needed"}}}},
                            waiter),
            "");
  EXPECT_EQ(protocol.handle(json{{"jsonrpc", "2.0"}, {"method", "notifications/whatever"}}, waiter), "");
  EXPECT_EQ(protocol.handle(json{{"jsonrpc", "2.0"}, {"method", "tools/list"}}, waiter), "");   // no id: no reply
  EXPECT_EQ(protocol.handle(json{{"jsonrpc", "2.0"}, {"id", 5}, {"result", json::object()}}, waiter), "");
  EXPECT_EQ(request("ping")["result"], json::object());   // and the server still works
}

TEST_F(ProtocolTest, ToolsAreListedInAFixedOrder) {
  const json first = request("tools/list")["result"]["tools"];
  const json second = request("tools/list")["result"]["tools"];
  std::vector<std::string> names;
  for (const auto& t : first) names.push_back(t["name"]);
  EXPECT_EQ(names, (std::vector<std::string>{"echo", "picture", "slow", "broken", "bytes"}));
  EXPECT_EQ(first, second);
  EXPECT_EQ(first[0]["inputSchema"]["required"], json::array({"text"}));
  EXPECT_EQ(first[0]["inputSchema"]["additionalProperties"], false);
}

TEST_F(ProtocolTest, ArgumentProblemsAreToolErrorsTheAgentCanRead) {
  initialize("2025-06-18");
  struct Case {
    json args;
    std::string says;
  };
  for (const Case& c : std::vector<Case>{{json::object(), "needs \"text\""},
                                         {json{{"text", "a"}, {"time", 2}}, "has no argument \"time\""},
                                         {json{{"text", 5}}, "must be text"},
                                         {json{{"text", ""}}, "can't be empty"},
                                         {json{{"text", "a"}, {"times", 9}}, "between 1 and 3"},
                                         {json{{"text", "a"}, {"times", 1.5}}, "whole number"},
                                         {json{{"text", "a"}, {"mode", "shout"}}, "one of"}}) {
    const json r = call("echo", c.args)["result"];
    EXPECT_EQ(r["isError"], true) << c.args.dump();
    EXPECT_NE(r["content"][0]["text"].get<std::string>().find(c.says), std::string::npos)
        << r["content"][0]["text"].get<std::string>();
    EXPECT_FALSE(r.contains("structuredContent"));
  }
  const json ok = call("echo", json{{"text", "a"}, {"times", 2.0}, {"mode", nullptr}})["result"];
  EXPECT_EQ(ok["isError"], false);
  EXPECT_EQ(ok["structuredContent"], (json{{"said", "aa"}, {"times", 2}, {"mode", "soft"}}));
  EXPECT_EQ(call("echo", json{{"text", "b"}})["result"]["structuredContent"]["times"], 1);   // the default
}

TEST_F(ProtocolTest, ToolFailuresArePlainErrors) {
  const json r = call("broken")["result"];
  EXPECT_EQ(r["isError"], true);
  EXPECT_EQ(r["content"][0]["text"], "Vyrn couldn't finish broken: boom");
}

TEST_F(ProtocolTest, ImagesAreBase64Png) {
  const json content = call("picture")["result"]["content"];
  ASSERT_EQ(content.size(), 2u);
  EXPECT_EQ(content[0]["type"], "text");
  EXPECT_EQ(content[1]["type"], "image");
  EXPECT_EQ(content[1]["mimeType"], "image/png");
  EXPECT_EQ(content[1]["data"], "iVBORw==");
}

TEST_F(ProtocolTest, RepliesAreOneLineOfValidUtf8) {
  const std::string line = protocol.handle(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                                {"params", json{{"name", "bytes"}}}},
                                           waiter);
  EXPECT_EQ(line.find('\n'), std::string::npos);
  const json r = json::parse(line)["result"];
  EXPECT_EQ(r["content"][0]["text"], "a\nb\xEF\xBF\xBD" "c");
}

TEST_F(ProtocolTest, BatchesGetOneArray) {
  const json reply = send_line(
      R"([{"jsonrpc":"2.0","id":1,"method":"ping"},{"jsonrpc":"2.0","method":"notifications/initialized"},)"
      R"({"jsonrpc":"2.0","id":2,"method":"nope"}])");
  ASSERT_TRUE(reply.is_array());
  ASSERT_EQ(reply.size(), 2u);
  EXPECT_EQ(reply[0]["id"], 1);
  EXPECT_EQ(reply[1]["error"]["code"], -32601);
  EXPECT_EQ(protocol.handle_line(R"([{"jsonrpc":"2.0","method":"notifications/initialized"}])", waiter), "");
}

TEST_F(ProtocolTest, ACancelledCallGetsNoReply) {
  waiter.cancel = true;
  EXPECT_EQ(protocol.handle(json{{"jsonrpc", "2.0"}, {"id", 7}, {"method", "tools/call"},
                                 {"params", json{{"name", "slow"}}}},
                            waiter),
            "");
  EXPECT_EQ(waiter.waits, 1);
  EXPECT_EQ(protocol.current_request(), "");
  EXPECT_FALSE(protocol.current_cancelled());
  waiter.cancel = false;
  EXPECT_EQ(call("slow")["result"]["content"][0]["text"], "done");   // the next call runs normally
  EXPECT_EQ(waiter.waits, 101);
}

TEST_F(ProtocolTest, ResourcesAndTemplates) {
  initialize("2025-06-18");
  const json list = request("resources/list")["result"]["resources"];
  ASSERT_EQ(list.size(), 1u);
  EXPECT_EQ(list[0]["uri"], "vyrn://guide");
  EXPECT_EQ(list[0]["mimeType"], "text/markdown");
  EXPECT_EQ(list[0]["title"], "Guide");
  const json templates = request("resources/templates/list")["result"]["resourceTemplates"];
  EXPECT_EQ(templates[0]["uriTemplate"], "vyrn://projects/{project}/{file}");
  const json read = request("resources/read", json{{"uri", "vyrn://guide"}})["result"];
  EXPECT_EQ(read["contents"][0]["uri"], "vyrn://guide");
  EXPECT_FALSE(read.contains("ttlMs"));
  const json missing = request("resources/read", json{{"uri", "vyrn://x"}});
  EXPECT_EQ(missing["error"]["data"]["uri"], "vyrn://x");
}

TEST(ToolTable, NamesAreUnique) {
  ToolTable table;
  ToolDef t;
  t.name = "a";
  table.add(t);
  EXPECT_THROW(table.add(t), std::logic_error);
  ToolDef unnamed;
  EXPECT_THROW(table.add(unnamed), std::logic_error);
  EXPECT_NE(table.find("a"), nullptr);
  EXPECT_EQ(table.find("b"), nullptr);
}

TEST(DumpPretty, ShortPartsStayOnOneLine) {
  const json j = {{"points", json::array({1, 2, 3})}, {"choice", json{{"name", "left"}}}, {"text", "hi"}};
  EXPECT_EQ(vyrn::mcp::dump_pretty(j),
            "{\n  \"points\": [1,2,3],\n  \"choice\": {\"name\":\"left\"},\n  \"text\": \"hi\"\n}");
}

}  // namespace
