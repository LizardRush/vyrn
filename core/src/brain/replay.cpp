#include "brain/replay.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace vyrn::brain {

Replay::Replay(int64_t capacity, size_t obs_bytes, bool obs_is_float, int stack_k, int n_step, float gamma)
    : capacity_(capacity), obs_bytes_(obs_bytes), obs_is_float_(obs_is_float), k_(stack_k), n_(n_step),
      gamma_(gamma) {
  if (capacity < 2) throw std::invalid_argument("The replay buffer needs room for at least 2 observations.");
  if (obs_bytes == 0) throw std::invalid_argument("Replay: empty observations");
  if (obs_is_float && obs_bytes % sizeof(float) != 0) throw std::invalid_argument("Replay: odd float size");
  if (stack_k < 1 || n_step < 1) throw std::invalid_argument("Replay: stack_k and n_step must be at least 1");
  storage_ = torch::empty({capacity, static_cast<int64_t>(obs_bytes)}, torch::kUInt8);
  data_ = storage_.data_ptr<uint8_t>();
  stamp_.assign(static_cast<size_t>(capacity), -1);
  flags_.assign(static_cast<size_t>(capacity), 0);
  action_.assign(static_cast<size_t>(capacity), -1);
  reward_.assign(static_cast<size_t>(capacity), 0.0f);
}

int64_t Replay::oldest_usable() const {
  // The slot at the write head holds global index written_ - capacity_: the next add replaces it.
  return std::max<int64_t>(0, written_ - capacity_ + 1);
}

int64_t Replay::global_of(int64_t slot) const {
  if (slot < 0 || slot >= capacity_ || stamp_[slot] < 0) {
    throw std::out_of_range("Replay: slot " + std::to_string(slot) + " holds no observation");
  }
  return stamp_[slot];
}

int64_t Replay::add_obs(const void* obs, bool segment_start) {
  std::lock_guard<std::mutex> lock(mu_);
  const int64_t t = written_;
  const int64_t slot = slot_of(t);
  bool start = segment_start || t == 0;
  if (t > 0) {
    uint8_t& prev = flags_[slot_of(t - 1)];
    if (prev & kEnd) {
      start = true;   // whatever follows a segment's end begins a new one
    } else if (start) {
      // A cut without end_segment(): the open segment ends Truncated and its open transition is dropped.
      prev = static_cast<uint8_t>((prev | kEnd) & ~(kTerminal | kHasAction));
    }
  }
  std::memcpy(data_ + slot * obs_bytes_, obs, obs_bytes_);
  stamp_[slot] = t;
  flags_[slot] = start ? kSegStart : 0;
  action_[slot] = -1;
  reward_[slot] = 0.0f;
  written_ = t + 1;
  return slot;
}

void Replay::set_transition(int64_t slot, int action, float reward) {
  if (action < 0) throw std::invalid_argument("Replay: negative action");
  std::lock_guard<std::mutex> lock(mu_);
  global_of(slot);
  action_[slot] = action;
  reward_[slot] = reward;
  flags_[slot] |= kHasAction;
}

void Replay::end_segment(int64_t last_slot, EndKind kind) {
  std::lock_guard<std::mutex> lock(mu_);
  const int64_t t = global_of(last_slot);
  uint8_t f = static_cast<uint8_t>((flags_[last_slot] | kEnd) & ~(kTerminal | kHasAction));
  if (kind == EndKind::Terminal) f |= kTerminal;
  flags_[last_slot] = f;
  // Observations already stored after the final one belong to the next segment.
  if (t + 1 < written_) flags_[slot_of(t + 1)] |= kSegStart;
}

int64_t Replay::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return std::min(written_, capacity_);
}

int64_t Replay::added() const {
  std::lock_guard<std::mutex> lock(mu_);
  return written_;
}

bool Replay::stack_of(int64_t t, int64_t lo, std::vector<int64_t>* frames) const {
  frames->assign(static_cast<size_t>(k_), t);
  int64_t cur = t;
  for (int j = 1; j < k_; ++j) {
    if (!(flags_[slot_of(cur)] & kSegStart)) {
      --cur;
      if (cur < lo) return false;   // that frame is overwritten (or about to be)
    }
    (*frames)[static_cast<size_t>(k_ - 1 - j)] = cur;
  }
  return true;
}

bool Replay::window_of(int64_t t, Window* w) const {
  const int64_t lo = oldest_usable();
  if (t < lo || t >= written_) return false;
  const uint8_t ft = flags_[slot_of(t)];
  if (!(ft & kHasAction) || (ft & kEnd)) return false;

  w->t = t;
  w->rewards.assign(static_cast<size_t>(n_), 0.0f);
  double ret = 0, discount = 1;
  bool terminal = false;
  int m = 0;
  for (int k = 1; k <= n_; ++k) {
    const int64_t u = t + k;
    if (u >= written_) return false;              // o_{t+k} isn't stored yet
    const uint8_t fu = flags_[slot_of(u)];
    if (fu & kSegStart) return false;             // never cross into another segment
    const float r = reward_[slot_of(u - 1)];
    w->rewards[static_cast<size_t>(k - 1)] = r;
    ret += discount * r;
    discount *= gamma_;
    if (fu & kEnd) {                              // the segment ends at o_{t+k}
      m = k;
      terminal = (fu & kTerminal) != 0;
      break;
    }
    if (k == n_) {
      m = n_;
      break;
    }
    if (!(fu & kHasAction)) return false;         // the window isn't complete yet
  }
  w->m = m;
  w->ret = static_cast<float>(ret);
  w->discount = terminal ? 0.0f : static_cast<float>(discount);
  return stack_of(t, lo, &w->obs_frames) && stack_of(t + m, lo, &w->next_frames);
}

bool Replay::is_valid(int64_t slot) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (slot < 0 || slot >= capacity_ || stamp_[slot] < 0) return false;
  Window w;
  return window_of(stamp_[slot], &w);
}

std::vector<int64_t> Replay::valid_slots() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<int64_t> out;
  Window w;
  for (int64_t t = oldest_usable(); t < written_; ++t) {
    if (window_of(t, &w)) out.push_back(slot_of(t));
  }
  return out;
}

Batch Replay::gather(const std::vector<int64_t>& slots) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<Window> picked(slots.size());
  for (size_t i = 0; i < slots.size(); ++i) {
    const int64_t slot = slots[i];
    if (slot < 0 || slot >= capacity_ || stamp_[slot] < 0 || !window_of(stamp_[slot], &picked[i])) {
      throw std::invalid_argument("Replay::gather: slot " + std::to_string(slot) + " isn't sampleable");
    }
  }
  return make_batch(picked);
}

Batch Replay::sample(int batch_size, std::mt19937_64& rng) const {
  if (batch_size < 1) throw std::invalid_argument("Replay::sample: batch_size must be at least 1");
  std::lock_guard<std::mutex> lock(mu_);
  const int64_t lo = oldest_usable();
  if (written_ == 0 || lo >= written_) {
    throw std::runtime_error("The replay memory is empty, so there is nothing to learn from yet.");
  }
  std::vector<Window> picked;
  picked.reserve(static_cast<size_t>(batch_size));
  // Rejection sampling: almost every stored transition is valid, so this rarely retries.
  std::uniform_int_distribution<int64_t> any_index(lo, written_ - 1);
  Window w;
  for (int attempts = 64 * batch_size + 256; attempts > 0 && static_cast<int>(picked.size()) < batch_size;
       --attempts) {
    if (window_of(any_index(rng), &w)) picked.push_back(w);
  }
  if (static_cast<int>(picked.size()) < batch_size) {
    // Few valid transitions (just after a start): pick uniformly from the full list.
    std::vector<Window> all;
    for (int64_t t = lo; t < written_; ++t) {
      if (window_of(t, &w)) all.push_back(w);
    }
    if (all.empty()) throw std::runtime_error("There are no complete moves to learn from yet.");
    std::uniform_int_distribution<size_t> any_valid(0, all.size() - 1);
    while (static_cast<int>(picked.size()) < batch_size) picked.push_back(all[any_valid(rng)]);
  }
  return make_batch(picked);
}

Batch Replay::make_batch(const std::vector<Window>& picked) const {
  const int64_t b = static_cast<int64_t>(picked.size());
  const int64_t row_bytes = static_cast<int64_t>(obs_bytes_) * k_;
  Batch out;
  out.obs = torch::empty({b, row_bytes}, torch::kUInt8);
  out.next_obs = torch::empty({b, row_bytes}, torch::kUInt8);
  out.actions = torch::empty({b}, torch::kLong);
  out.returns = torch::empty({b}, torch::kFloat);
  out.discounts = torch::empty({b}, torch::kFloat);
  out.rewards = torch::empty({b, n_}, torch::kFloat);
  uint8_t* obs = out.obs.data_ptr<uint8_t>();
  uint8_t* next = out.next_obs.data_ptr<uint8_t>();
  int64_t* actions = out.actions.data_ptr<int64_t>();
  float* returns = out.returns.data_ptr<float>();
  float* discounts = out.discounts.data_ptr<float>();
  float* rewards = out.rewards.data_ptr<float>();
  for (int64_t i = 0; i < b; ++i) {
    const Window& w = picked[static_cast<size_t>(i)];
    for (int j = 0; j < k_; ++j) {
      std::memcpy(obs + (i * k_ + j) * obs_bytes_, data_ + slot_of(w.obs_frames[j]) * obs_bytes_, obs_bytes_);
      std::memcpy(next + (i * k_ + j) * obs_bytes_, data_ + slot_of(w.next_frames[j]) * obs_bytes_, obs_bytes_);
    }
    actions[i] = action_[slot_of(w.t)];
    returns[i] = w.ret;
    discounts[i] = w.discount;
    std::copy(w.rewards.begin(), w.rewards.end(), rewards + i * n_);
  }
  if (obs_is_float_) {
    // Same bytes, viewed as float32 numbers: [B, K * obs_bytes / 4].
    const int64_t floats = row_bytes / static_cast<int64_t>(sizeof(float));
    out.obs = out.obs.view(torch::kFloat).reshape({b, floats});
    out.next_obs = out.next_obs.view(torch::kFloat).reshape({b, floats});
  }
  return out;
}

}  // namespace vyrn::brain
