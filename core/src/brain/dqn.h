// Double DQN: settings, acting (epsilon-greedy), learning updates, immutable Policy snapshots and model files.
#pragma once

#include "brain/nets.h"
#include "brain/replay.h"
#include "brain/spec.h"

#include <torch/torch.h>

#include <cstdint>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace vyrn::brain {

// Learning settings. Defaults come from PLAN.md §3; vyrn.json "advanced" may override any field by name.
struct DqnConfig {
  int batch = 32;
  float lr = 1e-4f, adam_eps = 1.5e-4f;
  int target_every = 1000;          // hard copy online -> target every N updates
  int n_step = 3;
  float gamma = 0.99f;
  float grad_clip = 10.0f;          // max gradient norm
  int64_t learning_starts = 2000;   // ticks of warm-up before the first update
  double updates_per_tick = 0.25;
  int64_t explore_ticks = 20000;    // epsilon goes from eps_start to eps_end over this many ticks
  float eps_start = 1.0f, eps_end = 0.05f, eps_watch = 0.01f;
  int64_t buffer = 50000;           // replay capacity (observations)
  int frames = 4;                   // stacked frames (pictures only; numbers always use 1)
};

DqnConfig default_config(const Spec& spec, bool realtime);

// Applies vyrn.json "advanced" overrides by field name. Bad values and unknown names are skipped and
// reported as plain-English warnings (the return value). `frames` is ignored for numbers.
std::vector<std::string> apply_overrides(DqnConfig& cfg, const json& advanced, ObsSpec::Kind kind);

void to_json(json& j, const DqnConfig& c);
void from_json(const json& j, DqnConfig& c);   // missing fields keep their defaults

// Exploration rate after `ticks` training ticks: linear from eps_start to eps_end over explore_ticks.
float epsilon_at(const DqnConfig& cfg, int64_t ticks);

constexpr float kRewardClip = 10.0f;   // scaled rewards are clipped to +-10

// sum_k gamma^k * clip(scale * rewards[:, k], +-clip) for rewards [B, n] -> [B].
torch::Tensor n_step_returns(const torch::Tensor& rewards, float gamma, float scale, float clip);

// Double-DQN targets: returns + discounts * Q_target(s', argmax_a Q_online(s', a)).
// q_online_next, q_target_next: [B, A]; returns, discounts: [B]. Example: Q_on=[1,3], Q_tgt=[5,2], r=1,
// discount 0.9 -> 1 + 0.9 * 2 = 2.8.
torch::Tensor double_dqn_targets(const torch::Tensor& q_online_next, const torch::Tensor& q_target_next,
                                 const torch::Tensor& returns, const torch::Tensor& discounts);

struct UpdateStats {
  double loss = 0, q_mean = 0;
  int64_t updates = 0;
};

// Counters stored with the weights in model.pt.
struct ModelState {
  int64_t updates = 0;
  double max_abs_reward = 0;   // largest |points| seen during warm-up
  bool reward_frozen = false;
  float reward_scale() const;  // 1 / max(1, max_abs_reward)
};

// A saved brain doesn't fit the network it is loaded into (different input or choices).
struct BrainMismatch : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// An immutable snapshot of the online network for acting on another thread (the real-time actor).
// Eval mode, no gradients; act() is const and safe to call from several threads at once.
class Policy {
 public:
  Policy(QNet net, const ModelState& state);
  // obs1: one observation (any shape with the right number of elements; uint8 for pictures, float for
  // numbers). probs (optional): the chance act() picks each choice (epsilon/A + (1-epsilon) on the best).
  // q_values (optional): the network's value for each choice.
  int act(const torch::Tensor& obs1, float epsilon, std::mt19937_64& rng, std::vector<float>* probs,
          std::vector<float>* q_values = nullptr) const;
  const QNet& net() const { return net_; }
  const ModelState& state() const { return state_; }

  uint64_t version = 0;   // the number of updates the weights had when the snapshot was taken

 private:
  QNet net_;
  ModelState state_;
  torch::Device device_;
};

class Dqn {
 public:
  Dqn(const DqnConfig& cfg, const NetConfig& net, torch::Device device = torch::kCPU);

  int act(const torch::Tensor& obs1, float epsilon, std::mt19937_64& rng, std::vector<float>* probs,
          std::vector<float>* q_values = nullptr);
  // Double DQN + Huber + Adam + grad-norm clip; hard target copy every target_every updates.
  // Uses batch.rewards (raw points) with the reward scale and the +-10 clip when present, else batch.returns
  // as given. The first update freezes the reward scale if it isn't frozen yet.
  UpdateStats update(const Batch& batch);
  std::shared_ptr<const Policy> snapshot() const;   // deep copy of the online net

  // Reward scale: 1 / max(1, largest |points| seen during warm-up), frozen when learning starts.
  float reward_scale() const { return state_.reward_scale(); }
  void observe_warmup_reward(float r);   // ignored once frozen
  void freeze_reward_scale() { state_.reward_frozen = true; }
  bool reward_scale_frozen() const { return state_.reward_frozen; }

  // persistence: <dir>/model.pt (online + target + counters), <dir>/optim.pt (Adam state)
  void save(const std::string& dir) const;
  // All or nothing: on any error the Dqn is unchanged. Throws BrainMismatch if the saved network has a
  // different shape. A missing optim.pt (a weights-only save) keeps a fresh optimizer.
  void load(const std::string& dir);

  int64_t updates() const { return state_.updates; }
  const ModelState& state() const { return state_; }
  const DqnConfig& config() const { return cfg_; }
  const NetConfig& net_config() const { return net_cfg_; }
  json state_json() const;   // counters and reward scale, for brain.json

  void grow_actions(int new_n);           // appended choices (M5): grows both nets, keeps Adam's other state
  void set_weights(const Policy& policy);  // copy a snapshot's weights into online and target

  QNet& online() { return online_; }
  QNet& target() { return target_; }
  const QNet& online() const { return online_; }
  const QNet& target() const { return target_; }
  const torch::optim::Adam& optimizer() const { return *opt_; }

 private:
  std::unique_ptr<torch::optim::Adam> make_optimizer(const QNet& net) const;

  DqnConfig cfg_;
  NetConfig net_cfg_;
  torch::Device device_;
  QNet online_{nullptr}, target_{nullptr};
  std::unique_ptr<torch::optim::Adam> opt_;
  ModelState state_;
};

// model.pt: both networks, their config and the counters. Used by Dqn::save and weights-only checkpoints.
void save_model_file(const std::string& path, const QNet& online, const QNet& target, const ModelState& state);

}  // namespace vyrn::brain
