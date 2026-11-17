#include "util/throttle.h"

namespace vyrn::util {

LineThrottler::Verdict LineThrottler::offer(double now_s) {
  Verdict v;
  if (!started_ || now_s - window_start_ >= 1.0) {
    v.dropped_before = dropped_;
    dropped_ = 0;
    shown_ = 0;
    window_start_ = now_s;
    started_ = true;
  }
  if (shown_ < per_second_) {
    ++shown_;
    v.show = true;
  } else {
    ++dropped_;
    ++total_dropped_;
  }
  return v;
}

int64_t LineThrottler::flush(double now_s, bool force) {
  if (!started_ || dropped_ == 0 || (!force && now_s - window_start_ < 1.0)) return 0;
  const int64_t n = dropped_;
  dropped_ = 0;
  return n;
}

}  // namespace vyrn::util
