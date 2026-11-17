// The real worker (python/vyrn_worker.py) on a copy of templates/catch-pictures:
// hello -> load -> reset -> 50 random steps -> close. Skipped (with the reason) if the Python side
// is missing or reports itself broken.
#include <gtest/gtest.h>

#include "platform_test.h"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "ipc/worker.h"
#include "vyrn/platform.h"

namespace {

namespace fs = std::filesystem;
using vyrn::ipc::ArrayView;
using vyrn::ipc::Frame;
using vyrn::ipc::json;
using vyrn::ipc::WorkerDied;
using vyrn::ipc::WorkerError;
using vyrn::ipc::WorkerOptions;
using vyrn::ipc::WorkerProcess;

// Linux: /usr/bin/python3 (apt's numpy and Pillow); elsewhere the platform's default (docs/dev/platforms.md).
std::string python() { return VYRN_TEST_LINUX ? std::string("/usr/bin/python3") : vyrn::platform::default_python(); }

class TempDir {
 public:
  TempDir() {
    path_ = (fs::temp_directory_path() / "vyrn-ipc-real-XXXXXX").string();
    if (::mkdtemp(path_.data()) == nullptr) throw std::runtime_error("mkdtemp failed");
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

class ScopedEnv {
 public:
  ScopedEnv(const char* name, const std::string& value) : name_(name) {
    if (const char* old = std::getenv(name)) old_ = old;
    ::setenv(name, value.c_str(), 1);
  }
  ~ScopedEnv() {
    if (old_) {
      ::setenv(name_, old_->c_str(), 1);
    } else {
      ::unsetenv(name_);
    }
  }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 private:
  const char* name_;
  std::optional<std::string> old_;
};

std::string joined(const std::vector<std::string>& lines) {
  std::string text;
  for (const std::string& line : lines) text += "\n  " + line;
  return text;
}

// The catch picture: 10x10 RGB, one white fruit pixel, a 3-pixel basket on the bottom row.
struct CatchPicture {
  int white = 0;            // pixels that are exactly (255, 255, 255)
  int fruit_row = -1;
  int bottom_lit = 0;       // non-black pixels on row 9 (basket + maybe the fruit)
};

CatchPicture look_at(const ArrayView& picture) {
  CatchPicture seen;
  for (int y = 0; y < 10; ++y) {
    for (int x = 0; x < 10; ++x) {
      const uint8_t* rgb = picture.data + (y * 10 + x) * 3;
      const bool white = rgb[0] == 255 && rgb[1] == 255 && rgb[2] == 255;
      if (white) {
        ++seen.white;
        seen.fruit_row = y;
      }
      if (y == 9 && (rgb[0] || rgb[1] || rgb[2])) ++seen.bottom_lit;
    }
  }
  return seen;
}

void expect_picture(const Frame& frame, const json& ref, const std::string& what) {
  ASSERT_TRUE(ref.is_object() && ref.contains("$a")) << what << " is not an array: " << ref.dump();
  const ArrayView picture = frame.array(ref);
  EXPECT_EQ(picture.dtype, "u8") << what;
  ASSERT_EQ(picture.shape, (std::vector<int64_t>{10, 10, 3})) << what;
  ASSERT_EQ(picture.bytes, 300u) << what;
  EXPECT_EQ(look_at(picture).white, 1) << what << " should show exactly one fruit";
}

TEST(RealWorker, CatchPicturesHelloLoadResetAndFiftySteps) {
  const std::string source = VYRN_SOURCE_DIR;
  const std::string template_dir = source + "/templates/catch-pictures";
  if (!fs::exists(source + "/python/vyrn_worker.py") || !fs::exists(source + "/python/vyrn/_host.py")) {
    GTEST_SKIP() << "python/ is missing in " << source;
  }
  for (const char* name : {"inputs.py", "responses.py", "points.py", "world.py"}) {
    if (!fs::exists(template_dir + "/" + name)) GTEST_SKIP() << "templates/catch-pictures/" << name << " is missing";
  }
  const std::string interpreter = python();
  if (fs::path(interpreter).is_absolute() && !fs::exists(interpreter)) GTEST_SKIP() << interpreter << " is missing";

  TempDir tmp;
  const std::string runtime = tmp.path() + "/runtime";
  fs::create_directories(runtime);
  ScopedEnv xdg(vyrn_test::runtime_env_name(), runtime);   // pid files go below it, not to the real runtime dir
  const std::string project = tmp.path() + "/catch-pictures";
  fs::copy(template_dir, project, fs::copy_options::recursive);
  fs::remove_all(project + "/__pycache__");   // in case a tool left one in the source tree
  const std::string keys_log = tmp.path() + "/keys.log";

  WorkerOptions options;
  options.python = interpreter;
  options.data_dir = source;
  options.project_dir = project;
  options.env = {{"VYRN_KEYS_BACKEND", "record:" + keys_log}};   // belt and braces: never a real keyboard
  std::string pid_file;

  try {
    WorkerProcess worker(options);
    worker.spawn();
    pid_file = vyrn::platform::runtime_dir() + "/" + std::to_string(worker.pid()) + ".pid";
    EXPECT_TRUE(fs::exists(pid_file));

    const json hello = worker.hello(15);
    EXPECT_EQ(hello["t"], "hello");
    EXPECT_EQ(hello["id"], 0);
    EXPECT_EQ(hello["proto"], 1);
    EXPECT_EQ(hello["pid"], worker.pid());
    EXPECT_TRUE(hello.contains("backends")) << hello.dump();

    const json load = {{"t", "load"},          {"project", project},  {"check", false},
                       {"allow_input", false}, {"seed", 12345},       {"round_limit", 1000},
                       {"decisions_per_second", 10.0}, {"focus_helper", ""}};
    const Frame spec = worker.request(load, 30, 2);
    EXPECT_EQ(spec.j["t"], "spec");
    EXPECT_EQ(spec.j["id"], 1);
    EXPECT_EQ(spec.j["mode"], "sim");
    EXPECT_EQ(spec.j["obs"]["kind"], "picture");
    EXPECT_EQ(spec.j["obs"]["shape"], json({10, 10, 3}));
    ASSERT_EQ(spec.j["choices"], json({"left", "right", "stay"}));
    if (spec.has_array("preview")) expect_picture(spec, spec.j["preview"], "spec.preview");

    const Frame reset = worker.request({{"t", "reset"}}, 30, 2);
    EXPECT_EQ(reset.j["t"], "result");
    EXPECT_EQ(reset.j["id"], 2);
    EXPECT_EQ(reset.j["first"], true);
    EXPECT_TRUE(reset.j["points"].is_null());
    expect_picture(reset, reset.j["obs"], "reset.obs");

    std::mt19937 rng(7);
    int rounds = 0;
    int64_t expected_id = 2;
    for (int step = 0; step < 50; ++step) {
      const int action = static_cast<int>(rng() % 3);
      const Frame r = worker.request({{"t", "step"}, {"a", action}}, 5, 2);
      const std::string where = "step " + std::to_string(step);
      ASSERT_EQ(r.j["t"], "result") << where;
      ASSERT_EQ(r.j["id"], ++expected_id) << where;
      expect_picture(r, r.j["obs"], where + " obs");
      ASSERT_TRUE(r.j["points"].is_number()) << where << ": " << r.j.dump();
      const double points = r.j["points"].get<double>();
      ASSERT_TRUE(r.j["game_over"].is_boolean()) << where;
      ASSERT_TRUE(r.j["truncated"].is_boolean()) << where;
      ASSERT_TRUE(r.j["first"].is_boolean()) << where;
      EXPECT_FALSE(r.j["truncated"].get<bool>()) << where;
      EXPECT_TRUE(r.j["ms"].is_object()) << where;
      if (r.j["first"].get<bool>()) {
        // Round over: final_obs is the last picture of the round (fruit on the bottom row), obs the
        // first of the next. Caught (+1) = fruit inside the basket = 3 lit pixels on row 9, missed (-1) = 4.
        ++rounds;
        EXPECT_TRUE(r.j["game_over"].get<bool>()) << where;
        expect_picture(r, r.j["final_obs"], where + " final_obs");
        const CatchPicture last = look_at(r.array(r.j["final_obs"]));
        EXPECT_EQ(last.fruit_row, 9) << where;
        EXPECT_TRUE(points == 1.0 || points == -1.0) << where << ": points " << points;
        EXPECT_EQ(last.bottom_lit, points > 0 ? 3 : 4) << where << ": points " << points;
        EXPECT_EQ(look_at(r.array(r.j["obs"])).fruit_row, 0) << where << ": a new round starts at the top";
      } else {
        EXPECT_FALSE(r.j["game_over"].get<bool>()) << where;
        EXPECT_TRUE(r.j["final_obs"].is_null()) << where;
        EXPECT_EQ(points, 0.0) << where;
      }
    }
    EXPECT_EQ(rounds, 5) << "the fruit falls for 9 ticks, so 50 steps finish 5 rounds";

    const Frame bye = worker.request({{"t", "close"}}, 2, 0);
    EXPECT_EQ(bye.j["t"], "bye");
  } catch (const WorkerError& e) {
    GTEST_SKIP() << "the Python side reported an error, so python/ or the template looks broken: " << e.what()
                 << "\n" << e.error.dump(2);
  } catch (const WorkerDied& e) {
    const std::string tail = joined(e.stderr_tail);
    if (tail.find("Traceback") != std::string::npos || tail.find("Error") != std::string::npos) {
      GTEST_SKIP() << "Python crashed, so python/ looks broken: " << e.what() << tail;
    }
    FAIL() << e.what() << tail;
  }

  EXPECT_FALSE(fs::exists(pid_file)) << "the pid file must go once the worker is reaped";
  std::error_code ec;
  EXPECT_TRUE(!fs::exists(keys_log) || fs::file_size(keys_log, ec) == 0) << "a simulation must never press keys";
  EXPECT_FALSE(fs::exists(project + "/__pycache__")) << "no bytecode in the user's project";
}

}  // namespace
