// Replay memory: segment starts, Terminal/Truncated ends, n-step windows, wrap-around and the write head.
// Most tests use Replay(capacity 8, stack 3, n 2, gamma 0.5) with one-byte observations that name themselves.
#include "brain/replay.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <thread>

using namespace vyrn::brain;

namespace {

constexpr float kGamma = 0.5f;   // powers of 1/2 keep every hand-computed sum exact

struct Tiny {
  Replay r{8, 1, false, 3, 2, kGamma};
  int64_t add(char c, bool start = false) {
    const uint8_t b = static_cast<uint8_t>(c);
    return r.add_obs(&b, start);
  }
};

std::string row_text(const torch::Tensor& t, int64_t row) {
  torch::Tensor r = t[row].contiguous();
  const uint8_t* p = r.data_ptr<uint8_t>();
  return std::string(reinterpret_cast<const char*>(p), static_cast<size_t>(r.numel()));
}

struct Expect {
  std::string obs, next;
  int action;
  float ret, discount;
  std::vector<float> rewards;
};

void check_batch(const Batch& b, const std::vector<Expect>& want) {
  ASSERT_EQ(b.obs.size(0), static_cast<int64_t>(want.size()));
  for (size_t i = 0; i < want.size(); ++i) {
    SCOPED_TRACE("row " + std::to_string(i) + " (" + want[i].obs + ")");
    const int64_t r = static_cast<int64_t>(i);
    EXPECT_EQ(row_text(b.obs, r), want[i].obs);
    EXPECT_EQ(row_text(b.next_obs, r), want[i].next);
    EXPECT_EQ(b.actions[r].item<int64_t>(), want[i].action);
    EXPECT_FLOAT_EQ(b.returns[r].item<float>(), want[i].ret);
    EXPECT_FLOAT_EQ(b.discounts[r].item<float>(), want[i].discount);
    for (size_t k = 0; k < want[i].rewards.size(); ++k) {
      EXPECT_FLOAT_EQ(b.rewards[r][static_cast<int64_t>(k)].item<float>(), want[i].rewards[k]);
    }
  }
}

}  // namespace

TEST(Replay, StacksAtASegmentStartRepeatTheFirstFrame) {
  Tiny t;
  const int64_t a0 = t.add('a', true);
  const int64_t a1 = t.add('b');
  t.r.set_transition(a0, 1, 1.0f);
  const int64_t a2 = t.add('c');
  t.r.set_transition(a1, 0, 2.0f);
  t.add('d');
  t.r.set_transition(a2, 1, 4.0f);
  // a2's window would need d's action (not known yet); d has no action.
  EXPECT_EQ(t.r.valid_slots(), (std::vector<int64_t>{a0, a1}));
  EXPECT_EQ(t.r.size(), 4);
  check_batch(t.r.gather({a0, a1}), {
                                        {"aaa", "abc", 1, 1 + 0.5f * 2, 0.25f, {1, 2}},
                                        {"aab", "bcd", 0, 2 + 0.5f * 4, 0.25f, {2, 4}},
                                    });
  EXPECT_THROW(t.r.gather({a2}), std::invalid_argument);
}

TEST(Replay, TerminalAndTruncatedEndings) {
  Tiny t;
  // Segment 1: a b c, ends in a game over at c.
  const int64_t a = t.add('a', true), b = t.add('b');
  t.r.set_transition(a, 0, 1.0f);
  const int64_t c = t.add('c');
  t.r.set_transition(b, 1, 2.0f);
  t.r.end_segment(c, EndKind::Terminal);
  // Segment 2: p q r s, cut by the time limit at s.
  const int64_t p = t.add('p', true), q = t.add('q');
  t.r.set_transition(p, 2, 4.0f);
  const int64_t r = t.add('r');
  t.r.set_transition(q, 0, 8.0f);
  const int64_t s = t.add('s');
  t.r.set_transition(r, 1, 16.0f);
  t.r.end_segment(s, EndKind::Truncated);

  EXPECT_EQ(t.r.valid_slots(), (std::vector<int64_t>{a, b, p, q, r}));   // never the final c or s
  check_batch(t.r.gather({a, b, p, q, r}),
              {
                  {"aaa", "abc", 0, 1 + 0.5f * 2, 0.0f, {1, 2}},       // game over inside the window: no bootstrap
                  {"aab", "abc", 1, 2, 0.0f, {2, 0}},                  // m = 1, game over
                  {"ppp", "pqr", 2, 4 + 0.5f * 8, 0.25f, {4, 8}},      // never stacks across into c
                  {"ppq", "qrs", 0, 8 + 0.5f * 16, 0.25f, {8, 16}},    // truncated end bootstraps: gamma^2
                  {"pqr", "qrs", 1, 16, 0.5f, {16, 0}},                // m = 1: gamma^1
              });
}

TEST(Replay, HoldCutsNeverSampleTheOpenTransition) {
  // The documented way: end_segment(last fully observed slot, Truncated) without a transition on it.
  {
    Tiny t;
    const int64_t h = t.add('h', true), i = t.add('i');
    t.r.set_transition(h, 0, 1.0f);
    const int64_t j = t.add('j');
    t.r.set_transition(i, 1, 2.0f);
    t.r.end_segment(j, EndKind::Truncated);   // the choice made at j never got its result
    t.add('k', true);
    EXPECT_EQ(t.r.valid_slots(), (std::vector<int64_t>{h, i}));
    check_batch(t.r.gather({h, i}), {{"hhh", "hij", 0, 2, 0.25f, {1, 2}}, {"hhi", "hij", 1, 2, 0.5f, {2, 0}}});
  }
  // Starting a new segment without end_segment() cuts the open one the same way, even if its transition
  // was already recorded.
  {
    Tiny t;
    const int64_t h = t.add('h', true), i = t.add('i');
    t.r.set_transition(h, 0, 1.0f);
    const int64_t j = t.add('j');
    t.r.set_transition(i, 1, 2.0f);
    t.r.set_transition(j, 2, 64.0f);
    const int64_t k = t.add('k', true);
    const int64_t l = t.add('l');
    t.r.set_transition(k, 0, 1.0f);
    t.add('m');
    t.r.set_transition(l, 0, 1.0f);
    EXPECT_EQ(t.r.valid_slots(), (std::vector<int64_t>{h, i, k}));
    check_batch(t.r.gather({i, k}), {{"hhi", "hij", 1, 2, 0.5f, {2, 0}}, {"kkk", "klm", 0, 1.5f, 0.25f, {1, 1}}});
  }
}

TEST(Replay, EndingAnOlderSlotStartsANewSegmentAfterIt) {
  Tiny t;
  const int64_t a = t.add('a', true), b = t.add('b');
  t.r.set_transition(a, 0, 1.0f);
  const int64_t c = t.add('c');
  t.r.set_transition(b, 0, 2.0f);
  const int64_t d = t.add('d');
  t.r.set_transition(c, 0, 4.0f);
  t.r.end_segment(b, EndKind::Terminal);
  t.add('e');
  t.r.set_transition(d, 0, 8.0f);
  EXPECT_EQ(t.r.valid_slots(), (std::vector<int64_t>{a, c}));
  check_batch(t.r.gather({a, c}), {{"aaa", "aab", 0, 1, 0.0f, {1, 0}}, {"ccc", "cde", 0, 8, 0.25f, {4, 8}}});
}

TEST(Replay, WrapAroundNeverSamplesOverwrittenFramesOrTheWriteHead) {
  // One long segment A..L (12 observations, capacity 8): A-D are overwritten, E sits at the write head.
  {
    Tiny t;
    std::vector<int64_t> slots;
    for (int i = 0; i < 12; ++i) {
      slots.push_back(t.add(static_cast<char>('A' + i), i == 0));
      if (i > 0) t.r.set_transition(slots[static_cast<size_t>(i - 1)], 0, 1.0f);
    }
    EXPECT_EQ(t.r.size(), 8);
    EXPECT_EQ(slots[11], 3);
    // F and G would need E or older in their stacks; K's window needs the next observation.
    EXPECT_EQ(t.r.valid_slots(), (std::vector<int64_t>{7, 0, 1}));   // H, I, J
    check_batch(t.r.gather({7, 0, 1}), {{"FGH", "HIJ", 0, 1.5f, 0.25f, {1, 1}},
                                        {"GHI", "IJK", 0, 1.5f, 0.25f, {1, 1}},
                                        {"HIJ", "JKL", 0, 1.5f, 0.25f, {1, 1}}});
    std::mt19937_64 rng(1);
    std::set<std::string> seen;
    for (int i = 0; i < 200; ++i) {
      Batch b = t.r.sample(8, rng);
      for (int64_t row = 0; row < 8; ++row) seen.insert(row_text(b.obs, row));
    }
    EXPECT_EQ(seen, (std::set<std::string>{"FGH", "GHI", "HIJ"}));
  }
  // A segment that starts exactly at the write head: its first frame is about to be overwritten.
  {
    Tiny t;
    std::vector<int64_t> slots;
    for (int i = 0; i < 12; ++i) {
      const bool start = i == 0 || i == 4;
      slots.push_back(t.add(static_cast<char>('A' + i), start));
      if (i == 3) {
        t.r.end_segment(slots[3], EndKind::Truncated);
      } else if (i > 0 && i != 4) {
        t.r.set_transition(slots[static_cast<size_t>(i - 1)], 0, 1.0f);
      }
    }
    // E (t=4) starts segment 2 but is the write head, so F and G (whose stacks repeat E) are not usable.
    EXPECT_EQ(t.r.valid_slots(), (std::vector<int64_t>{7, 0, 1}));
  }
  // A segment that starts one slot after the write head: its stacks only repeat its own first frame.
  {
    Tiny t;
    std::vector<int64_t> slots;
    for (int i = 0; i < 12; ++i) {
      const bool start = i == 0 || i == 5;
      slots.push_back(t.add(static_cast<char>('A' + i), start));
      if (i == 4) {
        t.r.end_segment(slots[4], EndKind::Terminal);
      } else if (i > 0 && i != 5) {
        t.r.set_transition(slots[static_cast<size_t>(i - 1)], 0, 1.0f);
      }
    }
    EXPECT_EQ(t.r.valid_slots(), (std::vector<int64_t>{5, 6, 7, 0, 1}));   // F..J
    check_batch(t.r.gather({5, 6}), {{"FFF", "FGH", 0, 1.5f, 0.25f, {1, 1}}, {"FFG", "GHI", 0, 1.5f, 0.25f, {1, 1}}});
  }
}

TEST(Replay, NumbersAreStoredAsFloats) {
  Replay r(8, 2 * sizeof(float), true, 1, 3, 0.9f);
  const float o0[2] = {1.5f, -2.0f}, o1[2] = {3.0f, 4.0f}, o2[2] = {5.0f, 6.0f};
  const int64_t s0 = r.add_obs(o0, true);
  const int64_t s1 = r.add_obs(o1, false);
  r.set_transition(s0, 1, 1.0f);
  const int64_t s2 = r.add_obs(o2, false);
  r.set_transition(s1, 0, 2.0f);
  r.end_segment(s2, EndKind::Terminal);
  Batch b = r.gather({s0, s1});
  EXPECT_EQ(b.obs.scalar_type(), torch::kFloat);
  EXPECT_EQ(b.obs.sizes(), (std::vector<int64_t>{2, 2}));
  EXPECT_FLOAT_EQ(b.obs[0][0].item<float>(), 1.5f);
  EXPECT_FLOAT_EQ(b.obs[0][1].item<float>(), -2.0f);
  EXPECT_FLOAT_EQ(b.next_obs[0][0].item<float>(), 5.0f);   // window stops at the game over (m = 2 < n = 3)
  EXPECT_FLOAT_EQ(b.returns[0].item<float>(), 1.0f + 0.9f * 2.0f);
  EXPECT_FLOAT_EQ(b.discounts[0].item<float>(), 0.0f);
  EXPECT_FLOAT_EQ(b.next_obs[1][1].item<float>(), 6.0f);
  EXPECT_EQ(b.rewards.sizes(), (std::vector<int64_t>{2, 3}));
}

TEST(Replay, SamplingNeedsCompleteTransitions) {
  Tiny t;
  std::mt19937_64 rng(3);
  EXPECT_THROW(t.r.sample(4, rng), std::runtime_error);   // empty
  const int64_t a = t.add('a', true);
  t.r.set_transition(a, 0, 1.0f);
  t.add('b');
  EXPECT_THROW(t.r.sample(4, rng), std::runtime_error);   // a's 2-step window isn't complete yet
  EXPECT_THROW(t.r.set_transition(5, 0, 1.0f), std::out_of_range);   // nothing stored there
}

// ---- randomized check against a straightforward reference model ---------------------------------------

namespace {

// The rules written out plainly over the full, never-overwritten history.
struct Reference {
  int64_t capacity;
  int k, n;
  float gamma;
  struct Obs {
    int seg = 0, pos = 0;
    bool has_action = false, end = false, terminal = false;
    int action = -1;
    float reward = 0;
  };
  std::vector<Obs> obs;
  int segments = 0;

  void add(bool start) {
    if (obs.empty() || obs.back().end) start = true;
    if (start && !obs.empty()) {   // implicit cut of an open segment
      Obs& prev = obs.back();
      if (!prev.end) {
        prev.end = true;
        prev.terminal = false;
        prev.has_action = false;
      }
    }
    if (start) ++segments;
    Obs o;
    o.seg = segments;
    o.pos = start ? 0 : obs.back().pos + 1;
    obs.push_back(o);
  }
  void set(int64_t t, int a, float r) {
    obs[t].has_action = true;
    obs[t].action = a;
    obs[t].reward = r;
  }
  void end(int64_t t, bool terminal) {
    obs[t].end = true;
    obs[t].terminal = terminal;
    obs[t].has_action = false;
  }

  struct Transition {
    std::vector<int64_t> frames, next_frames;
    int action;
    float ret, discount;
    std::vector<float> rewards;
  };
  std::optional<Transition> transition(int64_t t) const {
    const int64_t written = static_cast<int64_t>(obs.size());
    const int64_t lo = std::max<int64_t>(0, written - capacity + 1);
    if (t < lo || t >= written || !obs[t].has_action || obs[t].end) return std::nullopt;
    Transition tr;
    tr.action = obs[t].action;
    tr.rewards.assign(static_cast<size_t>(n), 0.0f);
    double ret = 0, g = 1;
    int m = 0;
    bool terminal = false;
    for (int j = 1; j <= n; ++j) {
      const int64_t u = t + j;
      if (u >= written || obs[u].seg != obs[t].seg) return std::nullopt;
      ret += g * obs[u - 1].reward;
      tr.rewards[static_cast<size_t>(j - 1)] = obs[u - 1].reward;
      g *= gamma;
      if (obs[u].end) {
        m = j;
        terminal = obs[u].terminal;
        break;
      }
      if (j == n) {
        m = n;
        break;
      }
      if (!obs[u].has_action) return std::nullopt;
    }
    tr.ret = static_cast<float>(ret);
    tr.discount = terminal ? 0.0f : static_cast<float>(g);
    auto stack = [&](int64_t s, std::vector<int64_t>& frames) {
      for (int j = k - 1; j >= 0; --j) {
        const int64_t idx = s - std::min<int64_t>(j, obs[s].pos);   // repeat the segment's first frame
        if (idx < lo) return false;
        frames.push_back(idx);
      }
      return true;
    };
    if (!stack(t, tr.frames) || !stack(t + m, tr.next_frames)) return std::nullopt;
    return tr;
  }
};

uint8_t id_of(int64_t t) { return static_cast<uint8_t>(t % 200); }

std::string frames_text(const std::vector<int64_t>& frames) {
  std::string s;
  for (int64_t f : frames) s += static_cast<char>(id_of(f));
  return s;
}

}  // namespace

TEST(Replay, RandomOperationsMatchTheReferenceModel) {
  for (uint64_t seed : {1u, 2u, 3u, 4u}) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    std::mt19937 rng(static_cast<uint32_t>(seed));
    std::mt19937_64 sample_rng(seed);
    std::uniform_real_distribution<double> coin(0, 1);
    std::uniform_int_distribution<int> action(0, 3), reward(-3, 3);
    const int k = seed % 2 ? 3 : 1, n = seed <= 2 ? 2 : 3;
    Replay r(8, 1, false, k, n, kGamma);
    Reference ref{8, k, n, kGamma, {}, 0};
    bool need_start = true;
    struct Pending { int64_t t, slot; int a; float r; };
    std::optional<Pending> pending;
    for (int64_t t = 0; t < 3000; ++t) {
      const bool start = need_start || coin(rng) < 0.05;   // 5%: a cut without end_segment()
      const uint8_t id = id_of(t);
      const int64_t slot = r.add_obs(&id, start);
      ref.add(start);
      need_start = false;
      ASSERT_EQ(slot, t % 8);
      if (pending) {   // real-time order: the result of the previous choice arrives with this observation
        r.set_transition(pending->slot, pending->a, pending->r);
        ref.set(pending->t, pending->a, pending->r);
        pending.reset();
      }
      const double u = coin(rng);
      if (u < 0.12) {
        const bool terminal = coin(rng) < 0.5;
        r.end_segment(slot, terminal ? EndKind::Terminal : EndKind::Truncated);
        ref.end(t, terminal);
        need_start = true;
      } else if (u < 0.92) {
        const int a = action(rng);
        const float rew = static_cast<float>(reward(rng));
        if (coin(rng) < 0.5) {
          r.set_transition(slot, a, rew);
          ref.set(t, a, rew);
        } else {
          pending = Pending{t, slot, a, rew};
        }
      }   // else: this observation never gets an action

      std::vector<int64_t> want_slots;
      std::map<int64_t, Reference::Transition> want;
      for (int64_t s = std::max<int64_t>(0, t - 8); s <= t; ++s) {
        if (auto tr = ref.transition(s)) {
          want_slots.push_back(s % 8);
          want.emplace(s % 8, *tr);
        }
      }
      ASSERT_EQ(r.valid_slots(), want_slots) << "after observation " << t;
      if (want_slots.empty()) continue;
      Batch b = r.gather(want_slots);
      for (size_t i = 0; i < want_slots.size(); ++i) {
        const auto& w = want.at(want_slots[i]);
        const int64_t row = static_cast<int64_t>(i);
        ASSERT_EQ(row_text(b.obs, row), frames_text(w.frames)) << "t " << t;
        ASSERT_EQ(row_text(b.next_obs, row), frames_text(w.next_frames)) << "t " << t;
        ASSERT_EQ(b.actions[row].item<int64_t>(), w.action);
        ASSERT_FLOAT_EQ(b.returns[row].item<float>(), w.ret);
        ASSERT_FLOAT_EQ(b.discounts[row].item<float>(), w.discount);
        for (int j = 0; j < n; ++j) ASSERT_FLOAT_EQ(b.rewards[row][j].item<float>(), w.rewards[static_cast<size_t>(j)]);
      }
      if (t % 50 == 0) {
        // Random samples are always one of the valid transitions.
        Batch s = r.sample(16, sample_rng);
        std::set<std::string> valid;
        for (const auto& [slot_id, w] : want) valid.insert(frames_text(w.frames) + "|" + frames_text(w.next_frames));
        for (int64_t row = 0; row < 16; ++row) {
          ASSERT_TRUE(valid.count(row_text(s.obs, row) + "|" + row_text(s.next_obs, row)));
        }
      }
    }
  }
}

TEST(Replay, ConcurrentAddAndSample) {
  // One thread adds (like the real-time actor) while another samples (like the learner).
  Replay r(64, 512, false, 4, 3, 0.99f);
  std::atomic<bool> stop{false};
  std::atomic<int64_t> added{0};
  std::thread writer([&] {
    std::vector<uint8_t> obs(512);
    int64_t prev = -1;
    for (int64_t t = 0; !stop; ++t) {
      std::fill(obs.begin(), obs.end(), static_cast<uint8_t>(t));
      const bool start = t % 37 == 0;
      const int64_t slot = r.add_obs(obs.data(), start);
      if (prev >= 0 && !start) r.set_transition(prev, static_cast<int>(t % 3), 1.0f);
      if (t % 101 == 100) r.end_segment(slot, EndKind::Terminal);
      prev = slot;
      added = t + 1;
    }
  });
  std::mt19937_64 rng(9);
  int64_t batches = 0;
  bool torn = false;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (batches < 2000 && !torn && std::chrono::steady_clock::now() < until) {
    try {
      Batch b = r.sample(8, rng);
      // Each stored frame is 512 equal bytes: a torn copy would mix two observations.
      for (int64_t row = 0; row < 8; ++row) {
        torch::Tensor frames = b.obs[row].reshape({4, 512});
        torn = torn || !torch::equal(frames, frames.select(1, 0).unsqueeze(1).expand({4, 512}));
      }
      ++batches;
    } catch (const std::runtime_error&) {
      // nothing complete yet
    }
  }
  stop = true;
  writer.join();
  EXPECT_FALSE(torn);
  EXPECT_EQ(batches, 2000);
  EXPECT_GT(added.load(), 64);   // the ring wrapped while sampling
}
