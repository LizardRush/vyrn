// Console flood control: at most N lines per second are shown, the rest are counted. No torch in this header.
#pragma once

#include <cstdint>

namespace vyrn::util {

// Splits time into one-second windows (each starts with the first line after the previous one ended) and lets
// at most `per_second` lines through per window. Times are seconds on any monotonic clock. Not thread-safe.
class LineThrottler {
 public:
  explicit LineThrottler(int per_second = 200) : per_second_(per_second > 0 ? per_second : 1) {}

  struct Verdict {
    bool show = false;            // show this line
    int64_t dropped_before = 0;   // lines dropped in the window that just ended: report them first
  };
  Verdict offer(double now_s);

  // Lines dropped in a window that has ended (or in any window when `force`, e.g. at the end of a run) and
  // that weren't reported yet. Call it now and then, so a flood that stops suddenly is still reported.
  int64_t flush(double now_s, bool force = false);

  int64_t total_dropped() const { return total_dropped_; }

 private:
  int per_second_;
  bool started_ = false;
  double window_start_ = 0;
  int shown_ = 0;
  int64_t dropped_ = 0;
  int64_t total_dropped_ = 0;
};

}  // namespace vyrn::util
