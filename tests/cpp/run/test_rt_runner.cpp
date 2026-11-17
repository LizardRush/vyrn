// RealtimeRunner's Train and Watch against a scripted fake worker (plain Python, no vyrn library): the exact
// requests it sends through countdown, focus lock, restart polling, rounds, holds, game over and the round
// limit, what it records for the brain, and how stop(), set_hold() and slow ticks are handled.
// The fake replies by request count from <project>/fake_script.json and logs every request (with its time) to
// <project>/fake_log.jsonl. Nothing presses keys (VYRN_KEYS_BACKEND=record:... anyway) or opens a window.
#include "platform_test.h"

#if VYRN_TEST_POSIX

#include "run/rt_runner.h"
#include "vyrn/session.h"

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
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using nlohmann::json;
using vyrn::RunConfig;
using vyrn::RunState;

namespace {

const char* const kFakeWorker = R"PY(
import json, os, signal, socket, struct, sys, time

PROJECT = sys.argv[1]
with open(os.path.join(PROJECT, "fake_script.json")) as f:
    SCRIPT = json.load(f)
LOG = os.path.join(PROJECT, "fake_log.jsonl")
START = time.monotonic()
sock = socket.socket(fileno=3)

def log(entry):
    entry["at"] = round(time.monotonic() - START, 4)
    with open(LOG, "a") as f:
        f.write(json.dumps(entry) + "\n")

signal.signal(signal.SIGUSR1, lambda signum, frame: log({"t": "SIGUSR1"}))

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
    return json.loads(recv_exact(json_len + bin_len)[:json_len])

def take(name, default):
    queue = SCRIPT.get(name) or []
    return queue.pop(0) if queue else default

send({"t": "hello", "id": 0, "proto": 1, "pid": os.getpid(), "python": "fake",
      "backends": {"screen": None, "keys": "record"}})
observes = rounds = 0
locked = False
while True:
    msg = recv()
    if msg is None:
        sys.exit(0)
    t, rid = msg["t"], msg["id"]
    entry = {"t": t}
    if t == "observe":
        entry["first"] = msg.get("first")
    elif t == "act":
        entry["a"] = msg.get("a")
    elif t == "load":
        entry["allow_input"] = msg.get("allow_input")
    log(entry)
    if t == "load":
        send({"t": "spec", "id": rid, "mode": "realtime",
              "obs": {"kind": "numbers", "shape": [2], "names": ["tick", "round"]},
              "choices": ["left", "right", "wait"], "labels": ["left", "right", "wait"],
              "effects": ["hold A", "hold D", "do nothing"],
              "uses": {"screen": False, "keys": SCRIPT.get("keys", False)},
              "hooks": {"see": "inputs.py:1", "points": "points.py:1", "game_over": "points.py:4",
                        "restart": "points.py:7"},
              "warnings": []})
    elif t == "lock_focus":
        locked = True
        send({"t": "ok", "id": rid, "app_id": "fake-game", "title": "Fake game"})
    elif t == "restart":
        send({"t": "ok", "id": rid, "did": [], "ms": {"restart": 0.1}})
    elif t == "check_over":
        send({"t": "ok", "id": rid, "game_over": take("check_over", False), "ms": {"game_over": 0.1}})
    elif t == "observe":
        observes += 1
        first = bool(msg.get("first"))
        rounds += first
        rule = SCRIPT.get("observe", {}).get(str(observes), {})
        if rule.get("sleep"):
            time.sleep(rule["sleep"])
        send({"t": "result", "id": rid, "obs": {"$a": [0, "f32", [2]]},
              "points": None if first else rule.get("points", 1.0),
              "game_over": False if first else rule.get("game_over", False),
              "truncated": False, "first": first, "final_obs": None,
              "focused": rule.get("focused", True) if locked else None,
              "show": {}, "did": [], "warnings": [],
              "ms": {"see": 0.5, "points": 0.1, "game_over": 0.1, "respond": 0.0, "step": 0.0}},
             struct.pack("<2f", observes, rounds))
    elif t == "act":
        send({"t": "ok", "id": rid, "did": [], "ms": {"respond": 0.2}})
    elif t == "status":
        focused = take("status", True)
        send({"t": "ok", "id": rid, "focused": focused if locked else True, "active": None, "held": [],
              "frozen": False})
    elif t in ("pause", "resume"):
        send({"t": "ok", "id": rid})
    elif t == "close":
        send({"t": "bye", "id": rid})
        sys.exit(0)
    else:
        send({"t": "error", "id": rid, "fatal": False, "kind": "protocol", "message": "unknown request " + t})
)PY";

class TempDir {
 public:
  TempDir() {
    std::string pattern = (fs::temp_directory_path() / "vyrn-rt-XXXXXX").string();
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

// A short countdown; late cuts off unless a test wants them (a busy machine must not add requests).
struct Timings {
  Timings() {
    auto& t = vyrn::run::test_hooks::rt_timings;
    saved = t;
    t.countdown_s = 0.2;
    t.late_cut_periods = 1e9;
  }
  ~Timings() { vyrn::run::test_hooks::rt_timings = saved; }
  vyrn::run::RtTimings saved;
};

// Collects the brain-side steps.
struct Trace {
  Trace() {
    vyrn::run::test_hooks::rt_trace = [this](const std::string& step) {
      std::lock_guard<std::mutex> lock(mu);
      steps.push_back(step);
    };
  }
  ~Trace() { vyrn::run::test_hooks::rt_trace = nullptr; }
  std::vector<std::string> get() {
    std::lock_guard<std::mutex> lock(mu);
    return steps;
  }
  std::mutex mu;
  std::vector<std::string> steps;
};

// A project for the fake worker, and the data folder whose python/vyrn_worker.py is the fake.
struct FakeProject {
  explicit FakeProject(const json& script, double dps = 20, double round_seconds = 60) {
    data = tmp.path() / "data";
    project = tmp.path() / "rt";
    fs::create_directories(data / "python");
    fs::create_directories(project);
    write_file(data / "python" / "vyrn_worker.py", kFakeWorker);
    write_file(project / "inputs.py", "def see():\n    return [0, 0]\n");
    write_file(project / "responses.py", "left = \"a\"\nright = \"d\"\nwait = None\n");
    write_file(project / "points.py", "def points():\n    return 1\n");
    write_file(project / "fake_script.json", script.dump());
    write_file(project / "vyrn.json",
               json{{"decisions_per_second", dps}, {"round_seconds", round_seconds}}.dump(2));
    keys_log = tmp.path() / "keys.log";
    ::setenv("VYRN_KEYS_BACKEND", ("record:" + keys_log.string()).c_str(), 1);
  }
  ~FakeProject() { ::unsetenv("VYRN_KEYS_BACKEND"); }

  RunConfig config(RunConfig::Action action) const {
    RunConfig cfg;
    cfg.action = action;
    cfg.project_dir = project.string();
    cfg.data_dir = data.string();
    cfg.seed = 7;
    return cfg;
  }

  std::vector<json> requests() const {
    std::vector<json> out;
    std::ifstream in(project / "fake_log.jsonl");
    for (std::string line; std::getline(in, line);) {
      json j = json::parse(line, nullptr, false);   // the fake may be writing the last line right now
      if (!j.is_discarded()) out.push_back(std::move(j));
    }
    return out;
  }

  TempDir tmp;
  fs::path data, project, keys_log;
};

struct Outcome {
  std::vector<json> events;
  bool finished = false;
  double seconds = 0;
};

// Runs a session to the end. `on_event` may call into the session (stop, set_hold...); `tick` runs every 10 ms.
Outcome run(const RunConfig& cfg, std::function<void(vyrn::Session&, const json&)> on_event = {},
            std::function<void(vyrn::Session&)> tick = {}, double timeout_s = 60) {
  vyrn::Session session(cfg);
  const auto started = std::chrono::steady_clock::now();
  session.start();
  Outcome out;
  const auto deadline = started + std::chrono::duration<double>(timeout_s);
  auto take = [&](size_t max) {
    for (const auto& text : session.drain(max)) {
      out.events.push_back(json::parse(text));
      if (on_event) on_event(session, out.events.back());
    }
  };
  while (!session.finished() && std::chrono::steady_clock::now() < deadline) {
    take(1000);
    if (tick) tick(session);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  take(1u << 30);
  out.finished = session.finished();
  out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
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
    if (e.value("type", "") == "timing") continue;
    text += e.dump() + "\n";
  }
  return text;
}

// "observe+" (first), "observe", "act", "status"... in order, without the load and hello.
std::vector<std::string> names(const std::vector<json>& requests) {
  std::vector<std::string> out;
  for (const auto& r : requests) {
    const std::string t = r.value("t", "");
    if (t == "load") continue;
    out.push_back(t == "observe" && r.value("first", false) ? "observe+" : t);
  }
  return out;
}

std::string joined(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& x : v) s += (s.empty() ? "" : " ") + x;
  return s;
}

}  // namespace

// One Train through every state: countdown, lock, restart polling, a hold (the game window lost focus) and the
// refocus, a game over, restart polling again, a round that reaches the round limit, the next round without
// restart(), and the end at --ticks. Then Watch plays the saved brain.
TEST(RealtimeRunner, TrainGoesThroughHoldGameOverRestartAndRoundLimit) {
  Timings timings;
  Trace trace;
  const json script = {{"keys", true},
                       {"check_over", {true, false, true, false}},
                       {"status", {true, false, false, true, true}},   // before restart(), 3 hold polls, before restart()
                       {"observe", {{"3", {{"focused", false}}}, {"6", {{"game_over", true}, {"points", -1.0}}}}}};
  FakeProject fake(script, 20, 0.3);   // round limit: 0.3 s x 20/s = 6 ticks
  RunConfig cfg = fake.config(RunConfig::Action::Train);
  cfg.allow_input = true;
  cfg.max_ticks = 10;
  const Outcome o = run(cfg);
  ASSERT_TRUE(o.finished);
  ASSERT_TRUE(of_type(o, "error").empty()) << dump_events(o);

  const std::vector<json> requests = fake.requests();
  ASSERT_FALSE(requests.empty());
  EXPECT_EQ(requests[0]["t"], "load");
  EXPECT_EQ(requests[0]["allow_input"], true);
  const std::vector<std::string> expected = {
      "lock_focus", "status", "restart", "check_over", "check_over",          // countdown, lock, restart polling
      "observe+", "act", "observe", "act",                                     // round 1
      "observe", "pause", "status", "status", "status",                        // unfocused: hold, 3 polls
      "observe", "act", "observe", "act", "observe",                           // refocus ... game over
      "status", "restart", "check_over", "check_over",                         // restart polling
      "observe+", "act", "observe", "act", "observe", "act", "observe", "act",  // round 2: 6 ticks
      "observe", "act", "observe", "act", "observe",                           // ... the round limit
      "observe+", "act", "observe", "act",                                     // round 3, no restart()
      "close"};
  EXPECT_EQ(joined(names(requests)), joined(expected));

  // Status polls while held: 4-10 Hz.
  std::vector<double> polls;
  for (size_t i = 0; i < requests.size(); ++i) {
    if (requests[i]["t"] == "status" && i > 0 && (requests[i - 1]["t"] == "status" || requests[i - 1]["t"] == "pause")) {
      polls.push_back(requests[i]["at"].get<double>());
    }
  }
  ASSERT_EQ(polls.size(), 3u);
  for (size_t i = 1; i < polls.size(); ++i) {
    EXPECT_GE(polls[i] - polls[i - 1], 0.1);   // 5 Hz: 0.2 s apart (a busy machine adds a little)
    EXPECT_LE(polls[i] - polls[i - 1], 1.0);
  }

  // The brain side: the transition open when focus was lost is never recorded (cut), the refocus starts a new
  // segment, a game over ends one terminally, the round limit truncates it. Each record names the choice the
  // act before it sent.
  std::vector<int> acts;
  for (const auto& r : requests) {
    if (r["t"] == "act") acts.push_back(r["a"].get<int>());
  }
  ASSERT_EQ(acts.size(), 12u);
  auto rec = [&](size_t act, const char* points) { return "record " + std::to_string(acts[act]) + " " + points; };
  const std::vector<std::string> steps = {
      "start", rec(0, "1"), "cut",                                        // round 1 until the hold
      "start", rec(2, "1"), rec(3, "-1"), "end terminal",                 // refocus .. game over
      "start", rec(4, "1"), rec(5, "1"), rec(6, "1"), rec(7, "1"), rec(8, "1"), rec(9, "1"), "end truncated",
      "start", rec(10, "1")};
  EXPECT_EQ(joined(trace.get()), joined(steps));

  // Events: the countdown, the hold, two rounds.
  const auto countdown = of_type(o, "countdown");
  ASSERT_GE(countdown.size(), 2u);
  EXPECT_EQ(countdown.back()["seconds_left"], 0);
  const auto holds = of_type(o, "hold");
  ASSERT_EQ(holds.size(), 2u) << dump_events(o);
  EXPECT_EQ(holds[0]["on"], true);
  EXPECT_EQ(holds[0]["reason"], "unfocused");
  EXPECT_EQ(holds[1]["on"], false);
  const auto rounds = of_type(o, "round");
  ASSERT_EQ(rounds.size(), 2u);
  EXPECT_EQ(rounds[0]["points"], 3.0);   // 1 + 1 (seen while away) + 1 + 1 - 1
  EXPECT_EQ(rounds[0]["ticks"], 5);
  EXPECT_EQ(rounds[0]["truncated"], false);
  EXPECT_EQ(rounds[1]["points"], 6.0);
  EXPECT_EQ(rounds[1]["ticks"], 6);
  EXPECT_EQ(rounds[1]["truncated"], true);
  std::vector<std::string> states;
  for (const auto& s : of_type(o, "state")) states.push_back(s["state"]);
  for (const char* s : {"countdown", "restarting", "running", "holding", "stopping", "finished"}) {
    EXPECT_NE(std::find(states.begin(), states.end(), s), states.end()) << s;
  }
  const auto saved = of_type(o, "checkpoint");
  ASSERT_FALSE(saved.empty());
  EXPECT_EQ(saved.back()["which"], "latest");
  EXPECT_TRUE(!fs::exists(fake.keys_log) || fs::file_size(fake.keys_log) == 0);

  // Watch plays the saved brain: no learning, no saves.
  fs::remove(fake.project / "fake_log.jsonl");
  write_file(fake.project / "fake_script.json", json{{"keys", true}}.dump());
  RunConfig watch = fake.config(RunConfig::Action::Watch);
  watch.allow_input = true;
  watch.watch_brain = "latest";
  watch.max_ticks = 4;
  const Outcome w = run(watch);
  ASSERT_TRUE(w.finished);
  ASSERT_TRUE(of_type(w, "error").empty()) << dump_events(w);
  const auto specs = of_type(w, "spec");
  ASSERT_EQ(specs.size(), 1u);
  EXPECT_EQ(specs[0]["checkpoint"], "latest");
  EXPECT_EQ(specs[0]["brain"], "resumed");
  EXPECT_TRUE(of_type(w, "checkpoint").empty());
  EXPECT_EQ(joined(names(fake.requests())),
            "lock_focus status restart check_over observe+ act observe act observe act observe act observe act close");
}

// restart() never clears game_over(): restart() again every restart_every_s, then pause with an error after
// restart_give_up_s. No keys: no countdown, no lock, no status before restart().
TEST(RealtimeRunner, RestartPollingGivesUp) {
  Timings timings;
  vyrn::run::test_hooks::rt_timings.restart_every_s = 0.5;   // 5 ticks at 10 a second
  vyrn::run::test_hooks::rt_timings.restart_give_up_s = 2.0;   // 20 ticks
  FakeProject fake(json{{"check_over", std::vector<bool>(100, true)}}, 10);   // 10 ticks a second
  bool stopped = false;
  const Outcome o = run(fake.config(RunConfig::Action::Train), [&](vyrn::Session& s, const json& e) {
    if (e.value("type", "") == "state" && e.value("state", "") == "error" && !stopped) {
      stopped = true;
      s.stop();
    }
  });
  ASSERT_TRUE(o.finished);
  const auto errors = of_type(o, "error");
  ASSERT_EQ(errors.size(), 1u) << dump_events(o);
  EXPECT_EQ(errors[0]["reason"], "restart_stuck");
  EXPECT_EQ(errors[0]["fatal"], false);
  EXPECT_EQ(errors[0]["script"], "points.py");   // where restart() is
  EXPECT_EQ(errors[0]["line"], 7);
  EXPECT_NE(errors[0]["message"].get<std::string>().find("restart() didn't clear game_over()"), std::string::npos);
  // restart, 5 x check_over (0.5 s), four times (2 s), then close. A tick a busy machine skipped counts as polled.
  const std::vector<std::string> n = names(fake.requests());
  const std::string five = "restart check_over check_over check_over check_over check_over";
  const std::string exact = five + " " + five + " " + five + " " + five + " close";
  if (joined(n) != exact) {
    ASSERT_GE(n.size(), 2u);
    EXPECT_EQ(n.back(), "close") << joined(n);
    int restarts = 0, polls = 0, run = 0;
    for (size_t i = 0; i + 1 < n.size(); ++i) {
      if (n[i] == "restart") {
        ++restarts;
        run = 0;
      } else {
        EXPECT_EQ(n[i], "check_over") << joined(n);
        ++polls;
        EXPECT_LE(++run, 5) << joined(n);
      }
    }
    EXPECT_GE(restarts, 3) << joined(n);
    EXPECT_LE(restarts, 4) << joined(n);
    EXPECT_LE(polls, 20) << joined(n);
  }
  EXPECT_TRUE(of_type(o, "round").empty());
}

// A project that presses keys doesn't play without allow_input.
TEST(RealtimeRunner, KeysNeedAllowInput) {
  FakeProject fake(json{{"keys", true}});
  const Outcome o = run(fake.config(RunConfig::Action::Train));
  ASSERT_TRUE(o.finished);
  const auto errors = of_type(o, "error");
  ASSERT_EQ(errors.size(), 1u) << dump_events(o);
  EXPECT_EQ(errors[0]["reason"], "input_not_allowed");
  EXPECT_EQ(errors[0]["fatal"], true);
  const auto requests = fake.requests();
  ASSERT_EQ(requests.size(), 2u);   // load and close
  EXPECT_EQ(requests[0]["allow_input"], false);
  EXPECT_EQ(requests[1]["t"], "close");
  EXPECT_FALSE(fs::exists(fake.project / "brains"));
}

// The app holds (set_hold) while you use Vyrn: pause (keys up), status polls, and on release the round goes on
// with a new segment.
TEST(RealtimeRunner, AppHoldPausesAndContinues) {
  Timings timings;
  Trace trace;
  FakeProject fake(json::object());
  bool held = false, released = false;
  double held_at = 0;
  RunConfig cfg = fake.config(RunConfig::Action::Train);
  cfg.max_seconds = 30;
  const Outcome o = run(
      cfg,
      [&](vyrn::Session& s, const json& e) {
        if (e.value("type", "") == "hold" && e.value("on", false) == false && released) s.stop();
      },
      [&](vyrn::Session& s) {
        const auto now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const vyrn::LiveView v = s.live();
        if (!held && v.total_ticks >= 5) {
          held = true;
          held_at = now;
          s.set_hold(true);
        } else if (held && !released && now - held_at > 0.7) {
          released = true;
          s.set_hold(false);
        }
      });
  ASSERT_TRUE(o.finished);
  ASSERT_TRUE(of_type(o, "error").empty()) << dump_events(o);
  const auto holds = of_type(o, "hold");
  ASSERT_EQ(holds.size(), 2u) << dump_events(o);
  EXPECT_EQ(holds[0]["reason"], "user");
  EXPECT_EQ(holds[0]["on"], true);
  EXPECT_EQ(holds[1]["on"], false);
  const std::vector<std::string> n = names(fake.requests());
  const auto pause = std::find(n.begin(), n.end(), "pause");
  ASSERT_NE(pause, n.end()) << joined(n);
  int polls = 0;
  auto it = pause + 1;
  for (; it != n.end() && (*it == "status" || *it == "resume"); ++it) polls += *it == "status";
  EXPECT_GE(polls, 2) << joined(n);   // ~0.7 s at 5 Hz
  ASSERT_NE(it, n.end());
  EXPECT_EQ(*it, "observe") << joined(n);   // the round goes on (not observe+)
  const auto steps = trace.get();
  EXPECT_NE(std::find(steps.begin(), steps.end(), "cut"), steps.end()) << joined(steps);
}

// A tick much later than its deadline cuts the segment: keys up (pause), and the next observation starts a new
// segment. The fake takes 5 periods to answer one observe.
TEST(RealtimeRunner, ALateTickCutsTheSegment) {
  Timings timings;
  vyrn::run::test_hooks::rt_timings.late_cut_periods = 3;
  Trace trace;
  FakeProject fake(json{{"observe", {{"4", {{"sleep", 0.25}}}}}}, 20);
  RunConfig cfg = fake.config(RunConfig::Action::Train);
  cfg.max_ticks = 8;
  const Outcome o = run(cfg);
  ASSERT_TRUE(o.finished);
  ASSERT_TRUE(of_type(o, "error").empty()) << dump_events(o);
  bool late = false;
  for (const auto& h : of_type(o, "hold")) late = late || h.value("reason", "") == "late";
  EXPECT_TRUE(late) << dump_events(o);
  // restart check_over observe+ act observe act observe act observe(slow) act pause observe ...
  const std::vector<std::string> n = names(fake.requests());
  size_t slow = 0;
  for (int seen = 0; slow < n.size() && seen < 4; ++slow) seen += n[slow].rfind("observe", 0) == 0;
  ASSERT_LT(slow + 1, n.size()) << joined(n);
  EXPECT_EQ(n[slow], "act") << joined(n);
  EXPECT_EQ(n[slow + 1], "pause") << joined(n);
  const auto steps = trace.get();
  EXPECT_NE(std::find(steps.begin(), steps.end(), "cut"), steps.end()) << joined(steps);
}

// Counts platform::release_all_keys() calls by the runner while it lives.
struct KeyReleases {
  KeyReleases() { vyrn::run::test_hooks::keys_released = [this] { ++count; }; }
  ~KeyReleases() { vyrn::run::test_hooks::keys_released = nullptr; }
  std::atomic<int> count{0};
};

// A worker that may press keys and is killed without answering (frozen by stop(), stuck): Vyrn lets go of every key
// itself, once (macOS/Windows: release_all_keys(); a no-op on Linux). One that answers "close" let go itself.
TEST(RealtimeRunner, KeysAreReleasedOnlyAfterAKeyWorkerIsKilled) {
  Timings timings;
  {
    KeyReleases released;
    FakeProject fake(json{{"keys", true}, {"observe", {{"6", {{"sleep", 30}}}}}});
    RunConfig cfg = fake.config(RunConfig::Action::Train);
    cfg.allow_input = true;
    bool asked = false;
    const Outcome o = run(cfg, {}, [&](vyrn::Session& s) {
      if (asked) return;
      int observes = 0;
      for (const auto& r : fake.requests()) observes += r["t"] == "observe";
      if (observes >= 6) {   // the fake is asleep in the 6th observe
        asked = true;
        s.stop();
      }
    });
    ASSERT_TRUE(o.finished);
    ASSERT_TRUE(asked);
    EXPECT_EQ(fake.requests().back()["t"], "SIGUSR1");   // frozen, never answered, killed (no close)
    EXPECT_EQ(released.count.load(), 1);
  }
  {
    KeyReleases released;
    FakeProject fake(json{{"keys", true}});
    RunConfig cfg = fake.config(RunConfig::Action::Train);
    cfg.allow_input = true;
    cfg.max_ticks = 5;
    const Outcome o = run(cfg);
    ASSERT_TRUE(o.finished);
    EXPECT_EQ(fake.requests().back()["t"], "close");
    EXPECT_EQ(released.count.load(), 0) << "the worker answered close: it let go of its keys itself";
  }
}

// stop() while Python is stuck in user code: freeze it at once (SIGUSR1), and kill its process group when the
// request still hasn't come back 0.5 s later. The run ends quickly and the brain is still saved.
TEST(RealtimeRunner, StopFreezesThenKillsAStuckWorker) {
  FakeProject fake(json{{"observe", {{"6", {{"sleep", 30}}}}}});
  bool asked = false;
  double stop_at = 0;
  auto now = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
  const Outcome o = run(fake.config(RunConfig::Action::Train), {}, [&](vyrn::Session& s) {
    if (asked) return;
    int observes = 0;
    for (const auto& r : fake.requests()) observes += r["t"] == "observe";
    if (observes >= 6) {   // the fake is asleep in the 6th observe
      asked = true;
      stop_at = now();
      s.stop();
    }
  });
  const double finished_at = now();
  ASSERT_TRUE(o.finished);
  ASSERT_TRUE(asked);
  EXPECT_LT(finished_at - stop_at, 4.0);   // 0.5 s to answer the freeze, then SIGKILL and the save
  EXPECT_TRUE(of_type(o, "error").empty()) << dump_events(o);
  const auto requests = fake.requests();
  ASSERT_FALSE(requests.empty());
  EXPECT_EQ(requests.back()["t"], "SIGUSR1");   // frozen, never answered, killed (no close)
  const auto saved = of_type(o, "checkpoint");
  ASSERT_FALSE(saved.empty()) << dump_events(o);
  EXPECT_EQ(saved.back()["which"], "latest");
}

// Another program stops a real-time run like stop(): <project>/.vyrn-stop (Train saves the brain).
TEST(RealtimeRunner, TheStopMarkerStopsARealTimeRun) {
  FakeProject fake(json::object());
  bool marked = false;
  const Outcome o = run(fake.config(RunConfig::Action::Train), {}, [&](vyrn::Session& s) {
    if (!marked && s.live().total_ticks >= 5) {
      marked = true;
      write_file(fake.project / ".vyrn-stop", "");
    }
  });
  ASSERT_TRUE(o.finished);
  ASSERT_TRUE(marked);
  EXPECT_TRUE(of_type(o, "error").empty()) << dump_events(o);
  EXPECT_FALSE(fs::exists(fake.project / ".vyrn-stop"));
  bool logged = false;
  for (const auto& e : of_type(o, "log")) logged = logged || e.value("text", "").find(".vyrn-stop") != std::string::npos;
  EXPECT_TRUE(logged) << dump_events(o);
  const auto saved = of_type(o, "checkpoint");
  ASSERT_FALSE(saved.empty()) << dump_events(o);
  EXPECT_EQ(saved.back()["which"], "latest");
  EXPECT_EQ(fake.requests().back()["t"], "close");
}

#else  // VYRN_TEST_POSIX

TEST(RealtimeRunner, SkippedOnThisSystem) {
  GTEST_SKIP() << "POSIX only: the fake worker speaks the POSIX transport (fd 3, SIGUSR1 for freeze).";
}

#endif  // VYRN_TEST_POSIX
