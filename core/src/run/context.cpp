#include "run/context.h"

#include "util/clock.h"

#include <chrono>

namespace vyrn::run {

// ---- Controls ----------------------------------------------------------------------------------------

void Controls::request_stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
    ++changes_;
  }
  cv_.notify_all();
  wake_others();
}

void Controls::set_paused(bool paused) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    paused_ = paused;
    ++changes_;
  }
  cv_.notify_all();
  wake_others();
}

void Controls::request_resume() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    paused_ = false;
    ++resume_;
    ++changes_;
  }
  cv_.notify_all();
  wake_others();
}

void Controls::request_apply() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    ++apply_;
    ++changes_;
  }
  cv_.notify_all();
  wake_others();
}

void Controls::set_hold(bool on) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    hold_ = on;
    ++changes_;
  }
  cv_.notify_all();
  wake_others();
}

bool Controls::stop_requested() const {
  std::lock_guard<std::mutex> lock(mu_);
  return stop_;
}

bool Controls::paused() const {
  std::lock_guard<std::mutex> lock(mu_);
  return paused_;
}

bool Controls::hold() const {
  std::lock_guard<std::mutex> lock(mu_);
  return hold_;
}

bool Controls::take_apply() {
  std::lock_guard<std::mutex> lock(mu_);
  const bool pending = apply_ != apply_seen_;
  apply_seen_ = apply_;
  return pending;
}

bool Controls::take_resume() {
  std::lock_guard<std::mutex> lock(mu_);
  const bool pending = resume_ != resume_seen_;
  resume_seen_ = resume_;
  return pending;
}

void Controls::set_waker(std::function<void()> waker) {
  std::lock_guard<std::mutex> lock(waker_mu_);
  waker_ = std::move(waker);
}

void Controls::wake_others() {
  std::lock_guard<std::mutex> lock(waker_mu_);
  if (waker_) waker_();
}

void Controls::wait_for(double seconds) {
  if (seconds <= 0) return;
  std::unique_lock<std::mutex> lock(mu_);
  const uint64_t seen = changes_;
  cv_.wait_for(lock, std::chrono::duration<double>(seconds), [&] { return changes_ != seen; });
}

// ---- RunContext --------------------------------------------------------------------------------------

void RunContext::app_log(const std::string& text) { emit(log_event("app", text)); }

void RunContext::worker_line(ipc::Stream stream, const std::string& line) {
  util::LineThrottler::Verdict verdict;
  {
    std::lock_guard<std::mutex> lock(throttle_mu_);
    verdict = throttle_.offer(util::now_seconds());
  }
  if (verdict.dropped_before > 0) {
    emit(log_event("app", "(" + std::to_string(verdict.dropped_before) + " lines dropped)"));
  }
  if (verdict.show) emit(log_event(stream == ipc::Stream::Stdout ? "stdout" : "stderr", line));
}

void RunContext::flush_dropped_lines(bool force) {
  int64_t dropped = 0;
  {
    std::lock_guard<std::mutex> lock(throttle_mu_);
    dropped = throttle_.flush(util::now_seconds(), force);
  }
  if (dropped > 0) emit(log_event("app", "(" + std::to_string(dropped) + " lines dropped)"));
}

void RunContext::set_state(RunState state, const std::string& detail) {
  state_.store(state);
  emit(state_event(state, detail));
}

void RunContext::publish(LiveView view) {
  std::lock_guard<std::mutex> lock(live_mu_);
  view.version = live_.version + 1;
  live_ = std::move(view);
}

LiveView RunContext::live() const {
  LiveView copy;
  {
    std::lock_guard<std::mutex> lock(live_mu_);
    copy = live_;
  }
  copy.state = state();
  return copy;
}

void RunContext::set_interruptible(ipc::WorkerProcess* worker) {
  std::lock_guard<std::mutex> lock(interrupt_mu_);
  interruptible_ = worker;
}

void RunContext::request_stop() {
  controls_.request_stop();
  std::lock_guard<std::mutex> lock(interrupt_mu_);
  if (interruptible_) interruptible_->kill_group();   // the runner's request() then throws WorkerDied
}

}  // namespace vyrn::run
