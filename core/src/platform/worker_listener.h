// The app's side of the Windows worker transport (docs/dev/platforms.md section 1), written over platform/net.h so
// that every platform's tests run it: listen on 127.0.0.1 (any free port), then accept exactly the worker's two
// connections, "<token> main\n" and "<token> ctl\n" in any order. A wrong token, a garbled line or a second
// connection of a kind is closed; once both are there the listener closes, so a third connection is refused.
#pragma once

#include "platform/net.h"
#include "platform/transport.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace vyrn::platform {

class WorkerListener {
 public:
  WorkerListener() = default;
  ~WorkerListener();   // closes whatever wasn't taken
  WorkerListener(const WorkerListener&) = delete;
  WorkerListener& operator=(const WorkerListener&) = delete;

  // Listens on 127.0.0.1:0. False + err (an errno value) when it can't.
  bool open(std::string token, int& err);
  int port() const { return port_; }
  const std::string& token() const { return token_; }

  // Accepts and sorts connections for up to timeout_ms (-1: no limit). True once "main" and "ctl" are both there;
  // false after timeout_ms, or as soon as gone() (asked about every 50 ms) says the worker has exited.
  bool accept(int timeout_ms, const std::function<bool()>& gone);
  bool complete() const { return slots_.complete(); }

  // After accept() returned true: the connections (the caller owns them now; non-blocking, TCP_NODELAY) and the
  // bytes that came right after the main connection's line (the first frames).
  net::Socket take_main() { return std::exchange(main_, net::kNoSocket); }
  net::Socket take_ctl() { return std::exchange(ctl_, net::kNoSocket); }
  std::string take_early() { return std::exchange(early_, std::string()); }

 private:
  bool greet(std::pair<net::Socket, ConnectionGreeting>& pending);   // false: done with it (taken or closed)
  void close_pending();

  std::string token_;
  int port_ = 0;
  net::Socket listener_ = net::kNoSocket, main_ = net::kNoSocket, ctl_ = net::kNoSocket;
  std::vector<std::pair<net::Socket, ConnectionGreeting>> pending_;
  ConnectionSlots slots_;
  std::string early_;
};

}  // namespace vyrn::platform
