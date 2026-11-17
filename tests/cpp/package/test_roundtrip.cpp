// Export -> inspect -> import round trips for the three networks (weights bit-identical, equal Q-values), and a
// whole project with its brain (docs/dev/package.md sections 4-6).
#include "helpers.h"
#include "vyrn/package.h"
#include "vyrn/project.h"
#include "platform_test.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace pkg_test;
using json = nlohmann::json;

namespace {

const char* kId = "20261113-181009";

std::string brain_dir(const std::string& project, const std::string& id) {
  return (fs::path(project) / "brains" / id).string();
}

const char* net_name(Net net) {
  switch (net) {
    case Net::MLP: return "mlp";
    case Net::SmallCNN: return "small_cnn";
    case Net::DER: return "der";
  }
  return "?";
}

// A data folder with Vyrn's own agent instructions in its templates (find_data_dir needs the worker script).
std::string make_data_dir(const std::string& root) {
  const fs::path data = fs::path(root) / "data";
  write_file(data / "python" / "vyrn_worker.py", "# test\n");
  write_file(data / "templates" / "catch-numbers" / "AGENTS.md", "Vyrn's own AGENTS.md\n");
  write_file(data / "templates" / "catch-numbers" / "CLAUDE.md", "Vyrn's own CLAUDE.md\n");
  write_file(data / "templates" / "my-game" / "AGENTS.md", "my-game AGENTS.md\n");
  write_file(data / "templates" / "my-game" / "CLAUDE.md", "my-game CLAUDE.md\n");
  return data.string();
}

}  // namespace

class PackageRoundTrip : public ::testing::TestWithParam<Net> {};

TEST_P(PackageRoundTrip, BrainFileKeepsWeightsBitForBit) {
  TempDir dir;
  const std::string src = make_project(dir.path(), "src", "Catch (numbers)");
  add_brain(src, kId, GetParam(), 7);
  vyrn::ExportOptions options;
  options.author = "Ada";
  options.description = "Line one\nLine two";
  const std::string out = dir / "Catch.vrl";
  const vyrn::PackageInfo written = vyrn::export_brain(src, out, options);
  EXPECT_FALSE(present(out + ".part"));

  const vyrn::PackageInfo info = vyrn::inspect_package(out);
  EXPECT_EQ(info.kind, "brain");
  EXPECT_EQ(info.name, "Catch (numbers)");
  EXPECT_EQ(info.author, "Ada");
  EXPECT_EQ(info.description, "Line one\nLine two");
  EXPECT_EQ(info.template_id, "catch-numbers");
  EXPECT_EQ(info.mode, "sim");
  EXPECT_EQ(info.created.size(), 20u);
  EXPECT_EQ(info.file_size, written.file_size);
  ASSERT_EQ(info.brains.size(), 1u);
  const vyrn::BrainSummary& b = info.brains[0];
  EXPECT_EQ(b.id, kId);
  EXPECT_EQ(b.which, "best");
  EXPECT_EQ(b.net, net_name(GetParam()));
  EXPECT_EQ(b.choices, (std::vector<std::string>{"left", "right", "stay"}));
  EXPECT_TRUE(b.has_best);
  EXPECT_DOUBLE_EQ(b.best_mean20, 0.75);
  EXPECT_EQ(b.ticks, 6000);
  EXPECT_EQ(b.rounds, 66);
  EXPECT_DOUBLE_EQ(b.time_trained_s, 12.5);
  if (GetParam() == Net::MLP) {
    EXPECT_EQ(b.input, "numbers");
    EXPECT_EQ(b.input_shape, (std::vector<int>{3}));
    EXPECT_EQ(b.input_names, (std::vector<std::string>{"x", "y", "c"}));
  } else {
    EXPECT_EQ(b.input, "picture");
    EXPECT_EQ(b.input_shape.size(), 2u);
  }

  const std::string dst = make_project(dir.path(), "dst", "Mine");
  const vyrn::ImportResult r = vyrn::import_brain(out, dst, spec_json(GetParam()));
  EXPECT_EQ(r.brain_id, kId);
  EXPECT_EQ(fs::path(r.project), fs::absolute(dst));
  EXPECT_EQ(saved_weights(brain_dir(src, kId), "best"), saved_weights(brain_dir(dst, kId), "best"));
  EXPECT_EQ(saved_weights(brain_dir(src, kId), "best"), saved_weights(brain_dir(dst, kId), "latest"));
  EXPECT_EQ(saved_q_values(brain_dir(src, kId), "best"), saved_q_values(brain_dir(dst, kId), "best"));
  EXPECT_EQ(saved_q_values(brain_dir(src, kId), "best"), saved_q_values(brain_dir(dst, kId), "latest"));
  EXPECT_EQ(saved_updates(brain_dir(dst, kId), "latest"), 1);

  const json meta = json::parse(saved_meta(brain_dir(dst, kId), "best"));
  const json source = json::parse(saved_meta(brain_dir(src, kId), "best"));
  EXPECT_EQ(meta["brain"], kId);
  EXPECT_EQ(meta["imported"]["file"], "Catch.vrl");
  EXPECT_EQ(meta["imported"]["author"], "Ada");
  EXPECT_EQ(meta["shape"], source["shape"]);
  EXPECT_EQ(meta["normalizer"], source["normalizer"]);
  EXPECT_EQ(meta["counters"]["ticks"], 6000);
  EXPECT_EQ(meta["best_mean20"], 0.75);
  EXPECT_EQ(meta["effects"], source["effects"]);
  EXPECT_FALSE(meta.contains("history"));
  EXPECT_NE(vyrn::read_settings_text(dst).find(std::string("\"brain\": \"") + kId + "\""), std::string::npos);
  const auto brains = vyrn::list_brains(dst);
  ASSERT_EQ(brains.size(), 1u);
  EXPECT_TRUE(brains[0].active);
  EXPECT_TRUE(brains[0].has_best);
}

TEST_P(PackageRoundTrip, LatestIsTheOtherSave) {
  TempDir dir;
  const std::string src = make_project(dir.path(), "src", "Catch");
  add_brain(src, kId, GetParam(), 3);
  vyrn::ExportOptions options;
  options.which = "latest";
  const std::string out = dir / "latest.vrl";
  const vyrn::PackageInfo info = vyrn::export_brain(src, out, options);
  EXPECT_EQ(info.brains.at(0).which, "latest");
  EXPECT_EQ(info.brains.at(0).ticks, 7000);
  const std::string dst = make_project(dir.path(), "dst", "Mine");
  vyrn::import_brain(out, dst, spec_json(GetParam()));
  EXPECT_EQ(saved_weights(brain_dir(src, kId), "latest"), saved_weights(brain_dir(dst, kId), "best"));
  EXPECT_NE(saved_weights(brain_dir(src, kId), "best"), saved_weights(brain_dir(dst, kId), "best"));
  EXPECT_EQ(saved_updates(brain_dir(dst, kId), "best"), 2);
}

INSTANTIATE_TEST_SUITE_P(Nets, PackageRoundTrip, ::testing::Values(Net::MLP, Net::SmallCNN, Net::DER),
                         [](const auto& info) { return std::string(net_name(info.param)); });

TEST(PackageProject, RoundTripWithBrain) {
  VYRN_SKIP_WITHOUT_SYMLINKS();
  TempDir dir;
  const ScopedEnv data("VYRN_DATA", make_data_dir(dir.path()));
  const std::string src = make_project(dir.path(), "src", "Catch (numbers)");
  add_brain(src, kId, Net::MLP, 5);
  write_file(fs::path(src) / "pictures" / "game_over.png", tiny_png(4, 4));
  write_file(fs::path(src) / "notes.md", "# Notes\n");
  write_file(fs::path(src) / "LICENSE", "MIT\n");
  write_file(fs::path(src) / "a" / "b" / "c" / "d" / "e" / "f" / "g" / "h" / "deep.txt", "8 deep\n");
  write_file(fs::path(src) / "a" / "b" / "c" / "d" / "e" / "f" / "g" / "h" / "i" / "too-deep.txt", "9 deep\n");
  write_file(fs::path(src) / "game.exe", "MZ");
  write_file(fs::path(src) / "__pycache__" / "inputs.cpython-313.pyc", "pyc");
  write_file(fs::path(src) / "helper.pyc", "pyc");
  write_file(fs::path(src) / ".secret.py", "TOKEN = 1\n");
  write_file(fs::path(src) / ".vyrn-lock", "{}");
  write_file(fs::path(src) / "runs" / "20261113-100000" / "rounds.csv", "round,mean20\n1,0.5\n");
  write_file(fs::path(src) / "AGENTS.md", "their agents file\n");
  write_file(fs::path(src) / "docs" / "Claude.md", "their claude file\n");
  write_file(fs::path(src) / "GEMINI.MD", "their gemini file\n");
  write_file(fs::path(src) / "CLAUDE.local.md", "their local file\n");
  fs::create_symlink("/etc/hostname", fs::path(src) / "link.txt");
  vyrn::merge_settings(src,
                       R"({"python": "/home/ada/venv/bin/python", "start_program": "chromium chrome://dino",
                           "agents": {"screen": true}, "secret_key": "x", "decisions_per_second": 15,
                           "link": {"port": 7457}, "advanced": {"frames": 2}})",
                       nullptr);

  vyrn::ExportOptions options;
  options.author = "Ada";
  const std::string preview = tiny_png(8, 8);
  options.preview_png.assign(preview.begin(), preview.end());
  const std::string out = dir / "Catch.vrlp";
  const vyrn::PackageInfo info = vyrn::export_project(src, out, options);
  std::vector<std::string> paths;
  for (const auto& f : info.files) paths.push_back(f.path);
  EXPECT_EQ(paths, (std::vector<std::string>{"LICENSE", "a/b/c/d/e/f/g/h/deep.txt", "inputs.py", "notes.md",
                                             "pictures/game_over.png", "points.py", "responses.py", "vyrn.json",
                                             "world.py"}));
  ASSERT_EQ(info.brains.size(), 1u);
  EXPECT_EQ(info.brains[0].id, kId);
  EXPECT_EQ(info.start_program, "chromium chrome://dino");
  EXPECT_TRUE(info.uses_link);
  EXPECT_EQ(info.mode, "sim");
  EXPECT_FALSE(info.preview_png.empty());

  // The packaged vyrn.json keeps only the allow-list.
  const std::string bytes = read_file(out);
  const json packaged = json::parse(entry_of(bytes, "project/vyrn.json"));
  EXPECT_EQ(packaged, json::parse(R"J({"name": "Catch (numbers)", "template": "catch-numbers", "brain": ")J" +
                                  std::string(kId) +
                                  R"J(", "decisions_per_second": 15, "link": {"port": 7457}, "advanced": {"frames": 2}})J"));
  EXPECT_EQ(bytes.substr(30, 34), "mimetypeapplication/x-vyrn-project");
  const auto entries = unzip_all(bytes);
  EXPECT_EQ(entries[1].first, "vyrn-project.json");
  EXPECT_EQ(entries[2].first, std::string("brains/") + kId + ".vrl");
  EXPECT_EQ(entries[3].first, "preview.png");

  const std::string root = dir / "projects";
  const vyrn::ImportResult r = vyrn::import_project(out, root);
  EXPECT_EQ(fs::path(r.project), fs::path(root) / "Catch-numbers");
  EXPECT_EQ(r.brain_id, kId);
  EXPECT_EQ(read_file(fs::path(r.project) / "notes.md"), "# Notes\n");
  EXPECT_EQ(read_file(fs::path(r.project) / "pictures" / "game_over.png"), tiny_png(4, 4));
  EXPECT_TRUE(present(fs::path(r.project) / "a/b/c/d/e/f/g/h/deep.txt"));
  EXPECT_FALSE(present(fs::path(r.project) / "game.exe"));
  EXPECT_FALSE(present(fs::path(r.project) / "link.txt"));
  EXPECT_FALSE(present(fs::path(r.project) / ".secret.py"));
  EXPECT_FALSE(present(fs::path(r.project) / "runs"));
  EXPECT_FALSE(present(fs::path(r.project) / "docs"));
  EXPECT_FALSE(present(fs::path(r.project) / "GEMINI.MD"));
  EXPECT_FALSE(present(fs::path(r.project) / "CLAUDE.local.md"));
  EXPECT_EQ(read_file(fs::path(r.project) / "AGENTS.md"), "Vyrn's own AGENTS.md\n");
  EXPECT_EQ(read_file(fs::path(r.project) / "CLAUDE.md"), "Vyrn's own CLAUDE.md\n");
  const json imported = json::parse(vyrn::read_settings_text(r.project));
  EXPECT_EQ(imported["name"], "Catch (numbers)");
  EXPECT_EQ(imported["brain"], kId);
  EXPECT_FALSE(imported.contains("python"));
  EXPECT_FALSE(imported.contains("start_program"));
  EXPECT_FALSE(imported.contains("agents"));
  EXPECT_EQ(saved_weights(brain_dir(src, kId), "best"), saved_weights(brain_dir(r.project, kId), "best"));
  EXPECT_FALSE(vyrn::is_trusted(r.project));
  const auto origin = vyrn::import_origin(r.project);
  ASSERT_TRUE(origin.has_value());
  EXPECT_EQ(origin->file, "Catch.vrlp");
  EXPECT_EQ(origin->author, "Ada");
  EXPECT_EQ(origin->start_program, "chromium chrome://dino");
  bool hint = false;
  for (const auto& note : r.notes) hint = hint || note.find("chromium chrome://dino") != std::string::npos;
  EXPECT_TRUE(hint);
  const vyrn::ProjectInfo project = vyrn::read_project(r.project);
  EXPECT_EQ(project.mode, "sim");
  EXPECT_EQ(vyrn::list_brains(r.project).at(0).id, kId);
  // Files are plain 0644 files, folders 0755.
  EXPECT_EQ(fs::status(fs::path(r.project) / "notes.md").permissions() & fs::perms::all,
            fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read);
  EXPECT_EQ(fs::status(fs::path(r.project) / "pictures").permissions() & fs::perms::all,
            fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                fs::perms::others_exec);
  // No staging folder is left behind.
  for (const auto& name : tree(root)) EXPECT_EQ(name.find(".import-"), std::string::npos) << name;

  // Exporting the imported project again works and doesn't carry the trust marker.
  const vyrn::PackageInfo again = vyrn::export_project(r.project, dir / "again.vrlp");
  for (const auto& f : again.files) EXPECT_NE(f.path, ".vyrn-imported");
}

TEST(PackageProject, WithoutBrain) {
  TempDir dir;
  const std::string src = make_project(dir.path(), "src", "No AI");
  add_brain(src, kId, Net::MLP);
  vyrn::ExportOptions options;
  options.include_brain = false;
  const std::string out = dir / "x.vrlp";
  const vyrn::PackageInfo info = vyrn::export_project(src, out, options);
  EXPECT_TRUE(info.brains.empty());
  EXPECT_FALSE(json::parse(entry_of(read_file(out), "project/vyrn.json")).contains("brain"));
  const vyrn::ImportResult r = vyrn::import_project(out, dir / "projects");
  EXPECT_EQ(r.brain_id, "");
  EXPECT_FALSE(json::parse(vyrn::read_settings_text(r.project)).contains("brain"));
  // A project that never trained is shared without a brain too.
  const std::string fresh = make_project(dir.path(), "fresh", "Fresh");
  EXPECT_TRUE(vyrn::export_project(fresh, dir / "fresh.vrlp").brains.empty());
}
