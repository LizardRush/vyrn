// Round scores: the 20-round mean shown on the dashboard and the best one (which picks the Best brain).
// No torch in this header.
#pragma once

#include <cstdint>
#include <deque>
#include <optional>

namespace vyrn::run {

class RoundTracker {
 public:
  static constexpr int kWindow = 20;

  // `best` = the best 20-round mean of earlier runs of this brain (a new best must beat it).
  explicit RoundTracker(std::optional<double> best = std::nullopt) : best_(best) {}

  void add(double points);                  // a finished round
  int64_t rounds() const { return rounds_; }
  double mean20() const;                    // mean of the last 20 rounds (of all of them before 20; 0 before 1)
  std::optional<double> best_mean20() const { return best_; }   // only full 20-round windows count
  bool improved() const { return improved_; }                   // the last add() set a new best

 private:
  std::deque<double> last_;   // the last kWindow rounds' points
  int64_t rounds_ = 0;
  std::optional<double> best_;
  bool improved_ = false;
};

}  // namespace vyrn::run
