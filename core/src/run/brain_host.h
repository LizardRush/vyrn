// The brain side of a run: which brain to use (new, resumed or the one to watch), turning observations into
// network input, acting, learning, and saving checkpoints (PLAN.md section 3).
// A simulation does everything on its brain thread. Real time splits it (PLAN.md section 2): after
// share_with_learner(), an actor thread plays on a Policy snapshot while a learner thread owns the network.
#pragma once

#include "brain/checkpoint.h"
#include "brain/dqn.h"
#include "brain/preprocess.h"
#include "brain/replay.h"
#include "brain/spec.h"
#include "run/observation.h"
#include "vyrn/session.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace vyrn::run {

// Which brain a run uses, decided from vyrn.json's "brain" and the project's brains/ folder.
struct BrainPlan {
  std::string id;               // folder name in brains/
  std::string status;           // "new" | "resumed" | "incompatible" (a new brain because the scripts changed)
  std::string why;              // plain English: why a new brain, or a note about the saved one ("" if none)
  bool load = false;            // load brains/<id>/<which>
  std::string which = "latest";
  brain::BrainShape shape;      // the network's input and choices
};

// There is no brain to watch, or it doesn't fit the scripts any more.
struct BrainUnavailable : std::runtime_error {
  BrainUnavailable(const std::string& reason, const std::string& message, const std::string& hint)
      : std::runtime_error(message), reason(reason), hint(hint) {}
  std::string reason;   // "no_brain" | "brain_mismatch"
  std::string hint;
};

std::string brains_dir(const std::string& project_dir);

// Train: resume the active brain when it fits the scripts, else a new brain (whose folder is created now so
// the id is taken). `frames` = stacked frames for a new picture brain.
BrainPlan plan_training(const std::string& project_dir, const std::string& active_id, bool start_over,
                        const brain::Spec& spec, int frames);
// Check: what Train would do, without creating anything ("new" brains get no id).
BrainPlan plan_check(const std::string& project_dir, const std::string& active_id, const brain::Spec& spec,
                     int frames);
// Watch: the active brain's `which` checkpoint ("best" falls back to "latest"). Throws BrainUnavailable.
BrainPlan plan_watching(const std::string& project_dir, const std::string& active_id, const std::string& which,
                        const brain::Spec& spec);

// Totals over every run of a brain (stored in brain.json).
struct Counters {
  int64_t ticks = 0;
  int64_t rounds = 0;
  double seconds = 0;   // training time
};

class BrainHost {
 public:
  // Builds the network for `plan` and loads its checkpoint when plan.load (throws std::runtime_error).
  // training = false (Watch): no replay memory, frozen number statistics, nothing is saved.
  BrainHost(const std::string& project_dir, const brain::Spec& spec, const BrainPlan& plan, brain::DqnConfig cfg,
            bool training, uint64_t seed);

  const brain::BrainShape& shape() const { return shape_; }
  const brain::DqnConfig& config() const { return cfg_; }
  std::string net_description() const { return brain::describe(net_cfg_); }
  const Counters& saved_counters() const { return saved_counters_; }   // zeros for a new brain
  std::optional<double> saved_best() const { return saved_best_; }
  const std::string& dir() const { return dir_; }

  // ---- observations of one segment (a round, or the part of it since a restart of Python)
  void start_segment(const ObsView& obs);
  void continue_segment(const ObsView& obs);   // the observation after an action
  void end_segment(const ObsView* final_obs, bool terminal);   // final_obs: the round's last observation

  // ---- acting on the current observation, then what that action scored
  int act(float epsilon, std::vector<float>* probs);
  void record(int action, float points);

  // ---- learning (training only): the updates owed after `session_ticks` ticks. Returns how many ran.
  int learn(int64_t session_ticks);
  int64_t updates() const { return updates_.load(); }
  double take_loss_sum(int64_t* count, double* q_sum);   // sums since the last call (for update events)

  // ---- real time (training only). Call share_with_learner() once, before the learner thread starts. Then:
  //   actor thread:   start/continue/end_segment, act (on the latest published Policy), record, keep_best,
  //                   fill_live, best_pending
  //   learner thread: learn_once, publish_snapshot, save_latest, write_best
  // updates() and take_loss_sum() may be called from either.
  void share_with_learner();
  bool learn_once();         // one update from the replay; false (nothing done) when it has no sample yet
  void publish_snapshot();   // act() plays with the network's current weights from now on
  uint64_t policy_version() const;

  void fill_live(LiveView& view) const;   // kind, input / numbers

  // ---- checkpoints (training only). Each returns the folder it wrote.
  std::string save_latest(const Counters& counters, std::optional<double> best_mean20);
  // A copy of the weights in RAM (real time: the snapshot the actor plays with, which scored the best).
  void keep_best(const Counters& counters, double best_mean20);
  bool best_pending() const;
  std::string write_best();   // writes the copy kept by keep_best() ("" when none is pending)

 private:
  void preprocess(const ObsView& obs, bool update_stats);   // fills frame_ (pictures) or raw_ (numbers)
  const void* stored_obs() const;
  nlohmann::json meta(const Counters& counters, std::optional<double> best_mean20) const;
  void normalize_batch(brain::Batch& batch, std::vector<float>& scratch) const;   // numbers: raw -> network input
  void count_update(const brain::UpdateStats& stats);
  std::shared_ptr<const brain::Policy> policy() const;

  std::string project_dir_, id_, dir_;
  brain::Spec spec_;
  brain::BrainShape shape_;
  brain::DqnConfig cfg_;
  brain::NetConfig net_cfg_;
  bool training_;
  bool picture_;
  std::unique_ptr<brain::Dqn> dqn_;
  std::unique_ptr<brain::Replay> replay_;
  std::mt19937_64 rng_;

  // pictures
  brain::Geometry geo_;
  size_t frame_bytes_ = 0;
  std::vector<uint8_t> frame_;
  std::unique_ptr<brain::FrameStack> stack_;
  // numbers
  std::unique_ptr<brain::RunningMeanStd> rms_;
  std::vector<float> raw_, norm_;

  int64_t slot_ = -1;   // replay slot of the current observation
  double update_debt_ = 0;
  std::atomic<int64_t> updates_{0};   // dqn_->updates(), readable from any thread
  mutable std::mutex loss_mu_;
  double loss_sum_ = 0, q_sum_ = 0;
  int64_t loss_count_ = 0;

  Counters saved_counters_;
  std::optional<double> saved_best_;
  nlohmann::json kept_meta_ = nlohmann::json::object();   // brain.json keys every save keeps (an imported brain's)
  mutable std::mutex best_mu_;
  std::shared_ptr<const brain::Policy> best_policy_;
  nlohmann::json best_meta_;

  // real time: what the actor and the learner share besides the (thread-safe) replay memory
  bool concurrent_ = false;
  mutable std::mutex stats_mu_;                      // rms_: the actor updates it, the learner scales batches
  mutable std::mutex policy_mu_;
  std::shared_ptr<const brain::Policy> policy_;      // the weights the actor plays with
  std::atomic<float> warmup_max_abs_{0.0f};          // largest |points| the actor saw (the reward scale)
  std::mt19937_64 learn_rng_;
  std::vector<float> learn_norm_;
};

}  // namespace vyrn::run
