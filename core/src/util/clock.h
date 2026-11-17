// Small time helpers. No torch in this header.
#pragma once

#include <chrono>
#include <string>

namespace vyrn::util {

using SteadyClock = std::chrono::steady_clock;

// Seconds on the monotonic clock (only differences mean anything).
inline double now_seconds() {
  return std::chrono::duration<double>(SteadyClock::now().time_since_epoch()).count();
}

// Local time as "20261113-153012" (folder names: brain ids, runs/).
std::string local_stamp();
// Local time as "2026-11-13T15:30:12" (brain.json).
std::string local_iso_time();

// Measures running time; pause() / resume() leave the paused time out.
class Stopwatch {
 public:
  Stopwatch() : started_(now_seconds()) {}
  double seconds() const { return paused_ ? total_ : total_ + (now_seconds() - started_); }
  void pause() {
    if (paused_) return;
    total_ += now_seconds() - started_;
    paused_ = true;
  }
  void resume() {
    if (!paused_) return;
    started_ = now_seconds();
    paused_ = false;
  }

 private:
  double started_;
  double total_ = 0;
  bool paused_ = false;
};

}  // namespace vyrn::util
