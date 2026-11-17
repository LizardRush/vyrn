// vyrn.json: defaults, validation, and writing that keeps the user's other keys (and their order).
#include "util/files.h"
#include "util/settings.h"

#include <gtest/gtest.h>

#include "platform_test.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace vyrn::util;

namespace {

class TempDir {
 public:
  TempDir() {
    std::string pattern = (fs::temp_directory_path() / "vyrn-settings-XXXXXX").string();
    path_ = ::mkdtemp(pattern.data());
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

std::string slurp(const std::string& path) {
  std::ifstream in(path);
  std::ostringstream os;
  os << in.rdbuf();
  return os.str();
}

void write(const std::string& path, const std::string& text) { std::ofstream(path) << text; }

}  // namespace

TEST(Settings, MissingFileGivesDefaults) {
  TempDir dir;
  const ProjectSettings s = read_settings(dir.path());
  EXPECT_FALSE(s.exists);
  EXPECT_TRUE(s.readable);
  EXPECT_TRUE(s.warnings.empty());
  EXPECT_DOUBLE_EQ(s.decisions_per_second, 10.0);
  EXPECT_DOUBLE_EQ(s.round_seconds, 60.0);
  EXPECT_EQ(s.round_ticks, 1000);
  EXPECT_TRUE(s.brain.empty());
  EXPECT_TRUE(s.advanced.is_null());
}

TEST(Settings, ReadsTheDocumentedKeys) {
  const ProjectSettings s = parse_settings(R"({"name":"Catch","template":"catch-pictures","decisions_per_second":20,
      "round_seconds":30,"round_ticks":500,"start_program":"game","python":"/usr/bin/python3.13","game_window":"Dodge",
      "brain":"20261113-153012","advanced":{"explore_ticks":5000,"frames":2}})");
  EXPECT_TRUE(s.warnings.empty());
  EXPECT_EQ(s.name, "Catch");
  EXPECT_EQ(s.template_id, "catch-pictures");
  EXPECT_DOUBLE_EQ(s.decisions_per_second, 20);
  EXPECT_DOUBLE_EQ(s.round_seconds, 30);
  EXPECT_EQ(s.round_ticks, 500);
  EXPECT_EQ(s.start_program, "game");
  EXPECT_EQ(s.python, "/usr/bin/python3.13");
  EXPECT_EQ(s.game_window, "Dodge");
  EXPECT_EQ(s.brain, "20261113-153012");
  EXPECT_EQ(s.advanced, (nlohmann::json{{"explore_ticks", 5000}, {"frames", 2}}));
}

TEST(Settings, BadValuesFallBackToDefaultsWithAWarning) {
  const ProjectSettings s = parse_settings(
      R"({"decisions_per_second":-3,"round_ticks":2.5,"round_seconds":"long","python":7,"brain":"../../etc"})");
  EXPECT_DOUBLE_EQ(s.decisions_per_second, 10.0);
  EXPECT_EQ(s.round_ticks, 1000);
  EXPECT_DOUBLE_EQ(s.round_seconds, 60.0);
  EXPECT_TRUE(s.python.empty());
  EXPECT_TRUE(s.brain.empty());   // never a path outside brains/
  EXPECT_EQ(s.warnings.size(), 5u);
  for (const auto& w : s.warnings) EXPECT_EQ(w.rfind("vyrn.json", 0), 0u) << w;
}

TEST(Settings, BrokenFileIsReportedNotThrown) {
  ProjectSettings s = parse_settings("{\"name\": ");
  EXPECT_FALSE(s.readable);
  ASSERT_EQ(s.warnings.size(), 1u);
  EXPECT_NE(s.warnings[0].find("isn't valid JSON"), std::string::npos);
  s = parse_settings("[1, 2]");
  EXPECT_FALSE(s.readable);
  EXPECT_EQ(s.round_ticks, 1000);
}

TEST(Settings, UpdateKeepsUnknownKeysAndTheirOrder) {
  TempDir dir;
  const std::string path = settings_path(dir.path());
  write(path, R"({"zeta": 1, "name": "Mine", "brain": "old", "my_notes": {"keep": [1, 2, {"x": null}]}, "alpha": "a"})");
  ordered_json change = ordered_json::object();
  change["brain"] = "20261113-153012";
  std::string why;
  ASSERT_TRUE(update_settings(dir.path(), change, &why)) << why;

  const ordered_json after = ordered_json::parse(slurp(path));
  std::vector<std::string> keys;
  for (auto it = after.begin(); it != after.end(); ++it) keys.push_back(it.key());
  EXPECT_EQ(keys, (std::vector<std::string>{"zeta", "name", "brain", "my_notes", "alpha"}));
  EXPECT_EQ(after["brain"], "20261113-153012");
  EXPECT_EQ(after["zeta"], 1);
  EXPECT_EQ(after["my_notes"]["keep"][2]["x"], nullptr);
  EXPECT_EQ(after["alpha"], "a");
  EXPECT_EQ(read_settings(dir.path()).brain, "20261113-153012");

  // A new key goes at the end; the file stays readable for people (indented, final newline).
  change = ordered_json::object();
  change["decisions_per_second"] = 15;
  ASSERT_TRUE(update_settings(dir.path(), change, &why)) << why;
  const std::string text = slurp(path);
  EXPECT_EQ(text.back(), '\n');
  EXPECT_NE(text.find("\n  \"zeta\": 1"), std::string::npos);
  const ordered_json again = ordered_json::parse(text);
  EXPECT_EQ((--again.end()).key(), "decisions_per_second");
}

TEST(Settings, UpdateCreatesAMissingFile) {
  TempDir dir;
  ordered_json change = ordered_json::object();
  change["brain"] = "b1";
  std::string why;
  ASSERT_TRUE(update_settings(dir.path(), change, &why)) << why;
  EXPECT_EQ(ordered_json::parse(slurp(settings_path(dir.path()))), (ordered_json{{"brain", "b1"}}));
}

TEST(Settings, UpdateNeverOverwritesAFileItCantRead) {
  TempDir dir;
  const std::string path = settings_path(dir.path());
  const std::string broken = "{\"name\": \"Mine\",, oops\n";
  write(path, broken);
  ordered_json change = ordered_json::object();
  change["brain"] = "b1";
  std::string why;
  EXPECT_FALSE(update_settings(dir.path(), change, &why));
  EXPECT_FALSE(why.empty());
  EXPECT_EQ(slurp(path), broken);   // the user's text is untouched
  // No temporary files are left behind.
  int files = 0;
  for (const auto& entry : fs::directory_iterator(dir.path())) {
    (void)entry;
    ++files;
  }
  EXPECT_EQ(files, 1);
}

TEST(Files, SafeNames) {
  EXPECT_TRUE(is_safe_name("20261113-153012"));
  EXPECT_TRUE(is_safe_name("my_brain-2"));
  EXPECT_FALSE(is_safe_name(""));
  EXPECT_FALSE(is_safe_name(".."));
  EXPECT_FALSE(is_safe_name("a/b"));
  EXPECT_FALSE(is_safe_name("with space"));
  EXPECT_FALSE(is_safe_name(std::string(65, 'a')));
}

TEST(Files, UniqueChildName) {
  TempDir dir;
  EXPECT_EQ(unique_child_name(dir.path(), "run"), "run");
  fs::create_directory(fs::path(dir.path()) / "run");
  EXPECT_EQ(unique_child_name(dir.path(), "run"), "run-2");
  fs::create_directory(fs::path(dir.path()) / "run-2");
  EXPECT_EQ(unique_child_name(dir.path(), "run"), "run-3");
}

TEST(Files, AtomicWriteReplacesTheWholeFile) {
  TempDir dir;
  const std::string path = (fs::path(dir.path()) / "f.txt").string();
  write_text_file_atomic(path, "first version, quite long\n");
  write_text_file_atomic(path, "second\n");
  std::string text;
  ASSERT_TRUE(read_text_file(path, text));
  EXPECT_EQ(text, "second\n");
  EXPECT_THROW(write_text_file_atomic((fs::path(dir.path()) / "missing" / "f.txt").string(), "x"),
               std::runtime_error);
}
