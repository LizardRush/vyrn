// Unit tests for the MCP server's safety rules (docs/dev/mcp.md section 4) and its project tools: where agents may
// work, script names and writes, real games, settings, locks, resources and options. Nothing here starts Python
// except vyrn_guide (which reads the library's docstrings once).
#include "server.h"
#include "workspace.h"

#include "vyrn/project.h"

#include <gtest/gtest.h>

#include "platform/net.h"
#include "platform_test.h"

#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using vyrn::mcp::json;
using vyrn::mcp::Options;
using vyrn::mcp::Project;
using vyrn::mcp::Server;
using vyrn::mcp::SleepWaiter;
using vyrn::mcp::ToolResult;

class TempDir {
 public:
  TempDir() {
    std::string pattern = (fs::temp_directory_path() / "vyrn-mcp-test-XXXXXX").string();
    if (::mkdtemp(pattern.data())) path_ = fs::canonical(pattern);
  }
  ~TempDir() {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(path_, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code perm;
      if (!it->is_symlink(perm)) fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, perm);
    }
    fs::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_file(const fs::path& p, const std::string& text) {
  std::ofstream out(p, std::ios::binary);
  out << text;
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

class Safety : public ::testing::Test {
 protected:
  void SetUp() override {
    projects = tmp.path() / "projects";
    outside = tmp.path() / "outside";
    fs::create_directories(projects);
    fs::create_directories(outside);
    write_file(outside / "inputs.py", "def see():\n    return 1\n");
    write_file(outside / "secret.py", "TOKEN = 'nope'\n");
    options.projects_dir = projects.string();
    options.data_dir = VYRN_SOURCE_DIR;
    server = std::make_unique<Server>(options);
    sim = create("Sim", "catch-numbers");
    real = create("Real", "my-game");
  }

  ToolResult call(const std::string& tool, json args) { return server->call(tool, std::move(args), waiter); }

  fs::path create(const std::string& name, const std::string& tmpl) {
    if (tmpl == "link-runner") {   // a test-only project (not a preset): copy it from tests/fixtures
      const fs::path dest = projects / name;
      fs::copy(fs::path(VYRN_SOURCE_DIR) / "tests" / "fixtures" / tmpl, dest, fs::copy_options::recursive);
      return dest;
    }
    const ToolResult r = call("vyrn_create_project", json{{"name", name}, {"template", tmpl}});
    EXPECT_FALSE(r.is_error) << r.text;
    return r.data.value("path", "");
  }

  Project project(const fs::path& path) {
    Project p;
    std::string why;
    EXPECT_TRUE(server->context().workspace.resolve(path.string(), p, why)) << why;
    return p;
  }

  TempDir tmp;
  fs::path projects, outside, sim, real;
  Options options;
  std::unique_ptr<Server> server;
  SleepWaiter waiter;
};

TEST_F(Safety, ToolsAreListedInTheDocumentedOrder) {
  std::vector<std::string> names;
  for (const auto& t : server->tools().all()) names.push_back(t.name);
  EXPECT_EQ(names, (std::vector<std::string>{"vyrn_guide", "vyrn_list_templates", "vyrn_list_projects",
                                             "vyrn_create_project", "vyrn_read_project", "vyrn_write_script",
                                             "vyrn_check", "vyrn_train", "vyrn_status", "vyrn_stop", "vyrn_watch",
                                             "vyrn_see", "vyrn_settings", "vyrn_share", "vyrn_inspect_package",
                                             "vyrn_import_package"}));
  for (const auto& t : server->tools().all()) {
    EXPECT_FALSE(t.description.empty()) << t.name;
    EXPECT_EQ(t.input_schema["additionalProperties"], false) << t.name;
  }
}

TEST_F(Safety, ProjectsResolveByFolderNameDisplayNameOrPath) {
  for (const std::string& arg : {std::string("Sim"), sim.string(), std::string(" Sim ")}) {
    Project p;
    std::string why;
    ASSERT_TRUE(server->context().workspace.resolve(arg, p, why)) << arg << ": " << why;
    EXPECT_EQ(p.path, sim.string());
    EXPECT_EQ(p.folder, "Sim");
    EXPECT_TRUE(p.sim);
  }
  EXPECT_FALSE(project(real).sim);
}

TEST_F(Safety, ProjectsOutsideTheAllowedFoldersAreRefused) {
  VYRN_SKIP_WITHOUT_SYMLINKS();
  fs::create_symlink(outside, projects / "Sneaky");
  fs::create_directories(projects / "Empty");
  const std::vector<std::string> bad = {
      "../outside", outside.string(), "/etc", "/", "..", ".", ".hidden", "Sneaky", (projects / "Sneaky").string(),
      (projects / ".." / "outside").string(), "Sim/../../outside", "~", "No-such-project", "Empty", "",
      "Sim\nx", (sim / "points.py").string()};
  for (const auto& arg : bad) {
    const ToolResult r = call("vyrn_read_project", json{{"project", arg}});
    EXPECT_TRUE(r.is_error) << "accepted " << arg;
    EXPECT_FALSE(r.text.empty());
  }
  EXPECT_TRUE(contains(call("vyrn_read_project", json{{"project", "Sneaky"}}).text, "outside"));
  EXPECT_TRUE(contains(call("vyrn_read_project", json{{"project", "/etc"}}).text, "outside"));
  EXPECT_TRUE(contains(call("vyrn_read_project", json{{"project", "Empty"}}).text, "not a Vyrn project"));
  // The symlinked folder isn't listed either.
  const ToolResult listed = call("vyrn_list_projects", json::object());
  for (const auto& p : listed.data["projects"]) EXPECT_NE(p["folder"], "Sneaky");
  EXPECT_EQ(listed.data["projects"].size(), 2u);
}

TEST_F(Safety, AllowDirAddsOneMoreFolder) {
  options.allow_dirs = {outside.string()};
  Server allowed(options);
  SleepWaiter w;
  const ToolResult r = allowed.call("vyrn_read_project", json{{"project", outside.string()}}, w);
  EXPECT_FALSE(r.is_error) << r.text;
  EXPECT_EQ(r.data["scripts"]["inputs.py"], "def see():\n    return 1\n");
  EXPECT_FALSE(allowed.call("vyrn_read_project", json{{"project", "outside"}}, w).is_error);
  EXPECT_TRUE(allowed.call("vyrn_read_project", json{{"project", "/etc"}}, w).is_error);
}

TEST_F(Safety, ScriptNamesMustBePlainPythonFilesInTheProject) {
  const auto& ws = server->context().workspace;
  const Project p = project(sim);
  for (const std::string name : {"../evil.py", "sub/evil.py", ".hidden.py", "notes.txt", "", "/tmp/evil.py", "..",
                                 "evil.py\n", "a\\b.py", ".py", "points.py/x"}) {
    std::string why;
    EXPECT_FALSE(ws.write_script(p, name, "x = 1\n", why)) << "wrote " << name;
    EXPECT_FALSE(why.empty());
    const ToolResult r = call("vyrn_write_script", json{{"project", "Sim"}, {"file", name}, {"content", "x = 1\n"}});
    EXPECT_TRUE(r.is_error) << name;
  }
  EXPECT_FALSE(fs::exists(projects / "evil.py"));
  EXPECT_FALSE(fs::exists(tmp.path() / "evil.py"));
  EXPECT_FALSE(fs::exists(sim / "notes.txt"));
}

TEST_F(Safety, ScriptWritesAreLimitedAndNeverFollowLinks) {
  VYRN_SKIP_WITHOUT_SYMLINKS();
  const auto& ws = server->context().workspace;
  const Project p = project(sim);
  std::string why;
  ASSERT_TRUE(ws.write_script(p, "helper.py", "def f():\n    return 2\n", why)) << why;
  EXPECT_EQ(read_file(sim / "helper.py"), "def f():\n    return 2\n");
  ASSERT_TRUE(ws.write_script(p, "helper.py", std::string(vyrn::mcp::kMaxScriptBytes, '#'), why)) << why;
  EXPECT_FALSE(ws.write_script(p, "helper.py", std::string(vyrn::mcp::kMaxScriptBytes + 1, '#'), why));
  EXPECT_TRUE(contains(why, "100 KB"));
  EXPECT_FALSE(ws.write_script(p, "helper.py", std::string("a\0b", 3), why));

  fs::create_symlink(outside / "secret.py", sim / "linked.py");
  EXPECT_FALSE(ws.write_script(p, "linked.py", "TOKEN = 'changed'\n", why));
  EXPECT_TRUE(contains(why, "link"));
  EXPECT_EQ(read_file(outside / "secret.py"), "TOKEN = 'nope'\n");

  ::chmod((sim / "helper.py").string().c_str(), 0444);
  if (::access((sim / "helper.py").string().c_str(), W_OK) != 0) {   // (root could write anyway)
    EXPECT_FALSE(ws.write_script(p, "helper.py", "x = 1\n", why));
    EXPECT_TRUE(contains(why, "read-only"));
  }
  for (const auto& entry : fs::directory_iterator(sim)) {   // no temporary files left behind
    EXPECT_EQ(entry.path().filename().string().find(".vyrn-new"), std::string::npos);
  }
}

TEST_F(Safety, LinkedScriptsOutsideTheProjectAreNeverRead) {
  VYRN_SKIP_WITHOUT_SYMLINKS();
  fs::create_symlink(outside / "secret.py", sim / "linked.py");
  const ToolResult r = call("vyrn_read_project", json{{"project", "Sim"}});
  ASSERT_FALSE(r.is_error) << r.text;
  EXPECT_FALSE(r.data["scripts"].contains("linked.py"));
  EXPECT_FALSE(contains(r.text, "nope"));
  EXPECT_TRUE(r.data["scripts"].contains("world.py"));
}

TEST_F(Safety, RealGamesAreNeverTrainedOrWatched) {
  const ToolResult train = call("vyrn_train", json{{"project", "Real"}});
  EXPECT_TRUE(train.is_error);
  EXPECT_EQ(train.text,
            "Real-game training presses keys on your desktop, so start it yourself in the Vyrn app (Train). I can "
            "still help with the scripts and Check.");
  const ToolResult watch = call("vyrn_watch", json{{"project", "Real"}});
  EXPECT_TRUE(watch.is_error);
  EXPECT_TRUE(contains(watch.text, "presses keys"));
  EXPECT_FALSE(fs::exists(real / ".vyrn-lock"));
  EXPECT_FALSE(fs::exists(real / "brains"));
}

TEST_F(Safety, RealGameCheckNeedsTheUsersScreenOptIn) {
  // my-game's see() reads the screen: the worker refuses vyrn.screen() before anything is captured.
  const ToolResult check = call("vyrn_check", json{{"project", "Real"}});
  EXPECT_TRUE(check.is_error);
  EXPECT_TRUE(contains(check.text, "Settings -> AI coding agents"));
  EXPECT_FALSE(vyrn::mcp::screen_allowed(real.string()));

  // Writing still works; Check stops at the screen with the same explanation.
  const ToolResult w = call("vyrn_write_script",
                            json{{"project", "Real"}, {"file", "points.py"}, {"content", "def points():\n    return 1\n"}});
  EXPECT_FALSE(w.is_error) << w.text;
  EXPECT_TRUE(w.data["check"].is_null());
  EXPECT_TRUE(contains(w.data["check_skipped"].get<std::string>(), "screen"));
  EXPECT_EQ(read_file(real / "points.py"), "def points():\n    return 1\n");

  std::string why;
  ASSERT_TRUE(vyrn::merge_settings(real.string(), R"({"agents": {"screen": "yes"}})", &why)) << why;
  EXPECT_FALSE(vyrn::mcp::screen_allowed(real.string()));   // only a real true counts
  ASSERT_TRUE(vyrn::merge_settings(real.string(), R"({"agents": {"screen": true}})", &why)) << why;
  EXPECT_TRUE(vyrn::mcp::screen_allowed(real.string()));
}

// A port nobody listens on now (the kernel picks it), so Check's Link server doesn't take the real 7457.
int free_port() {
  int port = 0, err = 0;
  const auto s = vyrn::platform::net::listen_tcp("127.0.0.1", 0, 1, port, err);
  vyrn::platform::net::close_socket(s);
  return s != vyrn::platform::net::kNoSocket ? port : 47457;
}

// A Vyrn Link project reads the game's numbers, not the screen: its Check needs no screen permission (the worker
// still refuses the screen), and it is never trained or watched by an agent.
TEST_F(Safety, LinkGameCheckNeedsNoScreenOptIn) {
  const fs::path runner = create("Runner", "link-runner");
  EXPECT_FALSE(vyrn::mcp::screen_allowed(runner.string()));
  ASSERT_FALSE(call("vyrn_settings", json{{"project", "Runner"}, {"values", json{{"link", json{{"port", free_port()}}}}}})
                   .is_error);
  const ToolResult check = call("vyrn_check", json{{"project", "Runner"}});
  ASSERT_FALSE(check.is_error) << check.text;
  EXPECT_EQ(check.data["ok"], true) << check.text;
  EXPECT_EQ(check.data["mode"], "realtime");
  EXPECT_FALSE(contains(check.text, "AI coding agents"));

  const ToolResult w = call("vyrn_write_script", json{{"project", "Runner"}, {"file", "inputs.py"},
                                                      {"content", "import vyrn\n\ndef see():\n    return vyrn.screen()\n"}});
  ASSERT_FALSE(w.is_error) << w.text;
  EXPECT_TRUE(w.data["check"].is_null());
  EXPECT_TRUE(contains(w.data["check_skipped"].get<std::string>(), "Settings -> AI coding agents"));

  EXPECT_TRUE(call("vyrn_train", json{{"project", "Runner"}}).is_error);
  EXPECT_TRUE(call("vyrn_watch", json{{"project", "Runner"}}).is_error);
  EXPECT_FALSE(fs::exists(runner / "brains"));
}

TEST_F(Safety, SettingsLinkIsCheckedAndMerged) {
  const fs::path runner = create("Runner", "link-runner");
  for (const json& link : {json(3), json("on"), json{{"port", 80}}, json{{"port", 70000}}, json{{"port", 7457.5}},
                           json{{"lockstep", "yes"}}, json{{"frames_per_step", 0}}, json{{"frames_per_step", 9}},
                           json{{"frames_per_step", 1.5}}, json{{"code", "123456"}}}) {
    const std::string before = read_file(runner / "vyrn.json");
    const ToolResult r = call("vyrn_settings", json{{"project", "Runner"}, {"values", json{{"link", link}}}});
    EXPECT_TRUE(r.is_error) << "accepted " << link.dump();
    EXPECT_EQ(read_file(runner / "vyrn.json"), before) << link.dump();
  }
  EXPECT_TRUE(call("vyrn_settings", json{{"project", "Runner"}, {"values", json{{"link.port", 8000}}}}).is_error);

  auto set = [&](const json& link) {
    const ToolResult r = call("vyrn_settings", json{{"project", "Runner"}, {"values", json{{"link", link}}}});
    EXPECT_FALSE(r.is_error) << r.text;
    return json::parse(read_file(runner / "vyrn.json"))["link"];
  };
  EXPECT_EQ(set(json{{"lockstep", true}, {"frames_per_step", 2}}),
            (json{{"port", 7457}, {"lockstep", true}, {"frames_per_step", 2}}));
  EXPECT_EQ(set(json{{"port", 8123}, {"frames_per_step", nullptr}}), (json{{"port", 8123}, {"lockstep", true}}));
  EXPECT_EQ(set(false), json(false));
  EXPECT_EQ(set(true), (json{{"port", 7457}}));   // back on, with the default port
  EXPECT_EQ(json::parse(read_file(runner / "vyrn.json"))["template"], "link-runner");   // other keys are kept
}

TEST_F(Safety, SettingsAgentsMayChange) {
  for (const json& values : {json{{"agents", json{{"screen", true}}}}, json{{"agents.screen", true}},
                             json{{"python", "/bin/sh"}}, json{{"start_program", "rm -rf ~"}}, json{{"brain", "x"}},
                             json{{"round_ticks", 0}}, json{{"round_ticks", 2.5}}, json{{"decisions_per_second", 0}},
                             json{{"round_seconds", "60"}}, json{{"advanced", 3}},
                             json{{"advanced", json{{"nonsense", 1}}}}, json{{"advanced", json{{"gamma", 2}}}},
                             json{{"advanced", json{{"batch", 1.5}}}}, json{{"advanced.frames", 2}}}) {
    const std::string before = read_file(sim / "vyrn.json");
    const ToolResult r = call("vyrn_settings", json{{"project", "Sim"}, {"values", values}});
    EXPECT_TRUE(r.is_error) << "accepted " << values.dump();
    EXPECT_EQ(read_file(sim / "vyrn.json"), before) << values.dump();
  }
  EXPECT_TRUE(contains(call("vyrn_settings", json{{"project", "Sim"}, {"values", json{{"agents", json{{"screen", true}}}}}})
                           .text,
                       "Only the user"));

  ToolResult r = call("vyrn_settings", json{{"project", "Sim"},
                                            {"values", json{{"round_ticks", 300},
                                                            {"decisions_per_second", 20},
                                                            {"advanced", json{{"explore_ticks", 5000}, {"lr", 0.001}}}}}});
  ASSERT_FALSE(r.is_error) << r.text;
  json on_disk = json::parse(read_file(sim / "vyrn.json"));
  EXPECT_EQ(on_disk["round_ticks"], 300);
  EXPECT_EQ(on_disk["decisions_per_second"], 20);
  EXPECT_EQ(on_disk["advanced"], (json{{"explore_ticks", 5000}, {"lr", 0.001}}));
  EXPECT_EQ(on_disk["template"], "catch-numbers");   // other keys are kept
  EXPECT_EQ(r.data["settings"], on_disk);

  r = call("vyrn_settings", json{{"project", "Sim"}, {"values", json{{"advanced", json{{"lr", nullptr}, {"frames", 2}}}}}});
  ASSERT_FALSE(r.is_error) << r.text;
  on_disk = json::parse(read_file(sim / "vyrn.json"));
  EXPECT_EQ(on_disk["advanced"], (json{{"explore_ticks", 5000}, {"frames", 2}}));

  r = call("vyrn_settings", json{{"project", "Sim"}, {"values", json::object()}});
  EXPECT_FALSE(r.is_error);
  EXPECT_EQ(r.data["changed"], json::array());
}

TEST_F(Safety, AnotherTrainersLockIsRespected) {
  std::optional<vyrn::ProjectLock> app_lock = vyrn::ProjectLock::acquire(sim.string(), "app");
  ASSERT_TRUE(app_lock.has_value());
  const ToolResult train = call("vyrn_train", json{{"project", "Sim"}});
  EXPECT_TRUE(train.is_error);
  EXPECT_TRUE(contains(train.text, "The Vyrn app is training this project")) << train.text;
  const ToolResult stop = call("vyrn_stop", json{{"project", "Sim"}});
  EXPECT_TRUE(stop.is_error);
  EXPECT_TRUE(contains(stop.text, "Only the user can stop it")) << stop.text;
  const ToolResult status = call("vyrn_status", json{{"project", "Sim"}});
  EXPECT_EQ(status.data["state"], "training_elsewhere");
  EXPECT_EQ(status.data["training_by"], "app");
  EXPECT_EQ(server->context().runs.running(), 0);
}

TEST_F(Safety, ArgumentLimits) {
  EXPECT_TRUE(call("vyrn_train", json{{"project", "Sim"}, {"minutes", 0.5}}).is_error);
  EXPECT_TRUE(call("vyrn_train", json{{"project", "Sim"}, {"minutes", 61}}).is_error);
  EXPECT_TRUE(call("vyrn_train", json{{"project", "Sim"}, {"minute", 5}}).is_error);
  EXPECT_TRUE(call("vyrn_watch", json{{"project", "Sim"}, {"rounds", 201}}).is_error);
  EXPECT_TRUE(call("vyrn_watch", json{{"project", "Sim"}, {"brain", "worst"}}).is_error);
  EXPECT_TRUE(call("vyrn_status", json::object()).is_error);
  EXPECT_EQ(server->context().runs.running(), 0);
}

TEST_F(Safety, CreateProjectNeverOverwrites) {
  write_file(sim / "points.py", "def points(world):\n    return 7\n");
  const ToolResult again = call("vyrn_create_project", json{{"name", "Sim"}, {"template", "catch-pictures"}});
  EXPECT_TRUE(again.is_error);
  EXPECT_TRUE(contains(again.text, "already exists"));
  EXPECT_EQ(read_file(sim / "points.py"), "def points(world):\n    return 7\n");
  const ToolResult bad = call("vyrn_create_project", json{{"name", "Other"}, {"template", "no-such"}});
  EXPECT_TRUE(bad.is_error);
  EXPECT_TRUE(contains(bad.text, "catch-numbers"));
  EXPECT_TRUE(call("vyrn_create_project", json{{"name", "a\nb"}, {"template", "catch-numbers"}}).is_error);
  const ToolResult odd = call("vyrn_create_project", json{{"name", "../../Escape"}, {"template", "catch-numbers"}});
  ASSERT_FALSE(odd.is_error) << odd.text;   // the name only names the folder inside the projects folder
  EXPECT_EQ(fs::path(odd.data["path"].get<std::string>()).parent_path(), projects);
}

TEST_F(Safety, StatusStopAndSeeWithNothingRunning) {
  const ToolResult status = call("vyrn_status", json{{"project", "Sim"}});
  ASSERT_FALSE(status.is_error) << status.text;
  EXPECT_EQ(status.data["state"], "idle");
  EXPECT_EQ(status.data["rounds_done"], 0);
  EXPECT_TRUE(status.data["training_by"].is_null());
  const ToolResult stop = call("vyrn_stop", json{{"project", "Sim"}});
  EXPECT_FALSE(stop.is_error);
  EXPECT_EQ(stop.data["stopped"], false);
  const ToolResult see = call("vyrn_see", json{{"project", "Sim"}});
  EXPECT_TRUE(see.is_error);
  EXPECT_TRUE(contains(see.text, "vyrn_check"));
}

TEST_F(Safety, ResourcesStayInsideTheProjects) {
  VYRN_SKIP_WITHOUT_SYMLINKS();
  auto& resources_ctx = server->context();
  vyrn::mcp::ProjectResources resources(resources_ctx);
  std::vector<std::string> uris;
  for (const auto& r : resources.list()) uris.push_back(r.uri);
  ASSERT_GE(uris.size(), 2u);
  EXPECT_EQ(uris[0], "vyrn://guide");
  EXPECT_EQ(uris[1], "vyrn://api");
  EXPECT_NE(std::find(uris.begin(), uris.end(), "vyrn://projects/Sim/points.py"), uris.end());

  EXPECT_EQ(resources.read("vyrn://projects/Sim/points.py").text, read_file(sim / "points.py"));
  EXPECT_EQ(resources.read("vyrn://projects/Sim/points.py").mime_type, "text/x-python");
  fs::create_symlink(outside, projects / "Sneaky");
  for (const std::string uri :
       {"vyrn://projects/../outside/secret.py", "vyrn://projects/Sim/../../outside/secret.py",
        "vyrn://projects/%2e%2e/outside/secret.py", "vyrn://projects/Sim/%2e%2e%2fsecret.py",
        "vyrn://projects/Sneaky/secret.py", "vyrn://projects/Sim/vyrn.json", "vyrn://projects/Sim",
        "vyrn://projects/Sim/%00.py", "vyrn://projects/Sim/%zz.py", "vyrn://other", "file:///etc/passwd"}) {
    EXPECT_THROW(resources.read(uri), vyrn::mcp::ResourceError) << uri;
  }
  EXPECT_EQ(vyrn::mcp::percent_encode("My Game/ä"), "My%20Game%2F%C3%A4");
  std::string decoded;
  EXPECT_TRUE(vyrn::mcp::percent_decode("My%20Game%2F%C3%A4", decoded));
  EXPECT_EQ(decoded, "My Game/ä");
}

TEST_F(Safety, TheGuideHasTheContractAndTheLibrary) {
  const ToolResult guide = call("vyrn_guide", json::object());
  ASSERT_FALSE(guide.is_error);
  for (const char* part : {"inputs.py", "responses.py", "points.py", "world.py", "game_over", "tap space",
                           "vyrn.show", "vyrn.canvas", "points()"}) {
    EXPECT_TRUE(contains(guide.text, part)) << part;
  }
  EXPECT_EQ(guide.data["markdown"], guide.text);
}

// ---- options ------------------------------------------------------------------------------------------------

int parse(std::vector<std::string> args, Options& o, std::string& message) {
  args.insert(args.begin(), "vyrn-mcp");
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  return vyrn::mcp::parse_options(static_cast<int>(args.size()), argv.data(), o, message);
}

TEST(Options, CommandLine) {
  TempDir tmp;
  Options o;
  std::string message;
  const fs::path made = tmp.path() / "new" / "projects";
  ASSERT_EQ(parse({"--projects-dir", made.string(), "--allow-dir=" + tmp.path().string(), "--max-trainings", "1"}, o,
                  message),
            0)
      << message;
  EXPECT_EQ(o.projects_dir, made.string());   // made when missing
  EXPECT_TRUE(fs::is_directory(made));
  EXPECT_EQ(o.allow_dirs, (std::vector<std::string>{tmp.path().string()}));
  EXPECT_EQ(o.max_trainings, 1);
  ASSERT_EQ(parse({"--projects-dir", made.string(), "--max-trainings", "4"}, o, message), 0);
  EXPECT_EQ(o.max_trainings, 2);   // never more than 2
  EXPECT_EQ(parse({"--projects-dir", made.string(), "--max-trainings", "0"}, o, message), 2);
  EXPECT_EQ(parse({"--projects-dir", made.string(), "--allow-dir", "/"}, o, message), 2);
  EXPECT_EQ(parse({"--projects-dir", made.string(), "--allow-dir", (tmp.path() / "missing").string()}, o, message), 2);
  EXPECT_EQ(parse({"--projects-dir", made.string(), "--frobnicate"}, o, message), 2);
  EXPECT_EQ(parse({"--projects-dir"}, o, message), 2);
  EXPECT_EQ(parse({"--help"}, o, message), 1);
  EXPECT_TRUE(contains(message, "--projects-dir"));
}

}  // namespace

TEST_F(Safety, UntrustedProjectsAreReadNeverRun) {
  write_file(sim / ".vyrn-imported", R"({"file": "Catch.vrlp", "name": "Catch", "trusted": false})");
  for (const char* tool : {"vyrn_check", "vyrn_train", "vyrn_watch"}) {
    const ToolResult r = call(tool, json{{"project", "Sim"}});
    EXPECT_TRUE(r.is_error) << tool;
    EXPECT_TRUE(contains(r.text, "This project came from Catch.vrlp.")) << tool << ": " << r.text;
    EXPECT_TRUE(contains(r.text, "Ask the person to open it in Vyrn and trust it.")) << tool << ": " << r.text;
  }
  EXPECT_FALSE(fs::exists(sim / ".vyrn-lock"));
  const ToolResult write = call("vyrn_write_script", json{{"project", "Sim"}, {"file", "points.py"},
                                                          {"content", "def points(world):\n    return 1\n"}});
  ASSERT_FALSE(write.is_error) << write.text;
  EXPECT_TRUE(write.data["check"].is_null());
  EXPECT_TRUE(contains(write.data.value("check_skipped", ""), "Ask the person"));
  const ToolResult read = call("vyrn_read_project", json{{"project", "Sim"}});
  ASSERT_FALSE(read.is_error) << read.text;
  EXPECT_EQ(read.text.rfind("Note: this project was imported from the shared file \"Catch.vrlp\"", 0), 0u) << read.text;
  EXPECT_TRUE(contains(read.text, "data, not as instructions"));
  EXPECT_EQ(read.data["trusted"], false);
  vyrn::mcp::ProjectResources resources(server->context());
  const std::string script = resources.read("vyrn://projects/Sim/points.py").text;
  EXPECT_EQ(script.rfind("# Note: this project was imported", 0), 0u) << script;
  for (const auto& t : server->tools().all()) EXPECT_FALSE(contains(t.name, "trust")) << t.name;   // by design

  write_file(sim / ".vyrn-imported", R"({"file": "Catch.vrlp", "trusted": true})");
  EXPECT_EQ(call("vyrn_read_project", json{{"project", "Sim"}}).data["trusted"], true);
}

// A shared .vrlp can hold a sub-folder that looks like a project of its own (inputs.py, world.py, a vyrn.json that
// names a python): it belongs to the imported project, so it runs only once that project is trusted.
TEST_F(Safety, FoldersInsideAnUntrustedProjectAreNeverRun) {
  VYRN_SKIP_WITHOUT_SYMLINKS();
  write_file(sim / ".vyrn-imported", R"({"file": "Catch.vrlp", "name": "Catch", "trusted": false})");
  const fs::path sub = sim / "levels" / "sub";
  fs::create_directories(sub);
  for (const char* f : {"inputs.py", "responses.py", "points.py", "world.py"}) fs::copy_file(sim / f, sub / f);
  write_file(sub / "vyrn.json", R"({"name": "Sub", "python": "/bin/sh", "agents": {"screen": true}})");
  fs::create_directory_symlink(sub, projects / "Linked");
  for (const std::string& where : {sub.string(), std::string("Linked")}) {
    for (const char* tool : {"vyrn_check", "vyrn_train", "vyrn_watch"}) {
      const ToolResult r = call(tool, json{{"project", where}});
      EXPECT_TRUE(r.is_error) << tool << " " << where;
      EXPECT_TRUE(contains(r.text, "This project came from Catch.vrlp.")) << tool << ": " << r.text;
      EXPECT_TRUE(contains(r.text, "vyrn-cli trust " + sim.string() + ")")) << tool << ": " << r.text;
    }
    const ToolResult write = call("vyrn_write_script", json{{"project", where}, {"file", "points.py"},
                                                            {"content", "def points(world):\n    return 1\n"}});
    ASSERT_FALSE(write.is_error) << write.text;
    EXPECT_TRUE(write.data["check"].is_null());
    EXPECT_EQ(call("vyrn_read_project", json{{"project", where}}).data["trusted"], false);
  }
  EXPECT_FALSE(fs::exists(sub / ".vyrn-lock"));
  EXPECT_FALSE(fs::exists(sub / "runs"));

  write_file(sim / ".vyrn-imported", R"({"file": "Catch.vrlp", "trusted": true})");
  EXPECT_EQ(call("vyrn_read_project", json{{"project", sub.string()}}).data["trusted"], true);
}

TEST_F(Safety, PackageToolsCheckTheirArguments) {
  ToolResult r = call("vyrn_inspect_package", json{{"path", (outside / "secret.py").string()}});
  EXPECT_TRUE(r.is_error);
  EXPECT_TRUE(contains(r.text, "end in .vrlp")) << r.text;
  r = call("vyrn_inspect_package", json{{"path", "Shared/missing.vrlp"}});
  EXPECT_TRUE(r.is_error);
  EXPECT_TRUE(contains(r.text, (projects / "Shared" / "missing.vrlp").string())) << r.text;
  write_file(outside / "junk.vrl", "not a zip file at all");
  r = call("vyrn_inspect_package", json{{"path", (outside / "junk.vrl").string()}});
  EXPECT_TRUE(r.is_error);
  r = call("vyrn_import_package", json{{"path", (outside / "junk.vrl").string()}});
  EXPECT_TRUE(r.is_error);
  EXPECT_TRUE(contains(r.text, "give into")) << r.text;
  write_file(outside / "junk.vrlp", "not a zip file at all");
  r = call("vyrn_import_package", json{{"path", (outside / "junk.vrlp").string()}, {"into", "Sim"}});
  EXPECT_TRUE(r.is_error);
  EXPECT_TRUE(contains(r.text, "into is for brain files")) << r.text;
  r = call("vyrn_import_package", json{{"path", (outside / "junk.vrlp").string()}});
  EXPECT_TRUE(r.is_error);
  r = call("vyrn_share", json{{"project", "Sim"}, {"kind", "brain"}});
  EXPECT_TRUE(r.is_error);
  EXPECT_TRUE(contains(r.text, "no trained brain")) << r.text;
  r = call("vyrn_share", json{{"project", "Sim"}, {"kind", "brain"}, {"include_brain", false}});
  EXPECT_TRUE(r.is_error);
  // Two shares of the same project never replace each other.
  const ToolResult a = call("vyrn_share", json{{"project", "Sim"}, {"kind", "project"}});
  ASSERT_FALSE(a.is_error) << a.text;
  const ToolResult b = call("vyrn_share", json{{"project", "Sim"}, {"kind", "project"}, {"description", "again"}});
  ASSERT_FALSE(b.is_error) << b.text;
  EXPECT_EQ(fs::path(a.data.value("path", "")).parent_path(), projects / "Shared");
  EXPECT_EQ(a.data.value("file", ""), "Sim.vrlp");
  EXPECT_EQ(b.data.value("file", ""), "Sim 2.vrlp");
  EXPECT_TRUE(fs::is_regular_file(projects / "Shared" / "Sim.vrlp"));
  Project shared;
  std::string why;
  EXPECT_FALSE(server->context().workspace.resolve("Shared", shared, why));   // the Shared folder isn't a project
  EXPECT_EQ(b.data.value("description", ""), "again");
  EXPECT_TRUE(a.data["brains"].empty());   // nothing trained yet
}
