// Projects on disk: the projects folder, templates, listing/reading projects, folder names, creating and
// duplicating projects, vyrn.json text and merging, and the brains list.
#include "helpers.h"
#include "vyrn/project.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <regex>
#include <string>
#include <vector>

#ifndef VYRN_SOURCE_DIR
#error "VYRN_SOURCE_DIR must be defined (tests/CMakeLists.txt does it)"
#endif

using namespace project_test;
using nlohmann::ordered_json;

namespace {

// Sets or unsets an environment variable for one test and restores the old value afterwards.
class EnvVar {
 public:
  EnvVar(const char* name, const char* value) : name_(name) {
    if (const char* old = std::getenv(name)) old_ = old;
    if (value) {
      ::setenv(name, value, 1);
    } else {
      ::unsetenv(name);
    }
  }
  ~EnvVar() {
    if (old_) {
      ::setenv(name_.c_str(), old_->c_str(), 1);
    } else {
      ::unsetenv(name_.c_str());
    }
  }
  EnvVar(const EnvVar&) = delete;
  EnvVar& operator=(const EnvVar&) = delete;

 private:
  std::string name_;
  std::optional<std::string> old_;
};

std::vector<std::string> keys_of(const std::string& json_text) {
  std::vector<std::string> keys;
  const ordered_json j = ordered_json::parse(json_text);
  for (auto it = j.begin(); it != j.end(); ++it) keys.push_back(it.key());
  return keys;
}

ordered_json settings_of(const fs::path& project) { return ordered_json::parse(read_file(project / "vyrn.json")); }

// Hidden entries left in a folder (staging folders must never be left behind).
std::vector<std::string> hidden_entries(const fs::path& dir) {
  std::vector<std::string> out;
  for (const auto& entry : fs::directory_iterator(dir)) {
    const std::string name = entry.path().filename().string();
    if (!name.empty() && name[0] == '.') out.push_back(name);
  }
  return out;
}

// A template folder with everything create_project must copy and everything it must leave out.
vyrn::TemplateInfo make_template(const fs::path& data, const std::string& id = "catch-pictures") {
  const fs::path dir = data / "templates" / id;
  write_file(dir / "vyrn.json",
             "{\n  \"name\": \"Catch (pictures)\",\n  \"template\": \"" + id +
                 "\",\n  \"round_ticks\": 1000,\n  \"custom\": {\"keep\": true}\n}\n");
  write_file(dir / "inputs.py", "import vyrn\n\ndef see(world):\n    return vyrn.canvas(10, 10)\n");
  write_file(dir / "points.py", "def points(world):\n    return 0\n");
  write_file(dir / "responses.py", "stay = None\n");
  write_file(dir / "world.py", "def step(world):\n    pass\n");
  write_file(dir / "pictures" / "fruit.png", "png bytes");
  write_file(dir / "helpers" / "util.py", "X = 1\n");
  write_file(dir / "helpers" / "__pycache__" / "util.cpython-313.pyc", "cache");
  write_file(dir / "__pycache__" / "points.cpython-313.pyc", "cache");
  write_file(dir / "stray.pyc", "cache");
  write_file(dir / "brains" / "20261113-120000" / "latest" / "current", "save-1\n");
  write_file(dir / "runs" / "20261113-120000" / "rounds.csv", "round,points\n");
  write_file(dir / ".vyrn-lock", "{\"pid\":1,\"by\":\"app\",\"since\":\"2026-11-13T12:00:00\"}\n");
  write_file(dir / "vyrn.json.tmp-123", "{half");
  fs::create_symlink("inputs.py", dir / "inputs_link.py");
  vyrn::TemplateInfo t;
  t.id = id;
  t.name = "Catch (pictures)";
  t.mode = "sim";
  t.path = dir.string();
  return t;
}

const std::regex kFolderName("^[A-Za-z0-9_][A-Za-z0-9_-]{0,63}$");

}  // namespace

// ---- projects_root ----------------------------------------------------------------------------------------------

TEST(ProjectsRoot, UsesVyrnProjectsAndCreatesIt) {
  TempDir tmp;
  const std::string wanted = tmp / "my/projects";
  EnvVar env("VYRN_PROJECTS", (wanted + "/").c_str());
  EXPECT_EQ(vyrn::projects_root(), wanted);
  EXPECT_TRUE(fs::is_directory(wanted));
}

TEST(ProjectsRoot, DefaultsToVyrnInHome) {
  TempDir tmp;
  EnvVar projects("VYRN_PROJECTS", nullptr);
  EnvVar home(vyrn_test::home_env_name(), tmp.path().c_str());
  EXPECT_EQ(vyrn::projects_root(), tmp / "Vyrn");
  EXPECT_TRUE(fs::is_directory(tmp / "Vyrn"));
}

TEST(ProjectsRoot, ExpandsTildeAndEmptyMeansDefault) {
  TempDir tmp;
  EnvVar home(vyrn_test::home_env_name(), tmp.path().c_str());
  {
    EnvVar projects("VYRN_PROJECTS", "~/games");
    EXPECT_EQ(vyrn::projects_root(), tmp / "games");
  }
  {
    EnvVar projects("VYRN_PROJECTS", "");
    EXPECT_EQ(vyrn::projects_root(), tmp / "Vyrn");
  }
}

// ---- folder_name_for ----------------------------------------------------------------------------------------------

TEST(FolderName, MakesSafeFolderNames) {
  EXPECT_EQ(vyrn::folder_name_for("My Game!"), "My-Game");
  EXPECT_EQ(vyrn::folder_name_for("Catch (pictures)"), "Catch-pictures");
  EXPECT_EQ(vyrn::folder_name_for("  lots   of\tspace  "), "lots-of-space");
  EXPECT_EQ(vyrn::folder_name_for("a - b"), "a-b");
  EXPECT_EQ(vyrn::folder_name_for("--dashes--"), "dashes");
  EXPECT_EQ(vyrn::folder_name_for("under_score-and-dash"), "under_score-and-dash");
  EXPECT_EQ(vyrn::folder_name_for("../../etc/passwd"), "etcpasswd");
  EXPECT_EQ(vyrn::folder_name_for(".hidden"), "hidden");
  EXPECT_EQ(vyrn::folder_name_for("Mario 64: Remix"), "Mario-64-Remix");
  EXPECT_EQ(vyrn::folder_name_for("Pok\xc3\xa9mon Caf\xc3\xa9"), "Pokemon-Cafe");     // é
  EXPECT_EQ(vyrn::folder_name_for("Stra\xc3\x9f" "e \xc3\x86on"), "Strasse-AEon");     // ß Æ
  EXPECT_EQ(vyrn::folder_name_for("\xe6\x97\xa5\xe6\x9c\xac game"), "game");           // other scripts are left out
  EXPECT_EQ(vyrn::folder_name_for("no\xc2\xa0" "break"), "no-break");                  // no-break space
}

TEST(FolderName, IsNeverEmpty) {
  EXPECT_EQ(vyrn::folder_name_for(""), "project");
  EXPECT_EQ(vyrn::folder_name_for("   "), "project");
  EXPECT_EQ(vyrn::folder_name_for("!!!???"), "project");
  EXPECT_EQ(vyrn::folder_name_for("---"), "project");
  EXPECT_EQ(vyrn::folder_name_for("\xe6\x97\xa5\xe6\x9c\xac"), "project");
}

TEST(FolderName, IsShortSafeAndStable) {
  const std::string long_name(200, 'x');
  EXPECT_EQ(vyrn::folder_name_for(long_name), std::string(64, 'x'));
  // Cutting at 64 never leaves a dash at the end.
  const std::string cut = vyrn::folder_name_for(std::string(63, 'a') + " b");
  EXPECT_EQ(cut, std::string(63, 'a'));
  for (const std::string name : {"My Game!", "a/b\\c", "\t\n", "x y z", "Caf\xc3\xa9", "~$HOME", "a\0b", "1"}) {
    const std::string folder = vyrn::folder_name_for(name);
    EXPECT_TRUE(std::regex_match(folder, kFolderName)) << folder;
    EXPECT_EQ(vyrn::folder_name_for(folder), folder) << "not stable for " << name;
  }
}

// ---- templates ----------------------------------------------------------------------------------------------------

TEST(Templates, ShippedTemplatesComeInTheirOrder) {
  const std::vector<vyrn::TemplateInfo> templates = vyrn::list_templates(VYRN_SOURCE_DIR);
  std::vector<std::string> ids;
  for (const auto& t : templates) ids.push_back(t.id);
  const std::vector<std::string> first = {"balance", "demo-game", "my-game", "catch-pictures", "catch-numbers"};
  ASSERT_GE(ids.size(), first.size());
  EXPECT_EQ(std::vector<std::string>(ids.begin(), ids.begin() + 5), first);
  EXPECT_EQ(ids.size(), first.size());   // nothing else ships as a preset
  for (size_t i = 0; i < first.size(); ++i) EXPECT_FALSE(templates[i].description.empty()) << templates[i].id;
  for (const auto& t : templates) {
    EXPECT_FALSE(t.name.empty()) << t.id;
    EXPECT_TRUE(fs::path(t.path).is_absolute()) << t.path;
    EXPECT_TRUE(fs::is_regular_file(fs::path(t.path) / "vyrn.json")) << t.path;
    EXPECT_EQ(t.mode, fs::exists(fs::path(t.path) / "world.py") ? "sim" : "realtime") << t.id;
  }
  EXPECT_EQ(templates[0].mode, "sim");        // balance
  EXPECT_EQ(templates[1].mode, "realtime");   // demo-game
  EXPECT_EQ(templates[2].mode, "realtime");   // my-game
  EXPECT_EQ(templates[3].mode, "sim");
  EXPECT_EQ(templates[4].mode, "sim");
  EXPECT_TRUE(templates[3].demo && templates[4].demo && !templates[0].demo);
  EXPECT_EQ(templates[3].name, "Catch (pictures)");
}

TEST(Templates, DescriptionsAndOrderOfOtherTemplates) {
  TempDir data;
  const fs::path t = fs::path(data.path()) / "templates";
  write_file(t / "zeta" / "vyrn.json", R"({"name": "Zeta", "description": "  A custom one  "})");
  write_file(t / "alpha" / "vyrn.json", R"({"name": "alpha game"})");
  write_file(t / "alpha" / "world.py", "def step(world):\n    pass\n");
  write_file(t / "my-game" / "vyrn.json", R"({"name": "My game", "description": "Mine wins"})");
  write_file(t / "catch-numbers" / "vyrn.json", R"J({"name": "Catch (numbers)"})J");
  write_file(t / "catch-numbers" / "world.py", "");
  write_file(t / "demo-game" / "vyrn.json", "this is not JSON");
  write_file(t / "catch-pictures" / "vyrn.json", R"J({"name": "Catch (pictures)", "description": 5})J");
  write_file(t / "no-settings" / "inputs.py", "");
  write_file(t / "link-runner" / "vyrn.json", R"({"name": "Link runner"})");
  write_file(t / ".hidden" / "vyrn.json", R"({"name": "Hidden"})");
  write_file(t / "file.json", "{}");

  const auto templates = vyrn::list_templates(data.path());
  std::vector<std::string> ids;
  for (const auto& tmpl : templates) ids.push_back(tmpl.id);
  EXPECT_EQ(ids,
            (std::vector<std::string>{"demo-game", "my-game", "catch-pictures", "catch-numbers", "alpha", "link-runner",
                                      "zeta"}));

  EXPECT_EQ(templates[0].name, "demo-game");   // unreadable vyrn.json: the folder name
  EXPECT_EQ(templates[0].description, "A simple dodge game to try real-game training, no setup needed");
  EXPECT_FALSE(templates[0].demo);
  EXPECT_EQ(templates[1].description, "Mine wins");
  EXPECT_EQ(templates[2].description,
            "Catch falling fruit from a 10x10 picture. A quick first look at how it all works");
  EXPECT_TRUE(templates[2].demo);
  EXPECT_EQ(templates[3].description, "The same fruit game, but the AI is handed three numbers instead of a picture");
  EXPECT_EQ(templates[3].mode, "sim");
  EXPECT_TRUE(templates[3].demo);
  EXPECT_EQ(templates[4].name, "alpha game");
  EXPECT_EQ(templates[4].mode, "sim");
  EXPECT_EQ(templates[4].description, "");
  EXPECT_EQ(templates[6].description, "A custom one");
  EXPECT_EQ(templates[6].mode, "realtime");
  EXPECT_EQ(templates[6].path, (t / "zeta").string());
}

TEST(Templates, MissingDataDirGivesNothing) {
  TempDir tmp;
  EXPECT_TRUE(vyrn::list_templates(tmp / "nowhere").empty());
}

// ---- create_project -----------------------------------------------------------------------------------------------

TEST(CreateProject, CopiesTheTemplateAndSetsNameAndTemplate) {
  TempDir data, root_parent;
  const vyrn::TemplateInfo tmpl = make_template(data.path());
  const std::string root = root_parent / "Vyrn";   // missing: created
  const std::string path = vyrn::create_project(root, "  My Game!  ", tmpl);
  EXPECT_EQ(path, root + "/My-Game");
  const fs::path p = path;

  for (const char* file : {"inputs.py", "points.py", "responses.py", "world.py", "pictures/fruit.png",
                           "helpers/util.py"}) {
    EXPECT_EQ(read_file(p / file), read_file(fs::path(tmpl.path) / file)) << file;
  }
  EXPECT_TRUE(fs::is_symlink(p / "inputs_link.py"));
  EXPECT_EQ(fs::read_symlink(p / "inputs_link.py"), fs::path("inputs.py"));
  for (const char* skipped : {"brains", "runs", "__pycache__", "helpers/__pycache__", ".vyrn-lock", "stray.pyc",
                              "vyrn.json.tmp-123"}) {
    EXPECT_FALSE(fs::exists(fs::symlink_status(p / skipped))) << skipped << " should not be copied";
  }

  // "name" and "template" are set; every other key stays, in order.
  EXPECT_EQ(keys_of(read_file(p / "vyrn.json")),
            (std::vector<std::string>{"name", "template", "round_ticks", "custom"}));
  const ordered_json s = settings_of(p);
  EXPECT_EQ(s["name"], "My Game!");
  EXPECT_EQ(s["template"], "catch-pictures");
  EXPECT_EQ(s["custom"]["keep"], true);

  EXPECT_TRUE(hidden_entries(root).empty());   // no staging folder left behind
  const vyrn::ProjectInfo info = vyrn::read_project(path);
  EXPECT_EQ(info.name, "My Game!");
  EXPECT_EQ(info.mode, "sim");
  EXPECT_EQ(info.template_id, "catch-pictures");
}

TEST(CreateProject, ControlCharactersInTheNameBecomeSpaces) {
  TempDir data, root;
  const vyrn::TemplateInfo tmpl = make_template(data.path());
  const std::string path = vyrn::create_project(root.path(), "My\nGame\t2\r", tmpl);
  EXPECT_EQ(fs::path(path).filename(), "My-Game-2");
  EXPECT_EQ(settings_of(path)["name"], "My Game 2");
  // Only ASCII spaces are trimmed: "\xc3\xa0" (a with grave accent) ends in the byte 0xA0.
  const std::string voila = vyrn::create_project(root.path(), " Voil\xc3\xa0 ", tmpl);
  EXPECT_EQ(fs::path(voila).filename(), "Voila");
  EXPECT_EQ(settings_of(voila)["name"], "Voil\xc3\xa0");
}

TEST(CreateProject, NeverOverwrites) {
  VYRN_SKIP_WITHOUT_SYMLINKS();
  TempDir data, root;
  const vyrn::TemplateInfo tmpl = make_template(data.path());
  const std::string path = vyrn::create_project(root.path(), "Game", tmpl);
  write_file(fs::path(path) / "points.py", "def points():\n    return 42\n");
  const std::string settings_before = read_file(fs::path(path) / "vyrn.json");

  EXPECT_THROW(vyrn::create_project(root.path(), "Game", tmpl), std::runtime_error);
  EXPECT_THROW(vyrn::create_project(root.path(), "Game?!", tmpl), std::runtime_error);   // same folder name
  EXPECT_EQ(read_file(fs::path(path) / "points.py"), "def points():\n    return 42\n");
  EXPECT_EQ(read_file(fs::path(path) / "vyrn.json"), settings_before);

  // An empty folder, a plain file and a dangling symlink with that name are not replaced either.
  fs::create_directory(root / "Empty");
  write_file(root / "Plain", "mine");
  fs::create_symlink(root / "missing-target", root / "Dangling");
  for (const char* name : {"Empty", "Plain", "Dangling"}) {
    try {
      vyrn::create_project(root.path(), name, tmpl);
      ADD_FAILURE() << "created over " << name;
    } catch (const std::runtime_error& e) {
      EXPECT_NE(std::string(e.what()).find("already"), std::string::npos) << e.what();
    }
  }
  EXPECT_TRUE(fs::is_empty(root / "Empty"));
  EXPECT_EQ(read_file(root / "Plain"), "mine");
  EXPECT_TRUE(fs::is_symlink(root / "Dangling"));
  EXPECT_TRUE(hidden_entries(root.path()).empty());
}

TEST(CreateProject, RefusesEmptyNamesAndMissingTemplates) {
  TempDir data, root;
  const vyrn::TemplateInfo tmpl = make_template(data.path());
  EXPECT_THROW(vyrn::create_project(root.path(), "", tmpl), std::runtime_error);
  EXPECT_THROW(vyrn::create_project(root.path(), " \t ", tmpl), std::runtime_error);
  vyrn::TemplateInfo missing = tmpl;
  missing.path = data / "templates/nope";
  EXPECT_THROW(vyrn::create_project(root.path(), "Game", missing), std::runtime_error);
  vyrn::TemplateInfo no_path = tmpl;
  no_path.path.clear();
  EXPECT_THROW(vyrn::create_project(root.path(), "Game", no_path), std::runtime_error);
  // A project inside its own template folder would copy itself forever.
  EXPECT_THROW(vyrn::create_project(tmpl.path, "Inside", tmpl), std::runtime_error);
  EXPECT_TRUE(fs::is_empty(root.path()));
}

TEST(CreateProject, CopiesOfReadOnlyTemplatesAreWritable) {
  TempDir data, root;
  const vyrn::TemplateInfo tmpl = make_template(data.path());
  fs::permissions(fs::path(tmpl.path) / "points.py", fs::perms::owner_read | fs::perms::group_read);
  fs::permissions(fs::path(tmpl.path) / "pictures", fs::perms::owner_read | fs::perms::owner_exec);
  const fs::path p = vyrn::create_project(root.path(), "Writable", tmpl);
  EXPECT_NE(fs::status(p / "points.py").permissions() & fs::perms::owner_write, fs::perms::none);
  EXPECT_NE(fs::status(p / "pictures").permissions() & fs::perms::owner_write, fs::perms::none);
  fs::permissions(fs::path(tmpl.path) / "pictures", fs::perms::owner_all);
}

TEST(CreateProject, FromTheShippedTemplates) {
  TempDir root;
  const auto templates = vyrn::list_templates(VYRN_SOURCE_DIR);
  for (const auto& t : templates) {
    const std::string path = vyrn::create_project(root.path(), "From " + t.id, t);
    const vyrn::ProjectInfo info = vyrn::read_project(path);
    EXPECT_EQ(info.name, "From " + t.id);
    EXPECT_EQ(info.template_id, t.id);
    EXPECT_EQ(info.mode, t.mode);
    EXPECT_FALSE(fs::exists(fs::path(path) / "brains"));
  }
}

// ---- duplicate_project ------------------------------------------------------------------------------------------

TEST(DuplicateProject, NamesCopiesAndLeavesOutRunData) {
  TempDir root;
  const fs::path src = fs::path(root.path()) / "X";
  write_file(src / "vyrn.json", "{\n  \"name\": \"X\",\n  \"brain\": \"20261113-120000\",\n  \"round_ticks\": 5\n}\n");
  write_file(src / "inputs.py", "def see():\n    return 1\n");
  write_file(src / "pictures" / "over.png", "png");
  write_brain_save(src, "20261113-120000", "latest", 1, SaveSpec{});
  write_file(src / "runs" / "20261113-120000" / "rounds.csv", "round\n");
  write_file(src / "__pycache__" / "inputs.cpython-313.pyc", "cache");
  write_file(src / ".vyrn-lock", "{\"pid\":1}\n");
  write_file(src / ".vyrn-link-code", "483920\n");   // Vyrn Link: each project makes its own pairing code

  const std::string copy1 = vyrn::duplicate_project(src.string());
  EXPECT_EQ(copy1, (fs::path(root.path()) / "X-copy").string());
  const ordered_json s1 = settings_of(copy1);
  EXPECT_EQ(s1["name"], "X copy");
  EXPECT_EQ(s1["brain"], "");   // the copy has no brains
  EXPECT_EQ(s1["round_ticks"], 5);
  EXPECT_EQ(keys_of(read_file(fs::path(copy1) / "vyrn.json")),
            (std::vector<std::string>{"name", "brain", "round_ticks"}));
  EXPECT_EQ(read_file(fs::path(copy1) / "inputs.py"), "def see():\n    return 1\n");
  EXPECT_TRUE(fs::exists(fs::path(copy1) / "pictures" / "over.png"));
  for (const char* skipped : {"brains", "runs", "__pycache__", ".vyrn-lock", ".vyrn-link-code"}) {
    EXPECT_FALSE(fs::exists(fs::path(copy1) / skipped)) << skipped;
  }

  const std::string copy2 = vyrn::duplicate_project(src.string());
  EXPECT_EQ(copy2, (fs::path(root.path()) / "X-copy-2").string());
  EXPECT_EQ(settings_of(copy2)["name"], "X copy 2");
  const std::string copy3 = vyrn::duplicate_project(src.string());
  EXPECT_EQ(settings_of(copy3)["name"], "X copy 3");

  // The first free name is used again once it is free.
  fs::remove_all(copy1);
  EXPECT_EQ(vyrn::duplicate_project(src.string()), copy1);

  // The source is untouched, and nothing hidden is left behind.
  EXPECT_EQ(settings_of(src)["name"], "X");
  EXPECT_TRUE(fs::exists(src / "brains"));
  EXPECT_EQ(hidden_entries(root.path()), std::vector<std::string>{});
  EXPECT_EQ(vyrn::list_projects(root.path()).size(), 4u);
}

TEST(DuplicateProject, UsesTheProjectNameForTheFolder) {
  TempDir root;
  const fs::path src = fs::path(root.path()) / "folder-name";
  write_file(src / "vyrn.json", R"J({"name": "Catch (pictures)"})J");
  const std::string copy = vyrn::duplicate_project(src.string());
  EXPECT_EQ(fs::path(copy).filename(), "Catch-pictures-copy");
  EXPECT_EQ(settings_of(copy)["name"], "Catch (pictures) copy");
}

TEST(DuplicateProject, WithoutOrWithBrokenSettings) {
  TempDir root;
  const fs::path bare = fs::path(root.path()) / "bare";
  write_file(bare / "inputs.py", "x = 1\n");
  const std::string copy = vyrn::duplicate_project(bare.string());
  EXPECT_EQ(fs::path(copy).filename(), "bare-copy");
  EXPECT_EQ(settings_of(copy)["name"], "bare copy");

  const fs::path broken = fs::path(root.path()) / "broken";
  write_file(broken / "vyrn.json", "{ \"name\": oops");
  const std::string copy2 = vyrn::duplicate_project(broken.string());
  EXPECT_EQ(fs::path(copy2).filename(), "broken-copy");
  EXPECT_EQ(read_file(fs::path(copy2) / "vyrn.json"), "{ \"name\": oops");   // never rewritten
}

TEST(DuplicateProject, LongNamesStillGetFreeFolders) {
  TempDir root;
  const std::string long_name(80, 'L');
  const fs::path src = fs::path(root.path()) / std::string(64, 'L');
  write_file(src / "vyrn.json", "{\"name\": \"" + long_name + "\"}");
  const fs::path copy1 = vyrn::duplicate_project(src.string());
  const fs::path copy2 = vyrn::duplicate_project(src.string());
  EXPECT_EQ(copy1.filename(), std::string(59, 'L') + "-copy");
  EXPECT_EQ(copy2.filename(), std::string(57, 'L') + "-copy-2");
  EXPECT_EQ(settings_of(copy2)["name"], long_name + " copy 2");
}

TEST(DuplicateProject, RefusesFoldersThatAreNotProjects) {
  TempDir root;
  write_file(fs::path(root.path()) / "notes" / "readme.txt", "hi");
  EXPECT_THROW(vyrn::duplicate_project(root / "notes"), std::runtime_error);
  EXPECT_THROW(vyrn::duplicate_project(root / "missing"), std::runtime_error);
}

// ---- list_projects / read_project ---------------------------------------------------------------------------

TEST(Projects, ListsProjectFoldersNewestFirst) {
  TempDir root;
  const fs::path r = root.path();
  write_file(r / "alpha" / "vyrn.json", R"({"name": "Alpha", "template": "catch-numbers"})");
  write_file(r / "alpha" / "world.py", "def step(world):\n    pass\n");
  write_file(r / "beta" / "inputs.py", "def see():\n    return 1\n");
  write_file(r / "notes" / "readme.txt", "not a project");
  write_file(r / ".hidden" / "vyrn.json", "{}");
  write_file(r / "file.py", "x = 1\n");
  const int64_t t0 = now_epoch() - 10000;
  set_mtime(r / "alpha" / "vyrn.json", t0);
  set_mtime(r / "alpha" / "world.py", t0 + 100);
  set_mtime(r / "beta" / "inputs.py", t0 + 50);

  auto projects = vyrn::list_projects(root.path());
  ASSERT_EQ(projects.size(), 2u);
  EXPECT_EQ(projects[0].name, "Alpha");
  EXPECT_EQ(projects[0].modified, t0 + 100);
  EXPECT_EQ(projects[0].mode, "sim");
  EXPECT_EQ(projects[0].template_id, "catch-numbers");
  EXPECT_EQ(projects[0].path, (r / "alpha").string());
  EXPECT_EQ(projects[1].name, "beta");   // no vyrn.json: the folder name
  EXPECT_EQ(projects[1].mode, "realtime");
  EXPECT_EQ(projects[1].template_id, "");
  EXPECT_EQ(projects[1].modified, t0 + 50);

  // A newer save of a brain counts as a change.
  write_file(r / "beta" / "vyrn.json", R"({"brain": "b1"})");
  set_mtime(r / "beta" / "vyrn.json", t0);
  write_brain_save(r / "beta", "b1", "latest", 1, SaveSpec{});
  set_mtime(r / "beta" / "brains" / "b1" / "latest" / "current", t0 + 500);
  set_mtime(r / "beta" / "brains" / "b1", t0);
  set_mtime(r / "beta" / "brains", t0);
  projects = vyrn::list_projects(root.path());
  ASSERT_EQ(projects.size(), 2u);
  EXPECT_EQ(projects[0].name, "beta");
  EXPECT_EQ(projects[0].modified, t0 + 500);

  EXPECT_TRUE(vyrn::list_projects(root / "missing").empty());
}

TEST(Projects, ReadProjectShowsTheActiveBrainAndTheTrainer) {
  TempDir root;
  const fs::path p = fs::path(root.path()) / "game";
  write_file(p / "vyrn.json", R"({"name": "Game", "brain": "b2"})");
  write_file(p / "inputs.py", "");
  SaveSpec latest;
  latest.best_mean20 = 0.75;
  latest.seconds = 300.5;
  latest.ticks = 40000;
  latest.saved = "2026-11-13T10:00:00";
  write_brain_save(p, "b2", "latest", 3, latest);
  SaveSpec best;
  best.best_mean20 = 0.8;
  best.seconds = 250;
  best.ticks = 30000;
  best.saved = "2026-11-13T09:59:00";
  write_brain_save(p, "b2", "best", 2, best);
  SaveSpec other;
  other.best_mean20 = 9;
  other.seconds = 1000;
  write_brain_save(p, "b1", "latest", 1, other);   // not active: ignored

  vyrn::ProjectInfo info = vyrn::read_project(p.string() + "/");
  EXPECT_EQ(info.path, p.string());
  EXPECT_EQ(info.name, "Game");
  EXPECT_TRUE(info.has_best);
  EXPECT_DOUBLE_EQ(info.best_mean20, 0.8);
  EXPECT_DOUBLE_EQ(info.time_trained_s, 300.5);
  EXPECT_EQ(info.training_by, "");
  {
    auto lock = vyrn::ProjectLock::acquire(p.string(), "agent");
    ASSERT_TRUE(lock.has_value());
    EXPECT_EQ(vyrn::read_project(p.string()).training_by, "agent");
  }
  EXPECT_EQ(vyrn::read_project(p.string()).training_by, "");

  // No brain yet.
  write_file(p / "vyrn.json", R"({"name": "Game"})");
  info = vyrn::read_project(p.string());
  EXPECT_FALSE(info.has_best);
  EXPECT_DOUBLE_EQ(info.best_mean20, 0);
  EXPECT_DOUBLE_EQ(info.time_trained_s, 0);
}

TEST(Projects, ReadProjectRefusesOtherFolders) {
  TempDir root;
  write_file(fs::path(root.path()) / "notes" / "readme.txt", "hi");
  write_file(fs::path(root.path()) / "plain.txt", "hi");
  EXPECT_THROW(vyrn::read_project(root / "notes"), std::runtime_error);
  EXPECT_THROW(vyrn::read_project(root / "missing"), std::runtime_error);
  EXPECT_THROW(vyrn::read_project(root / "plain.txt"), std::runtime_error);
  EXPECT_THROW(vyrn::read_project(""), std::runtime_error);
}

// ---- settings ---------------------------------------------------------------------------------------------------

TEST(Settings, ReadSettingsText) {
  TempDir p;
  EXPECT_EQ(vyrn::read_settings_text(p.path()), "{}");
  write_file(p / "vyrn.json", "{\n  \"name\": \"A\"\n}\n");
  EXPECT_EQ(vyrn::read_settings_text(p.path()), "{\n  \"name\": \"A\"\n}\n");
  write_file(p / "vyrn.json", "not json");
  EXPECT_EQ(vyrn::read_settings_text(p.path()), "not json");   // as text, even when broken
}

TEST(Settings, MergeKeepsUnknownKeysAndOrder) {
  TempDir p;
  write_file(p / "vyrn.json",
             R"({"name": "A", "zzz": 1, "advanced": {"frames": 2, "explore_ticks": 5000}, "aaa": [1, 2]})");
  std::string why = "old";
  ASSERT_TRUE(vyrn::merge_settings(p.path(), R"({"round_ticks": 500, "zzz": 2, "advanced": {"frames": 4}})", &why))
      << why;
  EXPECT_EQ(why, "");
  const std::string text = read_file(p / "vyrn.json");
  EXPECT_EQ(keys_of(text), (std::vector<std::string>{"name", "zzz", "advanced", "aaa", "round_ticks"}));
  const ordered_json s = ordered_json::parse(text);
  EXPECT_EQ(s["zzz"], 2);
  EXPECT_EQ(s["round_ticks"], 500);
  EXPECT_EQ(s["advanced"], ordered_json::parse(R"({"frames": 4})"));   // top-level keys are replaced as a whole
  EXPECT_EQ(s["aaa"], ordered_json::parse("[1, 2]"));
  EXPECT_EQ(text.substr(0, 4), "{\n  ");   // Vyrn's own format: 2-space indent
  EXPECT_EQ(text.back(), '\n');
}

TEST(Settings, MergeCreatesAMissingFile) {
  TempDir p;
  ASSERT_TRUE(vyrn::merge_settings(p.path(), R"({"name": "New", "agents": {"screen": true}})", nullptr));
  EXPECT_EQ(keys_of(read_file(p / "vyrn.json")), (std::vector<std::string>{"name", "agents"}));
  EXPECT_TRUE(vyrn::merge_settings(p.path(), "{}", nullptr));
}

TEST(Settings, MergeRefusesBrokenFilesAndBadInput) {
  TempDir p;
  for (const std::string broken : {"{ \"name\": oops", "[1, 2]", "\"text\"", ""}) {
    write_file(p / "vyrn.json", broken);
    std::string why;
    EXPECT_FALSE(vyrn::merge_settings(p.path(), R"({"name": "B"})", &why)) << broken;
    EXPECT_FALSE(why.empty());
    EXPECT_EQ(read_file(p / "vyrn.json"), broken) << "the user's text must never be lost";
  }
  write_file(p / "vyrn.json", R"({"name": "A"})");
  for (const std::string bad : {"[1]", "42", "nope", "", "{\"a\": }", "null"}) {
    std::string why;
    EXPECT_FALSE(vyrn::merge_settings(p.path(), bad, &why)) << bad;
    EXPECT_FALSE(why.empty()) << bad;
    EXPECT_FALSE(vyrn::merge_settings(p.path(), bad, nullptr)) << bad;
  }
  EXPECT_EQ(read_file(p / "vyrn.json"), R"({"name": "A"})");
  std::string why;
  EXPECT_FALSE(vyrn::merge_settings(p / "missing-folder", R"({"name": "A"})", &why));
  EXPECT_FALSE(why.empty());
}

// ---- list_brains ----------------------------------------------------------------------------------------------

TEST(Brains, ActiveFirstThenNewestWithTheirNumbers) {
  TempDir tmp;
  const fs::path p = tmp.path();
  write_file(p / "vyrn.json", R"({"name": "Game", "brain": "20261110-100000"})");

  SaveSpec a_latest;   // active, oldest
  a_latest.best_mean20 = 0.5;
  a_latest.seconds = 600;
  a_latest.ticks = 50000;
  a_latest.saved = "2026-11-10T10:30:15";
  write_brain_save(p, "20261110-100000", "latest", 7, a_latest);
  SaveSpec a_best = a_latest;
  a_best.best_mean20 = 0.65;   // the best checkpoint can be newer and better than the last latest save
  a_best.seconds = 640;
  a_best.ticks = 52000;
  a_best.saved = "2026-11-10T10:31:00";
  write_brain_save(p, "20261110-100000", "best", 4, a_best);

  SaveSpec b_latest;   // newest, no 20-round mean yet
  b_latest.seconds = 30;
  b_latest.ticks = 900;
  b_latest.saved = "2026-11-12T09:05:59";
  write_brain_save(p, "20261112-090000", "latest", 1, b_latest);

  SaveSpec c_latest;   // in the middle
  c_latest.best_mean20 = -1.5;
  c_latest.seconds = 100;
  c_latest.ticks = 2000;
  c_latest.saved = "2026-11-11T08:00:00";
  write_brain_save(p, "20261111-080000", "latest", 2, c_latest);

  fs::create_directories(p / "brains" / "20261113-000000");   // reserved by a run that never saved: left out
  fs::create_directories(p / "brains" / ".hidden" / "latest");
  fs::create_directories(p / "brains" / "bad name" / "latest");
  write_file(p / "brains" / "notes.txt", "hi");

  const std::vector<vyrn::BrainInfo> brains = vyrn::list_brains(p.string());
  ASSERT_EQ(brains.size(), 3u);
  EXPECT_EQ(brains[0].id, "20261110-100000");
  EXPECT_TRUE(brains[0].active);
  EXPECT_TRUE(brains[0].has_best);
  EXPECT_DOUBLE_EQ(brains[0].best_mean20, 0.65);
  EXPECT_DOUBLE_EQ(brains[0].time_trained_s, 640);
  EXPECT_EQ(brains[0].ticks, 52000);
  EXPECT_EQ(brains[0].updated, "2026-11-10 10:31");

  EXPECT_EQ(brains[1].id, "20261112-090000");
  EXPECT_FALSE(brains[1].active);
  EXPECT_FALSE(brains[1].has_best);
  EXPECT_DOUBLE_EQ(brains[1].best_mean20, 0);
  EXPECT_DOUBLE_EQ(brains[1].time_trained_s, 30);
  EXPECT_EQ(brains[1].ticks, 900);
  EXPECT_EQ(brains[1].updated, "2026-11-12 09:05");

  EXPECT_EQ(brains[2].id, "20261111-080000");
  EXPECT_TRUE(brains[2].has_best);
  EXPECT_DOUBLE_EQ(brains[2].best_mean20, -1.5);
  EXPECT_EQ(brains[2].updated, "2026-11-11 08:00");

  // Another active brain moves to the front.
  write_file(p / "vyrn.json", R"({"brain": "20261111-080000"})");
  const auto again = vyrn::list_brains(p.string());
  ASSERT_EQ(again.size(), 3u);
  EXPECT_EQ(again[0].id, "20261111-080000");
  EXPECT_EQ(again[1].id, "20261112-090000");
  EXPECT_EQ(again[2].id, "20261110-100000");
}

TEST(Brains, FallsBackLikeTheCoreWhenASaveIsDamaged) {
  TempDir tmp;
  const fs::path p = tmp.path();
  SaveSpec good;
  good.best_mean20 = 1.25;
  good.seconds = 50;
  good.saved = "2026-11-13T11:00:00";
  write_brain_save(p, "b", "latest", 3, good);
  SaveSpec newer_without_model = good;   // newest save, but its model.pt is missing: not loadable
  newer_without_model.best_mean20 = 99;
  newer_without_model.model = false;
  newer_without_model.pointer = false;
  write_brain_save(p, "b", "latest", 5, newer_without_model);
  write_file(p / "brains" / "b" / "latest" / "current", "save-9\n");   // points at a save that isn't there

  const auto brains = vyrn::list_brains(p.string());
  ASSERT_EQ(brains.size(), 1u);
  EXPECT_DOUBLE_EQ(brains[0].best_mean20, 1.25);
  EXPECT_EQ(brains[0].updated, "2026-11-13 11:00");
  EXPECT_FALSE(brains[0].active);

  // A damaged brain.json is skipped too.
  write_file(p / "brains" / "b" / "latest" / "save-3" / "brain.json", "{ broken");
  EXPECT_TRUE(vyrn::list_brains(p.string()).empty());
}

TEST(Brains, WithoutASaveTimeTheFileTimeIsUsed) {
  TempDir tmp;
  const fs::path p = tmp.path();
  SaveSpec spec;
  spec.ticks = 10;
  write_brain_save(p, "b", "latest", 1, spec);
  const int64_t when = local_epoch(2026, 1, 2, 3, 4, 5);
  set_mtime(p / "brains" / "b" / "latest" / "save-1" / "brain.json", when);
  const auto brains = vyrn::list_brains(p.string());
  ASSERT_EQ(brains.size(), 1u);
  EXPECT_EQ(brains[0].updated, "2026-01-02 03:04");
  EXPECT_EQ(brains[0].ticks, 10);
}

TEST(Brains, ActiveBrainWithoutSavesAndNoBrainsFolder) {
  TempDir tmp;
  const fs::path p = tmp.path();
  EXPECT_TRUE(vyrn::list_brains(p.string()).empty());
  write_file(p / "vyrn.json", R"({"brain": "fresh"})");
  fs::create_directories(p / "brains" / "fresh");
  const auto brains = vyrn::list_brains(p.string());
  ASSERT_EQ(brains.size(), 1u);
  EXPECT_EQ(brains[0].id, "fresh");
  EXPECT_TRUE(brains[0].active);
  EXPECT_FALSE(brains[0].has_best);
  EXPECT_EQ(brains[0].updated, "");
}
