// RealtimeRunner with Vyrn Link (docs/dev/link.md, "Real-time runner"): a scripted fake worker (plain Python, no
// vyrn library) plays a project whose vyrn.json has "link", and a small WebSocket game client in this test plays the
// game. It checks what the worker's requests carry from the game, what reaches the game (inputs, vyrn.send
// messages, stop), lockstep, waiting for the game, reconnecting, a port in use, Check, and big states.
// Every server listens on 127.0.0.1 (test_hooks::link_bind_address) on a free high port; nothing presses keys.
#include "platform_test.h"

#if VYRN_TEST_POSIX

#include "link/ws.h"
#include "run/game_link.h"
#include "run/rt_runner.h"
#include "vyrn/link.h"
#include "vyrn/session.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

namespace fs = std::filesystem;
using nlohmann::json;
using vyrn::RunConfig;
using vyrn::RunState;
namespace ws = vyrn::link::ws;

namespace {

// The worker: a spec with game buttons; observe/check_over read the request's "game" ("dead" ends a round);
// act answers with game_input (choice 0 holds jump); restart() sends {"type":"restart"}; an event named "ping"
// makes observe send {"type":"pong"}. Every request is logged (a big "game" only as its size).
const char* const kFakeWorker = R"PY(
import json, os, socket, struct, sys, time

PROJECT = sys.argv[1]
LOG = os.path.join(PROJECT, "fake_log.jsonl")
START = time.monotonic()
sock = socket.socket(fileno=3)

def log(entry):
    entry["at"] = round(time.monotonic() - START, 4)
    with open(LOG, "a") as f:
        f.write(json.dumps(entry) + "\n")

def send(msg, blob=b""):
    body = json.dumps(msg, separators=(",", ":")).encode()
    sock.sendall(b"VYRN" + struct.pack("<II", len(body), len(blob)) + body + blob)

def recv_exact(n):
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            return None
        data += chunk
    return data

def recv():
    head = recv_exact(12)
    if head is None:
        return None
    magic, json_len, bin_len = struct.unpack("<4sII", head)
    body = recv_exact(json_len + bin_len)
    return json.loads(body[:json_len]), json_len

send({"t": "hello", "id": 0, "proto": 1, "pid": os.getpid(), "python": "fake",
      "backends": {"screen": None, "keys": None}})
while True:
    got = recv()
    if got is None:
        sys.exit(0)
    msg, size = got
    t, rid = msg["t"], msg["id"]
    entry = {"t": t, "bytes": size}
    for key in ("first", "a", "connected", "game_events", "check"):
        if key in msg:
            entry[key] = msg[key]
    game = msg.get("game")
    if game is not None:
        entry["game"] = game if size < 20000 else {"keys": sorted(game), "frame": game.get("frame")}
    log(entry)
    game = game or {}
    if t == "load":
        send({"t": "spec", "id": rid, "mode": "realtime",
              "obs": {"kind": "numbers", "shape": [2], "names": ["frame", "y"]},
              "choices": ["jump", "wait"], "labels": ["jump", "wait"],
              "effects": ["hold game button jump", "do nothing"],
              "uses": {"screen": False, "keys": False, "game": True}, "buttons": ["jump"],
              "hooks": {"see": "inputs.py:1", "points": "points.py:1", "game_over": "points.py:4",
                        "restart": "points.py:7"},
              "warnings": []})
    elif t == "restart":
        send({"t": "ok", "id": rid, "did": [], "ms": {"restart": 0.1}, "send": [{"type": "restart"}]})
    elif t == "check_over":
        send({"t": "ok", "id": rid, "game_over": bool(game.get("dead")), "ms": {"game_over": 0.1}})
    elif t == "observe":
        first = bool(msg.get("first"))
        reply = {"t": "result", "id": rid, "obs": {"$a": [0, "f32", [2]]},
                 "points": None if first else 1.0, "game_over": (not first) and bool(game.get("dead")),
                 "truncated": False, "first": first, "final_obs": None, "focused": None,
                 "show": {}, "did": [], "warnings": [],
                 "ms": {"see": 0.1, "points": 0.1, "game_over": 0.1, "respond": 0.0, "step": 0.0}}
        if any(e.get("name") == "ping" for e in msg.get("game_events") or []):
            reply["send"] = [{"type": "pong", "n": 1}]
        send(reply, struct.pack("<2f", float(game.get("frame", 0)), float(game.get("y", 0))))
    elif t == "act":
        hold = ["jump"] if msg.get("a") == 0 else []
        send({"t": "ok", "id": rid, "did": [], "ms": {"respond": 0.1}, "game_input": {"hold": hold, "tap": []}})
    elif t == "status":
        send({"t": "ok", "id": rid, "focused": True, "active": None, "held": [], "frozen": False})
    elif t in ("pause", "resume"):
        send({"t": "ok", "id": rid})
    elif t == "close":
        send({"t": "bye", "id": rid})
        sys.exit(0)
    else:
        send({"t": "error", "id": rid, "fatal": False, "kind": "protocol", "message": "unknown request " + t})
)PY";

double now_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void sleep_s(double s) { std::this_thread::sleep_for(std::chrono::duration<double>(s)); }

int free_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
  socklen_t len = sizeof a;
  getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
  ::close(fd);
  return ntohs(a.sin_port);
}

class TempDir {
 public:
  TempDir() {
    std::string pattern = (fs::temp_directory_path() / "vyrn-rtlink-XXXXXX").string();
    path_ = ::mkdtemp(pattern.data());
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  fs::path path() const { return path_; }

 private:
  std::string path_;
};

void write_file(const fs::path& path, const std::string& text) { std::ofstream(path) << text; }

// Short holds and restarts; servers on 127.0.0.1 only.
struct Hooks {
  Hooks() {
    saved = vyrn::run::test_hooks::rt_timings;
    vyrn::run::test_hooks::rt_timings.late_cut_periods = 1e9;
    vyrn::run::test_hooks::link_bind_address = "127.0.0.1";
    vyrn::run::test_hooks::rt_trace = [this](const std::string& step) {
      std::lock_guard<std::mutex> lock(mu);
      trace.push_back(step);
    };
  }
  ~Hooks() {
    vyrn::run::test_hooks::rt_timings = saved;
    vyrn::run::test_hooks::link_bind_address = "0.0.0.0";
    vyrn::run::test_hooks::rt_trace = nullptr;
  }
  std::vector<std::string> steps() {
    std::lock_guard<std::mutex> lock(mu);
    return trace;
  }
  vyrn::run::RtTimings saved;
  std::mutex mu;
  std::vector<std::string> trace;
};

struct LinkProject {
  explicit LinkProject(bool lockstep, double dps = 20, double round_seconds = 60) : port(free_port()) {
    data = tmp.path() / "data";
    project = tmp.path() / "game";
    fs::create_directories(data / "python");
    fs::create_directories(project);
    write_file(data / "python" / "vyrn_worker.py", kFakeWorker);
    write_file(project / "inputs.py", "import vyrn\ndef see():\n    return [0, 0]\n");
    write_file(project / "responses.py", "jump = \"game jump\"\nwait = None\n");
    write_file(project / "points.py", "def points():\n    return 1\n");
    write_file(project / "vyrn.json", json{{"name", "Link test"},
                                           {"decisions_per_second", dps},
                                           {"round_seconds", round_seconds},
                                           {"link", {{"port", port}, {"lockstep", lockstep}, {"frames_per_step", 2}}}}
                                          .dump(2));
  }

  RunConfig config(RunConfig::Action action) const {
    RunConfig cfg;
    cfg.action = action;
    cfg.project_dir = project.string();
    cfg.data_dir = data.string();
    cfg.seed = 3;
    return cfg;
  }

  std::vector<json> requests(const std::string& type = "") const {
    std::vector<json> out;
    std::ifstream in(project / "fake_log.jsonl");
    for (std::string line; std::getline(in, line);) {
      json j = json::parse(line, nullptr, false);
      if (!j.is_discarded() && (type.empty() || j.value("t", "") == type)) out.push_back(std::move(j));
    }
    return out;
  }

  TempDir tmp;
  fs::path data, project;
  int port;
};

struct Outcome {
  std::vector<json> events;
  bool finished = false;
  double seconds = 0;
};

// Runs the session to its end (each test's run takes a few seconds; the timeout is for a very busy machine).
Outcome run(const RunConfig& cfg, std::function<void(vyrn::Session&, const json&)> on_event = {},
            double timeout_s = 180) {
  vyrn::Session session(cfg);
  const double started = now_s();
  session.start();
  Outcome out;
  auto take = [&](size_t max) {
    for (const auto& text : session.drain(max)) {
      out.events.push_back(json::parse(text));
      if (on_event) on_event(session, out.events.back());
    }
  };
  while (!session.finished() && now_s() - started < timeout_s) {
    take(1000);
    sleep_s(0.01);
  }
  take(1u << 30);
  out.finished = session.finished();
  out.seconds = now_s() - started;
  return out;
}

std::vector<json> of_type(const Outcome& o, const std::string& type) {
  std::vector<json> out;
  for (const auto& e : o.events) {
    if (e.value("type", "") == type) out.push_back(e);
  }
  return out;
}

std::string dump_events(const Outcome& o) {
  std::string text;
  for (const auto& e : o.events) {
    if (e.value("type", "") != "timing") text += e.dump() + "\n";
  }
  return text;
}

bool has_log(const Outcome& o, const std::string& words) {
  for (const auto& e : of_type(o, "log")) {
    if (e.value("text", "").find(words) != std::string::npos) return true;
  }
  return false;
}

// ---- the game ------------------------------------------------------------------------------------------------------

constexpr double kHandshakeWait = 30;   // Vyrn answers the upgrade and the hello at once; this is for a busy machine

// What the game does is tied to what Vyrn does (inputs that came, Vyrn saying that it waits), never to the clock
// alone, so a busy machine changes only how long a test takes.
struct GameOptions {
  bool connect_when_told = false;   // connect only after connect_now() (the runner waits for the game until then)
  int64_t die_at = -1;              // the state with this frame says "dead" until a restart message comes
  int leave_after_inputs = -1;      // close the connection after the first state sent once this many inputs came,
                                    // then connect again (left_at() is that state's frame)
  int ping_after_inputs = -1;       // send the event {"name":"ping"} with the first state after this many inputs
  int64_t stall_after = -1;         // lockstep: send states up to this frame, then nothing more
  size_t pad_bytes = 0;             // a "pad" string this long in every state
  std::string code;                 // the hello's pairing code
};

// A game that speaks Vyrn Link on its own thread: real time (a state every 10 ms) or lockstep (a state, then wait
// for its input; after a stop it plays on by itself until an input comes). It never sends a frame twice, and it
// keeps what Vyrn sent.
class Game {
 public:
  Game(int port, GameOptions o) : port_(port), o_(std::move(o)) {
    thread_ = std::thread([this] { main(); });
  }
  ~Game() { finish(); }
  void finish() {
    quit_ = true;
    if (thread_.joinable()) thread_.join();
  }
  void connect_now() { go_ = true; }
  int64_t left_at() const { return left_at_; }

  std::vector<json> received() {
    std::lock_guard<std::mutex> lock(mu_);
    return received_;
  }
  std::set<int64_t> sent_frames() {
    std::lock_guard<std::mutex> lock(mu_);
    return sent_;
  }
  json welcome() {
    std::lock_guard<std::mutex> lock(mu_);
    return welcome_;
  }
  int connections() const { return connections_; }

 private:
  void main() {
    while (o_.connect_when_told && !go_ && !quit_) sleep_s(0.01);
    while (!quit_) {
      if (!connect()) {
        sleep_s(0.05);
        continue;
      }
      const bool again = play();
      ::close(fd_);
      fd_ = -1;
      if (!again) break;
      sleep_s(0.3);   // like a mod: try again in a moment
    }
  }

  bool connect() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port_));
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    buf_.clear();
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    send_raw("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " +
             key + "\r\nSec-WebSocket-Version: 13\r\n\r\n");
    const double end = now_s() + kHandshakeWait;
    while (ws::http_head_size(buf_) == 0) {
      if (!fill(end - now_s())) break;
    }
    const size_t head = ws::http_head_size(buf_);
    if (head == 0 || buf_.rfind("HTTP/1.1 101", 0) != 0) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }
    buf_.erase(0, head);
    send_json({{"type", "hello"}, {"protocol", 1}, {"game", "GTest Game"}, {"client", "gtest 1.0"}, {"code", o_.code}});
    auto welcome = recv_json(kHandshakeWait);
    if (!welcome || (*welcome)["type"] != "welcome") {
      ::close(fd_);
      fd_ = -1;
      return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    welcome_ = *welcome;
    ++connections_;
    return true;
  }

  // Returns true to connect again (leave_after_inputs), false when done.
  bool play() {
    const bool lockstep = welcome()["lockstep"].get<bool>();
    bool free = false;   // lockstep after a stop: play on by itself until an input comes
    for (;;) {
      if (quit_) return false;
      if (o_.stall_after >= 0 && frame_ > o_.stall_after) {   // a game that hangs: answers nothing, sends nothing
        sleep_s(0.01);
        continue;
      }
      send_state();
      if (o_.leave_after_inputs >= 0 && inputs_ >= o_.leave_after_inputs) {
        left_at_ = frame_;
        ++frame_;
        o_.leave_after_inputs = -1;
        return true;
      }
      if (lockstep && !free) {
        // Wait for the input that answers this frame, however long Vyrn takes, or a stop. Either way the game goes
        // on to the next frame (after a stop it plays on by itself), like a real one: Vyrn answers every state
        // once, so a frame sent twice could be answered twice.
        const int64_t f = frame_;
        bool answered = false;
        while (!answered && !free && !quit_) {
          auto m = recv_json(0.05);
          if (!m) {
            if (closed_) return false;
            continue;
          }
          take(*m);
          if ((*m)["type"] == "input" && (*m)["frame"] == f) answered = true;
          if ((*m)["type"] == "stop") free = true;
        }
        ++frame_;
      } else {
        ++frame_;
        const double end = now_s() + 0.01;
        while (now_s() < end) {
          auto m = recv_json(std::max(0.0, end - now_s()));
          if (!m) {
            if (closed_) return false;
            break;
          }
          take(*m);
          if ((*m)["type"] == "input") free = false;
          if ((*m)["type"] == "stop") free = true;
        }
      }
    }
  }

  void take(const json& m) {
    if (m["type"] == "input") ++inputs_;
    if (m["type"] == "restart") {
      dead_ = false;
      send_json({{"type", "event"}, {"name", "reset"}});
    }
    std::lock_guard<std::mutex> lock(mu_);
    received_.push_back(m);
  }

  void send_state() {
    if (frame_ == o_.die_at) dead_ = true;
    json state = {{"type", "state"}, {"frame", frame_}, {"y", frame_ % 7}, {"dead", dead_}};
    if (o_.pad_bytes > 0) state["pad"] = std::string(o_.pad_bytes, 'x');
    send_json(state);
    if (o_.ping_after_inputs >= 0 && inputs_ >= o_.ping_after_inputs) {
      send_json({{"type", "event"}, {"name", "ping"}, {"frame", frame_}});
      o_.ping_after_inputs = -1;
    }
    std::lock_guard<std::mutex> lock(mu_);
    sent_.insert(frame_);
  }

  void send_raw(const std::string& bytes) {
    size_t off = 0;
    while (off < bytes.size()) {
      const ssize_t n = ::send(fd_, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL);
      if (n <= 0) return;
      off += static_cast<size_t>(n);
    }
  }
  void send_json(const json& j) {
    static const uint8_t mask[4] = {1, 2, 3, 4};
    send_raw(ws::encode_frame(ws::kText, j.dump(), true, mask));
  }
  std::optional<json> recv_json(double timeout_s) {
    const double end = now_s() + timeout_s;
    for (;;) {
      size_t pos = 0;
      auto f = ws::take_frame(buf_, pos, SIZE_MAX, false);
      if (f) {
        buf_.erase(0, pos);
        if (f->opcode == ws::kText) return json::parse(f->payload);
        if (f->opcode == ws::kPing) {
          static const uint8_t mask[4] = {1, 2, 3, 4};
          send_raw(ws::encode_frame(ws::kPong, f->payload, true, mask));
        }
        if (f->opcode == ws::kClose) {
          closed_ = true;
          return std::nullopt;
        }
        continue;
      }
      if (!fill(end - now_s())) return std::nullopt;
    }
  }
  bool fill(double timeout_s) {
    if (fd_ < 0) return false;
    pollfd p{fd_, POLLIN, 0};
    if (::poll(&p, 1, static_cast<int>(std::max(0.0, timeout_s) * 1000)) <= 0) return false;
    char b[65536];
    const ssize_t n = ::recv(fd_, b, sizeof b, 0);
    if (n <= 0) {
      closed_ = true;
      return false;
    }
    buf_.append(b, static_cast<size_t>(n));
    return true;
  }

  int port_;
  GameOptions o_;
  std::thread thread_;
  std::atomic<bool> quit_{false}, go_{false};
  std::atomic<int64_t> left_at_{-1};
  int64_t inputs_ = 0;   // inputs that came (on this thread only)
  int fd_ = -1;
  bool closed_ = false;
  std::string buf_;
  int64_t frame_ = 100;
  bool dead_ = false;
  std::mutex mu_;
  std::vector<json> received_;
  std::set<int64_t> sent_;
  json welcome_;
  std::atomic<int> connections_{0};
};

// An on_event for run(): the game connects once Vyrn says that it waits for it (so Vyrn surely waits first).
std::function<void(vyrn::Session&, const json&)> connect_when_waited_for(Game& game) {
  return [&game](vyrn::Session&, const json& e) {
    if (e.value("type", "") == "state" && e.value("state", "") == "holding" &&
        e.value("detail", "").find("Waiting for the game: connect it to ") != std::string::npos) {
      game.connect_now();
    }
  };
}

std::vector<json> of_kind(const std::vector<json>& messages, const std::string& type) {
  std::vector<json> out;
  for (const auto& m : messages) {
    if (m.value("type", "") == type) out.push_back(m);
  }
  return out;
}

}  // namespace

// Real time: Train starts listening before Python loads, waits for the game ("Waiting for the game", hold reason
// "game"), hands the game's state and events to every observe, and sends each tick's buttons with the frame of
// the state they answer; restart()'s vyrn.send() reaches the game, and so does a send from observe.
TEST(RealtimeLink, RealTimeTrainWaitsForTheGameAndPlaysIt) {
  Hooks hooks;
  LinkProject p(false);
  GameOptions o;
  o.connect_when_told = true;   // once Vyrn says that it waits for the game
  o.ping_after_inputs = 5;      // while Vyrn plays: an observe takes it (restart and check_over would too)
  Game game(p.port, o);
  RunConfig cfg = p.config(RunConfig::Action::Train);
  cfg.max_ticks = 40;
  const Outcome out = run(cfg, connect_when_waited_for(game));
  game.finish();
  ASSERT_TRUE(out.finished);
  ASSERT_TRUE(of_type(out, "error").empty()) << dump_events(out);

  // Where to connect, then the game.
  const std::vector<json> links = of_type(out, "link");
  ASSERT_GE(links.size(), 2u) << dump_events(out);
  EXPECT_EQ(links[0]["url"], "ws://127.0.0.1:" + std::to_string(p.port) + "/");
  EXPECT_EQ(links[0]["connected"], false);
  const std::string code = links[0]["code"];
  EXPECT_EQ(code.size(), 6u);
  EXPECT_EQ(code, vyrn::link_code(p.project.string())) << "the project keeps its code in .vyrn-link-code";
  struct stat st {};
  ASSERT_EQ(::stat((p.project / vyrn::kLinkCodeFile).c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0600u) << "only the owner can read the code";
  EXPECT_EQ(links[1]["connected"], true);
  EXPECT_EQ(links[1]["game"], "GTest Game");
  EXPECT_EQ(links[1]["client"], "gtest 1.0");
  EXPECT_EQ(links[1]["from"], "127.0.0.1");
  EXPECT_TRUE(has_log(out, "GTest Game (gtest 1.0) connected from 127.0.0.1")) << dump_events(out);

  bool waited = false;
  for (const auto& s : of_type(out, "state")) {
    if (s["state"] == "holding" && s["detail"].get<std::string>().find("Waiting for the game: connect it to ws://127.0.0.1:" +
                                                                        std::to_string(p.port) + "/ with the pairing "
                                                                        "code " + code) != std::string::npos) {
      waited = true;
    }
  }
  EXPECT_TRUE(waited) << dump_events(out);
  const std::vector<json> holds = of_type(out, "hold");
  ASSERT_GE(holds.size(), 2u);
  EXPECT_EQ(holds[0]["reason"], "game");
  EXPECT_EQ(holds[0]["on"], true);
  EXPECT_EQ(holds[1]["on"], false);

  // The welcome has the spec's buttons and the run's pace.
  const json welcome = game.welcome();
  EXPECT_EQ(welcome["buttons"], json::array({"jump"}));
  EXPECT_EQ(welcome["lockstep"], false);
  EXPECT_EQ(welcome["tick_hz"], 20);
  EXPECT_EQ(welcome["project"], "Link test");

  // Python got the game's state with every observe, and each event once.
  const std::vector<json> observes = p.requests("observe");
  ASSERT_GE(observes.size(), 40u);
  int pings = 0;
  for (const auto& r : observes) {
    EXPECT_EQ(r["connected"], true) << r.dump();
    ASSERT_TRUE(r.contains("game")) << r.dump();
    EXPECT_EQ(r["game"]["type"], "state");
    EXPECT_GE(r["game"]["frame"].get<int64_t>(), 100);
    for (const auto& e : r["game_events"]) pings += e.value("name", "") == "ping";
  }
  EXPECT_EQ(pings, 1);
  const std::vector<json> restarts = p.requests("restart");
  ASSERT_FALSE(restarts.empty());
  EXPECT_TRUE(restarts[0].contains("game"));

  // The game: restart (restart()'s send) before the first input; inputs for frames it sent, in order, holding
  // jump or nothing; the pong from observe; a stop at the end.
  const std::vector<json> got = game.received();
  ASSERT_FALSE(got.empty());
  EXPECT_EQ(got[0]["type"], "restart") << got[0].dump();
  const std::vector<json> inputs = of_kind(got, "input");
  EXPECT_GE(inputs.size(), 38u);
  const std::set<int64_t> sent = game.sent_frames();
  int64_t last = -1;
  for (const auto& in : inputs) {
    const int64_t f = in["frame"];
    EXPECT_TRUE(sent.count(f)) << in.dump();
    EXPECT_GE(f, last);
    last = f;
    EXPECT_TRUE(in["hold"] == json::array({"jump"}) || in["hold"] == json::array()) << in.dump();
    EXPECT_EQ(in["tap"], json::array());
  }
  EXPECT_EQ(of_kind(got, "pong").size(), 1u);
  EXPECT_EQ(got.back()["type"], "stop");
}

// Lockstep: every state is answered exactly once, with its own frame; the game sets the pace. A death ends the
// round: stop (the game plays on by itself), restart()'s message, restart polling on fresh states, and the next
// round's first input puts the game back into lockstep.
TEST(RealtimeLink, LockstepAnswersEveryStateOnceAndRestarts) {
  Hooks hooks;
  LinkProject p(true, 10);   // 10 decisions/s: lockstep must go much faster than that
  GameOptions o;
  o.die_at = 130;
  Game game(p.port, o);
  RunConfig cfg = p.config(RunConfig::Action::Train);
  cfg.max_ticks = 80;
  const Outcome out = run(cfg);
  game.finish();
  ASSERT_TRUE(out.finished);
  ASSERT_TRUE(of_type(out, "error").empty()) << dump_events(out);
  EXPECT_LT(out.seconds, 6.0) << "80 lockstep ticks at 10/s would take 8 s: the game sets the pace";
  EXPECT_EQ(game.welcome()["lockstep"], true);
  EXPECT_EQ(game.welcome()["frames_per_step"], 2);

  const std::vector<json> got = game.received();
  const std::vector<json> inputs = of_kind(got, "input");
  ASSERT_GE(inputs.size(), 79u);
  std::set<int64_t> answered;
  for (const auto& in : inputs) {
    const int64_t f = in["frame"];
    EXPECT_TRUE(answered.insert(f).second) << "frame " << f << " answered twice";
  }
  // The round that ended at the death: stop, then restart, then (after the game restarted) inputs again.
  size_t stop_at = got.size(), restart_at = got.size(), input_after = got.size();
  for (size_t i = 0; i < got.size(); ++i) {
    if (got[i]["type"] == "input" && got[i]["frame"].get<int64_t>() >= 130 && stop_at == got.size()) {
      ADD_FAILURE() << "the dead state 130 was answered with an input: " << got[i].dump();
    }
    if (got[i]["type"] == "stop" && stop_at == got.size()) stop_at = i;
    if (got[i]["type"] == "restart" && i > stop_at && restart_at == got.size()) restart_at = i;
    if (got[i]["type"] == "input" && i > restart_at && input_after == got.size()) input_after = i;
  }
  ASSERT_LT(stop_at, got.size()) << "a stop when the round ended";
  ASSERT_LT(restart_at, got.size()) << "restart() asked the game to restart";
  ASSERT_LT(input_after, got.size()) << "lockstep went on in the next round";
  const std::vector<json> rounds = of_type(out, "round");
  ASSERT_GE(rounds.size(), 1u);
  EXPECT_EQ(rounds[0]["game_over"], true);
  const std::vector<std::string> steps = hooks.steps();
  EXPECT_NE(std::find(steps.begin(), steps.end(), "end terminal"), steps.end());
}

// The game leaves in the middle of a round: the AI waits (the open move is cut), and goes on when it is back.
TEST(RealtimeLink, ADisconnectPausesTheAIUntilTheGameIsBack) {
  Hooks hooks;
  LinkProject p(false);
  GameOptions o;
  o.connect_when_told = true;   // so Vyrn surely waits for it first
  o.leave_after_inputs = 5;     // in the middle of a round, while Vyrn plays
  Game game(p.port, o);
  RunConfig cfg = p.config(RunConfig::Action::Train);
  cfg.max_ticks = 60;
  const Outcome out = run(cfg, connect_when_waited_for(game));
  game.finish();
  ASSERT_TRUE(out.finished);
  ASSERT_TRUE(of_type(out, "error").empty()) << dump_events(out);
  EXPECT_EQ(game.connections(), 2);
  const std::vector<json> links = of_type(out, "link");
  std::vector<bool> connected;
  for (const auto& l : links) connected.push_back(l["connected"].get<bool>());
  EXPECT_EQ(connected, (std::vector<bool>{false, true, false, true})) << dump_events(out);
  EXPECT_TRUE(has_log(out, "GTest Game disconnected.")) << dump_events(out);
  int game_holds = 0;
  for (const auto& h : of_type(out, "hold")) game_holds += h["reason"] == "game" && h["on"] == true;
  EXPECT_GE(game_holds, 2) << "before the first connection, and after the game left";
  const std::vector<std::string> steps = hooks.steps();
  EXPECT_NE(std::find(steps.begin(), steps.end(), "cut"), steps.end()) << "the move before the game left is dropped";
  // After the reconnect, observe sees the new connection's states (frames go on from left_at() + 1).
  ASSERT_GE(game.left_at(), 100);
  bool after = false;
  for (const auto& r : p.requests("observe")) {
    after = after || r["game"]["frame"].get<int64_t>() > game.left_at() + 1;
  }
  EXPECT_TRUE(after);
}

// Stop while lockstep waits for a game that hangs: the wait ends at once, the brain is saved, the game hears stop.
TEST(RealtimeLink, StopEndsAWaitForAHungGameAtOnce) {
  Hooks hooks;
  LinkProject p(true);
  GameOptions o;
  o.stall_after = 110;
  Game game(p.port, o);
  RunConfig cfg = p.config(RunConfig::Action::Train);
  double stop_at = 0;
  const Outcome out = run(cfg, [&](vyrn::Session& s, const json& e) {
    if (stop_at == 0 && e.value("type", "") == "state" &&
        e.value("detail", "").find("waiting for the game's next state") != std::string::npos) {
      stop_at = now_s();
      s.stop();
    }
  });
  game.finish();
  ASSERT_TRUE(out.finished);
  ASSERT_GT(stop_at, 0) << "lockstep said that it waits for the game\n" << dump_events(out);
  EXPECT_TRUE(of_type(out, "error").empty()) << dump_events(out);
  const std::vector<json> states = of_type(out, "state");
  EXPECT_EQ(states.back()["state"], "finished");
  EXPECT_EQ(of_kind(game.received(), "stop").empty(), false);
}

// Another program has the port: Train/Watch can't run (a plain error, reason "link"); Check goes on without the
// game.
TEST(RealtimeLink, APortInUseIsARunError) {
  Hooks hooks;
  LinkProject p(false);
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(p.port));
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  ASSERT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
  ASSERT_EQ(::listen(fd, 1), 0);

  const Outcome train = run(p.config(RunConfig::Action::Train));
  ASSERT_TRUE(train.finished);
  const std::vector<json> errors = of_type(train, "error");
  ASSERT_EQ(errors.size(), 1u) << dump_events(train);
  EXPECT_EQ(errors[0]["reason"], "link");
  EXPECT_EQ(errors[0]["fatal"], true);
  EXPECT_NE(errors[0]["message"].get<std::string>().find("port " + std::to_string(p.port)), std::string::npos);
  EXPECT_TRUE(p.requests().empty()) << "Python never started";

  const Outcome check = run(p.config(RunConfig::Action::Check));
  ASSERT_TRUE(check.finished);
  EXPECT_TRUE(of_type(check, "error").empty()) << dump_events(check);
  bool warned = false;
  for (const auto& w : of_type(check, "warning")) {
    warned = warned || w["message"].get<std::string>().find("Check goes on without the game") != std::string::npos;
  }
  EXPECT_TRUE(warned) << dump_events(check);
  ::close(fd);
}

// Check reads a connected game's state but never sends it anything (no input, no vyrn.send message); without a
// game it says so instead of warning that every input stays the same.
TEST(RealtimeLink, CheckReadsTheGameAndSendsNothing) {
  Hooks hooks;
  {
    LinkProject p(false);
    Game game(p.port, GameOptions{});
    const Outcome out = run(p.config(RunConfig::Action::Check));
    game.finish();
    ASSERT_TRUE(out.finished);
    ASSERT_TRUE(of_type(out, "error").empty()) << dump_events(out);
    const std::vector<json> observes = p.requests("observe");
    ASSERT_EQ(observes.size(), 5u);
    for (const auto& r : observes) {
      EXPECT_EQ(r["connected"], true);
      EXPECT_GE(r["game"]["frame"].get<int64_t>(), 100);
    }
    EXPECT_TRUE(p.requests("restart")[0].contains("game"));
    for (const auto& m : game.received()) EXPECT_EQ(m["type"], "stop") << "Check sent " << m.dump();
  }
  {
    LinkProject p(false);
    const Outcome out = run(p.config(RunConfig::Action::Check));
    ASSERT_TRUE(out.finished);
    const std::vector<json> checks = of_type(out, "check");
    ASSERT_EQ(checks.size(), 1u);
    bool noted = false;
    for (const auto& n : checks[0]["notes"]) noted = noted || n.get<std::string>().find("No game was connected") == 0;
    EXPECT_TRUE(noted) << checks[0].dump();
    for (const auto& w : of_type(out, "warning")) {
      EXPECT_EQ(w["message"].get<std::string>().find("never changed"), std::string::npos) << w.dump();
    }
    EXPECT_FALSE(of_type(out, "link").empty());
  }
}

// A state close to 1 MiB still reaches Python: observe stays under the worker's 1 MiB limit (the biggest field
// is left out, with one warning), and the run goes on.
TEST(RealtimeLink, ABigStateIsTrimmedToFitPythonsLimit) {
  Hooks hooks;
  LinkProject p(false);
  GameOptions o;
  o.pad_bytes = (1u << 20) - 200;   // the whole state stays under Vyrn Link's own 1 MiB limit
  Game game(p.port, o);
  RunConfig cfg = p.config(RunConfig::Action::Train);
  cfg.max_ticks = 10;
  const Outcome out = run(cfg);
  game.finish();
  ASSERT_TRUE(out.finished);
  ASSERT_TRUE(of_type(out, "error").empty()) << dump_events(out);
  const std::vector<json> observes = p.requests("observe");
  ASSERT_GE(observes.size(), 10u);
  for (const auto& r : observes) {
    EXPECT_LT(r["bytes"].get<int64_t>(), 1 << 20);
    EXPECT_FALSE(r["game"].contains("pad")) << "the biggest field is left out";
    EXPECT_GE(r["game"]["frame"].get<int64_t>(), 100) << "the rest is there";
  }
  int warnings = 0;
  for (const auto& w : of_type(out, "warning")) {
    warnings += w["message"].get<std::string>().find("leaves out its biggest fields: \"pad\"") != std::string::npos;
  }
  EXPECT_EQ(warnings, 1) << dump_events(out);
}

#else  // VYRN_TEST_POSIX

TEST(RealtimeLink, SkippedOnThisSystem) {
  GTEST_SKIP() << "POSIX only: the fake worker speaks the POSIX transport (fd 3).";
}

#endif  // VYRN_TEST_POSIX
