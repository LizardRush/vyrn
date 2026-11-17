#include "brain/dqn.h"

#include "brain/threads.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <type_traits>

namespace vyrn::brain {

namespace fs = std::filesystem;

namespace {

constexpr int64_t kModelFormat = 1;

// Every DqnConfig field with its name and allowed range (inclusive), for overrides and brain.json.
template <typename Config, typename Fn>
void visit_fields(Config& c, Fn&& fn) {
  fn("batch", c.batch, 1.0, 65536.0);
  fn("lr", c.lr, 1e-9, 1.0);
  fn("adam_eps", c.adam_eps, 1e-12, 1.0);
  fn("target_every", c.target_every, 1.0, 1e9);
  fn("n_step", c.n_step, 1.0, 100.0);
  fn("gamma", c.gamma, 0.0, 1.0);
  fn("grad_clip", c.grad_clip, 1e-6, 1e6);
  fn("learning_starts", c.learning_starts, 0.0, 1e12);
  fn("updates_per_tick", c.updates_per_tick, 0.0, 64.0);
  fn("explore_ticks", c.explore_ticks, 0.0, 1e12);
  fn("eps_start", c.eps_start, 0.0, 1.0);
  fn("eps_end", c.eps_end, 0.0, 1.0);
  fn("eps_watch", c.eps_watch, 0.0, 1.0);
  fn("buffer", c.buffer, 100.0, 1e8);
  fn("frames", c.frames, 1.0, 16.0);
}

std::string number_text(double v) {
  std::ostringstream os;
  os << v;
  return os.str();
}

// The epsilon-greedy step shared by Dqn::act and Policy::act.
int act_with(QNetImpl& net, const torch::Device& device, const torch::Tensor& obs1, float epsilon,
             std::mt19937_64& rng, std::vector<float>* probs, std::vector<float>* q_values) {
  const int n = net.config().n_actions;
  std::uniform_real_distribution<float> coin(0.0f, 1.0f);
  const bool explore = epsilon > 0.0f && coin(rng) < epsilon;
  const int random_choice = explore ? std::uniform_int_distribution<int>(0, n - 1)(rng) : -1;
  if (explore && !probs && !q_values) return random_choice;   // no need to run the network

  torch::Tensor q;
  {
    c10::InferenceMode inference;
    q = net.forward(obs1.to(device).reshape({1, -1})).reshape({n}).to(torch::kCPU, torch::kFloat).contiguous();
  }
  const float* qv = q.data_ptr<float>();
  int best = 0;
  for (int a = 1; a < n; ++a) {
    if (qv[a] > qv[best]) best = a;
  }
  if (q_values) q_values->assign(qv, qv + n);
  if (probs) {
    const float e = std::clamp(epsilon, 0.0f, 1.0f);
    probs->assign(static_cast<size_t>(n), e / static_cast<float>(n));
    (*probs)[static_cast<size_t>(best)] += 1.0f - e;
  }
  return explore ? random_choice : best;
}

torch::Device device_of(const QNet& net) {
  auto params = net->parameters();
  return params.empty() ? torch::Device(torch::kCPU) : params.front().device();
}

}  // namespace

// ---- settings ------------------------------------------------------------------------------------------

DqnConfig default_config(const Spec& spec, bool realtime) {
  DqnConfig c;
  c.n_step = 3;
  c.gamma = 0.99f;
  c.grad_clip = 10.0f;
  c.updates_per_tick = realtime ? 1.0 : 0.25;   // real time: at most one (async) update per tick
  c.explore_ticks = 20000;
  c.eps_start = 1.0f;
  c.eps_end = 0.05f;
  c.eps_watch = 0.01f;
  if (spec.obs.kind == ObsSpec::Kind::Numbers) {
    c.buffer = 100000;
    c.batch = 128;
    c.lr = 2.5e-4f;
    c.adam_eps = 1e-8f;
    c.learning_starts = 1000;
    c.target_every = 500;
    c.frames = 1;
  } else {
    c.buffer = 50000;
    c.batch = 32;
    c.lr = 1e-4f;
    c.adam_eps = 1.5e-4f;
    c.learning_starts = realtime ? 1000 : 2000;
    c.target_every = 1000;
    c.frames = 4;
  }
  return c;
}

std::vector<std::string> apply_overrides(DqnConfig& cfg, const json& advanced, ObsSpec::Kind kind) {
  std::vector<std::string> warnings;
  if (advanced.is_null()) return warnings;
  if (!advanced.is_object()) {
    warnings.push_back("vyrn.json: \"advanced\" should look like {\"frames\": 4}, so it is ignored.");
    return warnings;
  }
  for (auto it = advanced.begin(); it != advanced.end(); ++it) {
    bool known = false;
    visit_fields(cfg, [&](const char* name, auto& field, double lo, double hi) {
      if (it.key() != name) return;
      known = true;
      using T = std::decay_t<decltype(field)>;
      const json& v = it.value();
      const std::string where = std::string("vyrn.json: advanced.") + name;
      const std::string keep = "; Vyrn keeps " + number_text(static_cast<double>(field)) + ".";
      if (!v.is_number()) {
        warnings.push_back(where + " must be a number" + keep);
        return;
      }
      const double x = v.get<double>();
      if (std::is_integral_v<T> && x != std::floor(x)) {
        warnings.push_back(where + " must be a whole number" + keep);
        return;
      }
      if (!(x >= lo && x <= hi)) {
        warnings.push_back(where + " must be between " + number_text(lo) + " and " + number_text(hi) + keep);
        return;
      }
      field = static_cast<T>(x);
    });
    if (!known) warnings.push_back("vyrn.json: advanced." + it.key() + " isn't a setting Vyrn knows, so it is ignored.");
  }
  if (kind == ObsSpec::Kind::Numbers && cfg.frames != 1) {
    warnings.push_back("vyrn.json: advanced.frames only applies to pictures; numbers are never stacked.");
    cfg.frames = 1;
  }
  return warnings;
}

void to_json(json& j, const DqnConfig& c) {
  j = json::object();
  visit_fields(c, [&](const char* name, const auto& field, double, double) { j[name] = field; });
}

void from_json(const json& j, DqnConfig& c) {
  visit_fields(c, [&](const char* name, auto& field, double, double) {
    auto it = j.find(name);
    if (it != j.end() && it->is_number()) field = it->template get<std::decay_t<decltype(field)>>();
  });
}

float epsilon_at(const DqnConfig& cfg, int64_t ticks) {
  if (cfg.explore_ticks <= 0) return cfg.eps_end;
  const double frac = std::clamp(static_cast<double>(ticks) / static_cast<double>(cfg.explore_ticks), 0.0, 1.0);
  return static_cast<float>(cfg.eps_start + frac * (cfg.eps_end - cfg.eps_start));
}

// ---- pure maths -----------------------------------------------------------------------------------------

torch::Tensor n_step_returns(const torch::Tensor& rewards, float gamma, float scale, float clip) {
  const int64_t n = rewards.size(1);
  torch::Tensor powers = torch::pow(torch::full({n}, static_cast<double>(gamma), rewards.options()),
                                    torch::arange(n, rewards.options()));
  return (rewards * scale).clamp(-clip, clip).mul(powers).sum(1);
}

torch::Tensor double_dqn_targets(const torch::Tensor& q_online_next, const torch::Tensor& q_target_next,
                                 const torch::Tensor& returns, const torch::Tensor& discounts) {
  torch::Tensor best = q_online_next.argmax(1, /*keepdim=*/true);   // chosen by the online net...
  torch::Tensor value = q_target_next.gather(1, best).squeeze(1);    // ...valued by the target net
  return returns + discounts * value;
}

float ModelState::reward_scale() const {
  return static_cast<float>(1.0 / std::max(1.0, max_abs_reward));
}

// ---- Policy ---------------------------------------------------------------------------------------------

Policy::Policy(QNet net, const ModelState& state)
    : version(static_cast<uint64_t>(std::max<int64_t>(0, state.updates))),
      net_(std::move(net)),
      state_(state),
      device_(device_of(net_)) {
  net_->eval();
  for (auto& p : net_->parameters()) p.set_requires_grad(false);
}

int Policy::act(const torch::Tensor& obs1, float epsilon, std::mt19937_64& rng, std::vector<float>* probs,
                std::vector<float>* q_values) const {
  // net_.ptr() hands out a non-const module: forward() changes nothing, so concurrent calls are fine.
  return act_with(*net_.ptr(), device_, obs1, epsilon, rng, probs, q_values);
}

// ---- Dqn ------------------------------------------------------------------------------------------------

Dqn::Dqn(const DqnConfig& cfg, const NetConfig& net, torch::Device device)
    : cfg_(cfg), net_cfg_(net), device_(device) {
  init_torch_process();   // oneDNN default and inter-op threads, once per process
  online_ = make_qnet(net);
  online_->to(device_);
  target_ = clone_qnet(online_);
  for (auto& p : target_->parameters()) p.set_requires_grad(false);
  opt_ = make_optimizer(online_);
}

std::unique_ptr<torch::optim::Adam> Dqn::make_optimizer(const QNet& net) const {
  return std::make_unique<torch::optim::Adam>(
      net->parameters(), torch::optim::AdamOptions(cfg_.lr).eps(static_cast<double>(cfg_.adam_eps)));
}

int Dqn::act(const torch::Tensor& obs1, float epsilon, std::mt19937_64& rng, std::vector<float>* probs,
             std::vector<float>* q_values) {
  return act_with(*online_, device_, obs1, epsilon, rng, probs, q_values);
}

void Dqn::observe_warmup_reward(float r) {
  if (state_.reward_frozen || !std::isfinite(r)) return;
  state_.max_abs_reward = std::max(state_.max_abs_reward, static_cast<double>(std::fabs(r)));
}

UpdateStats Dqn::update(const Batch& batch) {
  if (!state_.reward_frozen) freeze_reward_scale();
  online_->train();
  const torch::Tensor obs = batch.obs.to(device_);
  const torch::Tensor next_obs = batch.next_obs.to(device_);
  const torch::Tensor actions = batch.actions.to(device_, torch::kLong).reshape({-1, 1});

  torch::Tensor targets;
  {
    torch::NoGradGuard no_grad;
    const torch::Tensor returns =
        batch.rewards.defined()
            ? n_step_returns(batch.rewards.to(device_, torch::kFloat), cfg_.gamma, reward_scale(), kRewardClip)
            : batch.returns.to(device_, torch::kFloat);
    targets = double_dqn_targets(online_->forward(next_obs), target_->forward(next_obs), returns,
                                 batch.discounts.to(device_, torch::kFloat));
  }
  const torch::Tensor q_taken = online_->forward(obs).gather(1, actions).squeeze(1);
  torch::Tensor loss = torch::huber_loss(q_taken, targets, at::Reduction::Mean, 1.0);
  opt_->zero_grad();
  loss.backward();
  torch::nn::utils::clip_grad_norm_(online_->parameters(), cfg_.grad_clip);
  opt_->step();

  ++state_.updates;
  if (cfg_.target_every > 0 && state_.updates % cfg_.target_every == 0) copy_weights(online_, target_);
  return UpdateStats{loss.item<double>(), q_taken.detach().mean().item<double>(), state_.updates};
}

std::shared_ptr<const Policy> Dqn::snapshot() const {
  return std::make_shared<const Policy>(clone_qnet(online_), state_);
}

json Dqn::state_json() const {
  return json{{"updates", state_.updates},
              {"reward_scale", reward_scale()},
              {"max_abs_reward", state_.max_abs_reward},
              {"reward_scale_frozen", state_.reward_frozen},
              {"net", to_string(net_cfg_.kind)},
              {"choices", net_cfg_.n_actions}};
}

void Dqn::set_weights(const Policy& policy) {
  copy_weights(policy.net(), online_);
  copy_weights(policy.net(), target_);
}

void Dqn::grow_actions(int new_n) {
  if (new_n == net_cfg_.n_actions) return;
  online_->grow_actions(new_n);
  target_->grow_actions(new_n);
  for (auto& p : target_->parameters()) p.set_requires_grad(false);
  // A new optimizer over the new parameter list; the unchanged parameters keep their Adam state.
  auto fresh = make_optimizer(online_);
  for (const auto& p : online_->parameters()) {
    auto it = opt_->state().find(p.unsafeGetTensorImpl());
    if (it != opt_->state().end()) fresh->state()[p.unsafeGetTensorImpl()] = std::move(it->second);
  }
  opt_ = std::move(fresh);
  net_cfg_.n_actions = new_n;
}

void save_model_file(const std::string& path, const QNet& online, const QNet& target, const ModelState& state) {
  const NetConfig& c = online->config();
  torch::serialize::OutputArchive archive;
  archive.write("format", torch::tensor(std::vector<int64_t>{kModelFormat}, torch::kLong));
  archive.write("net", torch::tensor(std::vector<int64_t>{static_cast<int64_t>(c.kind), c.in_c, c.in_h, c.in_w,
                                                          c.d, c.n_actions},
                                     torch::kLong));
  torch::serialize::OutputArchive online_archive(archive.compilation_unit());
  torch::serialize::OutputArchive target_archive(archive.compilation_unit());
  online->save(online_archive);
  target->save(target_archive);
  archive.write("online", online_archive);
  archive.write("target", target_archive);
  archive.write("updates", torch::tensor(std::vector<int64_t>{state.updates}, torch::kLong));
  archive.write("reward", torch::tensor(std::vector<double>{state.max_abs_reward, state.reward_frozen ? 1.0 : 0.0},
                                        torch::kDouble));
  archive.save_to(path);
}

void Dqn::save(const std::string& dir) const {
  fs::create_directories(dir);
  save_model_file((fs::path(dir) / "model.pt").string(), online_, target_, state_);
  torch::save(*opt_, (fs::path(dir) / "optim.pt").string());
}

void Dqn::load(const std::string& dir) {
  const fs::path model_path = fs::path(dir) / "model.pt";
  if (!fs::exists(model_path)) throw std::runtime_error("No saved brain in " + dir + ".");
  torch::serialize::InputArchive archive;
  archive.load_from(model_path.string(), device_);

  torch::Tensor format, net, updates, reward;
  archive.read("format", format);
  archive.read("net", net);
  if (format.numel() != 1 || format.item<int64_t>() != kModelFormat || net.numel() != 6) {
    throw std::runtime_error("The saved brain in " + dir + " was made by a different Vyrn version.");
  }
  net = net.to(torch::kCPU, torch::kLong).contiguous();
  const int64_t* v = net.data_ptr<int64_t>();
  if (v[0] < 0 || v[0] > static_cast<int64_t>(NetKind::DER)) throw std::runtime_error("Damaged brain file " + dir);
  NetConfig saved;
  saved.kind = static_cast<NetKind>(v[0]);
  saved.in_c = static_cast<int>(v[1]);
  saved.in_h = static_cast<int>(v[2]);
  saved.in_w = static_cast<int>(v[3]);
  saved.d = static_cast<int>(v[4]);
  saved.n_actions = static_cast<int>(v[5]);
  if (saved != net_cfg_) {
    throw BrainMismatch("The saved brain was built for " + describe(saved) + ", but this run needs " +
                        describe(net_cfg_) + ".");
  }

  // Load into fresh objects first, so a damaged file leaves this Dqn untouched.
  QNet online = make_qnet(net_cfg_), target = make_qnet(net_cfg_);
  online->to(device_);
  target->to(device_);
  torch::serialize::InputArchive online_archive, target_archive;
  archive.read("online", online_archive);
  archive.read("target", target_archive);
  online->load(online_archive);
  target->load(target_archive);
  for (auto& p : target->parameters()) p.set_requires_grad(false);
  archive.read("updates", updates);
  archive.read("reward", reward);
  reward = reward.to(torch::kCPU, torch::kDouble).contiguous();
  if (updates.numel() != 1 || reward.numel() != 2) throw std::runtime_error("Damaged brain file " + dir);
  ModelState state;
  state.updates = updates.item<int64_t>();
  state.max_abs_reward = reward.data_ptr<double>()[0];
  state.reward_frozen = reward.data_ptr<double>()[1] != 0.0;

  auto opt = make_optimizer(online);
  const fs::path optim_path = fs::path(dir) / "optim.pt";
  if (fs::exists(optim_path)) {
    torch::load(*opt, optim_path.string());
    // The settings in vyrn.json win over the ones saved with the optimizer.
    for (auto& group : opt->param_groups()) {
      auto& options = static_cast<torch::optim::AdamOptions&>(group.options());
      options.lr(cfg_.lr);
      options.eps(static_cast<double>(cfg_.adam_eps));
    }
  }

  online_ = online;
  target_ = target;
  opt_ = std::move(opt);
  state_ = state;
}

}  // namespace vyrn::brain
