#include "brain/bench.h"

#include "brain/dqn.h"
#include "brain/threads.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <iomanip>
#include <sstream>
#include <vector>

namespace vyrn::brain {

namespace {

struct BenchCase {
  std::string name;
  NetConfig net;
  DqnConfig cfg;
};

struct Row {
  size_t case_index = 0;
  bool onednn = false;
  int threads = 1;
  double act_ms = 0, update_ms = 0;
};

// Median milliseconds per call: one warm-up call, then calls until the time budget and the minimum count
// are both reached.
double median_ms(const std::function<void()>& call, const BenchOptions& opt) {
  using clock = std::chrono::steady_clock;
  call();
  std::vector<double> times;
  const auto start = clock::now();
  while (times.size() < 1000) {
    const auto t0 = clock::now();
    call();
    const auto t1 = clock::now();
    times.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    const double spent = std::chrono::duration<double>(t1 - start).count();
    if (static_cast<int>(times.size()) >= opt.min_iterations && spent >= opt.seconds_per_case) break;
  }
  std::nth_element(times.begin(), times.begin() + static_cast<std::ptrdiff_t>(times.size() / 2), times.end());
  return times[times.size() / 2];
}

torch::Tensor random_obs(const NetConfig& net, int64_t rows) {
  if (net.kind == NetKind::MLP) return torch::randn({rows, net.d});
  return torch::randint(0, 256, {rows, static_cast<int64_t>(net.in_c) * net.in_h * net.in_w}, torch::kUInt8);
}

Batch random_batch(const NetConfig& net, const DqnConfig& cfg) {
  const int64_t b = cfg.batch;
  Batch batch;
  batch.obs = random_obs(net, b);
  batch.next_obs = random_obs(net, b);
  batch.actions = torch::randint(0, net.n_actions, {b}, torch::kLong);
  batch.rewards = torch::randn({b, static_cast<int64_t>(cfg.n_step)});
  batch.returns = batch.rewards.sum(1);
  batch.discounts = torch::full({b}, 0.97f);
  return batch;
}

std::vector<BenchCase> bench_cases() {
  Spec numbers;
  numbers.obs.kind = ObsSpec::Kind::Numbers;
  Spec pictures;
  pictures.obs.kind = ObsSpec::Kind::Picture;
  const DqnConfig numbers_cfg = default_config(numbers, false);
  const DqnConfig pictures_cfg = default_config(pictures, false);
  const int stacked = 3 * pictures_cfg.frames;
  return {
      {"numbers: MLP, 4 inputs", NetConfig{NetKind::MLP, 0, 0, 0, 4, 3}, numbers_cfg},
      {"picture 10x10: small CNN", NetConfig{NetKind::SmallCNN, stacked, 10, 10, 0, 3}, pictures_cfg},
      {"picture 84x84: DER CNN", NetConfig{NetKind::DER, stacked, 84, 84, 0, 3}, pictures_cfg},
  };
}

const Row* find_row(const std::vector<Row>& rows, size_t case_index, bool onednn, int threads) {
  for (const Row& r : rows) {
    if (r.case_index == case_index && r.onednn == onednn && r.threads == threads) return &r;
  }
  return nullptr;
}

}  // namespace

std::string run_bench() { return run_bench(BenchOptions{}); }

std::string run_bench(const BenchOptions& options) {
  init_torch_process();
  const bool onednn_before = onednn_enabled();
  const int threads_before = torch_threads();
  const std::vector<BenchCase> cases = bench_cases();
  std::vector<Row> rows;

  for (const bool onednn : {false, true}) {
    set_onednn_enabled(onednn);
    for (const int threads : {1, 2, 3}) {
      set_torch_threads(threads);
      for (size_t i = 0; i < cases.size(); ++i) {
        const BenchCase& c = cases[i];
        torch::manual_seed(1);
        Dqn dqn(c.cfg, c.net);
        const Batch batch = random_batch(c.net, c.cfg);
        const torch::Tensor obs1 = random_obs(c.net, 1);
        std::mt19937_64 rng(1);
        std::vector<float> probs;
        Row row;
        row.case_index = i;
        row.onednn = onednn;
        row.threads = threads;
        row.act_ms = median_ms([&] { dqn.act(obs1, 0.0f, rng, &probs); }, options);
        row.update_ms = median_ms([&] { dqn.update(batch); }, options);
        rows.push_back(row);
      }
    }
  }
  set_onednn_enabled(onednn_before);
  set_torch_threads(threads_before);

  std::ostringstream os;
  os << std::fixed << std::setprecision(2);
  os << "Vyrn bench: how fast the brain runs on this computer (median milliseconds per call)\n\n";
  os << std::left << std::setw(27) << "network" << std::setw(8) << "oneDNN" << std::setw(9) << "threads"
     << std::setw(13) << "act (1 obs)" << "update (batch)\n";
  for (const Row& r : rows) {
    const BenchCase& c = cases[r.case_index];
    std::ostringstream update;
    update << std::fixed << std::setprecision(2) << r.update_ms << " (" << c.cfg.batch << ")";
    os << std::left << std::setw(27) << c.name << std::setw(8) << (r.onednn ? "on" : "off") << std::setw(9)
       << r.threads << std::setw(13) << r.act_ms << update.str() << "\n";
  }

  // Decide with the setting that matters most: 84x84 pictures at the real-time thread counts
  // (actor 1 thread acting, learner 2 threads updating), and check the simulation setting (3 threads).
  const size_t der = cases.size() - 1;
  auto cost = [&](bool onednn, int update_threads) {
    const Row* act = find_row(rows, der, onednn, 1);
    const Row* upd = find_row(rows, der, onednn, update_threads);
    return (act && upd) ? act->act_ms + upd->update_ms : 0.0;
  };
  const double rt_off = cost(false, 2), rt_on = cost(true, 2);
  const double sim_off = cost(false, 3), sim_on = cost(true, 3);
  os << "\nReal time with 84x84 pictures (act on 1 thread + update on 2 threads): oneDNN off " << rt_off
     << " ms, on " << rt_on << " ms.\n";
  os << "Simulation with 84x84 pictures (act + update on 3 threads): oneDNN off " << sim_off << " ms, on " << sim_on
     << " ms.\n";
  const bool on_wins = rt_on > 0 && rt_on < rt_off * 0.95 && sim_on < sim_off;
  os << std::setprecision(1);
  const char* on_how = kOneDnnDefault ? "(the default)" : "(start Vyrn with VYRN_ONEDNN=1)";
  const char* off_how = kOneDnnDefault ? "(start Vyrn with VYRN_ONEDNN=0)" : "(the default)";
  if (on_wins) {
    os << "Recommendation: turn oneDNN on " << on_how << ": real-time steps are " << rt_off / rt_on
       << "x faster with it.\n";
  } else if (rt_off > 0 && rt_on >= rt_off) {
    os << "Recommendation: keep oneDNN off " << off_how << ": real-time steps are " << rt_on / rt_off
       << "x faster without it.\n";
  } else {
    os << "Recommendation: keep oneDNN off " << off_how << ": turning it on gains less than 5% here.\n";
  }
  return os.str();
}

}  // namespace vyrn::brain
