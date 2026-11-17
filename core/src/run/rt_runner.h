// RealtimeRunner's timings, which tests shorten, and a trace of its brain-side steps for tests
// (docs/dev/core-internals.md section 4, PLAN.md section 3 "RealtimeRunner"). No torch in this header.
#pragma once

#include <functional>
#include <string>

namespace vyrn::run {

struct RtTimings {
  double countdown_s = 3.0;          // "Hands off: click your game window", then lock_focus
  double restart_every_s = 3.0;      // restart() again while game_over() stays True...
  double restart_give_up_s = 30.0;   // ...and pause with "restart() didn't clear game_over()" after this long
  double hold_poll_s = 0.2;          // while held: "status" at 5 Hz
  double late_cut_periods = 3.0;     // a tick later than this many periods cuts the segment
  double freeze_kill_s = 0.5;        // a freeze nobody answers: SIGKILL the worker's process group
};

namespace test_hooks {
extern RtTimings rt_timings;
// Called on the actor thread for each brain-side step of Train and Watch: "start" (a segment starts with this
// observation), "record <choice> <points>" (a transition), "cut" (the open transition is never sampled),
// "end terminal" / "end truncated" (the round's final observation).
extern std::function<void(const std::string&)> rt_trace;
// Called each time the runner let go of every key itself (platform::release_all_keys(): a worker that may have
// pressed keys ended without answering "close").
extern std::function<void()> keys_released;
}  // namespace test_hooks

}  // namespace vyrn::run
