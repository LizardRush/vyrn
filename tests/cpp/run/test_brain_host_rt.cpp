// BrainHost split between a real-time actor (acts on Policy snapshots, records) and a learner thread (updates,
// publishes snapshots, saves), as RealtimeRunner uses it (PLAN.md section 2).
#include "brain/checkpoint.h"
#include "brain/threads.h"
#include "run/brain_host.h"

#include <gtest/gtest.h>

#include "platform_test.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;
using nlohmann::json;
using namespace vyrn::run;
namespace brain = vyrn::brain;

namespace {

class TempDir {
 public:
  TempDir() {
    std::string pattern = (fs::temp_directory_path() / "vyrn-host-XXXXXX").string();
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

// One round of 20 ticks: points 0 until the last tick, which scores `last` (a game over).
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

TEST(BrainHost, RealTimeActorAndLearnerShareTheBrain) {
  brain::init_torch_process();
  brain::set_torch_threads(1);
  TempDir tmp;
  const json spec_msg = {{"t", "spec"},
                         {"mode", "realtime"},
                         {"obs", {{"kind", "numbers"}, {"shape", {2}}, {"names", {"a", "b"}}}},
                         {"choices", {"left", "right"}},
                         {"labels", {"left", "right"}},
                         {"effects", {"call left()", "call right()"}},
                         {"uses", {{"screen", false}, {"keys", false}}}};
  const brain::Spec spec = brain::parse_spec(spec_msg);
  brain::DqnConfig cfg = brain::default_config(spec, true);
  cfg.batch = 8;
  const BrainPlan plan = plan_training(tmp.path(), "", false, spec, cfg.frames);
  BrainHost host(tmp.path(), spec, plan, cfg, true, 7);
  host.share_with_learner();
  EXPECT_EQ(host.policy_version(), 0u);
  EXPECT_FALSE(host.learn_once());   // nothing complete to learn from yet

  // The warm-up: the largest |points| the actor sees (5) sets the reward scale at the first update.
  play_round(host, 0, 5.0f);
  std::atomic<bool> quit{false};
  std::atomic<int64_t> done{0};
  std::thread learner([&] {
    brain::set_torch_threads(1);
    while (!quit.load()) {
      if (host.learn_once()) {
        if (++done % 50 == 0) host.publish_snapshot();
      } else {
        std::this_thread::yield();
      }
    }
  });
  for (int round = 1; round <= 20; ++round) play_round(host, round, round % 2 ? 1.0f : -1.0f);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (done.load() < 120 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  quit = true;
  learner.join();
  ASSERT_GE(done.load(), 120);
  EXPECT_EQ(host.updates(), done.load());
  EXPECT_GE(host.policy_version(), 100u);   // the actor plays with a published snapshot
  int64_t count = 0;
  double q = 0;
  host.take_loss_sum(&count, &q);
  EXPECT_EQ(count, done.load());

  // The best brain is the snapshot the actor played with; the learner writes it and the latest one.
  host.keep_best(Counters{420, 21, 3.5}, 0.25);
  EXPECT_TRUE(host.best_pending());
  const std::string best = host.write_best();
  EXPECT_FALSE(host.best_pending());
  EXPECT_EQ(host.write_best(), "");
  host.save_latest(Counters{420, 21, 3.5}, 0.25);
  brain::BrainMeta meta;
  ASSERT_TRUE(brain::read_checkpoint_meta(host.dir(), "best", meta));
  EXPECT_EQ(meta.j["best_mean20"], 0.25);
  EXPECT_EQ(meta.j["counters"]["ticks"], 420);
  brain::Dqn dqn(host.config(), brain::net_config_for(host.shape()));
  ASSERT_TRUE(brain::load_checkpoint(host.dir(), "latest", dqn, meta));
  EXPECT_FLOAT_EQ(dqn.reward_scale(), 0.2f);
  EXPECT_TRUE(dqn.reward_scale_frozen());
  EXPECT_EQ(dqn.updates(), done.load());
  EXPECT_FALSE(best.empty());
}
