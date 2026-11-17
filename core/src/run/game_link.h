// Vyrn Link inside a run (docs/dev/link.md, "Real-time runner"): the server a game connects to, for projects whose
// vyrn.json has "link"; what the worker's requests carry from the game (protocol.md section 3.4) and what goes back
// to it. RealtimeRunner owns one for Check, Train and Watch and keeps it across worker restarts. No torch here.
//
// A helper thread forwards the server's notices to the console and emits a "link" event whenever a game connects
// or leaves (and once at the start): {"type":"link", "url", "address", "port", "code", "lockstep",
// "frames_per_step", "connected", "game", "client", "from"}. The pairing code is only in that event and in the
// "Waiting for the game" state, never in a console line.
#pragma once

#include "link/link_server.h"
#include "run/context.h"
#include "run/events.h"
#include "util/settings.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace vyrn::run {

namespace test_hooks {
// Where a run's Link server listens: "0.0.0.0" (every network: the game usually runs on another computer). Tests
// use "127.0.0.1".
extern std::string link_bind_address;
}  // namespace test_hooks

class GameLink {
 public:
  // nullptr when the project has no "link" (vyrn.json problems with it become "warning" events).
  static std::unique_ptr<GameLink> for_project(RunContext& ctx, const std::string& project_dir,
                                               const util::ProjectSettings& settings, double tick_hz);
  ~GameLink();   // stop()
  GameLink(const GameLink&) = delete;
  GameLink& operator=(const GameLink&) = delete;

  // Listens (throws link::LinkError with a plain reason, e.g. the port is in use) and starts the helper thread.
  void start();
  // Tells a connected game to stop, closes the connection and the port. Safe to call twice.
  void stop();
  // The game buttons the project's choices use (the spec's "buttons"), for the welcome of games that connect next.
  void set_buttons(const json& spec);

  link::LinkServer& server() { return *server_; }
  bool lockstep() const { return settings_.lockstep; }
  bool connected() const { return server_->connected(); }
  link::MessagePtr latest_state() const { return server_->latest_state(); }
  const std::string& url() const { return url_; }
  const std::string& code() const { return code_; }
  double started_at() const { return started_at_; }   // util::now_seconds() of start()

  // The state line while no game is connected: "Waiting for the game: connect it to ws://...:7457/ with the
  // pairing code 483920."
  std::string waiting_text() const;

  // Adds "connected", "game" (state's value; {} without one) and, with_events, "game_events" (drained) to a request
  // for observe, check_over or restart, keeping it within the worker's 1 MiB limit: the oldest events are dropped
  // first, then the state's biggest fields (one warning each, once per run).
  void attach(json& request, const link::MessagePtr& state, bool with_events);
  // Forwards the "send" messages of a worker reply to the game (in order). Check never calls it.
  void forward_sends(const json& reply);
  // The AI's buttons from an act reply ("game_input"), as the input that answers `state` (nothing without one).
  void send_input(const link::MessagePtr& state, const json& act_reply);
  // An input that holds nothing: the round ended (real time).
  void release(const link::MessagePtr& state);
  // {"type":"stop"}: the AI paused or stopped; the game lets go of every button and leaves lockstep.
  void send_stop() { server_->send_stop(); }
  // Ends every wait for the game that is running now (Stop, pause, hold...).
  void interrupt() { server_->interrupt_waits(); }

 private:
  GameLink(RunContext& ctx, link::LinkSettings settings, link::LinkConfig config);
  void watch();                   // the helper thread
  void emit_status(bool force);   // a "link" event when the connection changed
  void warn_once(std::atomic<bool>& said, const std::string& message);

  RunContext& ctx_;
  link::LinkSettings settings_;
  std::unique_ptr<link::LinkServer> server_;
  std::string code_, url_, address_;
  double started_at_ = 0;

  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool quit_ = false;
  uint64_t shown_connection_ = ~uint64_t{0};   // the connection the last "link" event showed (0 = none)
  std::atomic<bool> warned_events_{false}, warned_state_{false};
};

}  // namespace vyrn::run
