// RFC 6455 helpers of Vyrn Link (core/src/link/ws.h): accept key, HTTP upgrade, frames, masking, UTF-8.
#include "link/link_server.h"
#include "link/ws.h"

#include <gtest/gtest.h>

#include <cstdio>

using namespace vyrn::link;

namespace {

std::string hex(const std::string& bytes) {
  std::string out;
  char b[3];
  for (unsigned char c : bytes) {
    std::snprintf(b, sizeof b, "%02x", c);
    out += b;
  }
  return out;
}

const uint8_t kMask[4] = {0xA1, 0x02, 0xB3, 0x44};

}  // namespace

TEST(Ws, Sha1Base64AndTheRfcAcceptKey) {
  EXPECT_EQ(hex(ws::sha1("abc")), "a9993e364706816aba3e25717850c26c9cd0d89d");
  EXPECT_EQ(hex(ws::sha1("")), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  EXPECT_EQ(ws::base64("f"), "Zg==");
  EXPECT_EQ(ws::base64("fo"), "Zm8=");
  EXPECT_EQ(ws::base64("foo"), "Zm9v");
  EXPECT_EQ(ws::accept_key("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");   // RFC 6455 section 1.3
}

TEST(Ws, UpgradeRequestsAreAnswered) {
  const std::string good =
      "GET /chat HTTP/1.1\r\nHost: pi:7457\r\nUpgrade: WebSocket\r\nConnection: keep-alive, Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
  ASSERT_EQ(ws::http_head_size(good), good.size());
  EXPECT_EQ(ws::http_head_size(good.substr(0, good.size() - 1)), 0u);
  auto req = ws::parse_http_request(good);
  ASSERT_TRUE(req);
  EXPECT_EQ(req->method, "GET");
  EXPECT_EQ(req->target, "/chat");
  EXPECT_EQ(req->header("host"), "pi:7457");
  auto answer = ws::answer_upgrade(*req);
  EXPECT_TRUE(answer.upgrade);
  EXPECT_EQ(answer.response.rfind("HTTP/1.1 101 Switching Protocols\r\n", 0), 0u);
  EXPECT_NE(answer.response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"), std::string::npos);

  // A web browser opening http://pi:7457/ gets a short explanation.
  auto browser = ws::answer_upgrade(*ws::parse_http_request("GET / HTTP/1.1\r\nHost: pi\r\n\r\n"));
  EXPECT_FALSE(browser.upgrade);
  EXPECT_EQ(browser.response.rfind("HTTP/1.1 426", 0), 0u);
  EXPECT_NE(browser.response.find("This is Vyrn Link"), std::string::npos);

  std::string old_version = good;
  old_version.replace(old_version.find("Version: 13"), 11, "Version: 8");
  auto version = ws::answer_upgrade(*ws::parse_http_request(old_version));
  EXPECT_FALSE(version.upgrade);
  EXPECT_NE(version.response.find("Sec-WebSocket-Version: 13"), std::string::npos);

  std::string bad_key = good;
  bad_key.replace(bad_key.find("dGhlIHNhbXBsZSBub25jZQ=="), 24, "short");
  EXPECT_EQ(ws::answer_upgrade(*ws::parse_http_request(bad_key)).response.rfind("HTTP/1.1 400", 0), 0u);
  EXPECT_FALSE(ws::parse_http_request("hello there\r\n\r\n"));
  EXPECT_FALSE(ws::answer_upgrade(*ws::parse_http_request("POST / HTTP/1.1\r\n\r\n")).upgrade);
}

TEST(Ws, FramesRoundTripAtEveryLengthEncoding) {
  for (size_t n : {size_t{0}, size_t{1}, size_t{125}, size_t{126}, size_t{65535}, size_t{65536}, size_t{200000}}) {
    std::string payload(n, 'x');
    for (size_t i = 0; i < n; ++i) payload[i] = static_cast<char>('a' + i % 26);
    const std::string masked = ws::encode_frame(ws::kText, payload, true, kMask);
    size_t pos = 0;
    // Every prefix is "not complete yet".
    for (size_t cut : {size_t{0}, size_t{1}, masked.size() / 2, masked.size() - 1}) {
      size_t p = 0;
      if (cut < masked.size()) {
        EXPECT_FALSE(ws::take_frame(masked.substr(0, cut), p, 1 << 20, true)) << n;
      }
      EXPECT_EQ(p, 0u);
    }
    auto f = ws::take_frame(masked, pos, 1 << 20, true);
    ASSERT_TRUE(f) << n;
    EXPECT_EQ(pos, masked.size());
    EXPECT_TRUE(f->fin);
    EXPECT_EQ(f->opcode, ws::kText);
    EXPECT_EQ(f->payload, payload);
    const std::string plain = ws::encode_frame(ws::kBinary, payload, false);
    pos = 0;
    auto g = ws::take_frame(plain, pos, 1 << 20, false);
    ASSERT_TRUE(g);
    EXPECT_FALSE(g->fin);
    EXPECT_EQ(g->payload, payload);
  }
}

TEST(Ws, FramesThatBreakTheRulesAreRefused) {
  auto code_of = [](const std::string& bytes, bool from_client, size_t max = 1 << 20) -> int {
    size_t pos = 0;
    try {
      ws::take_frame(bytes, pos, max, from_client);
    } catch (const ws::WsError& e) {
      return e.close_code;
    }
    return 0;
  };
  EXPECT_EQ(code_of(ws::encode_frame(ws::kText, "hi"), true), ws::kCloseProtocolError) << "clients must mask";
  EXPECT_EQ(code_of(ws::encode_frame(ws::kText, "hi", true, kMask), false), ws::kCloseProtocolError) << "servers don't";
  std::string rsv = ws::encode_frame(ws::kText, "hi", true, kMask);
  rsv[0] = static_cast<char>(rsv[0] | 0x40);
  EXPECT_EQ(code_of(rsv, true), ws::kCloseProtocolError) << "no extensions";
  EXPECT_EQ(code_of(ws::encode_frame(0x3, "", true, kMask), true), ws::kCloseProtocolError) << "unknown opcode";
  EXPECT_EQ(code_of(ws::encode_frame(ws::kPing, std::string(126, 'p'), true, kMask), true), ws::kCloseProtocolError);
  EXPECT_EQ(code_of(ws::encode_frame(ws::kPing, "p", false, kMask), true), ws::kCloseProtocolError);
  // Too big is refused from the header alone, before the payload arrives.
  const std::string big = ws::encode_frame(ws::kText, std::string(70000, 'b'), true, kMask);
  EXPECT_EQ(code_of(big.substr(0, 10), true, 65536), ws::kCloseTooBig);
  EXPECT_EQ(code_of(big, true, 70000), 0);
  // Control frames don't count toward the data limit.
  EXPECT_EQ(code_of(ws::encode_frame(ws::kPing, std::string(100, 'p'), true, kMask), true, 10), 0);
  EXPECT_EQ(ws::close_payload(1000, "bye"), std::string("\x03\xe8" "bye", 5));
  EXPECT_EQ(ws::close_payload(1001, std::string(300, 'r')).size(), 125u);
}

TEST(Ws, Utf8IsChecked) {
  EXPECT_TRUE(ws::valid_utf8("plain"));
  EXPECT_TRUE(ws::valid_utf8("caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x8e\xae"));
  EXPECT_FALSE(ws::valid_utf8("\xc3"));                 // cut short
  EXPECT_FALSE(ws::valid_utf8("\xc0\xaf"));             // overlong
  EXPECT_FALSE(ws::valid_utf8("\xed\xa0\x80"));         // a surrogate
  EXPECT_FALSE(ws::valid_utf8("\xf4\x90\x80\x80"));     // above U+10FFFF
  EXPECT_FALSE(ws::valid_utf8("\xff"));
}

TEST(LinkSettings, ReadFromVyrnJson) {
  std::vector<std::string> warnings;
  EXPECT_FALSE(link_settings(json::parse(R"({"name":"x"})"), &warnings));
  EXPECT_FALSE(link_settings(json::parse(R"({"link":null})"), &warnings));
  EXPECT_FALSE(link_settings(json::parse(R"({"link":false})"), &warnings));
  EXPECT_FALSE(link_settings(json::parse("[]"), &warnings));
  EXPECT_TRUE(warnings.empty());

  auto plain = link_settings(json::parse(R"({"link":{"port":7457}})"), &warnings);
  ASSERT_TRUE(plain);
  EXPECT_EQ(plain->port, 7457);
  EXPECT_FALSE(plain->lockstep);
  EXPECT_EQ(plain->frames_per_step, 1);
  auto on = link_settings(json::parse(R"({"link":true})"));
  ASSERT_TRUE(on);
  EXPECT_EQ(on->port, kDefaultPort);
  auto lockstep = link_settings(json::parse(R"({"link":{"port":9000,"lockstep":true,"frames_per_step":4}})"));
  ASSERT_TRUE(lockstep);
  EXPECT_EQ(lockstep->port, 9000);
  EXPECT_TRUE(lockstep->lockstep);
  EXPECT_EQ(lockstep->frames_per_step, 4);
  EXPECT_TRUE(warnings.empty());

  auto bad = link_settings(json::parse(R"({"link":{"port":80,"lockstep":"yes","frames_per_step":0}})"), &warnings);
  ASSERT_TRUE(bad);
  EXPECT_EQ(bad->port, kDefaultPort);
  EXPECT_FALSE(bad->lockstep);
  EXPECT_EQ(bad->frames_per_step, 1);
  EXPECT_EQ(warnings.size(), 3u);
  warnings.clear();
  EXPECT_TRUE(link_settings(json::parse(R"({"link":7457})"), &warnings));
  EXPECT_EQ(warnings.size(), 1u);
}

TEST(LinkServer, PairingCodesHaveSixDigits) {
  for (int i = 0; i < 50; ++i) {
    const std::string code = LinkServer::make_code();
    ASSERT_EQ(code.size(), 6u);
    EXPECT_NE(code[0], '0') << "a code never starts with 0, so a mod may store it as a number";
  }
}
