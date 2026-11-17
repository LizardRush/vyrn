// Vyrn Link server (link_server.h). Protocol: docs/user/vyrn-link.md. Only the IO thread touches sockets; the
// API queues outgoing frames and wakes it (platform::net::Waker). Sockets go through platform/net.h.
#include "link/link_server.h"

#include "link/ws.h"
#include "platform/net.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <thread>

#ifndef VYRN_VERSION
#define VYRN_VERSION "0.1.0"
#endif

namespace vyrn::link {

namespace {

using ojson = nlohmann::ordered_json;

constexpr int kMaxJsonDepth = 64;          // game messages are flat; this only stops absurd nesting
constexpr double kCloseLinger_s = 2.0;     // how long a closing connection may take to say goodbye
constexpr double kShutdownLinger_s = 1.0;  // stop(): how long the game gets to answer the close
constexpr size_t kMaxNotices = 200;
constexpr size_t kMaxConnections = 64;     // sockets of every kind together (closing ones included)
constexpr size_t kMaxText = 80;            // characters of a game's name in a console line

double now_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

namespace net = platform::net;

std::string text_of(const ojson& j) { return j.dump(-1, ' ', false, ojson::error_handler_t::replace); }

// Whole numbers as JSON integers (30, not 30.0): mods may read tick_hz with an integer getter.
ojson number(double v) {
  if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15) return static_cast<int64_t>(v);
  return v;
}

std::optional<int64_t> whole_number(const json& v) {
  if (v.is_number_unsigned()) {
    const auto u = v.get<uint64_t>();
    if (u > static_cast<uint64_t>(INT64_MAX)) return std::nullopt;
    return static_cast<int64_t>(u);
  }
  if (v.is_number_integer()) return v.get<int64_t>();
  if (v.is_number_float()) {
    const double d = v.get<double>();
    if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 9e15) return static_cast<int64_t>(d);
  }
  return std::nullopt;
}

// A game's own text for a console line: no control characters, not too long.
std::string shown(const std::string& text) {
  std::string out;
  for (char c : text) {
    if (out.size() >= kMaxText) {
      out += "...";
      break;
    }
    out += (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) ? ' ' : c;
  }
  return out;
}

std::string string_field(const json& obj, const char* key) {
  const auto it = obj.find(key);
  return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string();
}

struct BadMessage {
  std::string message;
};

// The game's JSON text as an object with a string "type". Throws BadMessage with a plain reason.
json parse_message(const std::string& text) {
  if (!ws::valid_utf8(text)) throw BadMessage{"A message from the game isn't valid UTF-8 text."};
  json value;
  try {
    value = json::parse(text, [](int depth, json::parse_event_t event, json& parsed) {
      if (depth > kMaxJsonDepth) throw BadMessage{"A message from the game is nested more than 64 levels deep."};
      if (event == json::parse_event_t::value && parsed.is_number_float() && !std::isfinite(parsed.get<double>())) {
        throw BadMessage{"A message from the game has a number too big to use."};
      }
      return true;
    });
  } catch (const json::parse_error& e) {
    throw BadMessage{"A message from the game isn't valid JSON (near byte " + std::to_string(e.byte) + ")."};
  }
  if (!value.is_object()) throw BadMessage{R"(Each message must be a JSON object, like {"type":"state","frame":1}.)"};
  const auto type = value.find("type");
  if (type == value.end() || !type->is_string()) {
    throw BadMessage{R"(A message from the game has no "type", like {"type":"state", ...}.)"};
  }
  return value;
}

enum class Phase { Http, Hello, Active, Closing };

struct Conn {
  net::Socket fd = net::kNoSocket;
  std::string address;
  bool local = false;
  bool browser = false;      // the handshake had an Origin header: a web page, which needs the code even locally
  Phase phase = Phase::Http;
  uint64_t connection = 0;   // ClientInfo::connection once welcomed
  std::string name;          // the game's name for console lines
  std::string in;            // received, not read yet (from in_pos)
  size_t in_pos = 0;
  std::string out;           // to send (from out_pos)
  size_t out_pos = 0;
  std::string message;       // the fragments of the message being received
  bool in_message = false;
  double deadline = 0;       // Http/Hello: the hello must come by then; Closing: give up waiting then
  double last_rx = 0;
  double last_ping = 0;
  bool close_sent = false;
  bool close_received = false;
  bool discard = false;      // after a broken frame: read and drop everything (the stream can't be parsed)
  bool http_reply = false;   // closing after an HTTP error response
  bool write_shut = false;
  bool dead = false;
  // Hello: waiting to take the place of a connected game that went quiet (LinkConfig::replace_quiet_s).
  json pending_hello;
  double probe_at = 0;      // when the quiet game was pinged
  double replace_at = 0;    // 0 = not waiting; else when this game takes its place unless it answered
  size_t unsent() const { return out.size() - out_pos; }
};

}  // namespace

struct LinkServer::State {
  explicit State(LinkConfig c) : cfg(std::move(c)), welcome(cfg.welcome) {}

  const LinkConfig cfg;
  std::string code;
  int port = 0;

  // Shared with the API, guarded by mu.
  mutable std::mutex mu;
  mutable std::condition_variable cv;
  bool running = false;
  bool stopping = false;
  WelcomeInfo welcome;
  uint64_t active = 0;   // the welcomed game's connection number (0 = none)
  ClientInfo client;
  MessagePtr latest;
  std::deque<MessagePtr> events;
  uint64_t seq = 0;
  uint64_t interrupts = 0;
  struct Outgoing {
    uint64_t connection;
    std::string bytes;
  };
  std::vector<Outgoing> outbox;
  LinkStats stats;
  std::deque<LinkNotice> notices;

  // The IO thread's own.
  net::Socket listen_fd = net::kNoSocket;
  net::Waker waker;
  std::thread thread;
  std::vector<std::unique_ptr<Conn>> conns;
  uint64_t connections = 0;
  std::map<std::string, std::deque<double>> bad_codes;   // address -> when it sent a wrong code
  std::deque<double> bad_codes_all;

  // ---- shared helpers (lock held by the caller) ----
  void notice_locked(bool warning, std::string text) {
    notices.push_back({warning, std::move(text)});
    while (notices.size() > kMaxNotices) notices.pop_front();
  }
  bool live_locked() const { return running && !stopping; }

  void notice(bool warning, std::string text) {
    std::lock_guard<std::mutex> lk(mu);
    notice_locked(warning, std::move(text));
  }

  void wake() { waker.notify(); }   // a full pipe or buffer: the IO thread wakes up anyway

  bool queue(std::string bytes, bool input) {
    {
      std::lock_guard<std::mutex> lk(mu);
      if (!live_locked() || active == 0) return false;
      outbox.push_back({active, std::move(bytes)});
      ++(input ? stats.inputs : stats.sent);
    }
    wake();
    return true;
  }

  StepResult wait_state(uint64_t newer_than, double timeout_s, uint64_t connection) const {
    std::unique_lock<std::mutex> lk(mu);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(std::clamp(timeout_s, 0.0, 1e6)));
    const uint64_t gen = interrupts;
    if (connection == 0) connection = active;
    for (;;) {
      if (!live_locked()) return {Wait::Stopped, nullptr};
      if (interrupts != gen) return {Wait::Interrupted, nullptr};
      if (active == 0 || active != connection) return {Wait::Disconnected, nullptr};
      if (latest && latest->seq > newer_than) return {Wait::Ok, latest};
      if (cv.wait_until(lk, deadline) == std::cv_status::timeout) {
        if (live_locked() && interrupts == gen && active == connection && latest && latest->seq > newer_than) {
          return {Wait::Ok, latest};
        }
        return {Wait::Timeout, nullptr};
      }
    }
  }

  // ---- the IO thread ----
  void loop();
  void accept_all(double now);
  void read(Conn& c, double now);
  void process(Conn& c, double now);
  void on_frame(Conn& c, ws::Frame& f, double now);
  void on_text(Conn& c, std::string text, double now);
  void on_hello(Conn& c, const json& hello, double now);
  void welcome_game(Conn& c, const json& hello, double now);
  void decide_replace(Conn& c, double now);
  Conn* active_conn();
  void on_game_message(Conn& c, json value, std::string text, double now);
  void refuse(Conn& c, const char* code, const std::string& message, uint16_t close_code, double now);
  void begin_close(Conn& c, uint16_t code, const std::string& reason, double now);
  void lost(Conn& c, const std::string& why);
  void flush(Conn& c);
  void timers(Conn& c, double now);
  void finish_closing(Conn& c);
  void hang_up(Conn& c);
  double next_timer(const Conn& c) const;
  bool code_matches(const json& hello) const;
  bool too_many_bad_codes(const std::string& address, double now);
};

// ---- settings -----------------------------------------------------------------------------------------------------

std::optional<LinkSettings> link_settings(const json& project, std::vector<std::string>* warnings) {
  if (!project.is_object()) return std::nullopt;
  const auto it = project.find("link");
  if (it == project.end() || it->is_null() || (it->is_boolean() && !it->get<bool>())) return std::nullopt;
  LinkSettings s;
  auto warn = [&](const std::string& text) {
    if (warnings) warnings->push_back(text);
  };
  if (it->is_boolean()) return s;
  if (!it->is_object()) {
    warn(R"(vyrn.json: "link" should look like {"port": 7457}, so Vyrn uses port 7457.)");
    return s;
  }
  if (const auto p = it->find("port"); p != it->end()) {
    const auto v = whole_number(*p);
    if (v && *v >= 1024 && *v <= 65535) {
      s.port = static_cast<int>(*v);
    } else {
      warn("vyrn.json: the link port must be a whole number from 1024 to 65535, so Vyrn uses 7457.");
    }
  }
  if (const auto l = it->find("lockstep"); l != it->end()) {
    if (l->is_boolean()) {
      s.lockstep = l->get<bool>();
    } else {
      warn(R"(vyrn.json: link "lockstep" must be true or false, so Vyrn uses false.)");
    }
  }
  if (const auto f = it->find("frames_per_step"); f != it->end()) {
    const auto v = whole_number(*f);
    if (v && *v >= 1 && *v <= 1000) {
      s.frames_per_step = static_cast<int>(*v);
    } else {
      warn(R"(vyrn.json: link "frames_per_step" must be a whole number from 1 to 1000, so Vyrn uses 1.)");
    }
  }
  return s;
}

// ---- the IO thread ------------------------------------------------------------------------------------------------

void LinkServer::State::loop() {
  std::vector<net::PollItem> fds;
  std::vector<Conn*> polled;
  bool shutting = false;
  double shutdown_by = 0;
  for (;;) {
    double now = now_s();
    bool stop_now;
    std::vector<Outgoing> out;
    {
      std::lock_guard<std::mutex> lk(mu);
      stop_now = stopping;
      out.swap(outbox);
    }
    if (stop_now && !shutting) {
      shutting = true;
      shutdown_by = now + kShutdownLinger_s;
      net::close_socket(listen_fd);   // free the port right away
      listen_fd = net::kNoSocket;
      for (auto& c : conns) {
        if (c->phase == Phase::Active) {
          c->out += ws::encode_frame(ws::kText, R"({"type":"stop"})");
          begin_close(*c, ws::kCloseGoingAway, "Vyrn stopped", now);
        } else if (c->phase != Phase::Closing) {
          hang_up(*c);
        }
      }
    }
    for (auto& o : out) {
      for (auto& c : conns) {
        if (!c->dead && c->phase == Phase::Active && c->connection == o.connection) c->out += o.bytes;
      }
    }
    double next = now + 0.5;
    for (auto& c : conns) {
      if (!c->dead) flush(*c);
      if (!c->dead) timers(*c, now);
      if (!c->dead) flush(*c);
      if (!c->dead) finish_closing(*c);
      if (!c->dead) next = std::min(next, next_timer(*c));
    }
    conns.erase(std::remove_if(conns.begin(), conns.end(), [](const auto& c) { return c->dead; }), conns.end());
    if (shutting && (conns.empty() || now >= shutdown_by)) break;
    if (shutting) next = std::min(next, shutdown_by);

    fds.clear();
    polled.clear();
    const auto item = [](net::Socket s, bool write) {
      net::PollItem i;
      i.socket = s;
      i.want_read = true;
      i.want_write = write;
      return i;
    };
    fds.push_back(item(waker.socket(), false));
    if (listen_fd != net::kNoSocket) fds.push_back(item(listen_fd, false));
    for (auto& c : conns) {
      fds.push_back(item(c->fd, c->unsent() > 0));
      polled.push_back(c.get());
    }
    const int timeout_ms = static_cast<int>(std::clamp((next - now) * 1000.0 + 1.0, 0.0, 500.0));
    int err = 0;
    const int n = net::poll(fds, timeout_ms, err);
    if (n < 0 && err != EINTR) {
      notice(true, std::string("Vyrn Link stopped working: ") + std::strerror(err));
      break;
    }
    if (n <= 0) continue;
    now = now_s();
    if (fds[0].readable) waker.drain();
    size_t base = 1;
    if (listen_fd != net::kNoSocket) {
      if (fds[1].readable) accept_all(now);
      base = 2;
    }
    for (size_t i = 0; i < polled.size(); ++i) {
      Conn& c = *polled[i];
      const net::PollItem& re = fds[base + i];
      if (!c.dead && (re.readable || re.failed)) read(c, now);
      if (!c.dead && re.writable) flush(c);
    }
  }
  for (auto& c : conns) {
    if (!c->dead) hang_up(*c);
  }
  conns.clear();
  if (listen_fd != net::kNoSocket) net::close_socket(listen_fd);
  listen_fd = net::kNoSocket;
}

void LinkServer::State::accept_all(double now) {
  for (int i = 0; i < 16; ++i) {
    uint32_t peer = 0;
    int err = 0;
    // Non-blocking, close-on-exec, TCP_NODELAY (small messages, sent right away) and SO_KEEPALIVE.
    const net::Socket fd = net::accept_tcp(listen_fd, peer, err);
    if (fd == net::kNoSocket) return;   // EWOULDBLOCK: no more; anything else: try again on the next poll
    const size_t pending = std::count_if(conns.begin(), conns.end(), [](const auto& c) {
      return c->phase == Phase::Http || c->phase == Phase::Hello;
    });
    if (pending >= static_cast<size_t>(std::max(1, cfg.max_pending)) || conns.size() >= kMaxConnections) {
      net::close_socket(fd);
      std::lock_guard<std::mutex> lk(mu);
      ++stats.refused;
      continue;
    }
    auto c = std::make_unique<Conn>();
    c->fd = fd;
    c->address = net::ipv4_text(peer);
    c->local = (peer >> 24) == 127;
    c->deadline = now + cfg.hello_timeout_s;
    c->last_rx = now;
    conns.push_back(std::move(c));
  }
}

void LinkServer::State::read(Conn& c, double now) {
  char buf[64 * 1024];
  for (int round = 0; round < 64 && !c.dead; ++round) {
    int err = 0;
    const long long n = net::recv_some(c.fd, buf, sizeof buf, err);
    if (n > 0) {
      {
        std::lock_guard<std::mutex> lk(mu);
        stats.bytes_in += static_cast<uint64_t>(n);
      }
      c.last_rx = now;
      if (c.discard) continue;
      c.in.append(buf, static_cast<size_t>(n));
      process(c, now);
      if (static_cast<size_t>(n) < sizeof buf) return;
      continue;
    }
    if (n < 0 && err == EWOULDBLOCK) return;
    // EOF or a broken connection.
    if (c.phase == Phase::Active) lost(c, c.name + " disconnected.");
    hang_up(c);
    return;
  }
}

void LinkServer::State::process(Conn& c, double now) {
  if (c.phase == Phase::Http) {
    const size_t head = ws::http_head_size(c.in);
    if (head == 0) {
      if (c.in.size() > ws::kMaxHttpHead) {
        c.out += "HTTP/1.1 431 Request Header Fields Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        c.http_reply = true;
        c.phase = Phase::Closing;
        c.discard = true;
        c.deadline = now + kCloseLinger_s;
        std::lock_guard<std::mutex> lk(mu);
        ++stats.refused;
      }
      return;
    }
    const auto request = ws::parse_http_request(std::string_view(c.in).substr(0, head));
    ws::HandshakeAnswer answer;
    if (request) {
      answer = ws::answer_upgrade(*request);
    } else {
      answer.problem = "something that isn't HTTP";
      answer.response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    }
    c.out += answer.response;
    if (!answer.upgrade) {
      c.http_reply = true;
      c.phase = Phase::Closing;
      c.discard = true;
      c.deadline = now + kCloseLinger_s;
      std::lock_guard<std::mutex> lk(mu);
      ++stats.refused;
      notice_locked(true, "Refused a connection from " + c.address + ": " + answer.problem + ".");
      return;
    }
    c.browser = request && !request->header("origin").empty();
    c.in.erase(0, head);
    c.phase = Phase::Hello;
  }
  while (!c.dead && !c.discard) {
    const size_t used = c.in_message ? c.message.size() : 0;
    std::optional<ws::Frame> frame;
    try {
      frame = ws::take_frame(c.in, c.in_pos, cfg.max_message - std::min(used, cfg.max_message), true);
    } catch (const ws::WsError& e) {
      // The rest of the stream can't be read: answer once, then drop whatever else comes.
      if (c.phase != Phase::Closing) {
        std::string message = e.what();
        message[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(message[0])));
        refuse(c, "protocol", message + ".", e.close_code, now);
      }
      c.discard = true;
      c.in.clear();
      c.in_pos = 0;
      return;
    }
    if (!frame) break;
    on_frame(c, *frame, now);
  }
  if (c.in_pos > 0) {
    c.in.erase(0, c.in_pos);
    c.in_pos = 0;
  }
}

void LinkServer::State::on_frame(Conn& c, ws::Frame& f, double now) {
  switch (f.opcode) {
    case ws::kPing:
      if (c.phase != Phase::Closing) c.out += ws::encode_frame(ws::kPong, f.payload);
      return;
    case ws::kPong:
      return;
    case ws::kClose:
      c.close_received = true;
      if (c.phase == Phase::Active) lost(c, c.name + " closed the connection.");
      if (!c.close_sent) {
        c.out += ws::encode_frame(ws::kClose, f.payload.size() >= 2 ? f.payload.substr(0, 2) : std::string());
        c.close_sent = true;
      }
      if (c.phase != Phase::Closing) c.deadline = now + kCloseLinger_s;
      c.phase = Phase::Closing;
      return;
    default:
      break;
  }
  if (c.phase == Phase::Closing) return;   // goodbye was said: data doesn't matter any more
  if (f.opcode == ws::kContinuation) {
    if (!c.in_message) {
      refuse(c, "protocol", "A continuation frame came without a message to continue.", ws::kCloseProtocolError, now);
      return;
    }
  } else {
    if (c.in_message) {
      refuse(c, "protocol", "A new message started before the previous one was finished.", ws::kCloseProtocolError,
             now);
      return;
    }
    if (f.opcode == ws::kBinary) {
      refuse(c, "protocol", "Vyrn Link messages are text frames with JSON, and the game sent a binary frame.",
             ws::kCloseUnsupportedData, now);
      return;
    }
    c.in_message = true;
    c.message.clear();
  }
  c.message += f.payload;
  if (!f.fin) return;
  c.in_message = false;
  std::string text;
  text.swap(c.message);
  on_text(c, std::move(text), now);
}

void LinkServer::State::on_text(Conn& c, std::string text, double now) {
  json value;
  try {
    value = parse_message(text);
  } catch (const BadMessage& e) {
    refuse(c, "bad_message", e.message, ws::kCloseBadData, now);
    return;
  }
  if (c.phase == Phase::Hello) {
    if (c.replace_at > 0) return;   // still deciding whether this game may take the quiet game's place
    on_hello(c, value, now);
  } else if (c.phase == Phase::Active) {
    on_game_message(c, std::move(value), std::move(text), now);
  }
}

bool LinkServer::State::code_matches(const json& hello) const {
  const auto it = hello.find("code");
  if (it == hello.end()) return false;
  std::string given;
  if (it->is_string()) {
    for (char ch : it->get_ref<const std::string&>()) {
      if (ch != ' ' && ch != '-') given += ch;   // "483 920" as typed into a mod's settings
    }
  } else if (it->is_number_unsigned() || (it->is_number_integer() && it->get<int64_t>() >= 0)) {
    given = it->dump();
    if (given.size() < code.size()) given.insert(0, code.size() - given.size(), '0');
  } else {
    return false;
  }
  return given == code;
}

bool LinkServer::State::too_many_bad_codes(const std::string& address, double now) {
  const double since = now - cfg.bad_code_window_s;
  auto prune = [since](std::deque<double>& times) {
    while (!times.empty() && times.front() < since) times.pop_front();
  };
  prune(bad_codes_all);
  for (auto it = bad_codes.begin(); it != bad_codes.end();) {
    prune(it->second);
    it = it->second.empty() ? bad_codes.erase(it) : std::next(it);
  }
  const auto found = bad_codes.find(address);
  const size_t mine = found == bad_codes.end() ? 0 : found->second.size();
  return mine >= static_cast<size_t>(std::max(1, cfg.bad_codes_per_address)) ||
         bad_codes_all.size() >= static_cast<size_t>(std::max(1, cfg.bad_codes_total));
}

void LinkServer::State::on_hello(Conn& c, const json& hello, double now) {
  if (string_field(hello, "type") != "hello") {
    refuse(c, "protocol", R"(The first message must be the hello: {"type":"hello","protocol":1,...}.)",
           ws::kCloseProtocolError, now);
    return;
  }
  if (const auto p = hello.find("protocol"); p != hello.end() && whole_number(*p) != std::optional<int64_t>(kProtocol)) {
    refuse(c, "protocol",
           "This Vyrn speaks Vyrn Link protocol 1, and the game asked for protocol " + shown(p->dump()) + ".",
           ws::kCloseProtocolError, now);
    return;
  }
  // From another computer, or from a web page (a browser sends Origin; any page could connect to 127.0.0.1).
  if (!c.local || c.browser || cfg.local_needs_code) {
    if (too_many_bad_codes(c.address, now)) {
      refuse(c, "bad_code", "Too many wrong pairing codes. Wait a minute, then try again.", ws::kClosePolicy, now);
      return;
    }
    if (!code_matches(hello)) {
      bad_codes[c.address].push_back(now);
      bad_codes_all.push_back(now);
      refuse(c, "bad_code", "Wrong pairing code: use the 6-digit code Vyrn shows for this project.",
             ws::kClosePolicy, now);
      return;
    }
  }
  bool busy;
  {
    std::lock_guard<std::mutex> lk(mu);
    busy = active != 0;
  }
  if (busy) {
    // The connected game may have crashed or lost the network without saying goodbye. If it has been quiet, ask
    // it (a ping): when it doesn't answer in time, this game takes its place (decide_replace()).
    Conn* old = active_conn();
    const bool someone_waiting = std::any_of(conns.begin(), conns.end(), [](const auto& o) { return o->replace_at > 0; });
    if (old && !someone_waiting && now - old->last_rx >= cfg.replace_quiet_s) {
      c.pending_hello = hello;
      c.probe_at = now;
      c.replace_at = now + std::max(0.05, cfg.replace_probe_s);
      old->out += ws::encode_frame(ws::kPing, "vyrn");
      old->last_ping = now;
      return;
    }
    refuse(c, "busy", "Another game is already connected to this project. Only one game can play at a time.",
           ws::kClosePolicy, now);
    return;
  }
  welcome_game(c, hello, now);
}

Conn* LinkServer::State::active_conn() {
  for (auto& o : conns) {
    if (!o->dead && o->phase == Phase::Active) return o.get();
  }
  return nullptr;
}

// A game is waiting to take the place of a quiet one: it answered the ping (busy), went away by itself (welcome),
// or stayed silent until replace_at (it is dropped, and the new game is welcomed).
void LinkServer::State::decide_replace(Conn& c, double now) {
  Conn* old = active_conn();
  if (old && old->last_rx > c.probe_at) {
    c.replace_at = 0;
    refuse(c, "busy", "Another game is already connected to this project. Only one game can play at a time.",
           ws::kClosePolicy, now);
    return;
  }
  if (old && now < c.replace_at) return;
  c.replace_at = 0;
  if (old) {
    lost(*old, old->name + " stopped answering, so the game connecting from " + c.address + " takes its place.");
    old->out += ws::encode_frame(ws::kText, R"({"type":"stop"})");
    begin_close(*old, ws::kCloseGoingAway, "replaced by a new connection", now);
  }
  json hello;
  hello.swap(c.pending_hello);
  welcome_game(c, hello, now);
}

void LinkServer::State::welcome_game(Conn& c, const json& hello, double now) {
  WelcomeInfo w;
  {
    std::lock_guard<std::mutex> lk(mu);
    w = welcome;
  }
  const ojson message = {{"type", "welcome"},
                         {"protocol", kProtocol},
                         {"vyrn", w.vyrn.empty() ? std::string(VYRN_VERSION) : w.vyrn},
                         {"project", w.project},
                         {"buttons", w.buttons},
                         {"lockstep", w.lockstep},
                         {"frames_per_step", w.frames_per_step},
                         {"tick_hz", number(w.tick_hz)}};
  c.out += ws::encode_frame(ws::kText, text_of(message));   // before any input the API queues for this game
  c.phase = Phase::Active;
  c.connection = ++connections;
  const std::string game = string_field(hello, "game");
  const std::string client_name = string_field(hello, "client");
  c.name = game.empty() ? std::string("The game") : shown(game);
  std::lock_guard<std::mutex> lk(mu);
  active = c.connection;
  client = ClientInfo{game, client_name, c.address, c.connection};
  latest.reset();
  events.clear();
  ++stats.connections;
  notice_locked(false, c.name + (client_name.empty() ? "" : " (" + shown(client_name) + ")") + " connected from " +
                           c.address + ".");
  cv.notify_all();
}

void LinkServer::State::on_game_message(Conn& c, json value, std::string text, double now) {
  const std::string& type = value["type"].get_ref<const std::string&>();
  if (type == "state") {
    const auto f = value.find("frame");
    const auto frame = f == value.end() ? std::nullopt : whole_number(*f);
    if (!frame) {
      refuse(c, "bad_message", R"(A "state" needs "frame": the game's frame number, a whole number.)",
             ws::kCloseBadData, now);
      return;
    }
    auto msg = std::make_shared<GameMessage>();
    msg->connection = c.connection;
    msg->frame = *frame;
    msg->received_s = now;
    msg->text = std::move(text);
    msg->value = std::move(value);
    std::lock_guard<std::mutex> lk(mu);
    msg->seq = ++seq;
    latest = std::move(msg);
    ++stats.states;
    cv.notify_all();
  } else if (type == "event") {
    auto msg = std::make_shared<GameMessage>();
    msg->connection = c.connection;
    msg->received_s = now;
    msg->text = std::move(text);
    msg->value = std::move(value);
    std::lock_guard<std::mutex> lk(mu);
    msg->seq = ++seq;
    events.push_back(std::move(msg));
    ++stats.events;
    while (events.size() > std::max<size_t>(1, cfg.max_events)) {
      events.pop_front();
      ++stats.events_dropped;
    }
    cv.notify_all();
  }
  // Anything else (a second hello, types from a newer protocol) is ignored.
}

void LinkServer::State::refuse(Conn& c, const char* code, const std::string& message, uint16_t close_code, double now) {
  const bool was_active = c.phase == Phase::Active;
  const ojson error = {{"type", "error"}, {"code", code}, {"message", message}};
  c.out += ws::encode_frame(ws::kText, text_of(error));
  begin_close(c, close_code, code, now);
  std::lock_guard<std::mutex> lk(mu);
  if (was_active) {
    notice_locked(true, "Closed the connection to " + c.name + ": " + message);
  } else {
    ++stats.refused;
    notice_locked(true, "Refused a game from " + c.address + ": " + message);
  }
}

void LinkServer::State::begin_close(Conn& c, uint16_t code, const std::string& reason, double now) {
  if (c.phase == Phase::Active) lost(c, "");
  if (!c.close_sent) {
    c.out += ws::encode_frame(ws::kClose, ws::close_payload(code, reason));
    c.close_sent = true;
  }
  c.phase = Phase::Closing;
  c.deadline = now + kCloseLinger_s;
  c.in_message = false;
  c.message.clear();
}

void LinkServer::State::lost(Conn& c, const std::string& why) {
  std::lock_guard<std::mutex> lk(mu);
  if (c.connection == 0 || active != c.connection) return;
  active = 0;
  client = ClientInfo{};
  latest.reset();
  if (!why.empty()) notice_locked(true, why);
  cv.notify_all();
}

void LinkServer::State::flush(Conn& c) {
  uint64_t sent = 0;
  while (c.unsent()) {
    int err = 0;
    const long long n = net::send_some(c.fd, c.out.data() + c.out_pos, c.unsent(), err);
    if (n > 0) {
      c.out_pos += static_cast<size_t>(n);
      sent += static_cast<uint64_t>(n);
      continue;
    }
    if (n < 0 && err == EWOULDBLOCK) break;
    if (c.phase == Phase::Active) lost(c, c.name + " disconnected.");
    hang_up(c);
    break;
  }
  if (sent) {
    std::lock_guard<std::mutex> lk(mu);
    stats.bytes_out += sent;
  }
  if (c.dead) return;
  if (!c.unsent()) {
    c.out.clear();
    c.out_pos = 0;
  } else if (c.out_pos > (1u << 20)) {
    c.out.erase(0, c.out_pos);
    c.out_pos = 0;
  }
  if (c.unsent() > cfg.max_unsent) {
    if (c.phase == Phase::Active) lost(c, c.name + " stopped reading Vyrn's messages, so Vyrn closed the connection.");
    hang_up(c);
  }
}

void LinkServer::State::timers(Conn& c, double now) {
  switch (c.phase) {
    case Phase::Http:
      if (now >= c.deadline) hang_up(c);   // no WebSocket yet: just hang up
      break;
    case Phase::Hello:
      if (c.replace_at > 0) {
        decide_replace(c, now);
      } else if (now >= c.deadline) {
        refuse(c, "protocol", R"(No hello: a game must send {"type":"hello",...} first, right after connecting.)",
               ws::kClosePolicy, now);
      }
      break;
    case Phase::Active:
      if (cfg.idle_timeout_s > 0 && now - c.last_rx >= cfg.idle_timeout_s) {
        lost(c, c.name + " stopped answering, so Vyrn closed the connection.");
        begin_close(c, ws::kCloseGoingAway, "no answer", now);
      } else if (cfg.ping_after_s > 0 && now - c.last_rx >= cfg.ping_after_s && now - c.last_ping >= cfg.ping_after_s) {
        c.out += ws::encode_frame(ws::kPing, "vyrn");
        c.last_ping = now;
      }
      break;
    case Phase::Closing:
      if (now >= c.deadline) hang_up(c);
      break;
  }
}

void LinkServer::State::finish_closing(Conn& c) {
  if (c.phase != Phase::Closing || c.unsent()) return;
  if (c.close_received && !c.http_reply) {
    hang_up(c);   // the closing handshake is complete: the server closes TCP first
  } else if ((c.http_reply || c.discard) && !c.write_shut) {
    net::shutdown_send(c.fd);   // all said: let the other side read it, then wait for its EOF (or the deadline)
    c.write_shut = true;
  }
}

void LinkServer::State::hang_up(Conn& c) {
  if (c.dead) return;
  if (c.phase == Phase::Active) lost(c, "");
  net::close_socket(c.fd);
  c.fd = net::kNoSocket;
  c.dead = true;
}

double LinkServer::State::next_timer(const Conn& c) const {
  if (c.phase == Phase::Hello && c.replace_at > 0) return std::min(c.deadline, c.replace_at);
  if (c.phase != Phase::Active) return c.deadline;
  double next = 1e300;
  if (cfg.idle_timeout_s > 0) next = std::min(next, c.last_rx + cfg.idle_timeout_s);
  if (cfg.ping_after_s > 0) next = std::min(next, std::max(c.last_rx, c.last_ping) + cfg.ping_after_s);
  return next;
}

// ---- the API ------------------------------------------------------------------------------------------------------

LinkServer::LinkServer(LinkConfig config) : s_(std::make_unique<State>(std::move(config))) {
  s_->code = s_->cfg.code.empty() ? make_code() : s_->cfg.code;
}

LinkServer::~LinkServer() { stop(); }

std::string LinkServer::make_code() {
  std::random_device rd;
  std::uniform_int_distribution<int> digits(100000, 999999);
  return std::to_string(digits(rd));
}

void LinkServer::start() {
  {
    std::lock_guard<std::mutex> lk(s_->mu);
    if (s_->running) throw LinkError("Vyrn Link is already running.");
  }
  if (s_->code.size() != 6 || !std::all_of(s_->code.begin(), s_->code.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    throw LinkError("The Vyrn Link pairing code must be 6 digits.");
  }
  const int port = s_->cfg.port;
  if (port < 0 || port > 65535) throw LinkError("The Vyrn Link port must be a number from 1024 to 65535.");
  uint32_t address = 0;
  if (!net::parse_ipv4(s_->cfg.bind_address, address)) {
    throw LinkError("Vyrn Link can't listen on \"" + s_->cfg.bind_address + "\": that isn't an IPv4 address.");
  }
  int err = 0;
  int bound_port = 0;
  // Non-blocking, close-on-exec, SO_REUSEADDR on POSIX (restart quickly after old connections).
  const net::Socket fd = net::listen_tcp(s_->cfg.bind_address, port, 16, bound_port, err);
  if (fd == net::kNoSocket) {
    const std::string p = std::to_string(port);
    if (err == EADDRINUSE) {
      throw LinkError("Vyrn Link can't listen on port " + p +
                      ": another program, or another Vyrn project, already uses it. Choose another port in the "
                      "project's Settings (Link), and the same one in your game's mod.");
    }
    if (err == EACCES) throw LinkError("Port " + p + " needs administrator rights: use a port from 1024 to 65535.");
    throw LinkError("Vyrn Link can't listen on port " + p + ": " + std::strerror(err));
  }
  if (!s_->waker.open(err)) {
    net::close_socket(fd);
    throw LinkError(std::string("Vyrn Link can't start: ") + std::strerror(err));
  }
  s_->listen_fd = fd;
  s_->port = bound_port;
  {
    std::lock_guard<std::mutex> lk(s_->mu);
    s_->running = true;
    s_->stopping = false;
    s_->active = 0;
    s_->latest.reset();
    s_->events.clear();
    s_->outbox.clear();
  }
  s_->thread = std::thread([state = s_.get()] { state->loop(); });
}

void LinkServer::stop() {
  {
    std::lock_guard<std::mutex> lk(s_->mu);
    if (!s_->running || s_->stopping) return;   // not started, or another thread is stopping it
    s_->stopping = true;
    s_->cv.notify_all();
  }
  s_->wake();
  if (s_->thread.joinable()) s_->thread.join();
  {
    std::lock_guard<std::mutex> lk(s_->mu);
    s_->running = false;
    s_->active = 0;
    s_->client = ClientInfo{};
    s_->latest.reset();
    s_->outbox.clear();
    s_->cv.notify_all();
  }
  s_->waker.close();
}

bool LinkServer::running() const {
  std::lock_guard<std::mutex> lk(s_->mu);
  return s_->live_locked();
}

int LinkServer::port() const { return s_->port; }

std::string LinkServer::code() const { return s_->code; }

void LinkServer::set_welcome(const WelcomeInfo& welcome) {
  std::lock_guard<std::mutex> lk(s_->mu);
  s_->welcome = welcome;
}

WelcomeInfo LinkServer::welcome() const {
  std::lock_guard<std::mutex> lk(s_->mu);
  return s_->welcome;
}

bool LinkServer::connected() const {
  std::lock_guard<std::mutex> lk(s_->mu);
  return s_->live_locked() && s_->active != 0;
}

std::optional<ClientInfo> LinkServer::client() const {
  std::lock_guard<std::mutex> lk(s_->mu);
  if (!s_->live_locked() || s_->active == 0) return std::nullopt;
  return s_->client;
}

MessagePtr LinkServer::latest_state() const {
  std::lock_guard<std::mutex> lk(s_->mu);
  return s_->latest;
}

std::vector<MessagePtr> LinkServer::drain_events() {
  std::lock_guard<std::mutex> lk(s_->mu);
  std::vector<MessagePtr> out(s_->events.begin(), s_->events.end());
  s_->events.clear();
  return out;
}

bool LinkServer::send_input(int64_t frame, const std::vector<std::string>& hold, const std::vector<std::string>& tap) {
  const ojson message = {{"type", "input"}, {"frame", frame}, {"hold", hold}, {"tap", tap}};
  return s_->queue(ws::encode_frame(ws::kText, text_of(message)), true);
}

bool LinkServer::send_stop() { return s_->queue(ws::encode_frame(ws::kText, R"({"type":"stop"})"), false); }

bool LinkServer::send_custom(const json& message) {
  if (!message.is_object()) return false;
  const auto type = message.find("type");
  if (type == message.end() || !type->is_string()) return false;
  const std::string& name = type->get_ref<const std::string&>();
  static const char* const kReserved[] = {"input", "stop", "welcome", "error", "hello", "state", "event"};
  if (name.empty() || std::any_of(std::begin(kReserved), std::end(kReserved), [&](const char* r) { return name == r; })) {
    return false;
  }
  // "type" first, so the game's log reads well; the other fields follow.
  std::string text = "{\"type\":" + type->dump(-1, ' ', false, json::error_handler_t::replace);
  for (auto it = message.begin(); it != message.end(); ++it) {
    if (it.key() == "type") continue;
    text += "," + json(it.key()).dump(-1, ' ', false, json::error_handler_t::replace) + ":" +
            it.value().dump(-1, ' ', false, json::error_handler_t::replace);
  }
  text += "}";
  if (text.size() > s_->cfg.max_message) return false;
  return s_->queue(ws::encode_frame(ws::kText, text), false);
}

StepResult LinkServer::wait_state(uint64_t newer_than_seq, double timeout_s) const {
  return s_->wait_state(newer_than_seq, timeout_s, 0);
}

StepResult LinkServer::step(const GameMessage& answered, const std::vector<std::string>& hold,
                            const std::vector<std::string>& tap, double timeout_s) {
  {
    std::lock_guard<std::mutex> lk(s_->mu);
    if (!s_->live_locked()) return {Wait::Stopped, nullptr};
    if (s_->active == 0 || s_->active != answered.connection) return {Wait::Disconnected, nullptr};
  }
  if (!send_input(answered.frame, hold, tap)) return {Wait::Disconnected, nullptr};
  return s_->wait_state(answered.seq, timeout_s, answered.connection);
}

bool LinkServer::wait_connected(double timeout_s) const {
  std::unique_lock<std::mutex> lk(s_->mu);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(std::clamp(timeout_s, 0.0, 1e6)));
  const uint64_t gen = s_->interrupts;
  for (;;) {
    if (!s_->live_locked() || s_->interrupts != gen) return false;
    if (s_->active != 0) return true;
    if (s_->cv.wait_until(lk, deadline) == std::cv_status::timeout) return s_->live_locked() && s_->active != 0;
  }
}

void LinkServer::interrupt_waits() {
  std::lock_guard<std::mutex> lk(s_->mu);
  ++s_->interrupts;
  s_->cv.notify_all();
}

LinkStats LinkServer::stats() const {
  std::lock_guard<std::mutex> lk(s_->mu);
  return s_->stats;
}

std::vector<LinkNotice> LinkServer::take_notices() {
  std::lock_guard<std::mutex> lk(s_->mu);
  std::vector<LinkNotice> out(s_->notices.begin(), s_->notices.end());
  s_->notices.clear();
  return out;
}

}  // namespace vyrn::link
