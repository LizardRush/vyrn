// Absolute-deadline pacing (Watch at decisions/s x speed; the real-time runner's tick clock).
// No torch in this header.
#pragma once

#include <cstdint>
#include <vector>

namespace vyrn::run {

// Tick k is due at start + k * period. A tick that can't start on time runs at once; whole periods that were
// missed are skipped, never made up in a burst.
struct PaceStep {
  double due = 0;        // when this tick is due (seconds)
  double wait_s = 0;     // how long to wait before running it (0 when late)
  double late_s = 0;     // how late it starts (0 when on time; always less than one period)
  int64_t skipped = 0;   // whole ticks that were missed and are skipped
};

// The next tick after one that was due at `prev_due`, seen at time `now`. period > 0.
PaceStep pace(double prev_due, double now, double period);

class Pacer {
 public:
  explicit Pacer(double period_s) : period_(period_s) {}
  bool paced() const { return period_ > 0; }
  double period() const { return period_; }
  // Call once per tick, before running it. The first tick (and the first after restart()) is due now.
  PaceStep next(double now);
  void restart() { started_ = false; }   // e.g. after a pause: don't count the pause as lateness
  int64_t skipped() const { return skipped_; }

 private:
  double period_;
  bool started_ = false;
  double due_ = 0;
  int64_t skipped_ = 0;
};

// The p-th percentile (0..100, nearest rank) of `values`; 0 when empty.
double percentile(std::vector<double> values, double p);

}  // namespace vyrn::run
