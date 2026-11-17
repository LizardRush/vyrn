// Whole Sessions with the real Python worker on small projects: Check, Train (save, resume, start over),
// Watch, errors that pause the run, apply_changes, stop, the console throttle and real-time Check (real-time
// Train and Watch: test_rt_runner.cpp).
// Projects are copied to temporary folders; nothing presses keys or opens windows.
#include "vyrn/session.h"

#include <gtest/gtest.h>

#include "platform_test.h"
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using nlohmann::json;
using vyrn::RunConfig;
using vyrn::RunState;

namespace {

class TempDir {
 public:
  TempDir() {
    std::string pattern = (fs::temp_directory_path() / "vyrn-session-XXXXXX").string();
    path_ = ::mkdtemp(pattern.data());
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  std::string path() const { return path_; }

 private:
  std::string path_;
};

std::string copy_project(const std::string& from, const TempDir& tmp, const std::string& name) {
  const fs::path dst = fs::path(tmp.path()) / name;
  fs::copy(fs::path(VYRN_SOURCE_DIR) / from, dst, fs::copy_options::recursive);
  fs::remove_all(dst / "brains");
  fs::remove_all(dst / "runs");
  return dst.string();
}

void write_file(const fs::path& path, const std::string& text) { std::ofstream(path) << text; }

std::string read_file(const fs::path& path) {
  std::ifstream in(path);
  std::ostringstream os;
  os << in.rdbuf();
  return os.str();
}

struct Outcome {
  std::vector<json> events;
  vyrn::LiveView live;
  RunState state = RunState::Starting;
  bool finished = false;
};

// Runs a session to the end. `on_event` may call into the session (stop, resume...).
Outcome run(RunConfig cfg, std::function<void(vyrn::Session&, const json&)> on_event = {}, double timeout_s = 120) {
  cfg.data_dir = VYRN_SOURCE_DIR;
  vyrn::Session session(cfg);
  session.start();
  Outcome out;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_s);
  auto take = [&](size_t max) {
    for (const auto& text : session.drain(max)) {
      out.events.push_back(json::parse(text));
      if (on_event) on_event(session, out.events.back());
    }
  };
  while (!session.finished() && std::chrono::steady_clock::now() < deadline) {
    take(1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  take(1u << 30);
  out.finished = session.finished();
  out.live = session.live();
  out.state = session.state();
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
    if (e.value("type", "") == "round" || e.value("type", "") == "timing") continue;
    text += e.dump() + "\n";
  }
  return text;
}

RunConfig config(RunConfig::Action action, const std::string& project) {
  RunConfig cfg;
  cfg.action = action;
  cfg.project_dir = project;
  cfg.seed = 7;
  return cfg;
}

}  // namespace

TEST(Session, CheckCatchNumbers) {
  TempDir tmp;
  const std::string project = copy_project("templates/catch-numbers", tmp, "catch");
  const Outcome o = run(config(RunConfig::Action::Check, project));
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(o.state, RunState::Finished);
  EXPECT_TRUE(of_type(o, "error").empty()) << dump_events(o);
  const auto specs = of_type(o, "spec");
  ASSERT_EQ(specs.size(), 1u);
  EXPECT_EQ(specs[0]["mode"], "sim");
  EXPECT_EQ(specs[0]["choices"], json::array({"left", "right", "stay"}));
  EXPECT_EQ(specs[0]["obs"]["names"], json::array({"fruit x", "fruit y", "basket"}));
  EXPECT_EQ(specs[0]["brain"], "new");
  EXPECT_EQ(specs[0]["action"], "check");
  EXPECT_FALSE(specs[0].contains("preview"));
  const auto checks = of_type(o, "check");
  ASSERT_EQ(checks.size(), 1u);
  EXPECT_EQ(checks[0]["ok"], true);
  EXPECT_EQ(checks[0]["steps"], 20);
  EXPECT_TRUE(checks[0]["ms"].contains("see"));
  EXPECT_EQ(o.live.kind, "numbers");
  EXPECT_EQ(o.live.numbers.size(), 3u);
  EXPECT_EQ(o.live.choices.size(), 3u);
  EXPECT_FALSE(fs::exists(fs::path(project) / "brains"));   // Check creates nothing
  EXPECT_EQ(o.events.back()["type"], "state");
  EXPECT_EQ(o.events.back()["state"], "finished");
}

TEST(Session, CheckCatchPicturesShowsTheNetworkInput) {
  TempDir tmp;
  const std::string project = copy_project("templates/catch-pictures", tmp, "catch");
  const Outcome o = run(config(RunConfig::Action::Check, project));
  ASSERT_TRUE(o.finished);
  const auto checks = of_type(o, "check");
  ASSERT_EQ(checks.size(), 1u) << dump_events(o);
  EXPECT_EQ(checks[0]["ok"], true);
  EXPECT_EQ(o.live.kind, "picture");
  EXPECT_EQ(o.live.input.w, 10);
  EXPECT_EQ(o.live.input.h, 10);
  EXPECT_EQ(o.live.input.rgb.size(), 300u);
}

TEST(Session, TrainSavesResumesWatchesAndStartsOver) {
  TempDir tmp;
  const std::string project = copy_project("templates/catch-numbers", tmp, "catch");

  // 1. A new brain learns for a while and is saved.
  RunConfig cfg = config(RunConfig::Action::Train, project);
  cfg.max_ticks = 1500;
  Outcome o = run(cfg);
  ASSERT_TRUE(o.finished);
  ASSERT_TRUE(of_type(o, "error").empty()) << dump_events(o);
  auto specs = of_type(o, "spec");
  ASSERT_EQ(specs.size(), 1u);
  EXPECT_EQ(specs[0]["brain"], "new");
  const std::string id = specs[0]["brain_id"];
  ASSERT_FALSE(id.empty());
  EXPECT_NE(specs[0]["net"].get<std::string>().find("MLP"), std::string::npos);
  const auto rounds = of_type(o, "round");
  ASSERT_GE(rounds.size(), 100u);   // a round is 9 ticks
  EXPECT_EQ(rounds.front()["round"], 1);
  EXPECT_EQ(rounds.back()["round"].get<int64_t>(), static_cast<int64_t>(rounds.size()));
  EXPECT_FALSE(rounds.back()["best_mean20"].is_null());
  for (const auto& r : rounds) EXPECT_EQ(r["ticks"], 9);
  EXPECT_FALSE(of_type(o, "update").empty());   // learning starts after 1000 ticks
  bool saved_latest = false, saved_best = false;
  for (const auto& c : of_type(o, "checkpoint")) {
    saved_latest |= c["which"] == "latest";
    saved_best |= c["which"] == "best";
  }
  EXPECT_TRUE(saved_latest);
  EXPECT_TRUE(saved_best);
  const fs::path brain = fs::path(project) / "brains" / id;
  EXPECT_TRUE(fs::exists(brain / "latest" / "current"));
  EXPECT_TRUE(fs::exists(brain / "best" / "current"));
  EXPECT_EQ(json::parse(read_file(fs::path(project) / "vyrn.json"))["brain"], id);
  EXPECT_EQ(json::parse(read_file(fs::path(project) / "vyrn.json"))["template"], "catch-numbers");   // kept
  // CSV logs.
  ASSERT_TRUE(fs::exists(fs::path(project) / "runs"));
  const fs::path run_dir = fs::directory_iterator(fs::path(project) / "runs")->path();
  const std::string csv = read_file(run_dir / "rounds.csv");
  EXPECT_EQ(csv.rfind("round,points,ticks,truncated,game_over,mean20,best_mean20,total_ticks,explore,seconds\n", 0), 0u);
  EXPECT_EQ(static_cast<size_t>(std::count(csv.begin(), csv.end(), '\n')), rounds.size() + 1);
  EXPECT_TRUE(fs::exists(run_dir / "updates.csv"));
  // The live view.
  EXPECT_EQ(o.live.kind, "numbers");
  EXPECT_EQ(o.live.number_names, (std::vector<std::string>{"fruit x", "fruit y", "basket"}));
  ASSERT_EQ(o.live.probs.size(), 3u);
  EXPECT_NEAR(o.live.probs[0] + o.live.probs[1] + o.live.probs[2], 1.0, 1e-4);
  EXPECT_GE(o.live.chosen, 0);
  EXPECT_GT(o.live.version, 0u);
  EXPECT_GT(o.live.total_ticks, 0);

  // 2. Train again: the same brain resumes, round numbers go on.
  cfg.max_ticks = 200;
  o = run(cfg);
  ASSERT_TRUE(of_type(o, "error").empty()) << dump_events(o);
  specs = of_type(o, "spec");
  ASSERT_EQ(specs.size(), 1u);
  EXPECT_EQ(specs[0]["brain"], "resumed");
  EXPECT_EQ(specs[0]["brain_id"], id);
  const auto more = of_type(o, "round");
  ASSERT_FALSE(more.empty());
  EXPECT_EQ(more.front()["round"].get<int64_t>(), rounds.back()["round"].get<int64_t>() + 1);
  EXPECT_GT(more.front()["total_ticks"].get<int64_t>(), 1500);

  // 3. Watch the best brain, as fast as possible.
  RunConfig watch = config(RunConfig::Action::Watch, project);
  watch.watch_speed = 0;
  watch.max_ticks = 90;
  o = run(watch);
  ASSERT_TRUE(of_type(o, "error").empty()) << dump_events(o);
  specs = of_type(o, "spec");
  ASSERT_EQ(specs.size(), 1u);
  EXPECT_EQ(specs[0]["brain"], "resumed");
  EXPECT_EQ(of_type(o, "round").size(), 10u);
  EXPECT_EQ(of_type(o, "round").front()["round"], 1);
  EXPECT_TRUE(of_type(o, "checkpoint").empty());   // Watch never saves
  EXPECT_DOUBLE_EQ(of_type(o, "round").front()["explore"].get<double>(), 0.01);

  // 4. Start over: a new brain id, which becomes the active one.
  cfg.new_brain = true;
  cfg.max_ticks = 50;
  o = run(cfg);
  specs = of_type(o, "spec");
  ASSERT_EQ(specs.size(), 1u);
  EXPECT_EQ(specs[0]["brain"], "new");
  const std::string fresh = specs[0]["brain_id"];
  EXPECT_NE(fresh, id);
  EXPECT_EQ(json::parse(read_file(fs::path(project) / "vyrn.json"))["brain"], fresh);
  EXPECT_TRUE(fs::exists(brain / "latest" / "current"));   // the old brain is kept
}

TEST(Session, WatchPacesAtDecisionsPerSecond) {
  TempDir tmp;
  const std::string project = copy_project("templates/catch-numbers", tmp, "catch");
  RunConfig cfg = config(RunConfig::Action::Train, project);
  cfg.max_ticks = 100;
  ASSERT_TRUE(of_type(run(cfg), "error").empty());
  RunConfig watch = config(RunConfig::Action::Watch, project);
  watch.watch_brain = "latest";
  watch.watch_speed = 5;   // 10 decisions/s x 5 = one tick per 20 ms
  watch.max_ticks = 25;
  const auto start = std::chrono::steady_clock::now();
  const Outcome o = run(watch);
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  ASSERT_TRUE(of_type(o, "error").empty()) << dump_events(o);
  EXPECT_EQ(of_type(o, "round").size(), 2u);   // 25 ticks = 2 rounds of 9 and a bit
  EXPECT_GE(seconds, 0.45);                    // 24 waits of 20 ms (Python's start-up comes on top)
}

TEST(Session, WatchWithoutABrainSaysSo) {
  TempDir tmp;
  const std::string project = copy_project("templates/catch-numbers", tmp, "catch");
  const Outcome o = run(config(RunConfig::Action::Watch, project));
  ASSERT_TRUE(o.finished);
  const auto errors = of_type(o, "error");
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0]["kind"], "app");
  EXPECT_EQ(errors[0]["reason"], "no_brain");
  EXPECT_EQ(errors[0]["fatal"], true);
}

TEST(Session, ALoadErrorIsFatalAndPointsAtTheLine) {
  TempDir tmp;
  const std::string project = copy_project("templates/catch-numbers", tmp, "catch");
  write_file(fs::path(project) / "inputs.py", "import vyrn\n\ndef see(world)\n    return [1]\n");
  for (auto action : {RunConfig::Action::Check, RunConfig::Action::Train}) {
    const Outcome o = run(config(action, project));
    ASSERT_TRUE(o.finished);
    const auto errors = of_type(o, "error");
    ASSERT_EQ(errors.size(), 1u) << dump_events(o);
    EXPECT_EQ(errors[0]["kind"], "syntax");
    EXPECT_EQ(errors[0]["script"], "inputs.py");
    EXPECT_EQ(errors[0]["line"], 3);
    EXPECT_EQ(errors[0]["fatal"], true);
    if (action == RunConfig::Action::Check) {
      ASSERT_EQ(of_type(o, "check").size(), 1u);
      EXPECT_EQ(of_type(o, "check")[0]["ok"], false);
    }
  }
  EXPECT_FALSE(fs::exists(fs::path(project) / "brains"));
}

TEST(Session, AScriptErrorPausesTheRunAndResumeRespawnsPython) {
  TempDir tmp;
  const std::string project = copy_project("templates/catch-numbers", tmp, "catch");
  const fs::path points = fs::path(project) / "points.py";
  const std::string good = read_file(points);
  write_file(points, "calls = 0\n\ndef points(world):\n    global calls\n    calls += 1\n    if calls > 30:\n"
                     "        return scor\n    return 0\n\ndef game_over(world):\n    return world.fruit_y == 9\n");
  int errors_seen = 0, rounds_after_resume = 0;
  bool resumed = false;
  const Outcome o = run(config(RunConfig::Action::Train, project), [&](vyrn::Session& s, const json& e) {
    const std::string type = e.value("type", "");
    if (type == "error") ++errors_seen;
    if (type == "state" && e["state"] == "error" && !resumed) {
      write_file(points, good);   // the user fixes the script...
      resumed = true;
      s.resume();                 // ...and presses Resume
    }
    if (type == "round" && resumed && ++rounds_after_resume == 5) s.stop();
  });
  ASSERT_TRUE(o.finished);
  EXPECT_EQ(errors_seen, 1);
  const auto errors = of_type(o, "error");
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0]["kind"], "exception");
  EXPECT_EQ(errors[0]["exc_type"], "NameError");
  EXPECT_EQ(errors[0]["script"], "points.py");
  EXPECT_EQ(errors[0]["line"], 7);
  EXPECT_EQ(errors[0]["fatal"], false);
  EXPECT_GE(rounds_after_resume, 5);   // stop() lands a few rounds later: the run is fast
  EXPECT_EQ(of_type(o, "spec").size(), 2u);   // Python was started again
  EXPECT_EQ(of_type(o, "spec")[1]["brain"], "resumed");
  EXPECT_FALSE(of_type(o, "checkpoint").empty());   // stop saved the brain
}

TEST(Session, StopWhileInTheErrorStateStillSaves) {
  TempDir tmp;
  const std::string project = copy_project("templates/catch-numbers", tmp, "catch");
  write_file(fs::path(project) / "world.py",
             "import random\n\ndef restart(world):\n    world.basket = 5\n    world.fruit_x = random.randint(0, 9)\n"
             "    world.fruit_y = 0\n    world.n = getattr(world, 'n', 0)\n\ndef step(world):\n    world.n += 1\n"
             "    if world.n > 40:\n        1 / 0\n    world.fruit_y += 1\n");
  const Outcome o = run(config(RunConfig::Action::Train, project), [](vyrn::Session& s, const json& e) {
    if (e.value("type", "") == "state" && e["state"] == "error") s.stop();
  });
  ASSERT_TRUE(o.finished);
  const auto errors = of_type(o, "error");
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0]["script"], "world.py");
  EXPECT_EQ(errors[0]["line"], 12);
  EXPECT_EQ(errors[0]["exc_type"], "ZeroDivisionError");
  ASSERT_EQ(of_type(o, "checkpoint").size(), 1u);
  EXPECT_EQ(of_type(o, "checkpoint")[0]["which"], "latest");
}

TEST(Session, ApplyChangesKeepsTheBrainAndPauseResume) {
  TempDir tmp;
  const std::string project = copy_project("templates/catch-numbers", tmp, "catch");
  int rounds = 0;
  bool paused_seen = false, running_again = false;
  const Outcome o = run(config(RunConfig::Action::Train, project), [&](vyrn::Session& s, const json& e) {
    const std::string type = e.value("type", "");
    if (type == "round") {
      ++rounds;
      if (rounds == 5) s.apply_changes();
      if (rounds == 15) s.pause();
      if (rounds == 30) s.stop();
    }
    if (type == "state" && e["state"] == "paused") {
      paused_seen = true;
      s.resume();
    }
    if (type == "state" && e["state"] == "running" && paused_seen) running_again = true;
  });
  ASSERT_TRUE(o.finished);
  EXPECT_TRUE(of_type(o, "error").empty()) << dump_events(o);
  const auto specs = of_type(o, "spec");
  ASSERT_EQ(specs.size(), 2u);
  EXPECT_EQ(specs[1]["brain"], "resumed");
  EXPECT_EQ(specs[1]["brain_id"], specs[0]["brain_id"]);
  EXPECT_TRUE(paused_seen);
  EXPECT_TRUE(running_again);
  EXPECT_GE(rounds, 30);
}

TEST(Session, LoudScriptsAreThrottledAndTheDropsCounted) {
  TempDir tmp;
  const std::string project = copy_project("templates/catch-numbers", tmp, "catch");
  write_file(fs::path(project) / "world.py",
             "import random\n\ndef restart(world):\n    world.basket = 5\n    world.fruit_x = random.randint(0, 9)\n"
             "    world.fruit_y = 0\n\ndef step(world):\n    for i in range(5):\n        print('tick', i)\n"
             "    world.fruit_y += 1\n");
  RunConfig cfg = config(RunConfig::Action::Train, project);
  cfg.max_ticks = 600;   // 3000 lines, much faster than 200 a second
  const Outcome o = run(cfg);
  ASSERT_TRUE(of_type(o, "error").empty()) << dump_events(o);
  int64_t shown = 0, dropped = 0;
  for (const auto& e : of_type(o, "log")) {
    if (e["stream"] == "stdout") ++shown;
    const std::string text = e["text"];
    if (e["stream"] == "app" && text.find(" lines dropped)") != std::string::npos) {
      dropped += std::stoll(text.substr(1));
    }
  }
  EXPECT_GT(dropped, 0);
  EXPECT_EQ(shown + dropped, 3000);
}

TEST(Session, RealTimeCheckDescribesKeysAndTrainNeedsAllowInput) {
  TempDir tmp;
  const fs::path project = fs::path(tmp.path()) / "rt";
  fs::create_directories(project);
  write_file(project / "inputs.py", "n = 0\n\ndef see():\n    global n\n    n += 1\n    return [n, 2.0]\n");
  write_file(project / "responses.py", "left = \"a\"\nright = \"d\"\njump = \"tap space\"\n");
  write_file(project / "points.py", "def points():\n    return 1\n");
  // Belt and braces: if a key were ever sent, it would land in this file instead of a virtual keyboard.
  const fs::path keys = fs::path(tmp.path()) / "keys.log";
  ::setenv("VYRN_KEYS_BACKEND", ("record:" + keys.string()).c_str(), 1);
  struct Unset {
    ~Unset() { ::unsetenv("VYRN_KEYS_BACKEND"); }
  } unset;
  Outcome o = run(config(RunConfig::Action::Check, project.string()));
  ASSERT_TRUE(o.finished);
  ASSERT_TRUE(of_type(o, "error").empty()) << dump_events(o);
  auto specs = of_type(o, "spec");
  ASSERT_EQ(specs.size(), 1u);
  EXPECT_EQ(specs[0]["mode"], "realtime");
  EXPECT_EQ(specs[0]["uses"]["keys"], true);
  const auto checks = of_type(o, "check");
  ASSERT_EQ(checks.size(), 1u);
  EXPECT_EQ(checks[0]["ok"], true);
  EXPECT_EQ(checks[0]["mode"], "realtime");
  EXPECT_EQ(checks[0]["steps"], 5);
  ASSERT_FALSE(checks[0]["did"].empty());
  EXPECT_EQ(checks[0]["did"][0].get<std::string>().rfind("would ", 0), 0u);
  EXPECT_EQ(checks[0]["constant_inputs"], json::array({"input 2"}));
  EXPECT_TRUE(!fs::exists(keys) || fs::file_size(keys) == 0);   // Check only describes keys

  // Train and Watch press keys only when the run allows it (the app's countdown flow, or --allow-input).
  for (auto action : {RunConfig::Action::Train, RunConfig::Action::Watch}) {
    o = run(config(action, project.string()));
    ASSERT_TRUE(o.finished);
    const auto errors = of_type(o, "error");
    ASSERT_EQ(errors.size(), 1u) << dump_events(o);
    EXPECT_EQ(errors[0]["reason"], "input_not_allowed");
    EXPECT_EQ(errors[0]["fatal"], true);
    EXPECT_TRUE(of_type(o, "spec").empty());   // nothing was played
    EXPECT_FALSE(fs::exists(project / "brains"));
  }
  EXPECT_TRUE(!fs::exists(keys) || fs::file_size(keys) == 0);
}

TEST(Session, NoProjectFolder) {
  const Outcome o = run(config(RunConfig::Action::Check, "/nonexistent/vyrn/project"));
  ASSERT_TRUE(o.finished);
  const auto errors = of_type(o, "error");
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0]["reason"], "no_project");
}

TEST(Session, StopBeforeStartAndDestroyWhileRunning) {
  TempDir tmp;
  const std::string project = copy_project("templates/catch-numbers", tmp, "catch");
  {
    vyrn::Session never_started(config(RunConfig::Action::Train, project));
    EXPECT_FALSE(never_started.finished());
    EXPECT_TRUE(never_started.drain().empty());
  }
  // Destroying a running session stops it and saves the brain first.
  RunConfig cfg = config(RunConfig::Action::Train, project);
  cfg.data_dir = VYRN_SOURCE_DIR;
  {
    vyrn::Session s(cfg);
    s.start();
    while (s.live().total_ticks < 50 && !s.finished()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(fs::exists(fs::path(project) / "brains"));
  const std::string id = json::parse(read_file(fs::path(project) / "vyrn.json"))["brain"];
  EXPECT_TRUE(fs::exists(fs::path(project) / "brains" / id / "latest" / "current"));
}

TEST(Session, ShowValuesReachTheLiveView) {
  TempDir tmp;
  const std::string project = copy_project("templates/catch-numbers", tmp, "catch");
  write_file(fs::path(project) / "inputs.py",
             "import vyrn\n\ndef see(world):\n    vyrn.show('height', world.fruit_y)\n"
             "    vyrn.show('label', 'falling')\n    vyrn.show('view', vyrn.canvas(4, 3, 'red'))\n"
             "    return {'fruit x': world.fruit_x, 'fruit y': world.fruit_y, 'basket': world.basket}\n");
  RunConfig cfg = config(RunConfig::Action::Train, project);
  cfg.max_ticks = 300;
  const Outcome o = run(cfg);
  ASSERT_TRUE(of_type(o, "error").empty()) << dump_events(o);
  std::map<std::string, std::string> text(o.live.show_text.begin(), o.live.show_text.end());
  ASSERT_EQ(text.count("height"), 1u);
  EXPECT_EQ(text["label"], "falling");
  ASSERT_EQ(o.live.show_pictures.size(), 1u);
  EXPECT_EQ(o.live.show_pictures[0].first, "view");
  EXPECT_EQ(o.live.show_pictures[0].second.w, 4);
  EXPECT_EQ(o.live.show_pictures[0].second.h, 3);
  ASSERT_EQ(o.live.show_pictures[0].second.rgb.size(), 36u);
  EXPECT_EQ(o.live.show_pictures[0].second.rgb[0], 255);   // red
  EXPECT_EQ(o.live.show_pictures[0].second.rgb[1], 0);
}

TEST(Session, StopDuringASlowLoadIsQuick) {
  TempDir tmp;
  const std::string project = copy_project("templates/catch-numbers", tmp, "catch");
  write_file(fs::path(project) / "inputs.py",
             "import time\ntime.sleep(20)\n\ndef see(world):\n    return [world.fruit_x]\n");
  RunConfig cfg = config(RunConfig::Action::Train, project);
  cfg.data_dir = VYRN_SOURCE_DIR;
  vyrn::Session s(cfg);
  s.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(700));   // Python is busy loading inputs.py
  const auto start = std::chrono::steady_clock::now();
  s.stop();
  while (!s.finished() && std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  ASSERT_TRUE(s.finished());
  EXPECT_LT(seconds, 3.0);
  for (const auto& text : s.drain(1u << 20)) EXPECT_NE(json::parse(text)["type"], "error") << text;
  EXPECT_FALSE(fs::exists(fs::path(project) / "brains"));
}
