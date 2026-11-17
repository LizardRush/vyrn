// `vyrn-cli bench`: times the brain on this computer. No torch in this header (the CLI includes it).
#pragma once

#include <string>

namespace vyrn::brain {

struct BenchOptions {
  double seconds_per_case = 0.4;   // time spent measuring each call (after one warm-up call)
  int min_iterations = 3;          // ...but at least this many calls
};

// Times act() on one observation and a full Double-DQN update at the default batch size for the three
// networks (MLP with 4 numbers, small CNN on 10x10 pictures, DER CNN on 84x84 pictures), with oneDNN
// off and on and 1, 2 and 3 torch threads. Returns a readable table and a recommendation line.
// Runs on the calling thread and restores its torch thread count and the oneDNN setting afterwards.
std::string run_bench();
std::string run_bench(const BenchOptions& options);

}  // namespace vyrn::brain
