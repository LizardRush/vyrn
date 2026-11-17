// Replay memory: a ring buffer with one stored observation per slot. Frame stacks and n-step windows are
// rebuilt by index when sampling, so pictures are stored once (uint8) instead of K times.
#pragma once

#include <torch/torch.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <random>
#include <vector>

namespace vyrn::brain {

enum class EndKind { Terminal, Truncated };

// A batch of n-step transitions (t -> t+m).
struct Batch {
  torch::Tensor obs;        // [B, K*obs]: uint8 bytes (pictures, K frames oldest..newest) or float32 (numbers)
  torch::Tensor actions;    // [B] int64
  torch::Tensor returns;    // [B] float32: sum_{k<m} gamma^k r_{t+k}, with the rewards exactly as stored
  torch::Tensor next_obs;   // like obs, for o_{t+m}
  torch::Tensor discounts;  // [B] float32: gamma^m, or 0 if the segment ended Terminal within the window
  torch::Tensor rewards;    // [B, n] float32: r_t .. r_{t+m-1} as stored, zeros after m. Dqn::update uses
                            // these to apply the frozen reward scale and the +-10 clip to each reward.
};

// Rules (global index t = the t-th observation ever added; its slot is t % capacity):
//  - A segment starts with add_obs(.., segment_start=true) and ends with end_segment(last_slot, kind). The
//    final observation has no action. Starting a new segment without end_segment() first cuts the open one
//    (Truncated) and drops its open transition, like a hold/late cut.
//  - t is sampleable iff a_t is set, o_t is not a segment's final observation, the n-step window is complete
//    inside the segment (it stops early only at the segment's end), and no frame of the stack of o_t or
//    o_{t+m} and nothing in the window is overwritten or sits at the write head (the oldest slot, which the
//    next add_obs replaces).
//  - Stacks never cross a segment start: they repeat the segment's first frame instead.
//  - n-step: m = min(n, steps to the segment end); discount = 0 if the segment ended Terminal at o_{t+m},
//    else gamma^m (a Truncated end bootstraps).
// Store raw points as rewards; Dqn::update applies the reward scale and the clip.
// Thread-safe: one thread may add while others sample (an internal mutex guards everything).
class Replay {
 public:
  Replay(int64_t capacity, size_t obs_bytes, bool obs_is_float, int stack_k, int n_step, float gamma);

  int64_t add_obs(const void* obs, bool segment_start);        // store o_t; returns its slot
  void set_transition(int64_t slot, int action, float reward);  // a_t taken at slot, r_t received after it
  void end_segment(int64_t last_slot, EndKind kind);            // last_slot holds the final obs (no action)
  int64_t size() const;                                         // observations stored (<= capacity)
  // Uniform over sampleable transitions, with replacement. Throws std::runtime_error if there is none.
  Batch sample(int batch_size, std::mt19937_64& rng) const;

  // Inspection (tests, diagnostics).
  bool is_valid(int64_t slot) const;
  std::vector<int64_t> valid_slots() const;                    // oldest first
  Batch gather(const std::vector<int64_t>& slots) const;        // throws std::invalid_argument if one isn't valid
  int64_t capacity() const { return capacity_; }
  int64_t added() const;                                        // observations ever added
  int stack_k() const { return k_; }
  int n_step() const { return n_; }

 private:
  struct Window {
    int64_t t = 0;
    int m = 0;
    float ret = 0, discount = 0;
    std::vector<int64_t> obs_frames, next_frames;   // global indices, oldest first
    std::vector<float> rewards;                     // n entries, zeros after m
  };
  enum Flag : uint8_t { kSegStart = 1, kHasAction = 2, kEnd = 4, kTerminal = 8 };

  int64_t slot_of(int64_t t) const { return t % capacity_; }
  int64_t oldest_usable() const;                    // lowest global index that may be read
  int64_t global_of(int64_t slot) const;            // throws if the slot holds nothing
  bool window_of(int64_t t, Window* w) const;       // false if t isn't sampleable now
  bool stack_of(int64_t t, int64_t lo, std::vector<int64_t>* frames) const;
  Batch make_batch(const std::vector<Window>& picked) const;

  const int64_t capacity_;
  const size_t obs_bytes_;
  const bool obs_is_float_;
  const int k_, n_;
  const float gamma_;

  mutable std::mutex mu_;
  torch::Tensor storage_;          // [capacity, obs_bytes] uint8; allocated lazily by the OS, never zeroed
  uint8_t* data_ = nullptr;
  std::vector<int64_t> stamp_;     // global index held by each slot (-1 = empty)
  std::vector<uint8_t> flags_;
  std::vector<int32_t> action_;
  std::vector<float> reward_;
  int64_t written_ = 0;
};

}  // namespace vyrn::brain
