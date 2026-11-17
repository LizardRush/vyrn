#include "run/rounds.h"

namespace vyrn::run {

void RoundTracker::add(double points) {
  ++rounds_;
  last_.push_back(points);
  if (static_cast<int>(last_.size()) > kWindow) last_.pop_front();
  improved_ = false;
  if (static_cast<int>(last_.size()) == kWindow) {
    const double mean = mean20();
    if (!best_ || mean > *best_) {
      best_ = mean;
      improved_ = true;
    }
  }
}

double RoundTracker::mean20() const {
  if (last_.empty()) return 0.0;
  double sum = 0;   // summed fresh each time: 20 additions, no drift
  for (const double p : last_) sum += p;
  return sum / static_cast<double>(last_.size());
}

}  // namespace vyrn::run
