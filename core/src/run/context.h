// What a Session shares between the UI thread (start/stop/drain/live...) and its run thread (the runner).
// Everything here is thread-safe and never waits for Python or torch. No torch in this header.
#pragma once

#include "ipc/worker.h"
#include "run/events.h"
#include "util/throttle.h"
#include "vyrn/session.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace vyrn::run {

// Requests from the UI thread to the runner.
class Controls {
 public:
  void request_stop();
  void set_paused(bool paused);   // pause() / resume()
  void request_resume();          // resume(): also asks to leave the Error state (respawning Python)
  void request_apply();           // apply_changes()
  void set_hold(bool on);         // set_hold() (real time)

  bool stop_requested() const;
  bool paused() const;
  bool hold() const;
  bool take_apply();    // true once per apply_changes() call
  bool take_resume();   // true once per resume() call

  // Runner side: sleeps until any request arrives or `seconds` pass.
  void wait_for(double seconds);
  // Runner side: also called (on the requesting thread) after every request, e.g. to interrupt a wait for the
  // game (Vyrn Link). nullptr removes it; once set_waker() returns, the old one isn't running any more.
  void set_waker(std::function<void()> waker);

 private:
  void wake_others();

  std::mutex waker_mu_;
  std::function<void()> waker_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false, paused_ = false, hold_ = false;
  uint64_t apply_ = 0, apply_seen_ = 0;
  uint64_t resume_ = 0, resume_seen_ = 0;
  uint64_t changes_ = 0;   // counts every request, so waiters can tell something happened
};

class RunContext {
 public:
  explicit RunContext(RunConfig config) : config_(std::move(config)) {}
  RunContext(const RunContext&) = delete;
  RunContext& operator=(const RunContext&) = delete;

  const RunConfig& config() const { return config_; }
  Controls& controls() { return controls_; }

  // ---- events
  void emit(const ojson& event) { events_.push(event); }
  std::vector<std::string> drain(size_t max) { return events_.drain(max); }
  void app_log(const std::string& text);                           // never throttled
  void worker_line(ipc::Stream stream, const std::string& line);   // the io thread; <= 200 lines/s shown
  void flush_dropped_lines(bool force = false);   // reports "(N lines dropped)" for a flood that has ended

  // ---- state
  void set_state(RunState state, const std::string& detail);   // emits a "state" event
  RunState state() const { return state_.load(); }
  void mark_finished() { finished_.store(true); }
  bool finished() const { return finished_.load(); }

  // ---- live view
  void publish(LiveView view);   // fills in version and state
  LiveView live() const;

  // ---- stopping
  // While a worker is busy with slow start-up work (spawn, hello, load), stop() may kill it so it doesn't
  // keep the user waiting. Pass nullptr when that part is over.
  void set_interruptible(ipc::WorkerProcess* worker);
  void request_stop();

 private:
  RunConfig config_;
  Controls controls_;
  EventQueue events_;
  std::atomic<RunState> state_{RunState::Starting};
  std::atomic<bool> finished_{false};

  std::mutex throttle_mu_;
  util::LineThrottler throttle_{200};

  mutable std::mutex live_mu_;
  LiveView live_;

  std::mutex interrupt_mu_;
  ipc::WorkerProcess* interruptible_ = nullptr;
};

}  // namespace vyrn::run
