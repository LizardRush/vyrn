#include "run/brain_host.h"

#include "util/clock.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <utility>

#ifndef VYRN_VERSION
#define VYRN_VERSION "0.0.0"
#endif

namespace vyrn::run {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// A fresh brain id (the local time, "-2", "-3"... when taken). Its folder is created now so no other run can
// take the same id.
std::string reserve_new_id(const std::string& project_dir) {
  const fs::path root = brains_dir(project_dir);
  try {
    fs::create_directories(root);
    const std::string base = util::local_stamp();
    for (int n = 1; n < 1000; ++n) {
      const std::string id = n == 1 ? base : base + "-" + std::to_string(n);
      if (fs::create_directory(root / id)) return id;
    }
  } catch (const fs::filesystem_error& e) {
    throw std::runtime_error("Vyrn can't create a folder for the new brain in " + root.string() + " (" +
                             e.code().message() + ").");
  }
  throw std::runtime_error("Vyrn can't find a free name for a new brain in " + root.string() + ".");
}

bool read_shape(const json& meta, brain::BrainShape& shape) {
  try {
    shape = meta.at("shape").get<brain::BrainShape>();
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

std::string with_space(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  return a + " " + b;
}

}  // namespace

std::string brains_dir(const std::string& project_dir) { return (fs::path(project_dir) / "brains").string(); }

BrainPlan plan_check(const std::string& project_dir, const std::string& active_id, const brain::Spec& spec,
                     int frames) {
  BrainPlan plan;
  plan.status = "new";
  plan.shape = brain::make_brain_shape(spec, frames);
  if (active_id.empty()) return plan;

  const std::string dir = (fs::path(brains_dir(project_dir)) / active_id).string();
  brain::BrainMeta meta;
  bool have = false;
  try {
    have = brain::read_checkpoint_meta(dir, "latest", meta);
  } catch (const std::exception&) {
    have = false;
  }
  if (!have) return plan;   // nothing saved yet: a new brain
  brain::BrainShape saved;
  if (!read_shape(meta.j, saved)) {
    plan.status = "incompatible";
    plan.why = "The saved brain " + active_id + " is damaged, so this starts a new brain.";
    return plan;
  }
  std::string why;
  const brain::Compat compat = brain::compare(saved, spec, &why);
  if (compat == brain::Compat::Same) {
    plan.id = active_id;
    plan.status = "resumed";
    plan.load = true;
    plan.which = "latest";
    plan.shape = saved;
    if (saved.kind == brain::ObsSpec::Kind::Picture && saved.frames != frames) {
      why = with_space(why, "This brain keeps its " + std::to_string(saved.frames) +
                                " stacked frames (the frames setting only changes new brains).");
    }
    plan.why = why;
    return plan;
  }
  if (compat == brain::Compat::AppendedChoices) {
    why = with_space(why, "For now that starts a new brain (growing a brain for new choices comes later).");
  }
  plan.status = "incompatible";
  plan.why = why;
  return plan;
}

BrainPlan plan_training(const std::string& project_dir, const std::string& active_id, bool start_over,
                        const brain::Spec& spec, int frames) {
  BrainPlan plan;
  if (start_over) {
    plan.status = "new";
    plan.why = active_id.empty() ? "" : "Starting over: this is a new brain (the old one stays in brains/).";
    plan.shape = brain::make_brain_shape(spec, frames);
  } else {
    plan = plan_check(project_dir, active_id, spec, frames);
  }
  if (!plan.load) plan.id = reserve_new_id(project_dir);
  return plan;
}

BrainPlan plan_watching(const std::string& project_dir, const std::string& active_id, const std::string& which,
                        const brain::Spec& spec) {
  if (active_id.empty()) {
    throw BrainUnavailable("no_brain", "This project has no trained brain yet, so there is nothing to watch.",
                           "Train it first; Watch then plays the best brain.");
  }
  const std::string dir = (fs::path(brains_dir(project_dir)) / active_id).string();
  brain::BrainMeta meta;
  std::string used = which == "latest" ? "latest" : "best";
  std::string note;
  bool have = false;
  try {
    have = brain::read_checkpoint_meta(dir, used, meta);
    if (!have && used == "best") {
      have = brain::read_checkpoint_meta(dir, "latest", meta);
      used = "latest";
      note = "There is no best brain yet (it needs 20 rounds of training), so Vyrn watches the latest one.";
    }
  } catch (const std::exception&) {
    have = false;
  }
  if (!have) {
    throw BrainUnavailable("no_brain", "The brain " + active_id + " has no saved checkpoint yet.",
                           "Train it for a little while first.");
  }
  BrainPlan plan;
  if (!read_shape(meta.j, plan.shape)) {
    throw BrainUnavailable("brain_mismatch", "The saved brain " + active_id + " is damaged.",
                           "Train to make a new brain.");
  }
  std::string why;
  if (brain::compare(plan.shape, spec, &why) != brain::Compat::Same) {
    throw BrainUnavailable("brain_mismatch", "This brain was trained for different scripts. " + why,
                           "Undo the change to watch it, or Train to make a new brain.");
  }
  plan.id = active_id;
  plan.status = "resumed";
  plan.why = with_space(note, why);
  plan.load = true;
  plan.which = used;
  return plan;
}

// ---- BrainHost ---------------------------------------------------------------------------------------------

BrainHost::BrainHost(const std::string& project_dir, const brain::Spec& spec, const BrainPlan& plan,
                     brain::DqnConfig cfg, bool training, uint64_t seed)
    : project_dir_(project_dir),
      id_(plan.id),
      dir_((fs::path(brains_dir(project_dir)) / plan.id).string()),
      spec_(spec),
      shape_(plan.shape),
      cfg_(cfg),
      training_(training),
      picture_(plan.shape.kind == brain::ObsSpec::Kind::Picture),
      rng_(seed),
      learn_rng_(seed ^ 0x9e3779b97f4a7c15ULL) {
  cfg_.frames = picture_ ? shape_.frames : 1;
  net_cfg_ = brain::net_config_for(shape_);
  torch::manual_seed(seed);
  dqn_ = std::make_unique<brain::Dqn>(cfg_, net_cfg_);

  json saved;
  if (plan.load) {
    brain::BrainMeta meta;
    if (!brain::load_checkpoint(dir_, plan.which, *dqn_, meta)) {
      throw std::runtime_error("Vyrn couldn't load the saved brain " + id_ + " (" + plan.which + ").");
    }
    saved = std::move(meta.j);
    const json counters = saved.value("counters", json::object());
    saved_counters_.ticks = counters.value("ticks", int64_t{0});
    saved_counters_.rounds = counters.value("rounds", int64_t{0});
    saved_counters_.seconds = counters.value("seconds", 0.0);
    auto best = saved.find("best_mean20");
    if (best != saved.end() && best->is_number()) saved_best_ = best->get<double>();
    // An imported brain (.vrl) says where it came from: its name, the "imported" file/author/date and the points
    // chart it came with. Every later save keeps them (docs/dev/package.md section 6).
    for (const char* key : {"name", "imported", "history"}) {
      if (auto it = saved.find(key); it != saved.end()) kept_meta_[key] = *it;
    }
  }
  updates_.store(dqn_->updates());

  if (picture_) {
    geo_ = brain::Geometry{shape_.small_picture, shape_.in_h, shape_.in_w};
    frame_bytes_ = static_cast<size_t>(3) * static_cast<size_t>(geo_.h) * static_cast<size_t>(geo_.w);
    frame_.assign(frame_bytes_, 0);
    stack_ = std::make_unique<brain::FrameStack>(cfg_.frames, frame_bytes_);
  } else {
    rms_ = std::make_unique<brain::RunningMeanStd>(shape_.d);
    raw_.assign(static_cast<size_t>(shape_.d), 0.0f);
    norm_.assign(static_cast<size_t>(shape_.d), 0.0f);
    learn_norm_.assign(static_cast<size_t>(shape_.d), 0.0f);
    auto stats = saved.is_object() ? saved.find("normalizer") : saved.end();
    if (saved.is_object() && stats != saved.end() && stats->is_object()) {
      try {
        rms_->from_json(*stats);
      } catch (const std::exception& e) {
        throw std::runtime_error(std::string("The saved brain's number statistics don't fit: ") + e.what());
      }
    }
    rms_->frozen = !training_;
  }
  if (training_) {
    const size_t obs_bytes = picture_ ? frame_bytes_ : static_cast<size_t>(shape_.d) * sizeof(float);
    replay_ = std::make_unique<brain::Replay>(cfg_.buffer, obs_bytes, !picture_, picture_ ? cfg_.frames : 1,
                                              cfg_.n_step, cfg_.gamma);
  }
}

void BrainHost::preprocess(const ObsView& obs, bool update_stats) {
  if (obs.picture != picture_) {
    throw std::runtime_error(picture_ ? "see() returned numbers, but this brain sees pictures."
                                      : "see() returned a picture, but this brain sees numbers.");
  }
  if (picture_) {
    brain::to_frame(obs.rgb, obs.h, obs.w, geo_, frame_.data());
    return;
  }
  if (static_cast<int>(obs.numbers.size()) != shape_.d) {
    throw std::runtime_error("see() returned " + std::to_string(obs.numbers.size()) +
                             " numbers, but this brain expects " + std::to_string(shape_.d) + ".");
  }
  raw_ = obs.numbers;
  if (update_stats && training_) {
    std::lock_guard<std::mutex> lock(stats_mu_);
    rms_->update(raw_.data(), 1);
  }
}

const void* BrainHost::stored_obs() const {
  return picture_ ? static_cast<const void*>(frame_.data()) : static_cast<const void*>(raw_.data());
}

void BrainHost::start_segment(const ObsView& obs) {
  preprocess(obs, true);
  if (picture_) stack_->reset(frame_.data());
  if (training_) slot_ = replay_->add_obs(stored_obs(), true);
}

void BrainHost::continue_segment(const ObsView& obs) {
  preprocess(obs, true);
  if (picture_) stack_->push(frame_.data());
  if (training_) slot_ = replay_->add_obs(stored_obs(), false);
}

void BrainHost::end_segment(const ObsView* final_obs, bool terminal) {
  if (!training_) return;
  if (final_obs) {
    preprocess(*final_obs, true);
    const int64_t last = replay_->add_obs(stored_obs(), false);
    replay_->end_segment(last, terminal ? brain::EndKind::Terminal : brain::EndKind::Truncated);
  } else if (slot_ >= 0) {
    replay_->end_segment(slot_, brain::EndKind::Truncated);   // no final observation: drop the open move
  }
  slot_ = -1;
}

int BrainHost::act(float epsilon, std::vector<float>* probs) {
  torch::Tensor input;
  if (picture_) {
    const std::vector<uint8_t>& stacked = stack_->stacked();
    input = torch::from_blob(const_cast<uint8_t*>(stacked.data()), {1, static_cast<int64_t>(stacked.size())},
                             torch::kUInt8);
  } else {
    {
      std::lock_guard<std::mutex> lock(stats_mu_);
      rms_->normalize(raw_.data(), norm_.data());
    }
    input = torch::from_blob(norm_.data(), {1, static_cast<int64_t>(norm_.size())}, torch::kFloat);
  }
  if (concurrent_) return policy()->act(input, epsilon, rng_, probs);
  return dqn_->act(input, epsilon, rng_, probs);
}

void BrainHost::record(int action, float points) {
  if (!training_ || slot_ < 0) return;
  replay_->set_transition(slot_, action, points);
  if (concurrent_) {
    // The learner owns the network: it hands the largest |points| to the reward scale before an update.
    const float magnitude = std::fabs(points);
    float seen = warmup_max_abs_.load();
    while (magnitude > seen && !warmup_max_abs_.compare_exchange_weak(seen, magnitude)) {
    }
    return;
  }
  if (!dqn_->reward_scale_frozen()) dqn_->observe_warmup_reward(points);
}

int BrainHost::learn(int64_t session_ticks) {
  if (!training_ || session_ticks < cfg_.learning_starts) return 0;
  update_debt_ += cfg_.updates_per_tick;
  int done = 0;
  while (update_debt_ >= 1.0) {
    update_debt_ -= 1.0;
    brain::Batch batch;
    try {
      batch = replay_->sample(cfg_.batch, rng_);
    } catch (const std::runtime_error&) {
      update_debt_ = 0;   // nothing complete to learn from yet
      break;
    }
    if (!picture_) normalize_batch(batch, norm_);
    count_update(dqn_->update(batch));
    ++done;
  }
  return done;
}

void BrainHost::normalize_batch(brain::Batch& batch, std::vector<float>& scratch) const {
  // Stored numbers are raw: scale them with today's statistics, like the network input when acting.
  std::lock_guard<std::mutex> lock(stats_mu_);
  for (torch::Tensor* t : {&batch.obs, &batch.next_obs}) {
    *t = t->contiguous();
    float* rows = t->data_ptr<float>();
    const int64_t n = t->size(0);
    const size_t d = static_cast<size_t>(shape_.d);
    for (int64_t r = 0; r < n; ++r) {
      float* row = rows + static_cast<size_t>(r) * d;
      rms_->normalize(row, scratch.data());
      std::memcpy(row, scratch.data(), d * sizeof(float));
    }
  }
}

void BrainHost::count_update(const brain::UpdateStats& stats) {
  updates_.store(dqn_->updates());
  std::lock_guard<std::mutex> lock(loss_mu_);
  loss_sum_ += stats.loss;
  q_sum_ += stats.q_mean;
  ++loss_count_;
}

// ---- real time: actor and learner --------------------------------------------------------------------------

void BrainHost::share_with_learner() {
  if (!training_) return;
  concurrent_ = true;
  publish_snapshot();
}

std::shared_ptr<const brain::Policy> BrainHost::policy() const {
  std::lock_guard<std::mutex> lock(policy_mu_);
  return policy_;
}

void BrainHost::publish_snapshot() {
  std::shared_ptr<const brain::Policy> fresh = dqn_->snapshot();
  std::lock_guard<std::mutex> lock(policy_mu_);
  policy_ = std::move(fresh);
}

uint64_t BrainHost::policy_version() const {
  const auto p = policy();
  return p ? p->version : static_cast<uint64_t>(updates());
}

bool BrainHost::learn_once() {
  if (!training_) return false;
  brain::Batch batch;
  try {
    batch = replay_->sample(cfg_.batch, learn_rng_);
  } catch (const std::runtime_error&) {
    return false;   // nothing complete to learn from yet
  }
  if (!picture_) normalize_batch(batch, learn_norm_);
  if (!dqn_->reward_scale_frozen()) dqn_->observe_warmup_reward(warmup_max_abs_.load());
  count_update(dqn_->update(batch));
  return true;
}

double BrainHost::take_loss_sum(int64_t* count, double* q_sum) {
  std::lock_guard<std::mutex> lock(loss_mu_);
  const double loss = loss_sum_;
  if (count) *count = loss_count_;
  if (q_sum) *q_sum = q_sum_;
  loss_sum_ = q_sum_ = 0;
  loss_count_ = 0;
  return loss;
}

void BrainHost::fill_live(LiveView& view) const {
  if (picture_) {
    view.kind = "picture";
    view.input = picture_from_chw(stack_->newest(), geo_.h, geo_.w);
    view.numbers.clear();
    view.number_names.clear();
    return;
  }
  view.kind = "numbers";
  view.input = Picture{};
  view.numbers.assign(raw_.size(), 0.0f);
  {
    std::lock_guard<std::mutex> lock(stats_mu_);
    rms_->normalize(raw_.data(), view.numbers.data());
  }
  view.number_names = spec_.obs.names;
  if (view.number_names.size() != view.numbers.size()) {
    view.number_names.clear();
    for (size_t i = 0; i < view.numbers.size(); ++i) view.number_names.push_back("input " + std::to_string(i + 1));
  }
}

json BrainHost::meta(const Counters& counters, std::optional<double> best_mean20) const {
  json m = json::object();
  m["format"] = 1;
  m["vyrn"] = VYRN_VERSION;
  m["brain"] = id_;
  m["shape"] = shape_;
  m["net"] = brain::to_string(net_cfg_.kind);
  m["net_description"] = brain::describe(net_cfg_);
  m["config"] = cfg_;
  m["obs"] = spec_.raw.value("obs", json::object());
  m["choices"] = spec_.choices;
  m["labels"] = spec_.labels;
  m["effects"] = spec_.effects;
  m["counters"] = {{"ticks", counters.ticks},
                   {"rounds", counters.rounds},
                   {"updates", updates()},
                   {"seconds", counters.seconds}};
  if (rms_) {
    std::lock_guard<std::mutex> lock(stats_mu_);
    m["normalizer"] = rms_->to_json();
  } else {
    m["normalizer"] = nullptr;
  }
  m["best_mean20"] = best_mean20 ? json(*best_mean20) : json(nullptr);
  for (const auto& item : kept_meta_.items()) m[item.key()] = item.value();   // name, imported, history
  m["saved"] = util::local_iso_time();
  return m;
}

std::string BrainHost::save_latest(const Counters& counters, std::optional<double> best_mean20) {
  brain::save_checkpoint(dir_, "latest", *dqn_, brain::BrainMeta{meta(counters, best_mean20)});
  return (fs::path(dir_) / "latest").string();
}

void BrainHost::keep_best(const Counters& counters, double best_mean20) {
  std::shared_ptr<const brain::Policy> weights = concurrent_ ? policy() : dqn_->snapshot();
  json m = meta(counters, best_mean20);
  std::lock_guard<std::mutex> lock(best_mu_);
  best_policy_ = std::move(weights);
  best_meta_ = std::move(m);
}

bool BrainHost::best_pending() const {
  std::lock_guard<std::mutex> lock(best_mu_);
  return best_policy_ != nullptr;
}

std::string BrainHost::write_best() {
  std::shared_ptr<const brain::Policy> weights;
  json m;
  {
    std::lock_guard<std::mutex> lock(best_mu_);
    weights = best_policy_;
    m = best_meta_;
  }
  if (!weights) return "";
  brain::save_checkpoint(dir_, "best", *weights, brain::BrainMeta{m});   // throws: it stays pending
  {
    std::lock_guard<std::mutex> lock(best_mu_);
    if (best_policy_ == weights) best_policy_.reset();   // a newer best (real time) stays pending
  }
  return (fs::path(dir_) / "best").string();
}

}  // namespace vyrn::run
