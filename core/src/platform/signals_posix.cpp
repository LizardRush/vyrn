// Stop requests and the MCP server's stdio on POSIX (vyrn/platform.h).
//
// SIGINT/SIGTERM(/SIGHUP) are blocked in the thread that installs the handlers, before any other thread exists, so
// every later thread inherits the block; a thread of our own takes them with sigwait() and reports each one through
// a pipe. A thread that a shared library started before main() never got the block: the kernel may deliver a stop
// signal there, and the handler reports it through the same pipe instead of letting the default action kill the
// process. Python workers get a clean signal mask from the core (worker_child_posix.cpp).
#include "vyrn/platform.h"

#include "platform/posix_util.h"

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace vyrn::platform {

namespace {

int g_stop_read = -1;
int g_stop_write = -1;
sigset_t g_stop_signals;

// Async-signal-safe; leaves errno as it was for the interrupted thread.
void on_stop_signal(int sig) {
  if (g_stop_write < 0) return;
  const int saved_errno = errno;
  const unsigned char b = static_cast<unsigned char>(sig);
  [[maybe_unused]] const ssize_t ignored = ::write(g_stop_write, &b, 1);
  errno = saved_errno;
}

void take_stop_signals() {
  for (;;) {
    int sig = 0;
    if (::sigwait(&g_stop_signals, &sig) == 0) on_stop_signal(sig);
  }
}

// Reads what the pipe holds: how many stop requests, and the last one.
int drain_stops(int* last) {
  int count = 0;
  unsigned char b = 0;
  while (g_stop_read >= 0 && ::read(g_stop_read, &b, 1) == 1) {
    ++count;
    if (last) *last = b;
  }
  return count;
}

}  // namespace

void install_stop_handlers(bool hangup) {
  static std::once_flag once;
  std::call_once(once, [hangup] {
    int fds[2];
    if (posix::pipe_cloexec(fds) == 0) {
      for (const int fd : fds) ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
      g_stop_read = fds[0];
      g_stop_write = fds[1];
    }
    sigemptyset(&g_stop_signals);
    sigaddset(&g_stop_signals, SIGINT);
    sigaddset(&g_stop_signals, SIGTERM);
    if (hangup) sigaddset(&g_stop_signals, SIGHUP);
    struct sigaction action {};
    action.sa_handler = on_stop_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    ::sigaction(SIGINT, &action, nullptr);
    ::sigaction(SIGTERM, &action, nullptr);
    if (hangup) ::sigaction(SIGHUP, &action, nullptr);
    pthread_sigmask(SIG_BLOCK, &g_stop_signals, nullptr);
    ::signal(SIGPIPE, SIG_IGN);
    std::thread(take_stop_signals).detach();   // it inherits the block: sigwait() takes them
  });
}

int wait_stop_requests(int ms, int* last) {
  if (g_stop_read < 0) {
    if (ms > 0) ::poll(nullptr, 0, ms);
    return 0;
  }
  pollfd p{g_stop_read, POLLIN, 0};
  if (::poll(&p, 1, ms) <= 0) return 0;
  return drain_stops(last);
}

const char* stop_request_name(int number) {
  switch (number) {
    case SIGINT: return "SIGINT";
    case SIGTERM: return "SIGTERM";
    case SIGHUP: return "SIGHUP";
    default: return "a signal";
  }
}

// ---- ProtocolStdio ---------------------------------------------------------------------------------------------

struct ProtocolStdio::Impl {
  int out_fd = -1;
};

ProtocolStdio::ProtocolStdio() : impl_(std::make_unique<Impl>()) {}

ProtocolStdio::~ProtocolStdio() {
  if (impl_->out_fd >= 0) ::close(impl_->out_fd);
}

bool ProtocolStdio::open(std::string& why) {
  // stdout carries only the protocol: it moves to a private descriptor, and fd 1 becomes stderr.
  const int out_fd = ::fcntl(1, F_DUPFD_CLOEXEC, 3);
  if (out_fd < 0) {
    why = std::strerror(errno);
    return false;
  }
  std::fflush(stdout);
  ::dup2(2, 1);
  impl_->out_fd = out_fd;
  return true;
}

ProtocolStdio::Wait ProtocolStdio::wait(int ms, bool read_input) {
  Wait w;
  pollfd fds[2];
  nfds_t n = 0;
  fds[n++] = {g_stop_read, POLLIN, 0};
  if (read_input) fds[n++] = {0, POLLIN, 0};
  if (::poll(fds, n, ms) <= 0) return w;
  w.stops = drain_stops(&w.last_stop);
  if (read_input && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
    char buf[65536];
    const ssize_t got = ::read(0, buf, sizeof buf);
    if (got > 0) {
      w.data.assign(buf, static_cast<size_t>(got));
    } else if (got == 0 || (errno != EINTR && errno != EAGAIN)) {
      w.end = true;
    }
  }
  return w;
}

bool ProtocolStdio::write_all(const std::string& bytes, std::string& why) {
  size_t done = 0;
  while (done < bytes.size()) {
    const ssize_t n = ::write(impl_->out_fd, bytes.data() + done, bytes.size() - done);
    if (n > 0) {
      done += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && errno == EAGAIN) {
      pollfd p{impl_->out_fd, POLLOUT, 0};
      ::poll(&p, 1, 1000);
      continue;
    }
    why = std::strerror(errno);
    return false;
  }
  return true;
}

}  // namespace vyrn::platform
