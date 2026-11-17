// The stop marker (docs/dev/core-internals.md section 4): another program asks the Sessions running a project to
// stop by creating an empty file <project>/.vyrn-stop (the app's Stop button for a training that vyrn-cli or an AI
// agent's MCP server runs). While a Session runs Check, Train or Watch it looks for that file about every 0.5 s;
// when it is there, the Session deletes it and stops exactly like Session::stop() (Train saves the brain first).
// A marker that is already there when the run starts is stale: it is deleted and ignored. No torch in this header.
#pragma once

#include "run/context.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace vyrn::run {

constexpr const char* kStopMarkerName = kStopMarkerFile;   // ".vyrn-stop" (vyrn/session.h)

// Deletes <dir>/.vyrn-stop when it is there. True when there was one (even if it couldn't be deleted).
bool take_stop_marker(const std::string& dir);

// Polls for the marker on its own thread from construction to destruction.
class StopMarkerWatch {
 public:
  StopMarkerWatch(RunContext& ctx, std::string project_dir, double every_s = 0.5);
  ~StopMarkerWatch();
  StopMarkerWatch(const StopMarkerWatch&) = delete;
  StopMarkerWatch& operator=(const StopMarkerWatch&) = delete;

 private:
  void poll();
  bool is_stale_leftover() const;   // the stale marker that couldn't be deleted is still the one there

  RunContext& ctx_;
  std::string dir_;
  double every_s_;
  bool stale_kept_ = false;         // a stale marker was there and couldn't be deleted
  uint64_t stale_dev_ = 0;
  uint64_t stale_ino_ = 0;
  std::mutex mu_;
  std::condition_variable cv_;
  bool done_ = false;
  std::thread thread_;
};

}  // namespace vyrn::run
