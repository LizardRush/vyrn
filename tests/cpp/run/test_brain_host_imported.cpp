// An imported brain (.vrl) keeps where it came from when it trains further: BrainHost::meta() copies "name",
// "imported" and "history" from the brain.json it loaded into every new save (docs/dev/package.md section 6).
#include "brain/checkpoint.h"
#include "brain/threads.h"
#include "run/brain_host.h"

#include <gtest/gtest.h>

#include "platform_test.h"

#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;
using nlohmann::json;
using namespace vyrn::run;
namespace brain = vyrn::brain;

namespace {

class TempDir {
 public:
  TempDir() {
    std::string pattern = (fs::temp_directory_path() / "vyrn-imported-XXXXXX").string();
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

ObsView numbers(float a, float b) {
  ObsView o;
  o.numbers = {a, b};
  return o;
}

// One round of 20 ticks that scores `last` on its last tick.
void play_round(BrainHost& host, int round, float last) {
  host.start_segment(numbers(0, static_cast<float>(round)));
  for (int t = 1; t <= 20; ++t) {
    const int a = host.act(0.5f, nullptr);
    host.record(a, t == 20 ? last : 0.0f);
    const ObsView o = numbers(static_cast<float>(t), static_cast<float>(round));
    if (t == 20) {
      host.end_segment(&o, true);
    } else {
      host.continue_segment(o);
    }
  }
}

}  // namespace

TEST(BrainHost, ImportedBrainKeepsItsNameImportInfoAndHistoryWhenTrainedFurther) {
  brain::init_torch_process();
  brain::set_torch_threads(1);
  TempDir tmp;
  const json spec_msg = {{"t", "spec"},
                         {"mode", "sim"},
                         {"obs", {{"kind", "numbers"}, {"shape", {2}}, {"names", {"a", "b"}}}},
                         {"choices", {"left", "right"}},
                         {"labels", {"left", "right"}},
                         {"effects", {"call left()", "call right()"}},
                         {"uses", {{"screen", false}, {"keys", false}}}};
  const brain::Spec spec = brain::parse_spec(spec_msg);
  brain::DqnConfig cfg = brain::default_config(spec, true);
  cfg.batch = 8;

  // A brain trained somewhere else. Its own saves have none of the keys an import adds.
  const BrainPlan first = plan_training(tmp.path(), "", false, spec, cfg.frames);
  ASSERT_FALSE(first.load);
  std::string brain_dir;
  brain::DqnConfig host_cfg;
  {
    BrainHost host(tmp.path(), spec, first, cfg, true, 3);
    for (int round = 0; round < 3; ++round) play_round(host, round, 1.0f);
    host.save_latest(Counters{60, 3, 1.5}, std::nullopt);
    brain_dir = host.dir();
    host_cfg = host.config();
  }
  brain::BrainMeta meta;
  ASSERT_TRUE(brain::read_checkpoint_meta(brain_dir, "latest", meta));
  EXPECT_FALSE(meta.j.contains("name"));
  EXPECT_FALSE(meta.j.contains("imported"));
  EXPECT_FALSE(meta.j.contains("history"));

  // Imported the way import_brain writes it: weights only, "best" and "latest", plus name, imported and history.
  const json name = "Ada's catcher";
  const json imported = {{"file", "catcher.vrl"}, {"author", "Ada"}, {"created", "2026-03-04T05:06:07Z"}};
  const json history = json::array({json::array({10, 0.5}), json::array({20, 1.25})});
  {
    brain::Dqn dqn(host_cfg, brain::net_config_for(first.shape));
    ASSERT_TRUE(brain::load_checkpoint(brain_dir, "latest", dqn, meta));
    meta.j.erase("dqn");
    meta.j.erase("checkpoint");
    meta.j["name"] = name;
    meta.j["imported"] = imported;
    meta.j["history"] = history;
    const auto weights = dqn.snapshot();
    brain::save_checkpoint(brain_dir, "best", *weights, meta);
    brain::save_checkpoint(brain_dir, "latest", *weights, meta);
  }

  // Train it further here: every new save (latest, and the best kept in RAM) keeps the three keys.
  const BrainPlan plan = plan_training(tmp.path(), first.id, false, spec, cfg.frames);
  ASSERT_TRUE(plan.load);
  EXPECT_EQ(plan.status, "resumed");
  EXPECT_EQ(plan.id, first.id);
  {
    BrainHost host(tmp.path(), spec, plan, cfg, true, 11);
    EXPECT_EQ(host.saved_counters().ticks, 60);
    int64_t ticks = 0;
    for (int round = 0; round < 5; ++round) {
      play_round(host, round, round % 2 ? 1.0f : -1.0f);
      ticks += 20;
      host.learn(ticks);
    }
    host.keep_best(Counters{160, 8, 4.0}, 0.75);
    EXPECT_FALSE(host.write_best().empty());
    host.save_latest(Counters{160, 8, 4.0}, 0.75);
  }
  for (const char* which : {"latest", "best"}) {
    SCOPED_TRACE(which);
    brain::BrainMeta saved;
    ASSERT_TRUE(brain::read_checkpoint_meta(brain_dir, which, saved));
    EXPECT_EQ(saved.j["counters"]["ticks"], 160);   // a new save, not the imported one
    EXPECT_EQ(saved.j["best_mean20"], 0.75);
    EXPECT_EQ(saved.j["name"], name);
    EXPECT_EQ(saved.j["imported"], imported);
    EXPECT_EQ(saved.j["history"], history);
  }
}
