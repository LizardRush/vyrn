// Checkpoints: round trip (weights, Adam state, meta) and crash safety (half-written saves, stale pointers).
#include "brain/checkpoint.h"
#include "brain/preprocess.h"

#include "platform/system.h"
#include "platform_test.h"

#include <gtest/gtest.h>
#if VYRN_TEST_POSIX
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#endif

#include <algorithm>
#include <chrono>
#include <random>
#include <thread>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace vyrn::brain;
namespace fs = std::filesystem;

namespace {

// A fresh folder under /tmp, removed at the end of the test.
class TempDir {
 public:
  TempDir() {
    std::string pattern = (fs::temp_directory_path() / "vyrn-brain-test-XXXXXX").string();
    if (!mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp failed");
    path_ = pattern;
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  std::string str() const { return path_.string(); }
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

const NetConfig kNet{NetKind::MLP, 0, 0, 0, 4, 3};

DqnConfig config() {
  DqnConfig c;
  c.lr = 1e-3f;
  c.target_every = 3;
  return c;
}

Batch batch_for(uint64_t seed) {
  torch::manual_seed(seed);
  Batch b;
  b.obs = torch::randn({16, 4});
  b.next_obs = torch::randn({16, 4});
  b.actions = torch::randint(0, 3, {16}, torch::kLong);
  b.rewards = torch::randn({16, 3});
  b.returns = b.rewards.sum(1);
  b.discounts = torch::full({16}, 0.9f);
  return b;
}

// A Dqn with its own weights and some Adam history.
std::unique_ptr<Dqn> trained(uint64_t seed, int updates) {
  torch::manual_seed(seed);
  auto dqn = std::make_unique<Dqn>(config(), kNet);
  dqn->observe_warmup_reward(5.0f);
  const Batch b = batch_for(seed + 100);
  for (int i = 0; i < updates; ++i) dqn->update(b);
  return dqn;
}

bool same_weights(const QNet& x, const QNet& y) {
  auto px = x->named_parameters();
  auto py = y->named_parameters();
  if (px.size() != py.size()) return false;
  for (const auto& item : px) {
    if (!torch::equal(item.value(), py[item.key()])) return false;
  }
  return true;
}

bool same_learner(const Dqn& a, const Dqn& b) {
  return same_weights(a.online(), b.online()) && same_weights(a.target(), b.target()) && a.updates() == b.updates();
}

void expect_same_adam(const Dqn& a, const Dqn& b) {
  const auto pa = a.online()->parameters(), pb = b.online()->parameters();
  ASSERT_EQ(pa.size(), pb.size());
  for (size_t i = 0; i < pa.size(); ++i) {
    const auto& sa = static_cast<const torch::optim::AdamParamState&>(*a.optimizer().state().at(pa[i].unsafeGetTensorImpl()));
    const auto& sb = static_cast<const torch::optim::AdamParamState&>(*b.optimizer().state().at(pb[i].unsafeGetTensorImpl()));
    EXPECT_EQ(sa.step(), sb.step());
    EXPECT_TRUE(torch::equal(sa.exp_avg(), sb.exp_avg()));
    EXPECT_TRUE(torch::equal(sa.exp_avg_sq(), sb.exp_avg_sq()));
  }
}

BrainMeta sample_meta() {
  RunningMeanStd rms(4);
  const float rows[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  rms.update(rows, 2);
  BrainMeta meta;
  meta.j = {{"version", 1},
            {"config", config()},
            {"counters", {{"ticks", 1234}, {"rounds", 56}}},
            {"normalizer", rms.to_json()},
            {"best_mean20", 3.5},
            {"shape", {{"kind", "numbers"}, {"d", 4}, {"choices", {"a", "b", "c"}}}}};
  return meta;
}

std::vector<std::string> folders(const fs::path& dir) {
  std::vector<std::string> names;
  for (const auto& e : fs::directory_iterator(dir)) names.push_back(e.path().filename().string());
  std::sort(names.begin(), names.end());
  return names;
}

struct SimulatedCrash {};

}  // namespace

TEST(Checkpoint, RoundTripKeepsWeightsAdamStateAndMeta) {
  TempDir tmp;
  auto saved = trained(1, 7);
  save_checkpoint(tmp.str(), "latest", *saved, sample_meta());
  EXPECT_EQ(folders(tmp.path() / "latest"), (std::vector<std::string>{"current", "save-1"}));
  EXPECT_EQ(folders(tmp.path() / "latest" / "save-1"), (std::vector<std::string>{"brain.json", "model.pt", "optim.pt"}));

  auto loaded = trained(2, 1);
  ASSERT_FALSE(same_learner(*saved, *loaded));
  BrainMeta meta;
  ASSERT_TRUE(load_checkpoint(tmp.str(), "latest", *loaded, meta));
  EXPECT_TRUE(same_learner(*saved, *loaded));
  EXPECT_EQ(loaded->updates(), 7);
  EXPECT_FLOAT_EQ(loaded->reward_scale(), 0.2f);
  EXPECT_TRUE(loaded->reward_scale_frozen());
  expect_same_adam(*saved, *loaded);

  // Meta comes back as written, plus the Dqn's own counters.
  const BrainMeta want = sample_meta();
  for (const auto& [key, value] : want.j.items()) EXPECT_EQ(meta.j.at(key), value) << key;
  EXPECT_EQ(meta.j["dqn"]["updates"], 7);
  EXPECT_FLOAT_EQ(meta.j["dqn"]["reward_scale"].get<float>(), 0.2f);
  RunningMeanStd rms(4);
  rms.from_json(meta.j["normalizer"]);
  EXPECT_DOUBLE_EQ(rms.mean()[0], 3.0);
  EXPECT_EQ(meta.j["config"].get<DqnConfig>().target_every, 3);

  // Same weights and Adam state: one more identical update gives identical learners.
  const Batch b = batch_for(77);
  saved->update(b);
  loaded->update(b);
  EXPECT_TRUE(same_learner(*saved, *loaded));

  // The next save replaces the folder; old ones are deleted.
  save_checkpoint(tmp.str(), "latest", *saved, sample_meta());
  EXPECT_EQ(folders(tmp.path() / "latest"), (std::vector<std::string>{"current", "save-2"}));
  BrainMeta read;
  ASSERT_TRUE(read_checkpoint_meta(tmp.str(), "latest", read));
  EXPECT_EQ(read.j["dqn"]["updates"], 8);
}

TEST(Checkpoint, NothingSavedYet) {
  TempDir tmp;
  auto dqn = trained(1, 0);
  BrainMeta meta;
  EXPECT_FALSE(load_checkpoint(tmp.str(), "latest", *dqn, meta));
  EXPECT_FALSE(read_checkpoint_meta(tmp.str(), "best", meta));
  fs::create_directories(tmp.path() / "latest" / "save-1");   // a crash before anything was written
  EXPECT_FALSE(load_checkpoint(tmp.str(), "latest", *dqn, meta));
  EXPECT_THROW(load_checkpoint(tmp.str(), "../escape", *dqn, meta), std::invalid_argument);
}

TEST(Checkpoint, KillDuringSaveLeavesThePreviousCheckpointLoadable) {
  for (const char* crash_at : {"folder", "model", "optim", "meta", "synced", "pointer", "swapped"}) {
    SCOPED_TRACE(crash_at);
    TempDir tmp;
    auto first = trained(1, 4);
    save_checkpoint(tmp.str(), "latest", *first, sample_meta());
    auto second = trained(3, 6);

    test_hooks::save_stage = [&](const char* stage) {
      if (std::string(stage) == crash_at) throw SimulatedCrash{};
    };
    EXPECT_THROW(save_checkpoint(tmp.str(), "latest", *second, sample_meta()), SimulatedCrash);
    test_hooks::save_stage = nullptr;
    EXPECT_TRUE(fs::exists(tmp.path() / "latest" / "save-2"));   // the half-written folder is still there

    auto loaded = trained(5, 1);
    BrainMeta meta;
    ASSERT_TRUE(load_checkpoint(tmp.str(), "latest", *loaded, meta));
    const bool swapped = std::string(crash_at) == "swapped";   // the crash came after the pointer moved
    EXPECT_TRUE(same_learner(swapped ? *second : *first, *loaded));

    // The next save works normally and cleans up.
    save_checkpoint(tmp.str(), "latest", *second, sample_meta());
    EXPECT_EQ(folders(tmp.path() / "latest"), (std::vector<std::string>{"current", "save-3"}));
    ASSERT_TRUE(load_checkpoint(tmp.str(), "latest", *loaded, meta));
    EXPECT_TRUE(same_learner(*second, *loaded));
  }
}

TEST(Checkpoint, StaleOrDamagedPointerFallsBackToALoadableSave) {
  TempDir tmp;
  auto first = trained(1, 4);
  save_checkpoint(tmp.str(), "latest", *first, sample_meta());
  const fs::path dir = tmp.path() / "latest";

  // A newer save whose model.pt was cut short, and a pointer to it.
  fs::copy(dir / "save-1", dir / "save-5", fs::copy_options::recursive);
  fs::resize_file(dir / "save-5" / "model.pt", fs::file_size(dir / "save-5" / "model.pt") / 2);
  std::ofstream(dir / "current") << "save-5\n";
  auto loaded = trained(2, 1);
  BrainMeta meta;
  ASSERT_TRUE(load_checkpoint(tmp.str(), "latest", *loaded, meta));
  EXPECT_TRUE(same_learner(*first, *loaded));

  // A pointer to a folder that is gone, or nonsense in the pointer file.
  std::ofstream(dir / "current") << "save-9\n";
  auto again = trained(3, 1);
  ASSERT_TRUE(load_checkpoint(tmp.str(), "latest", *again, meta));
  EXPECT_TRUE(same_learner(*first, *again));
  std::ofstream(dir / "current") << "garbage";
  auto third = trained(4, 1);
  ASSERT_TRUE(load_checkpoint(tmp.str(), "latest", *third, meta));
  EXPECT_TRUE(same_learner(*first, *third));
  // The next save numbers past every existing folder.
  save_checkpoint(tmp.str(), "latest", *first, sample_meta());
  EXPECT_EQ(folders(dir), (std::vector<std::string>{"current", "save-6"}));
}

TEST(Checkpoint, DamagedFilesNeverHalfLoad) {
  TempDir tmp;
  auto first = trained(1, 4);
  save_checkpoint(tmp.str(), "latest", *first, sample_meta());
  const fs::path save = tmp.path() / "latest" / "save-1";
  fs::resize_file(save / "model.pt", 100);
  auto other = trained(2, 3);
  auto before = trained(2, 3);
  EXPECT_ANY_THROW(other->load(save.string()));
  EXPECT_TRUE(same_learner(*other, *before));   // untouched
  BrainMeta meta;
  EXPECT_FALSE(load_checkpoint(tmp.str(), "latest", *other, meta));
  EXPECT_TRUE(same_learner(*other, *before));
}

TEST(Checkpoint, ADifferentNetworkIsReportedNotSkipped) {
  TempDir tmp;
  auto first = trained(1, 2);
  save_checkpoint(tmp.str(), "latest", *first, sample_meta());
  Dqn wider(config(), NetConfig{NetKind::MLP, 0, 0, 0, 4, 4});
  BrainMeta meta;
  EXPECT_THROW(load_checkpoint(tmp.str(), "latest", wider, meta), BrainMismatch);
  try {
    wider.load((tmp.path() / "latest" / "save-1").string());
  } catch (const BrainMismatch& e) {
    EXPECT_NE(std::string(e.what()).find("3 choices"), std::string::npos) << e.what();
  }
}

TEST(Checkpoint, WeightsOnlySaveFromASnapshot) {
  TempDir tmp;
  auto learner = trained(1, 5);
  const auto best = learner->snapshot();
  for (int i = 0; i < 3; ++i) learner->update(batch_for(9));   // keeps learning after the snapshot
  save_checkpoint(tmp.str(), "best", *best, sample_meta());
  EXPECT_EQ(folders(tmp.path() / "best" / "save-1"), (std::vector<std::string>{"brain.json", "model.pt"}));

  auto watcher = trained(2, 1);
  BrainMeta meta;
  ASSERT_TRUE(load_checkpoint(tmp.str(), "best", *watcher, meta));
  EXPECT_TRUE(same_weights(best->net(), watcher->online()));
  EXPECT_TRUE(same_weights(best->net(), watcher->target()));
  EXPECT_EQ(watcher->updates(), 5);
  EXPECT_FLOAT_EQ(watcher->reward_scale(), 0.2f);
  EXPECT_TRUE(watcher->optimizer().state().empty());   // fresh optimizer
  EXPECT_TRUE(meta.j["dqn"]["weights_only"].get<bool>());
  EXPECT_TRUE(std::isfinite(watcher->update(batch_for(3)).loss));
}

// ---- a real SIGKILL in the middle of saving ---------------------------------------------------------------

#if VYRN_TEST_POSIX
extern char** environ;

// Not run directly: the test below starts this binary again with only this test enabled. It saves
// checkpoints in a loop until it is killed. Save k has every online weight = k and every target weight = -k,
// and brain.json says k, so a mixed-up load is easy to spot.
TEST(Checkpoint, DISABLED_SaveForeverChild) {
  const char* dir = std::getenv("VYRN_TEST_CHECKPOINT_DIR");
  if (!dir) GTEST_SKIP() << "only used by SurvivesRealKillsWhileSaving";
  Dqn dqn(config(), kNet);
  for (int64_t k = 1;; ++k) {
    {
      torch::NoGradGuard no_grad;
      for (auto& p : dqn.online()->parameters()) p.fill_(static_cast<double>(k));
      for (auto& p : dqn.target()->parameters()) p.fill_(static_cast<double>(-k));
    }
    BrainMeta meta;
    meta.j = {{"k", k}};
    save_checkpoint(dir, "latest", dqn, meta);
  }
}

TEST(Checkpoint, SurvivesRealKillsWhileSaving) {
  const std::string exe = vyrn::platform::executable_path();
  std::mt19937 rng(42);
  std::uniform_int_distribution<int> delay_ms(0, 200);
  for (int round = 0; round < 5; ++round) {
    SCOPED_TRACE("round " + std::to_string(round));
    TempDir tmp;
    std::vector<std::string> env_strings;
    for (char** e = environ; *e; ++e) env_strings.emplace_back(*e);
    env_strings.push_back("VYRN_TEST_CHECKPOINT_DIR=" + tmp.str());
    std::vector<char*> envp;
    for (auto& e : env_strings) envp.push_back(e.data());
    envp.push_back(nullptr);
    std::string a0 = exe, a1 = "--gtest_filter=Checkpoint.DISABLED_SaveForeverChild",
                a2 = "--gtest_also_run_disabled_tests";
    std::vector<char*> argv = {a0.data(), a1.data(), a2.data(), nullptr};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, exe.c_str(), &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    ASSERT_EQ(rc, 0);

    // Wait for the first complete save, let it keep saving for a random moment, then kill it hard.
    const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!fs::exists(tmp.path() / "latest" / "current") && std::chrono::steady_clock::now() < give_up) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms(rng)));
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFSIGNALED(status)) << "the saving child stopped by itself";

    Dqn loaded(config(), kNet);
    BrainMeta meta;
    ASSERT_TRUE(load_checkpoint(tmp.str(), "latest", loaded, meta));
    const int64_t k = meta.j.at("k").get<int64_t>();
    EXPECT_GE(k, 1);
    for (const auto& p : loaded.online()->parameters()) EXPECT_TRUE(torch::all(p == static_cast<double>(k)).item<bool>());
    for (const auto& p : loaded.target()->parameters()) EXPECT_TRUE(torch::all(p == static_cast<double>(-k)).item<bool>());
    std::printf("killed after save %lld; %zu folder(s) left\n", static_cast<long long>(k), folders(tmp.path() / "latest").size());
  }
}
#else
TEST(Checkpoint, SurvivesRealKillsWhileSaving) {
  GTEST_SKIP() << "POSIX only: the saving child is started with posix_spawn and killed with SIGKILL";
}
#endif
