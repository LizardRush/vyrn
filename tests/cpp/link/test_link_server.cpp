// The Vyrn Link server (core/src/link/link_server.h) driven by a small blocking WebSocket client, and by the
// reference game client tools/link/link_fake_game.py. Every server listens on 127.0.0.1 on a free high port the
// kernel picks, and is stopped by the end of its test.
#include "link/link_server.h"
#include "link/ws.h"

#include "platform/net.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>

using namespace vyrn::link;

namespace {

double now_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void sleep_s(double s) { std::this_thread::sleep_for(std::chrono::duration<double>(s)); }

bool wait_until(const std::function<bool()>& ok, double timeout_s = 2.0) {
  const double end = now_s() + timeout_s;
  while (now_s() < end) {
    if (ok()) return true;
    sleep_s(0.005);
  }
  return ok();
}

LinkConfig test_config() {
  LinkConfig cfg;
  cfg.port = 0;                     // a free high port, picked by the kernel
  cfg.bind_address = "127.0.0.1";   // tests never listen on the network
  cfg.code = "483920";
  cfg.welcome.project = "GD bot";
  cfg.welcome.buttons = {"jump"};
  cfg.welcome.tick_hz = 30;
  return cfg;
}

// A blocking WebSocket client: just enough to poke the server, including with frames that break the rules.
class Client {
 public:
  explicit Client(int port) {
    int err = 0;
    fd_ = vyrn::platform::net::connect_tcp("127.0.0.1", port, err);   // blocking
  }
  ~Client() { close(); }
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  void close() {
    vyrn::platform::net::close_socket(fd_);
    fd_ = vyrn::platform::net::kNoSocket;
  }

  void send_raw(const std::string& bytes) {
    size_t off = 0;
    while (off < bytes.size()) {
      int err = 0;
      const long long n = vyrn::platform::net::send_some(fd_, bytes.data() + off, bytes.size() - off, err);
      if (n <= 0) return;
      off += static_cast<size_t>(n);
    }
  }

  // The HTTP upgrade; true when the server switched to WebSocket with the right accept key. extra_headers: more
  // header lines, each ending in \r\n (e.g. the Origin a browser sends).
  bool handshake(const std::string& extra_headers = "") {
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    send_raw("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
             "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n" + extra_headers + "\r\n");
    const double end = now_s() + 2;
    while (ws::http_head_size(buf_) == 0) {
      if (!fill(end - now_s())) return false;
    }
    const size_t head = ws::http_head_size(buf_);
    response = buf_.substr(0, head);
    buf_.erase(0, head);
    return response.rfind("HTTP/1.1 101", 0) == 0 &&
           response.find("Sec-WebSocket-Accept: " + ws::accept_key(key) + "\r\n") != std::string::npos;
  }

  void send_frame(uint8_t opcode, const std::string& payload, bool fin = true, bool masked = true) {
    static const uint8_t mask[4] = {0x37, 0xFA, 0x21, 0x3D};
    send_raw(ws::encode_frame(opcode, payload, fin, masked ? mask : nullptr));
  }
  void send_json(const json& j) { send_frame(ws::kText, j.dump()); }

  // The next frame from the server; nullopt on timeout or when the server hung up.
  std::optional<ws::Frame> recv_frame(double timeout_s = 2.0) {
    const double end = now_s() + timeout_s;
    for (;;) {
      size_t pos = 0;
      auto f = ws::take_frame(buf_, pos, SIZE_MAX, false);
      if (f) {
        buf_.erase(0, pos);
        return f;
      }
      if (!fill(end - now_s())) return std::nullopt;
    }
  }

  // The next text message as JSON. Answers pings and skips pongs; nullopt at a close frame (kept in last_close).
  std::optional<json> recv_json(double timeout_s = 2.0) {
    for (;;) {
      auto f = recv_frame(timeout_s);
      if (!f) return std::nullopt;
      if (f->opcode == ws::kPing) {
        send_frame(ws::kPong, f->payload);
        continue;
      }
      if (f->opcode == ws::kText) return json::parse(f->payload);
      if (f->opcode == ws::kClose) {
        last_close = f->payload;
        return std::nullopt;
      }
    }
  }

  // Reads (and drops) everything until the server closes the TCP connection.
  bool wait_eof(double timeout_s = 3.0) {
    const double end = now_s() + timeout_s;
    while (!eof_) {
      if (!fill(end - now_s()) && !eof_) return false;
    }
    return true;
  }

  int close_code() const {
    return last_close.size() >= 2 ? (static_cast<uint8_t>(last_close[0]) << 8) | static_cast<uint8_t>(last_close[1])
                                  : 0;
  }

  std::string response;
  std::string last_close;

 private:
  bool fill(double timeout_s) {
    if (fd_ == vyrn::platform::net::kNoSocket || eof_) return false;
    std::vector<vyrn::platform::net::PollItem> p(1);
    p[0].socket = fd_;
    p[0].want_read = true;
    int err = 0;
    if (vyrn::platform::net::poll(p, static_cast<int>(std::max(0.0, timeout_s) * 1000), err) <= 0) return false;
    char b[65536];
    const long long n = vyrn::platform::net::recv_some(fd_, b, sizeof b, err);
    if (n <= 0) {
      eof_ = true;
      return false;
    }
    buf_.append(b, static_cast<size_t>(n));
    return true;
  }

  vyrn::platform::net::Socket fd_ = vyrn::platform::net::kNoSocket;
  bool eof_ = false;
  std::string buf_;
};

json hello_message(const json& code = "483920") {
  return {{"type", "hello"}, {"protocol", 1}, {"game", "Test Game"}, {"client", "gtest 1.0"}, {"code", code}};
}

// Connects, upgrades and says hello; returns the welcome (or the error) the server answered.
json say_hello(Client& c, const json& hello = hello_message()) {
  EXPECT_TRUE(c.handshake()) << c.response;
  c.send_json(hello);
  auto reply = c.recv_json();
  return reply ? *reply : json();
}

// The refusal sequence: an error message with this code, a close frame with close_code, then the server hangs up
// once the client answered the close.
void expect_refused(Client& c, const std::string& code, int close_code) {
  auto error = c.recv_json();
  ASSERT_TRUE(error) << "expected an error " << code;
  EXPECT_EQ((*error)["type"], "error");
  EXPECT_EQ((*error)["code"], code) << error->dump();
  EXPECT_FALSE((*error)["message"].get<std::string>().empty());
  EXPECT_FALSE(c.recv_json()) << "the error is followed by a close frame";
  EXPECT_EQ(c.close_code(), close_code);
  c.send_frame(ws::kClose, c.last_close.substr(0, 2));
  EXPECT_TRUE(c.wait_eof()) << "the server hangs up after the closing handshake";
}

bool has_notice(LinkServer& server, const std::string& words, std::vector<LinkNotice>* seen) {
  for (auto& n : server.take_notices()) seen->push_back(n);
  for (const auto& n : *seen) {
    if (n.text.find(words) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

// ---- connecting -----------------------------------------------------------------------------------------------------

TEST(LinkServer, HandshakeHelloAndWelcome) {
  LinkServer server(test_config());
  server.start();
  ASSERT_GT(server.port(), 1023);
  EXPECT_EQ(server.code(), "483920");
  EXPECT_FALSE(server.connected());
  EXPECT_FALSE(server.client());

  Client game(server.port());
  json welcome = say_hello(game);
  EXPECT_EQ(welcome["type"], "welcome");
  EXPECT_EQ(welcome["protocol"], 1);
  EXPECT_TRUE(welcome["vyrn"].is_string() && !welcome["vyrn"].get<std::string>().empty());
  EXPECT_EQ(welcome["project"], "GD bot");
  EXPECT_EQ(welcome["buttons"], json::array({"jump"}));
  EXPECT_EQ(welcome["lockstep"], false);
  EXPECT_EQ(welcome["frames_per_step"], 1);
  EXPECT_TRUE(welcome["tick_hz"].is_number_integer()) << "30, not 30.0: " << welcome.dump();
  EXPECT_EQ(welcome["tick_hz"], 30);

  EXPECT_TRUE(server.connected());
  auto client = server.client();
  ASSERT_TRUE(client);
  EXPECT_EQ(client->game, "Test Game");
  EXPECT_EQ(client->client, "gtest 1.0");
  EXPECT_EQ(client->address, "127.0.0.1");
  EXPECT_EQ(client->connection, 1u);
  std::vector<LinkNotice> seen;
  EXPECT_TRUE(has_notice(server, "Test Game (gtest 1.0) connected from 127.0.0.1", &seen));
  EXPECT_EQ(server.stats().connections, 1u);
  server.stop();
  EXPECT_FALSE(server.running());
  EXPECT_FALSE(server.connected());
}

TEST(LinkServer, WelcomeTellsLockstepAndCanChange) {
  LinkConfig cfg = test_config();
  cfg.code = "";   // a fresh code
  LinkServer server(cfg);
  WelcomeInfo w = server.welcome();
  w.buttons = {"jump", "left", "right"};
  w.lockstep = true;
  w.frames_per_step = 4;
  w.tick_hz = 12.5;
  w.project = "Runner";
  server.set_welcome(w);
  server.start();
  ASSERT_EQ(server.code().size(), 6u);
  Client game(server.port());
  json welcome = say_hello(game, hello_message(server.code()));
  EXPECT_EQ(welcome["buttons"], json::array({"jump", "left", "right"}));
  EXPECT_EQ(welcome["lockstep"], true);
  EXPECT_EQ(welcome["frames_per_step"], 4);
  EXPECT_DOUBLE_EQ(welcome["tick_hz"].get<double>(), 12.5);
  EXPECT_EQ(welcome["project"], "Runner");
}

TEST(LinkServer, ABrowserGetsAnExplanation) {
  LinkServer server(test_config());
  server.start();
  Client browser(server.port());
  browser.send_raw("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nAccept: text/html\r\n\r\n");
  ASSERT_TRUE(browser.wait_eof());   // the server answers, then hangs up
  std::vector<LinkNotice> seen;
  EXPECT_TRUE(wait_until([&] { return has_notice(server, "Refused a connection from 127.0.0.1", &seen); }));
  EXPECT_FALSE(server.connected());
}

TEST(LinkServer, AWrongCodeIsRefused) {
  LinkConfig cfg = test_config();
  cfg.local_needs_code = true;   // 127.0.0.1 normally needs no code
  LinkServer server(cfg);
  server.start();
  {
    Client game(server.port());
    ASSERT_TRUE(game.handshake());
    game.send_json(hello_message("000000"));
    expect_refused(game, "bad_code", ws::kClosePolicy);
  }
  {
    Client game(server.port());
    ASSERT_TRUE(game.handshake());
    json no_code = hello_message();
    no_code.erase("code");
    game.send_json(no_code);
    expect_refused(game, "bad_code", ws::kClosePolicy);
  }
  EXPECT_FALSE(server.connected());
  EXPECT_EQ(server.stats().refused, 2u);
  std::vector<LinkNotice> seen;
  EXPECT_TRUE(has_notice(server, "Wrong pairing code", &seen));

  // The right code, as text with a space, or as a number, is welcome.
  for (const json& code : {json("483 920"), json(483920)}) {
    Client game(server.port());
    EXPECT_EQ(say_hello(game, hello_message(code))["type"], "welcome") << code.dump();
    game.send_frame(ws::kClose, ws::close_payload(1000));
    EXPECT_TRUE(game.wait_eof());
    EXPECT_TRUE(wait_until([&] { return !server.connected(); }));
  }
}

TEST(LinkServer, WrongCodesAreRateLimited) {
  LinkConfig cfg = test_config();
  cfg.local_needs_code = true;
  cfg.bad_codes_per_address = 3;
  cfg.bad_code_window_s = 1.0;
  LinkServer server(cfg);
  server.start();
  for (int i = 0; i < 3; ++i) {
    Client game(server.port());
    EXPECT_EQ(say_hello(game, hello_message("11111" + std::to_string(i)))["code"], "bad_code");
  }
  {
    Client game(server.port());   // even the right code waits now
    json error = say_hello(game);
    EXPECT_EQ(error["code"], "bad_code");
    EXPECT_NE(error["message"].get<std::string>().find("Too many wrong pairing codes"), std::string::npos);
  }
  sleep_s(1.1);
  Client game(server.port());
  EXPECT_EQ(say_hello(game)["type"], "welcome");
}

TEST(LinkServer, LocalGamesNeedNoCode) {
  LinkServer server(test_config());
  server.start();
  Client game(server.port());
  json hello = hello_message();
  hello.erase("code");
  hello.erase("protocol");   // like the reference server: a missing protocol means 1
  EXPECT_EQ(say_hello(game, hello)["type"], "welcome");
}

// A web page can open ws://127.0.0.1 too (a browser sends Origin): it needs the code like another computer.
TEST(LinkServer, AWebPageNeedsTheCodeEvenFromThisComputer) {
  LinkServer server(test_config());
  server.start();
  {
    Client page(server.port());
    ASSERT_TRUE(page.handshake("Origin: http://example.com\r\n")) << page.response;
    json hello = hello_message();
    hello.erase("code");
    page.send_json(hello);
    expect_refused(page, "bad_code", ws::kClosePolicy);
  }
  {
    Client page(server.port());
    ASSERT_TRUE(page.handshake("Origin: http://example.com\r\n")) << page.response;
    page.send_json(hello_message("000000"));
    expect_refused(page, "bad_code", ws::kClosePolicy);
  }
  Client page(server.port());
  ASSERT_TRUE(page.handshake("Origin: null\r\n")) << page.response;
  page.send_json(hello_message());
  auto welcome = page.recv_json();
  ASSERT_TRUE(welcome);
  EXPECT_EQ((*welcome)["type"], "welcome") << "the right code works from a web page";
}

// A game that crashed or lost the network may stay "connected" until the 20 s idle timeout. A new game with the
// right code takes its place once the old one doesn't answer a ping; a game that answers keeps its place.
TEST(LinkServer, ACorrectHelloReplacesAGameThatStoppedAnswering) {
  LinkConfig cfg = test_config();
  cfg.local_needs_code = true;
  cfg.replace_quiet_s = 0.3;
  cfg.replace_probe_s = 0.4;
  LinkServer server(cfg);
  server.start();
  Client old_game(server.port());
  ASSERT_EQ(say_hello(old_game)["type"], "welcome");
  old_game.send_json({{"type", "state"}, {"frame", 1}});
  ASSERT_TRUE(wait_until([&] { return server.latest_state() != nullptr; }));
  sleep_s(0.4);   // quiet now, and it never reads: the server's ping goes unanswered

  {
    Client wrong(server.port());   // the code is checked first: a wrong one never replaces anybody
    ASSERT_TRUE(wrong.handshake());
    wrong.send_json(hello_message("111111"));
    expect_refused(wrong, "bad_code", ws::kClosePolicy);
  }
  EXPECT_EQ(server.client()->connection, 1u);

  Client new_game(server.port());
  ASSERT_TRUE(new_game.handshake());
  const double asked = now_s();
  new_game.send_json(hello_message());
  auto welcome = new_game.recv_json(3.0);
  ASSERT_TRUE(welcome);
  EXPECT_EQ((*welcome)["type"], "welcome");
  EXPECT_GE(now_s() - asked, 0.3) << "the old game had its chance to answer the ping";
  EXPECT_LT(now_s() - asked, 2.0) << "no need to wait for the 20 s idle timeout";
  ASSERT_TRUE(server.client());
  EXPECT_EQ(server.client()->connection, 2u);
  EXPECT_EQ(server.latest_state(), nullptr) << "the old game's state is gone";
  std::vector<LinkNotice> seen;
  EXPECT_TRUE(has_notice(server, "stopped answering, so the game connecting from 127.0.0.1 takes its place", &seen));

  // The old game hears that it was stopped and closed, and the new one is served.
  bool stopped = false;
  for (auto m = old_game.recv_json(); m; m = old_game.recv_json()) stopped = stopped || (*m)["type"] == "stop";
  EXPECT_TRUE(stopped);
  EXPECT_EQ(old_game.close_code(), ws::kCloseGoingAway);
  ASSERT_TRUE(server.send_input(3, {"jump"}));
  auto input = new_game.recv_json();
  ASSERT_TRUE(input);
  EXPECT_EQ((*input)["frame"], 3);
}

TEST(LinkServer, AGameThatAnswersKeepsItsPlace) {
  LinkConfig cfg = test_config();
  cfg.replace_quiet_s = 0.2;
  cfg.replace_probe_s = 0.5;
  LinkServer server(cfg);
  server.start();
  Client game(server.port());
  ASSERT_EQ(say_hello(game)["type"], "welcome");
  // Quiet (a game waiting in lockstep, or in a menu), but its WebSocket still answers pings.
  std::atomic<bool> done{false};
  std::thread reader([&] {
    while (!done) game.recv_json(0.05);   // answers pings
  });
  sleep_s(0.3);
  {
    Client other(server.port());
    ASSERT_TRUE(other.handshake());
    other.send_json(hello_message());
    expect_refused(other, "busy", ws::kClosePolicy);
  }
  done = true;
  reader.join();
  EXPECT_EQ(server.client()->connection, 1u);

  // A game that is sending isn't even asked: busy at once.
  game.send_json({{"type", "state"}, {"frame", 2}});
  Client other(server.port());
  ASSERT_TRUE(other.handshake());
  const double asked = now_s();
  other.send_json(hello_message());
  expect_refused(other, "busy", ws::kClosePolicy);
  EXPECT_LT(now_s() - asked, 0.4);
  EXPECT_EQ(server.client()->connection, 1u);
}

TEST(LinkServer, OnlyOneGameAtATime) {
  LinkServer server(test_config());
  server.start();
  Client first(server.port());
  ASSERT_EQ(say_hello(first)["type"], "welcome");
  {
    Client second(server.port());
    ASSERT_TRUE(second.handshake());
    second.send_json(hello_message());
    expect_refused(second, "busy", ws::kClosePolicy);
  }
  EXPECT_TRUE(server.connected());
  EXPECT_EQ(server.client()->connection, 1u);
  ASSERT_TRUE(server.send_input(7, {"jump"}));
  auto input = first.recv_json();
  ASSERT_TRUE(input);
  EXPECT_EQ((*input)["frame"], 7) << "the first game is still served";

  first.close();
  ASSERT_TRUE(wait_until([&] { return !server.connected(); }));
  Client third(server.port());
  EXPECT_EQ(say_hello(third)["type"], "welcome");
  EXPECT_EQ(server.client()->connection, 2u);
  std::vector<LinkNotice> seen;
  EXPECT_TRUE(has_notice(server, "Test Game disconnected.", &seen));
}

TEST(LinkServer, TheFirstMessageMustBeAHello) {
  LinkServer server(test_config());
  server.start();
  {
    Client game(server.port());
    ASSERT_TRUE(game.handshake());
    game.send_json({{"type", "state"}, {"frame", 1}});
    expect_refused(game, "protocol", ws::kCloseProtocolError);
  }
  {
    Client game(server.port());
    json hello = hello_message();
    hello["protocol"] = 2;
    EXPECT_EQ(say_hello(game, hello)["code"], "protocol");
  }
  {
    Client game(server.port());
    ASSERT_TRUE(game.handshake());
    game.send_frame(ws::kText, "{\"type\": \"hello\", oops");
    expect_refused(game, "bad_message", ws::kCloseBadData);
  }
  {
    Client game(server.port());
    ASSERT_TRUE(game.handshake());
    game.send_frame(ws::kText, "[1, 2, 3]");
    expect_refused(game, "bad_message", ws::kCloseBadData);
  }
  EXPECT_FALSE(server.connected());
}

TEST(LinkServer, ASilentConnectionGetsNoHelloError) {
  LinkConfig cfg = test_config();
  cfg.hello_timeout_s = 0.3;
  LinkServer server(cfg);
  server.start();
  Client game(server.port());
  ASSERT_TRUE(game.handshake());
  const double start = now_s();
  expect_refused(game, "protocol", ws::kClosePolicy);
  EXPECT_LT(now_s() - start, 2.0);
}

// ---- WebSocket rules ------------------------------------------------------------------------------------------------

TEST(LinkServer, PingsAreAnswered) {
  LinkServer server(test_config());
  server.start();
  Client game(server.port());
  ASSERT_EQ(say_hello(game)["type"], "welcome");
  game.send_frame(ws::kPing, "are you there?");
  auto pong = game.recv_frame();
  ASSERT_TRUE(pong);
  EXPECT_EQ(pong->opcode, ws::kPong);
  EXPECT_EQ(pong->payload, "are you there?");
  game.send_frame(ws::kPong, "unasked");   // an unasked pong is fine
  game.send_json({{"type", "state"}, {"frame", 1}});
  EXPECT_EQ(server.wait_state(0, 2).status, Wait::Ok);
}

TEST(LinkServer, AQuietGameIsPingedAndASilentOneDropped) {
  LinkConfig cfg = test_config();
  cfg.ping_after_s = 0.15;
  cfg.idle_timeout_s = 0.6;
  LinkServer server(cfg);
  server.start();
  Client game(server.port());
  ASSERT_EQ(say_hello(game)["type"], "welcome");
  // Answering pings keeps it connected.
  const double end = now_s() + 0.9;
  int pings = 0;
  while (now_s() < end) {
    auto f = game.recv_frame(0.1);
    if (f && f->opcode == ws::kPing) {
      ++pings;
      game.send_frame(ws::kPong, f->payload);
    }
  }
  EXPECT_GE(pings, 2);
  EXPECT_TRUE(server.connected());
  // Not answering: dropped after the idle timeout.
  const double start = now_s();
  ASSERT_TRUE(wait_until([&] { return !server.connected(); }, 3.0));
  EXPECT_LT(now_s() - start, 1.5);
  std::vector<LinkNotice> seen;
  EXPECT_TRUE(has_notice(server, "stopped answering", &seen));
}

TEST(LinkServer, CloseIsAnsweredAndTheServerHangsUp) {
  LinkServer server(test_config());
  server.start();
  Client game(server.port());
  ASSERT_EQ(say_hello(game)["type"], "welcome");
  game.send_frame(ws::kClose, ws::close_payload(1000, "bye"));
  auto echo = game.recv_frame();
  ASSERT_TRUE(echo);
  EXPECT_EQ(echo->opcode, ws::kClose);
  EXPECT_EQ(echo->payload, ws::close_payload(1000));
  EXPECT_TRUE(game.wait_eof());
  EXPECT_FALSE(server.connected());
  std::vector<LinkNotice> seen;
  EXPECT_TRUE(has_notice(server, "closed the connection", &seen));
}

TEST(LinkServer, OversizedMessagesAreRefused) {
  LinkServer server(test_config());
  server.start();
  {
    // Exactly 1 MiB is allowed.
    Client game(server.port());
    ASSERT_EQ(say_hello(game)["type"], "welcome");
    const std::string head = R"({"type":"state","frame":1,"pad":")";
    const std::string text = head + std::string(kMaxMessage - head.size() - 2, 'x') + "\"}";
    ASSERT_EQ(text.size(), kMaxMessage);
    game.send_frame(ws::kText, text);
    auto got = server.wait_state(0, 5);
    ASSERT_EQ(got.status, Wait::Ok);
    EXPECT_EQ(got.state->text.size(), kMaxMessage);
    // One byte more is not.
    game.send_frame(ws::kText, text + " ");
    expect_refused(game, "protocol", ws::kCloseTooBig);
    EXPECT_FALSE(server.connected());
  }
  {
    // Nor is a message whose fragments add up to more.
    Client game(server.port());
    ASSERT_EQ(say_hello(game)["type"], "welcome");
    game.send_frame(ws::kText, R"({"type":"event","name":")" + std::string(600 * 1024, 'a'), false);
    game.send_frame(ws::kContinuation, std::string(600 * 1024, 'b') + "\"}", true);
    expect_refused(game, "protocol", ws::kCloseTooBig);
  }
}

TEST(LinkServer, FragmentedMessagesAreJoined) {
  LinkServer server(test_config());
  server.start();
  Client game(server.port());
  ASSERT_EQ(say_hello(game)["type"], "welcome");
  game.send_frame(ws::kText, R"({"type":"state",)", false);
  game.send_frame(ws::kPing, "between fragments");   // control frames may come in the middle
  game.send_frame(ws::kContinuation, R"("frame":5,)", false);
  game.send_frame(ws::kContinuation, R"("y":1.5})", true);
  auto pong = game.recv_frame();
  ASSERT_TRUE(pong);
  EXPECT_EQ(pong->opcode, ws::kPong);
  auto got = server.wait_state(0, 2);
  ASSERT_EQ(got.status, Wait::Ok);
  EXPECT_EQ(got.state->frame, 5);
  EXPECT_DOUBLE_EQ(got.state->value["y"].get<double>(), 1.5);

  game.send_frame(ws::kContinuation, "nothing to continue");
  expect_refused(game, "protocol", ws::kCloseProtocolError);

  Client other(server.port());
  ASSERT_EQ(say_hello(other)["type"], "welcome");
  other.send_frame(ws::kText, R"({"type":)", false);
  other.send_frame(ws::kText, R"({"type":"state"})", true);   // a new message before the first one finished
  expect_refused(other, "protocol", ws::kCloseProtocolError);
}

TEST(LinkServer, UnmaskedAndBinaryFramesAreRefused) {
  LinkServer server(test_config());
  server.start();
  {
    Client game(server.port());
    ASSERT_EQ(say_hello(game)["type"], "welcome");
    game.send_frame(ws::kText, R"({"type":"state","frame":1})", true, false);
    expect_refused(game, "protocol", ws::kCloseProtocolError);
  }
  {
    Client game(server.port());
    ASSERT_EQ(say_hello(game)["type"], "welcome");
    game.send_frame(ws::kBinary, "\x01\x02");
    expect_refused(game, "protocol", ws::kCloseUnsupportedData);
  }
  {
    Client game(server.port());
    ASSERT_TRUE(game.handshake());
    game.send_frame(ws::kText, "\xff\xfe not UTF-8");
    expect_refused(game, "bad_message", ws::kCloseBadData);
  }
}

// ---- states, events, inputs ---------------------------------------------------------------------------------------

TEST(LinkServer, StatesAndEventsReachTheRunner) {
  LinkServer server(test_config());
  server.start();
  EXPECT_FALSE(server.latest_state());
  Client game(server.port());
  ASSERT_EQ(say_hello(game)["type"], "welcome");
  game.send_json({{"type", "state"}, {"frame", 1}, {"x", 10.0}, {"dead", false}});
  game.send_json({{"type", "event"}, {"name", "death"}, {"percent", 12.5}});
  game.send_json({{"type", "log"}, {"text", "types Vyrn doesn't know are ignored"}});
  game.send_json({{"type", "state"}, {"frame", 2}, {"x", 16.0}, {"dead", true}});
  auto got = server.wait_state(0, 2);
  ASSERT_EQ(got.status, Wait::Ok);
  if (got.state->frame != 2) got = server.wait_state(got.state->seq, 2);
  ASSERT_EQ(got.status, Wait::Ok);
  MessagePtr state = server.latest_state();
  ASSERT_TRUE(state);
  EXPECT_EQ(state->frame, 2);
  EXPECT_EQ(state->value["x"], 16.0);
  EXPECT_EQ(state->value["dead"], true);
  EXPECT_EQ(json::parse(state->text), state->value);
  EXPECT_EQ(state->connection, 1u);

  auto events = server.drain_events();
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0]->value["name"], "death");
  EXPECT_DOUBLE_EQ(events[0]->value["percent"].get<double>(), 12.5);
  EXPECT_LT(events[0]->seq, state->seq) << "states and events share one order";
  EXPECT_TRUE(server.drain_events().empty());
  EXPECT_EQ(server.stats().states, 2u);
  EXPECT_EQ(server.stats().events, 1u);

  // A state without a whole-number frame is refused.
  game.send_json({{"type", "state"}, {"x", 1}});
  expect_refused(game, "bad_message", ws::kCloseBadData);
  EXPECT_FALSE(server.latest_state()) << "a game that left leaves no state behind";
}

TEST(LinkServer, OldEventsAreDroppedWhenNobodyDrains) {
  LinkConfig cfg = test_config();
  cfg.max_events = 5;
  LinkServer server(cfg);
  server.start();
  Client game(server.port());
  ASSERT_EQ(say_hello(game)["type"], "welcome");
  for (int i = 0; i < 8; ++i) game.send_json({{"type", "event"}, {"name", "tick"}, {"n", i}});
  game.send_json({{"type", "state"}, {"frame", 1}});
  ASSERT_EQ(server.wait_state(0, 2).status, Wait::Ok);
  auto events = server.drain_events();
  ASSERT_EQ(events.size(), 5u);
  EXPECT_EQ(events.front()->value["n"], 3);
  EXPECT_EQ(server.stats().events_dropped, 3u);
}

TEST(LinkServer, InputsStopAndCustomMessagesReachTheGame) {
  LinkServer server(test_config());
  server.start();
  EXPECT_FALSE(server.send_input(1, {"jump"})) << "no game, nothing queued";
  EXPECT_FALSE(server.send_stop());
  Client game(server.port());
  ASSERT_EQ(say_hello(game)["type"], "welcome");

  ASSERT_TRUE(server.send_input(42, {"jump"}, {}));
  auto f = game.recv_frame();
  ASSERT_TRUE(f);
  EXPECT_EQ(f->payload, R"({"type":"input","frame":42,"hold":["jump"],"tap":[]})");
  ASSERT_TRUE(server.send_input(43, {}, {"jump"}));
  EXPECT_EQ(*game.recv_json(), json::parse(R"({"type":"input","frame":43,"hold":[],"tap":["jump"]})"));
  ASSERT_TRUE(server.send_stop());
  EXPECT_EQ(*game.recv_json(), json::parse(R"({"type":"stop"})"));
  ASSERT_TRUE(server.send_custom({{"type", "speed"}, {"value", 2.0}}));
  f = game.recv_frame();
  ASSERT_TRUE(f);
  EXPECT_EQ(f->payload, R"({"type":"speed","value":2.0})") << "type first";
  ASSERT_TRUE(server.send_custom({{"type", "restart"}}));
  EXPECT_EQ(*game.recv_json(), json::parse(R"({"type":"restart"})"));

  for (const char* reserved : {"input", "stop", "welcome", "error", "hello", "state", "event"}) {
    EXPECT_FALSE(server.send_custom({{"type", reserved}})) << reserved;
  }
  EXPECT_FALSE(server.send_custom({{"value", 1}}));
  EXPECT_FALSE(server.send_custom(json::array()));
  EXPECT_FALSE(server.send_custom({{"type", "big"}, {"pad", std::string(kMaxMessage, 'x')}}));
  const LinkStats stats = server.stats();
  EXPECT_EQ(stats.inputs, 2u);
  EXPECT_EQ(stats.sent, 3u);
  EXPECT_GT(stats.bytes_out, 0u);
}

TEST(LinkServer, RealTimeFlow) {
  LinkServer server(test_config());
  server.start();
  Client game(server.port());
  ASSERT_EQ(say_hello(game)["type"], "welcome");
  std::atomic<bool> done{false};
  std::atomic<int> inputs{0};
  std::atomic<int64_t> last_answered{-1};
  std::thread play([&] {   // the game: a state every 5 ms, inputs read as they come
    int64_t frame = 0;
    while (!done) {
      game.send_json({{"type", "state"}, {"frame", frame++}});
      auto f = game.recv_frame(0.005);
      while (f) {
        if (f->opcode == ws::kText) {
          json m = json::parse(f->payload);
          if (m["type"] == "input") {
            ++inputs;
            last_answered = m["frame"].get<int64_t>();
          }
        }
        f = game.recv_frame(0.0);
      }
    }
  });
  int sent = 0;
  for (int tick = 0; tick < 20; ++tick) {   // the runner: 50 decisions per second
    sleep_s(0.02);
    MessagePtr state = server.latest_state();
    if (state && server.send_input(state->frame, tick % 2 ? std::vector<std::string>{"jump"} : std::vector<std::string>{})) {
      ++sent;
    }
  }
  wait_until([&] { return inputs == sent; });
  done = true;
  play.join();
  EXPECT_GE(sent, 18);
  EXPECT_EQ(inputs, sent);
  EXPECT_GE(last_answered, 0);
  EXPECT_GT(server.stats().states, 40u);
}

TEST(LinkServer, LockstepSteps) {
  LinkConfig cfg = test_config();
  cfg.welcome.lockstep = true;
  cfg.welcome.frames_per_step = 2;
  LinkServer server(cfg);
  server.start();
  Client game(server.port());
  json welcome = say_hello(game);
  ASSERT_EQ(welcome["lockstep"], true);
  ASSERT_EQ(welcome["frames_per_step"], 2);
  constexpr int kSteps = 50;
  std::atomic<int> answered{0};
  std::atomic<bool> wrong_frame{false};
  std::thread play([&] {   // the game: one state, wait for its input, advance 2 frames, repeat
    int64_t frame = 100;
    game.send_json({{"type", "state"}, {"frame", frame}, {"y", 0}});
    for (int i = 0; i < kSteps; ++i) {
      auto input = game.recv_json(2.0);
      if (!input || (*input)["type"] != "input") return;
      if ((*input)["frame"] != frame) wrong_frame = true;
      const bool holding = !(*input)["hold"].empty();
      ++answered;
      frame += 2;
      game.send_json({{"type", "state"}, {"frame", frame}, {"y", holding ? 1 : 0}});
    }
  });
  auto first = server.wait_state(0, 2);
  ASSERT_EQ(first.status, Wait::Ok);
  EXPECT_EQ(first.state->frame, 100);
  MessagePtr state = first.state;
  const double start = now_s();
  for (int i = 0; i < kSteps; ++i) {
    const bool jump = i % 3 == 0;
    StepResult next = server.step(*state, jump ? std::vector<std::string>{"jump"} : std::vector<std::string>{}, {}, 2.0);
    ASSERT_EQ(next.status, Wait::Ok) << "step " << i;
    EXPECT_EQ(next.state->frame, state->frame + 2);
    EXPECT_EQ(next.state->value["y"], jump ? 1 : 0);
    state = next.state;
  }
  const double elapsed = now_s() - start;
  play.join();
  EXPECT_EQ(answered, kSteps);
  EXPECT_FALSE(wrong_frame);
  EXPECT_LT(elapsed, 2.0) << kSteps << " steps over localhost";

  // Nobody answers now: the next step times out (the input still goes out).
  StepResult late = server.step(*state, {}, {}, 0.2);
  EXPECT_EQ(late.status, Wait::Timeout);
  auto input = game.recv_json();
  ASSERT_TRUE(input);
  EXPECT_EQ((*input)["frame"], state->frame);

  // Stop waiting when asked to.
  std::thread poke([&] {
    sleep_s(0.1);
    server.interrupt_waits();
  });
  EXPECT_EQ(server.wait_state(state->seq, 5).status, Wait::Interrupted);
  poke.join();

  // The game leaves: waits end, and a state from that connection can't be answered any more.
  game.close();
  EXPECT_EQ(server.wait_state(state->seq, 5).status, Wait::Disconnected);
  EXPECT_EQ(server.step(*state, {}, {}, 1).status, Wait::Disconnected);

  // A new connection gets a new number: old states stay unanswerable.
  Client again(server.port());
  ASSERT_EQ(say_hello(again)["type"], "welcome");
  EXPECT_EQ(server.step(*state, {}, {}, 0.1).status, Wait::Disconnected);
  EXPECT_TRUE(server.wait_connected(0));
  std::thread stopper([&] {
    sleep_s(0.1);
    server.stop();
  });
  EXPECT_EQ(server.wait_state(state->seq, 5).status, Wait::Stopped);
  stopper.join();
}

TEST(LinkServer, StopTellsTheGameAndFreesThePort) {
  LinkServer server(test_config());
  server.start();
  const int port = server.port();
  Client game(server.port());
  ASSERT_EQ(say_hello(game)["type"], "welcome");
  std::thread stopper([&] { server.stop(); });
  auto stop = game.recv_json();
  ASSERT_TRUE(stop);
  EXPECT_EQ(*stop, json::parse(R"({"type":"stop"})"));
  EXPECT_FALSE(game.recv_json()) << "then a close frame";
  EXPECT_EQ(game.close_code(), ws::kCloseGoingAway);
  game.send_frame(ws::kClose, game.last_close.substr(0, 2));
  EXPECT_TRUE(game.wait_eof());
  stopper.join();
  EXPECT_FALSE(server.running());
  server.stop();   // twice is fine

  LinkConfig same = test_config();
  same.port = port;
  LinkServer again(same);
  EXPECT_NO_THROW(again.start()) << "the port is free again";
}

TEST(LinkServer, APortInUseIsExplained) {
  LinkServer first(test_config());
  first.start();
  LinkConfig cfg = test_config();
  cfg.port = first.port();
  LinkServer second(cfg);
  try {
    second.start();
    FAIL() << "two servers on one port";
  } catch (const LinkError& e) {
    EXPECT_NE(std::string(e.what()).find("already uses it"), std::string::npos) << e.what();
  }
  LinkConfig bad = test_config();
  bad.code = "12ab56";
  EXPECT_THROW(LinkServer(bad).start(), LinkError);
}

// ---- the reference client -------------------------------------------------------------------------------------------

#if defined(VYRN_PYTHON) && defined(VYRN_SOURCE_DIR)
namespace {

struct FakeGameRun {
  json summary;          // the fake game's last line: {"frames", "deaths", "attempts", "inputs"}
  int steps = 0;         // inputs Vyrn sent
  int restarts = 0;
};

// Runs tools/link/link_fake_game.py against the server for `seconds`, with a simple "jump at the spike" policy.
FakeGameRun run_fake_game(LinkServer& server, bool lockstep, double seconds) {
  FakeGameRun run;
  std::atomic<bool> finished{false};
  std::string output;
  std::thread game([&] {
    const std::string cmd = std::string(VYRN_PYTHON) + " -B " + VYRN_SOURCE_DIR +
                            "/tools/link/link_fake_game.py ws://127.0.0.1:" + std::to_string(server.port()) +
                            "/ --quiet --seconds " + std::to_string(seconds) + " 2>&1";
    if (FILE* p = popen(cmd.c_str(), "r")) {
      char buf[4096];
      while (fgets(buf, sizeof buf, p)) output += buf;
      pclose(p);
    }
    finished = true;
  });
  auto policy = [](const json& s) -> std::vector<std::string> {
    const auto& ahead = s["ahead"];
    const double dx = ahead.is_array() && !ahead.empty() ? ahead[0].get<double>() : 1e9;
    if (s.value("on_ground", 0) == 1 && dx > 0 && dx < 75) return {"jump"};
    return {};
  };
  bool restart_sent = false;
  if (server.wait_connected(5)) {
    if (lockstep) {
      StepResult r = server.wait_state(0, 5);
      while (r.status == Wait::Ok) {
        MessagePtr state = r.state;
        server.drain_events();
        if (state->value.value("dead", false) && !restart_sent) {
          server.send_custom({{"type", "restart"}});
          restart_sent = true;
          ++run.restarts;
        } else if (!state->value.value("dead", false)) {
          restart_sent = false;
        }
        r = server.step(*state, policy(state->value), {}, 5);
        ++run.steps;
      }
    } else {
      while (!finished && server.connected()) {
        sleep_s(1.0 / 30);
        if (MessagePtr state = server.latest_state()) {
          if (server.send_input(state->frame, policy(state->value))) ++run.steps;
        }
      }
    }
  }
  game.join();
  const size_t brace = output.rfind('{');
  if (brace != std::string::npos) run.summary = json::parse(output.substr(brace), nullptr, false);
  EXPECT_TRUE(run.summary.is_object()) << output;
  return run;
}

}  // namespace

TEST(LinkServer, TheReferenceFakeGamePlaysInRealTime) {
  LinkServer server(test_config());
  server.start();
  FakeGameRun run = run_fake_game(server, false, 1.5);
  ASSERT_TRUE(run.summary.is_object());
  EXPECT_GT(run.summary["frames"].get<int>(), 45);
  EXPECT_GT(run.summary["inputs"].get<int>(), 20);
  EXPECT_LE(run.summary["inputs"].get<int>(), run.steps);
  EXPECT_GT(server.stats().states, 45u);
}

TEST(LinkServer, TheReferenceFakeGamePlaysInLockstep) {
  LinkConfig cfg = test_config();
  cfg.welcome.lockstep = true;
  cfg.welcome.frames_per_step = 2;
  LinkServer server(cfg);
  server.start();
  FakeGameRun run = run_fake_game(server, true, 1.5);
  ASSERT_TRUE(run.summary.is_object());
  EXPECT_GT(run.steps, 100) << "lockstep runs as fast as both sides can go";
  EXPECT_GE(run.summary["frames"].get<int>(), 2 * (run.summary["inputs"].get<int>() - 1));
  EXPECT_GE(run.summary["inputs"].get<int>(), run.steps - 2);
}
#endif
