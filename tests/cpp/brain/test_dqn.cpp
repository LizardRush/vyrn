// Double DQN maths, settings, acting, learning, torch threads and Policy snapshots.
#include "brain/bench.h"
#include "brain/dqn.h"
#include "brain/threads.h"

#include <ATen/Parallel.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

using namespace vyrn::brain;

namespace {

Spec spec_of(ObsSpec::Kind kind) {
  Spec s;
  s.mode = "sim";
  s.obs.kind = kind;
  s.obs.h = s.obs.w = 10;
  s.obs.d = 4;
  s.choices = {"left", "right", "stay"};
  return s;
}

Batch random_batch(const NetConfig& net, int b, int n, uint64_t seed) {
  torch::manual_seed(seed);
  Batch batch;
  const int64_t width = net.kind == NetKind::MLP ? net.d : int64_t{net.in_c} * net.in_h * net.in_w;
  if (net.kind == NetKind::MLP) {
    batch.obs = torch::randn({b, width});
    batch.next_obs = torch::randn({b, width});
  } else {
    batch.obs = torch::randint(0, 256, {b, width}, torch::kUInt8);
    batch.next_obs = torch::randint(0, 256, {b, width}, torch::kUInt8);
  }
  batch.actions = torch::randint(0, net.n_actions, {b}, torch::kLong);
  batch.rewards = torch::randint(-1, 2, {b, n}).to(torch::kFloat);
  batch.returns = batch.rewards.sum(1);
  batch.discounts = torch::full({b}, 0.9f);
  return batch;
}

}  // namespace

// ---- pure maths ------------------------------------------------------------------------------------------

TEST(Dqn, DoubleDqnTarget) {
  // Q_online(s') = [1, 3] picks action 1; Q_target(s') = [5, 2] values it at 2: 1 + 0.9 * 2 = 2.8.
  torch::Tensor y = double_dqn_targets(torch::tensor({{1.0f, 3.0f}}), torch::tensor({{5.0f, 2.0f}}),
                                       torch::tensor({1.0f}), torch::tensor({0.9f}));
  EXPECT_NEAR(y.item<float>(), 2.8f, 1e-6);
  // A terminal transition (discount 0) is just the return.
  y = double_dqn_targets(torch::tensor({{1.0f, 3.0f}}), torch::tensor({{5.0f, 2.0f}}), torch::tensor({1.0f}),
                         torch::tensor({0.0f}));
  EXPECT_FLOAT_EQ(y.item<float>(), 1.0f);
}

TEST(Dqn, NStepReturnsScaleAndClipEachReward) {
  torch::Tensor rewards = torch::tensor({{1.0f, 2.0f, 4.0f}, {300.0f, -1.0f, 0.0f}});
  torch::Tensor r = n_step_returns(rewards, 0.5f, 0.1f, 10.0f);
  EXPECT_NEAR(r[0].item<float>(), 0.1f + 0.5f * 0.2f + 0.25f * 0.4f, 1e-6);
  EXPECT_NEAR(r[1].item<float>(), 10.0f - 0.5f * 0.1f, 1e-5);   // 30 is clipped to 10 before summing
}

// ---- settings ------------------------------------------------------------------------------------------------

TEST(DqnConfig, DefaultsFollowThePlanTable) {
  const DqnConfig num = default_config(spec_of(ObsSpec::Kind::Numbers), false);
  EXPECT_EQ(num.buffer, 100000);
  EXPECT_EQ(num.batch, 128);
  EXPECT_FLOAT_EQ(num.lr, 2.5e-4f);
  EXPECT_EQ(num.learning_starts, 1000);
  EXPECT_EQ(num.target_every, 500);
  EXPECT_EQ(num.frames, 1);
  EXPECT_DOUBLE_EQ(num.updates_per_tick, 0.25);
  const DqnConfig pic = default_config(spec_of(ObsSpec::Kind::Picture), false);
  EXPECT_EQ(pic.buffer, 50000);
  EXPECT_EQ(pic.batch, 32);
  EXPECT_FLOAT_EQ(pic.lr, 1e-4f);
  EXPECT_EQ(pic.learning_starts, 2000);
  EXPECT_EQ(pic.target_every, 1000);
  EXPECT_EQ(pic.frames, 4);
  const DqnConfig rt = default_config(spec_of(ObsSpec::Kind::Picture), true);
  EXPECT_EQ(rt.learning_starts, 1000);
  EXPECT_DOUBLE_EQ(rt.updates_per_tick, 1.0);
  for (const DqnConfig& c : {num, pic, rt}) {
    EXPECT_EQ(c.n_step, 3);
    EXPECT_FLOAT_EQ(c.gamma, 0.99f);
    EXPECT_FLOAT_EQ(c.grad_clip, 10.0f);
    EXPECT_EQ(c.explore_ticks, 20000);
    EXPECT_FLOAT_EQ(c.eps_start, 1.0f);
    EXPECT_FLOAT_EQ(c.eps_end, 0.05f);
    EXPECT_FLOAT_EQ(c.eps_watch, 0.01f);
  }
}

TEST(DqnConfig, AdvancedOverridesByName) {
  DqnConfig c = default_config(spec_of(ObsSpec::Kind::Picture), false);
  const json advanced = {{"explore_ticks", 5000}, {"frames", 2},  {"lr", 3e-4},      {"batch", 2.5},
                         {"gamma", 1.5},          {"eps_end", "low"}, {"mystery", 1}, {"learning_starts", 1e3}};
  const auto warnings = apply_overrides(c, advanced, ObsSpec::Kind::Picture);
  EXPECT_EQ(c.explore_ticks, 5000);
  EXPECT_EQ(c.frames, 2);
  EXPECT_FLOAT_EQ(c.lr, 3e-4f);
  EXPECT_EQ(c.learning_starts, 1000);
  EXPECT_EQ(c.batch, 32);            // not a whole number: kept
  EXPECT_FLOAT_EQ(c.gamma, 0.99f);   // out of range: kept
  EXPECT_FLOAT_EQ(c.eps_end, 0.05f);
  ASSERT_EQ(warnings.size(), 4u);
  std::string all;
  for (const auto& w : warnings) all += w + "\n";
  EXPECT_NE(all.find("advanced.batch must be a whole number"), std::string::npos) << all;
  EXPECT_NE(all.find("advanced.gamma must be between 0 and 1"), std::string::npos) << all;
  EXPECT_NE(all.find("advanced.mystery"), std::string::npos) << all;

  DqnConfig n = default_config(spec_of(ObsSpec::Kind::Numbers), false);
  EXPECT_EQ(apply_overrides(n, {{"frames", 4}}, ObsSpec::Kind::Numbers).size(), 1u);
  EXPECT_EQ(n.frames, 1);
  EXPECT_TRUE(apply_overrides(n, nullptr, ObsSpec::Kind::Numbers).empty());

  const json saved = c;
  const DqnConfig back = saved.get<DqnConfig>();
  EXPECT_EQ(json(back), saved);
  EXPECT_EQ(back.explore_ticks, 5000);
}

TEST(DqnConfig, EpsilonSchedule) {
  DqnConfig c;
  c.explore_ticks = 1000;
  EXPECT_FLOAT_EQ(epsilon_at(c, 0), 1.0f);
  EXPECT_FLOAT_EQ(epsilon_at(c, 500), 0.525f);
  EXPECT_FLOAT_EQ(epsilon_at(c, 1000), 0.05f);
  EXPECT_FLOAT_EQ(epsilon_at(c, 50000), 0.05f);
}

// ---- acting and learning ------------------------------------------------------------------------------------

TEST(Dqn, ActingProbabilitiesAndSnapshots) {
  init_torch_process();
  torch::manual_seed(0);
  const NetConfig net{NetKind::MLP, 0, 0, 0, 4, 3};
  Dqn dqn(DqnConfig{}, net);
  std::mt19937_64 rng(1);
  const torch::Tensor obs = torch::randn({4});
  std::vector<float> probs, q;
  const int greedy = dqn.act(obs, 0.0f, rng, &probs, &q);
  ASSERT_EQ(q.size(), 3u);
  EXPECT_EQ(greedy, static_cast<int>(std::max_element(q.begin(), q.end()) - q.begin()));
  EXPECT_FLOAT_EQ(probs[static_cast<size_t>(greedy)], 1.0f);
  dqn.act(obs, 0.3f, rng, &probs);
  EXPECT_NEAR(probs[static_cast<size_t>(greedy)], 0.7f + 0.1f, 1e-6);
  EXPECT_NEAR(probs[0] + probs[1] + probs[2], 1.0f, 1e-6);
  // With epsilon 1 every choice comes up.
  std::array<int, 3> counts{};
  for (int i = 0; i < 300; ++i) ++counts[static_cast<size_t>(dqn.act(obs, 1.0f, rng, nullptr))];
  for (int c : counts) EXPECT_GT(c, 50);

  auto policy = dqn.snapshot();
  std::vector<float> pq;
  EXPECT_EQ(policy->act(obs, 0.0f, rng, nullptr, &pq), greedy);
  EXPECT_EQ(pq, q);
  EXPECT_EQ(policy->version, 0u);
  // The snapshot is a deep copy: learning doesn't change it.
  const Batch batch = random_batch(net, 16, 3, 2);
  for (int i = 0; i < 5; ++i) dqn.update(batch);
  policy->act(obs, 0.0f, rng, nullptr, &pq);
  EXPECT_EQ(pq, q);
  EXPECT_EQ(dqn.snapshot()->version, 5u);
}

TEST(Dqn, RewardScaleIsSetDuringWarmUpThenFrozen) {
  Dqn dqn(DqnConfig{}, NetConfig{NetKind::MLP, 0, 0, 0, 2, 2});
  EXPECT_FLOAT_EQ(dqn.reward_scale(), 1.0f);
  dqn.observe_warmup_reward(0.5f);
  EXPECT_FLOAT_EQ(dqn.reward_scale(), 1.0f);   // never scales up
  dqn.observe_warmup_reward(-10.0f);
  dqn.observe_warmup_reward(3.0f);
  dqn.observe_warmup_reward(std::nanf(""));
  EXPECT_FLOAT_EQ(dqn.reward_scale(), 0.1f);
  dqn.freeze_reward_scale();
  dqn.observe_warmup_reward(100.0f);
  EXPECT_FLOAT_EQ(dqn.reward_scale(), 0.1f);

  Dqn auto_frozen(DqnConfig{}, NetConfig{NetKind::MLP, 0, 0, 0, 2, 2});
  auto_frozen.observe_warmup_reward(4.0f);
  auto_frozen.update(random_batch(NetConfig{NetKind::MLP, 0, 0, 0, 2, 2}, 8, 3, 1));
  EXPECT_TRUE(auto_frozen.reward_scale_frozen());   // learning started
  auto_frozen.observe_warmup_reward(8.0f);
  EXPECT_FLOAT_EQ(auto_frozen.reward_scale(), 0.25f);
}

TEST(Dqn, UpdateFitsAFixedBatchAndCopiesTheTargetOnSchedule) {
  init_torch_process();
  set_torch_threads(2);
  const NetConfig net{NetKind::SmallCNN, 12, 10, 10, 0, 3};
  DqnConfig cfg = default_config(spec_of(ObsSpec::Kind::Picture), false);
  cfg.target_every = 10;
  cfg.lr = 1e-3f;
  torch::manual_seed(4);
  Dqn dqn(cfg, net);
  Batch batch = random_batch(net, 32, 3, 3);
  batch.discounts.zero_();   // fixed targets: plain regression onto the returns
  double first = 0, last = 0;
  for (int i = 1; i <= 60; ++i) {
    const UpdateStats s = dqn.update(batch);
    EXPECT_TRUE(std::isfinite(s.loss));
    EXPECT_EQ(s.updates, i);
    if (i == 1) first = s.loss;
    last = s.loss;
    // The target equals the online net right after a copy, and differs from it in between.
    const torch::Tensor x = batch.next_obs.narrow(0, 0, 4);
    const bool same = torch::equal(dqn.online()->forward(x), dqn.target()->forward(x));
    EXPECT_EQ(same, i % 10 == 0) << "update " << i;
  }
  EXPECT_LT(last, first * 0.5);
}

TEST(Dqn, GrowActionsKeepsLearning) {
  const NetConfig net{NetKind::MLP, 0, 0, 0, 3, 2};
  torch::manual_seed(0);
  Dqn dqn(DqnConfig{}, net);
  const Batch batch = random_batch(net, 8, 3, 1);
  dqn.update(batch);
  const torch::Tensor x = torch::randn({1, 3});
  const torch::Tensor before = dqn.online()->forward(x).detach();
  dqn.grow_actions(4);
  EXPECT_EQ(dqn.net_config().n_actions, 4);
  EXPECT_TRUE(torch::allclose(dqn.online()->forward(x).detach().narrow(1, 0, 2), before));
  EXPECT_EQ(dqn.target()->config().n_actions, 4);
  Batch wide = random_batch(NetConfig{NetKind::MLP, 0, 0, 0, 3, 4}, 8, 3, 2);
  EXPECT_TRUE(std::isfinite(dqn.update(wide).loss));
  // Adam kept its state for the unchanged layers (step 2 there) and started fresh for the new head.
  const auto& state = dqn.optimizer().state();
  const auto& fc1 = static_cast<const torch::optim::AdamParamState&>(
      *state.at(dqn.online()->named_parameters()["fc1.weight"].unsafeGetTensorImpl()));
  const auto& head = static_cast<const torch::optim::AdamParamState&>(
      *state.at(dqn.online()->named_parameters()["head.weight"].unsafeGetTensorImpl()));
  EXPECT_EQ(fc1.step(), 2);
  EXPECT_EQ(head.step(), 1);
}

// A 5-state chain: choice 1 moves right, choice 0 moves left. Reaching state 4 pays 10 points and ends the
// round (game over); rounds are cut after 30 ticks. The full loop: replay, warm-up, reward scale, updates.
TEST(DqnLearning, SolvesATinyChain) {
  init_torch_process();
  set_torch_threads(1);
  torch::manual_seed(7);
  DqnConfig cfg = default_config(spec_of(ObsSpec::Kind::Numbers), false);
  cfg.batch = 32;
  cfg.lr = 1e-3f;
  cfg.gamma = 0.9f;
  cfg.target_every = 100;
  cfg.learning_starts = 300;
  cfg.explore_ticks = 1500;
  const NetConfig net{NetKind::MLP, 0, 0, 0, 5, 2};
  Dqn dqn(cfg, net);
  Replay replay(5000, 5 * sizeof(float), true, 1, cfg.n_step, cfg.gamma);
  std::mt19937_64 rng(3);
  auto obs_of = [](int s) {
    std::array<float, 5> o{};
    o[static_cast<size_t>(s)] = 1.0f;
    return o;
  };
  auto tensor_of = [](std::array<float, 5>& o) { return torch::from_blob(o.data(), {5}).clone(); };

  const auto t0 = std::chrono::steady_clock::now();
  int state = 0, round_ticks = 0, wins = 0;
  auto obs = obs_of(0);
  int64_t slot = replay.add_obs(obs.data(), true);
  for (int64_t tick = 0; tick < 3000; ++tick) {
    const int a = dqn.act(tensor_of(obs), epsilon_at(cfg, tick), rng, nullptr);
    const int next = a == 1 ? state + 1 : std::max(0, state - 1);
    const float points = next == 4 ? 10.0f : 0.0f;
    ++round_ticks;
    if (tick < cfg.learning_starts) dqn.observe_warmup_reward(points);
    replay.set_transition(slot, a, points);
    auto next_obs = obs_of(next);
    const int64_t next_slot = replay.add_obs(next_obs.data(), false);
    if (next == 4 || round_ticks >= 30) {
      replay.end_segment(next_slot, next == 4 ? EndKind::Terminal : EndKind::Truncated);
      wins += next == 4;
      state = 0;
      round_ticks = 0;
      obs = obs_of(0);
      slot = replay.add_obs(obs.data(), true);
    } else {
      state = next;
      obs = next_obs;
      slot = next_slot;
    }
    if (tick >= cfg.learning_starts) dqn.update(replay.sample(cfg.batch, rng));
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  EXPECT_FLOAT_EQ(dqn.reward_scale(), 0.1f);   // 10 points were seen during warm-up
  EXPECT_GT(wins, 20);
  // Greedy: always right. Q(3, right) = 1 (10 points x 0.1), Q(2, right) = 0.9, Q(1, right) = 0.81.
  std::vector<float> q;
  for (int s = 0; s < 4; ++s) {
    auto o = obs_of(s);
    EXPECT_EQ(dqn.act(tensor_of(o), 0.0f, rng, nullptr, &q), 1) << "state " << s;
    EXPECT_NEAR(q[1], std::pow(0.9, 3 - s), 0.2) << "state " << s;
  }
  EXPECT_LT(seconds, 60.0);
  std::printf("chain solved in %.1f s\n", seconds);
}

// ---- threads and snapshots ----------------------------------------------------------------------------------

TEST(TorchThreads, EachThreadKeepsItsOwnCount) {
  init_torch_process();
  const char* env = std::getenv("VYRN_ONEDNN");
  if (env == nullptr || (std::strcmp(env, "0") != 0 && std::strcmp(env, "1") != 0)) {
    EXPECT_EQ(onednn_enabled(), kOneDnnDefault);   // the default chosen from the bench
    EXPECT_FALSE(onednn_enabled());                // ...which is off (docs/dev/bench.md)
  } else {
    EXPECT_EQ(onednn_enabled(), std::strcmp(env, "1") == 0);   // the override
  }
  std::array<int, 2> seen{};
  std::atomic<int> ready{0};
  auto worker = [&](int k, int* out) {
    set_torch_threads(k);
    ++ready;
    while (ready < 2) std::this_thread::yield();   // both threads have set their count before any work
    torch::nn::Conv2d conv(torch::nn::Conv2dOptions(3, 8, 3));
    const torch::Tensor x = torch::rand({8, 3, 64, 64});
    const torch::Tensor m = torch::rand({256, 256});
    for (int i = 0; i < 10; ++i) {
      torch::Tensor y = conv->forward(x).sum();
      y.backward();
      torch::Tensor z = torch::mm(m, m).relu().sum();
      (void)z.item<float>();
    }
    *out = at::get_num_threads();
  };
  std::thread a(worker, 1, &seen[0]), b(worker, 2, &seen[1]);
  a.join();
  b.join();
  EXPECT_EQ(seen[0], 1);
  EXPECT_EQ(seen[1], 2);
}

// PLAN.md section 8: 30 s of actor forwards while the learner publishes; every output finite.
TEST(PolicySnapshots, ActorActsWhileTheLearnerPublishes) {
  init_torch_process();
  const NetConfig net{NetKind::SmallCNN, 12, 10, 10, 0, 3};
  DqnConfig cfg = default_config(spec_of(ObsSpec::Kind::Picture), false);
  torch::manual_seed(11);
  Dqn dqn(cfg, net);
  std::mutex mu;
  std::shared_ptr<const Policy> published = dqn.snapshot();
  std::atomic<bool> stop{false};
  std::atomic<int64_t> publishes{0};

  const Batch batch = random_batch(net, cfg.batch, cfg.n_step, 5);
  torch::manual_seed(12);
  const torch::Tensor obs = torch::randint(0, 256, {12 * 10 * 10}, torch::kUInt8);
  std::thread learner([&] {
    set_torch_threads(2);
    while (!stop) {
      dqn.update(batch);
      if (dqn.updates() % 5 == 0) {
        auto fresh = dqn.snapshot();
        std::lock_guard<std::mutex> lock(mu);
        published = std::move(fresh);
        ++publishes;
      }
    }
  });

  set_torch_threads(1);
  std::mt19937_64 rng(2);
  int64_t acts = 0, version_changes = 0, bad = 0;
  uint64_t last_version = 0;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < until) {
    std::shared_ptr<const Policy> policy;
    {
      std::lock_guard<std::mutex> lock(mu);
      policy = published;
    }
    std::vector<float> probs, q;
    const int a = policy->act(obs, 0.1f, rng, &probs, &q);
    bad += a < 0 || a >= 3;
    float sum = 0;
    for (float p : probs) sum += p;
    bad += std::fabs(sum - 1.0f) > 1e-5f;
    for (float v : q) bad += !std::isfinite(v);
    bad += policy->version < last_version;
    version_changes += policy->version > last_version;
    last_version = policy->version;
    ++acts;
  }
  stop = true;
  learner.join();
  EXPECT_EQ(bad, 0);
  EXPECT_GT(acts, 100);
  EXPECT_GT(publishes.load(), 3);
  EXPECT_GT(version_changes, 3);
  std::printf("%lld acts, %lld publishes, %lld updates\n", static_cast<long long>(acts),
              static_cast<long long>(publishes.load()), static_cast<long long>(dqn.updates()));
}

TEST(Bench, ProducesATableAndARecommendation) {
  BenchOptions quick;
  quick.seconds_per_case = 0.0;
  quick.min_iterations = 1;
  set_torch_threads(3);
  const bool onednn_before = onednn_enabled();
  const std::string out = run_bench(quick);
  std::printf("%s", out.c_str());
  EXPECT_NE(out.find("DER CNN"), std::string::npos);
  EXPECT_NE(out.find("small CNN"), std::string::npos);
  EXPECT_NE(out.find("MLP"), std::string::npos);
  EXPECT_NE(out.find("Recommendation:"), std::string::npos);
  EXPECT_EQ(torch_threads(), 3);     // restored
  EXPECT_EQ(onednn_enabled(), onednn_before);   // restored
}
