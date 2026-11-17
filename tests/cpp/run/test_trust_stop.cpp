// Integration items of the core: imported projects run only once trusted (util/trust.h, Session), the stop marker
// <project>/.vyrn-stop stops a run like stop(), Train writes runs/<ts>/run.json, and the spec event says how many
// frames the brain stacks. Sessions run the real Python worker on copies of the templates in temporary folders.
#include "util/trust.h"
#include "vyrn/session.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "platform_test.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using nlohmann::json;
using vyrn::RunConfig;

namespace {

class TempDir {
 public:
  TempDir() {
    std::string pattern = (fs::temp_directory_path() / "vyrn-trust-XXXXXX").string();
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

std::string copy_template(const std::string& id, const TempDir& tmp, const std::string& name) {
  const fs::path dst = fs::path(tmp.path()) / name;
  fs::copy(fs::path(VYRN_SOURCE_DIR) / "templates" / id, dst, fs::copy_options::recursive);
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
  bool finished = false;
  double seconds = 0;
};

Outcome run(RunConfig cfg, const std::function<void(const json&)>& on_event = {}, double timeout_s = 120) {
  cfg.data_dir = VYRN_SOURCE_DIR;
  cfg.seed = 7;
  const auto start = std::chrono::steady_clock::now();
  Outcome out;
  {
    vyrn::Session session(cfg);
    session.start();
    const auto deadline = start + std::chrono::duration<double>(timeout_s);
    auto take = [&](size_t max) {
      for (const auto& text : session.drain(max)) {
        out.events.push_back(json::parse(text));
        if (on_event) on_event(out.events.back());
      }
    };
    while (!session.finished() && std::chrono::steady_clock::now() < deadline) {
      take(1000);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    take(1u << 30);
    out.finished = session.finished();
  }
  out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  return out;
}

std::vector<json> of_type(const Outcome& o, const std::string& type) {
  std::vector<json> out;
  for (const auto& e : o.events) {
    if (e.value("type", "") == type) out.push_back(e);
  }
  return out;
}

bool has_state_detail(const Outcome& o, const std::string& part) {
  for (const auto& e : of_type(o, "state")) {
    if (e.value("detail", "").find(part) != std::string::npos) return true;
  }
  return false;
}

RunConfig config(RunConfig::Action action, const std::string& project) {
  RunConfig cfg;
  cfg.action = action;
  cfg.project_dir = project;
  return cfg;
}

}  // namespace

// ---- the trust marker -------------------------------------------------------------------------------------------

TEST(Trust, MarkerRules) {
  VYRN_SKIP_WITHOUT_SYMLINKS();
  TempDir tmp;
  const fs::path p = tmp.path();
  EXPECT_TRUE(vyrn::util::project_trusted(p.string()));   // not imported
  EXPECT_EQ(vyrn::not_trusted_message(p.string()), "");
  const fs::path marker = p / ".vyrn-imported";
  write_file(marker, R"({"file": "Catch.vrlp", "trusted": false})");
  EXPECT_FALSE(vyrn::util::project_trusted(p.string()));
  write_file(marker, R"({"file": "Catch.vrlp", "trusted": true})");
  EXPECT_TRUE(vyrn::util::project_trusted(p.string()));
  for (const std::string& text : {std::string(R"({"trusted": "true"})"), std::string(R"({"trusted": 1})"),
                                  std::string("{\"trusted\": true"), std::string("[true]"), std::string("")}) {
    write_file(marker, text);
    EXPECT_FALSE(vyrn::util::project_trusted(p.string())) << text;   // damaged or unclear: untrusted
  }
  fs::remove(marker);
  write_file(p / "elsewhere.json", R"({"trusted": true})");
  fs::create_symlink(p / "elsewhere.json", marker);
  EXPECT_FALSE(vyrn::util::project_trusted(p.string()));   // a symlink is never followed
  fs::remove(marker);
  fs::create_directory(marker);
  EXPECT_FALSE(vyrn::util::project_trusted(p.string()));
}

TEST(Trust, MessageNamesTheFileAndHowToTrust) {
  TempDir tmp;
  const fs::path p = fs::path(tmp.path()) / "My Catch";
  fs::create_directory(p);
  write_file(p / ".vyrn-imported", "{\"file\": \"Ca\\u202etch\\n.vrlp\", \"trusted\": false}");
  const std::string message = vyrn::not_trusted_message(p.string());
  EXPECT_EQ(message, "This project came from Catch.vrlp. Its Python code hasn't run on this computer yet: read its "
                     "scripts, then trust it in Vyrn (or run: vyrn-cli trust '" + p.string() + "').");
  write_file(p / ".vyrn-imported", R"({"trusted": false})");
  EXPECT_EQ(vyrn::not_trusted_message(p.string()).rfind("This project came from a shared file. ", 0), 0u);
}

TEST(Session, AnUntrustedProjectRunsNothing) {
  TempDir tmp;
  const std::string project = copy_template("catch-numbers", tmp, "catch");
  write_file(fs::path(project) / ".vyrn-imported", R"({"file": "Catch.vrlp", "trusted": false})");
  for (const auto action : {RunConfig::Action::Check, RunConfig::Action::Train, RunConfig::Action::Watch}) {
    const Outcome o = run(config(action, project), {}, 30);
    ASSERT_TRUE(o.finished);
    const auto errors = of_type(o, "error");
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0]["kind"], "not_trusted");
    EXPECT_EQ(errors[0]["fatal"], true);
    EXPECT_EQ(errors[0]["message"], vyrn::not_trusted_message(project));
    EXPECT_NE(errors[0]["message"].get<std::string>().find("Catch.vrlp"), std::string::npos);
    EXPECT_TRUE(of_type(o, "spec").empty());   // Python never started
    EXPECT_TRUE(of_type(o, "log").empty());
    EXPECT_EQ(o.events.back()["state"], "finished");
    EXPECT_LT(o.seconds, 5.0);
  }
  EXPECT_FALSE(fs::exists(fs::path(project) / "brains"));
  EXPECT_FALSE(fs::exists(fs::path(project) / "runs"));
  EXPECT_FALSE(fs::exists(fs::path(project) / "__pycache__"));

  write_file(fs::path(project) / ".vyrn-imported", R"({"file": "Catch.vrlp", "trusted": true})");
  const Outcome o = run(config(RunConfig::Action::Check, project));
  EXPECT_TRUE(of_type(o, "error").empty());
  ASSERT_EQ(of_type(o, "check").size(), 1u);
  EXPECT_EQ(of_type(o, "check")[0]["ok"], true);
}

// ---- the stop marker --------------------------------------------------------------------------------------------

TEST(Session, TheStopMarkerStopsATrainingAndTheBrainIsSaved) {
  TempDir tmp;
  const std::string project = copy_template("catch-numbers", tmp, "catch");
  const fs::path marker = fs::path(project) / vyrn::kStopMarkerFile;
  RunConfig cfg = config(RunConfig::Action::Train, project);
  cfg.max_seconds = 90;   // only a guard: the marker stops it long before
  int rounds = 0;
  const Outcome o = run(cfg, [&](const json& e) {
    if (e.value("type", "") == "round" && ++rounds == 20) write_file(marker, "");
  });
  ASSERT_TRUE(o.finished);
  EXPECT_TRUE(of_type(o, "error").empty());
  EXPECT_FALSE(fs::exists(marker));                       // the Session deleted it
  EXPECT_FALSE(has_state_detail(o, "Reached"));           // it wasn't the time limit
  EXPECT_LT(o.seconds, 60.0);
  bool told = false;
  for (const auto& e : of_type(o, "log")) told |= e["text"].get<std::string>().find(".vyrn-stop") != std::string::npos;
  EXPECT_TRUE(told);
  bool saved_latest = false;
  for (const auto& c : of_type(o, "checkpoint")) saved_latest |= c["which"] == "latest";
  EXPECT_TRUE(saved_latest);
  const std::string id = of_type(o, "spec").at(0)["brain_id"];
  EXPECT_TRUE(fs::exists(fs::path(project) / "brains" / id / "latest" / "current"));
  EXPECT_EQ(json::parse(read_file(fs::path(project) / "vyrn.json"))["brain"], id);
}

TEST(Session, AStaleStopMarkerIsDeletedAndIgnored) {
  TempDir tmp;
  const std::string project = copy_template("catch-numbers", tmp, "catch");
  const fs::path marker = fs::path(project) / vyrn::kStopMarkerFile;
  write_file(marker, "");
  RunConfig cfg = config(RunConfig::Action::Train, project);
  cfg.max_ticks = 300;
  const Outcome o = run(cfg);
  ASSERT_TRUE(o.finished);
  EXPECT_TRUE(has_state_detail(o, "Reached 300 ticks")) << "the old marker stopped the run";
  EXPECT_FALSE(fs::exists(marker));
}

// ---- run.json and the spec event's frames ------------------------------------------------------------------------

TEST(Session, TrainWritesRunJsonNextToItsLogs) {
  TempDir tmp;
  const std::string project = copy_template("catch-numbers", tmp, "catch");
  RunConfig cfg = config(RunConfig::Action::Train, project);
  cfg.max_ticks = 200;
  const Outcome o = run(cfg);
  ASSERT_TRUE(o.finished);
  const std::string id = of_type(o, "spec").at(0)["brain_id"];
  const fs::path runs = fs::path(project) / "runs";
  ASSERT_TRUE(fs::is_directory(runs));
  const fs::path dir = fs::directory_iterator(runs)->path();
  ASSERT_TRUE(fs::exists(dir / "rounds.csv"));
  const json run_json = json::parse(read_file(dir / "run.json"));
  EXPECT_EQ(run_json["brain"], id);
  EXPECT_EQ(run_json["action"], "train");
  EXPECT_TRUE(std::regex_match(run_json["started"].get<std::string>(),
                               std::regex(R"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d)")))
      << run_json;
}

TEST(Session, TheSpecEventSaysHowManyFramesTheBrainStacks) {
  TempDir tmp;
  const std::string numbers = copy_template("catch-numbers", tmp, "numbers");
  Outcome o = run(config(RunConfig::Action::Check, numbers));
  ASSERT_EQ(of_type(o, "spec").size(), 1u);
  EXPECT_EQ(of_type(o, "spec")[0]["frames"], 1);

  const std::string pictures = copy_template("catch-pictures", tmp, "pictures");
  o = run(config(RunConfig::Action::Check, pictures));
  ASSERT_EQ(of_type(o, "spec").size(), 1u);
  EXPECT_EQ(of_type(o, "spec")[0]["frames"], 4);   // the default for pictures

  write_file(fs::path(pictures) / "vyrn.json", R"({"name": "Catch", "advanced": {"frames": 2}})");
  o = run(config(RunConfig::Action::Check, pictures));
  ASSERT_EQ(of_type(o, "spec").size(), 1u);
  EXPECT_EQ(of_type(o, "spec")[0]["frames"], 2);
}
