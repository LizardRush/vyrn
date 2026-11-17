// WorkerListener (worker_listener.h). Portable: it only uses platform/net.h.
#include "platform/worker_listener.h"

#include <algorithm>
#include <cerrno>
#include <chrono>

namespace vyrn::platform {

namespace {

constexpr size_t kMaxPending = 8;   // connections that haven't said who they are yet
constexpr int kLookEveryMs = 50;    // how often accept() asks gone()

}  // namespace

WorkerListener::~WorkerListener() {
  net::close_socket(listener_);
  net::close_socket(main_);
  net::close_socket(ctl_);
  close_pending();
}

bool WorkerListener::open(std::string token, int& err) {
  token_ = std::move(token);
  listener_ = net::listen_tcp("127.0.0.1", 0, 4, port_, err);
  return listener_ != net::kNoSocket;
}

void WorkerListener::close_pending() {
  for (auto& p : pending_) net::close_socket(p.first);
  pending_.clear();
}

bool WorkerListener::greet(std::pair<net::Socket, ConnectionGreeting>& p) {
  char buf[256];
  int err = 0;
  const long long got = net::recv_some(p.first, buf, sizeof buf, err);
  if (got < 0 && err == EWOULDBLOCK) return true;
  const ConnectionGreeting::State state =
      got > 0 ? p.second.add(buf, static_cast<size_t>(got)) : ConnectionGreeting::State::Refused;   // closed early
  if (state == ConnectionGreeting::State::Waiting) return true;
  const ConnectionRole role = p.second.role();
  if (state == ConnectionGreeting::State::Accepted && slots_.take(role)) {
    if (role == ConnectionRole::Main) {
      main_ = p.first;
      early_ = p.second.leftover();
    } else {
      ctl_ = p.first;
    }
  } else {
    net::close_socket(p.first);   // a wrong token or line, or a second "main" / "ctl"
  }
  p.first = net::kNoSocket;
  return false;
}

bool WorkerListener::accept(int timeout_ms, const std::function<bool()>& gone) {
  using Clock = std::chrono::steady_clock;
  const Clock::time_point start = Clock::now();
  while (!slots_.complete()) {
    if (listener_ == net::kNoSocket || (gone && gone())) return false;
    int slice = kLookEveryMs;
    if (timeout_ms >= 0) {
      const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
      if (spent >= timeout_ms) return false;
      slice = static_cast<int>(std::min<long long>(kLookEveryMs, timeout_ms - spent));
    }
    std::vector<net::PollItem> items(1 + pending_.size());
    items[0].socket = listener_;
    items[0].want_read = true;
    for (size_t i = 0; i < pending_.size(); ++i) {
      items[i + 1].socket = pending_[i].first;
      items[i + 1].want_read = true;
    }
    int err = 0;
    if (net::poll(items, slice, err) <= 0) continue;
    for (size_t i = pending_.size(); i-- > 0;) {
      const net::PollItem& item = items[i + 1];
      if ((item.readable || item.failed) && !greet(pending_[i])) {
        pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
      }
    }
    if (items[0].readable) {
      for (;;) {
        uint32_t peer = 0;
        const net::Socket s = net::accept_tcp(listener_, peer, err);
        if (s == net::kNoSocket) break;
        if (pending_.size() >= kMaxPending || (peer >> 24) != 127) {   // only this computer may connect
          net::close_socket(s);
          continue;
        }
        pending_.emplace_back(s, ConnectionGreeting(token_));
      }
    }
  }
  net::close_socket(listener_);
  listener_ = net::kNoSocket;
  close_pending();
  return true;
}

}  // namespace vyrn::platform
