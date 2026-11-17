#include "run/pacing.h"

#include <algorithm>
#include <cmath>

namespace vyrn::run {

PaceStep pace(double prev_due, double now, double period) {
  PaceStep s;
  s.due = prev_due + period;
  if (now <= s.due) {
    s.wait_s = s.due - now;
    return s;
  }
  // Late: skip the periods that are already over completely, then start at once.
  s.skipped = static_cast<int64_t>(std::floor((now - s.due) / period));
  s.due += static_cast<double>(s.skipped) * period;
  s.late_s = std::max(0.0, now - s.due);
  return s;
}

PaceStep Pacer::next(double now) {
  if (!paced()) return PaceStep{now, 0, 0, 0};
  if (!started_) {
    started_ = true;
    due_ = now;
    return PaceStep{now, 0, 0, 0};
  }
  PaceStep s = pace(due_, now, period_);
  due_ = s.due;
  skipped_ += s.skipped;
  return s;
}

double percentile(std::vector<double> values, double p) {
  if (values.empty()) return 0.0;
  p = std::clamp(p, 0.0, 100.0);
  // Nearest rank: the smallest value with at least p% of the values at or below it.
  size_t rank = static_cast<size_t>(std::ceil(p / 100.0 * static_cast<double>(values.size())));
  rank = std::clamp<size_t>(rank, 1, values.size());
  std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(rank - 1), values.end());
  return values[rank - 1];
}

}  // namespace vyrn::run
