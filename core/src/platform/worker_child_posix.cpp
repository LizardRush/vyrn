// worker_child.h on POSIX (Linux, macOS): a socketpair end as fd 3 (VYRN_FD=3), posix_spawn into the worker's own
// process group, SIGINT / SIGUSR1 for interrupt / freeze, SIGKILL to the group (protocol.md sections 1 and 7).
#include "platform/worker_child.h"

#include "platform/posix_util.h"
#include "platform/transport.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

extern char** environ;

namespace vyrn::platform {

namespace {

class UniqueFd {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) : fd_(fd) {}
  UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) reset(std::exchange(other.fd_, -1));
    return *this;
  }
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;
  ~UniqueFd() { reset(); }

  int get() const { return fd_; }
  explicit operator bool() const { return fd_ >= 0; }
  void reset(int fd = -1) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};

std::string errno_text(int err) { return std::strerror(err); }

[[noreturn]] void fail_setup(const char* what, int err) { throw StartError(what, errno_text(err)); }

void check_setup(int rc, const char* what) {   // posix_spawn_* functions return the error number
  if (rc != 0) fail_setup(what, rc);
}

class SpawnActions {
 public:
  SpawnActions() { check_setup(posix_spawn_file_actions_init(&actions_), "posix_spawn_file_actions_init"); }
  ~SpawnActions() { posix_spawn_file_actions_destroy(&actions_); }
  SpawnActions(const SpawnActions&) = delete;
  SpawnActions& operator=(const SpawnActions&) = delete;
  posix_spawn_file_actions_t* get() { return &actions_; }

 private:
  posix_spawn_file_actions_t actions_;
};

class SpawnAttr {
 public:
  SpawnAttr() { check_setup(posix_spawnattr_init(&attr_), "posix_spawnattr_init"); }
  ~SpawnAttr() { posix_spawnattr_destroy(&attr_); }
  SpawnAttr(const SpawnAttr&) = delete;
  SpawnAttr& operator=(const SpawnAttr&) = delete;
  posix_spawnattr_t* get() { return &attr_; }

 private:
  posix_spawnattr_t attr_;
};

// Both ends of a new pipe, close-on-exec.
void make_pipe(UniqueFd& read_end, UniqueFd& write_end, const char* what) {
  int fds[2];
  if (const int err = posix::pipe_cloexec(fds); err != 0) fail_setup(what, err);
  read_end.reset(fds[0]);
  write_end.reset(fds[1]);
}

// fds 0-3 are the child's targets: a source fd among them could be overwritten by an earlier action.
UniqueFd above_stdio(UniqueFd fd) {
  if (fd.get() > 3) return fd;
  const int moved = ::fcntl(fd.get(), F_DUPFD_CLOEXEC, 4);
  if (moved < 0) fail_setup("fcntl(F_DUPFD_CLOEXEC)", errno);
  return UniqueFd(moved);
}

void set_nonblocking(const UniqueFd& fd) {
  const int flags = ::fcntl(fd.get(), F_GETFL);
  if (flags < 0 || ::fcntl(fd.get(), F_SETFL, flags | O_NONBLOCK) < 0) fail_setup("fcntl(O_NONBLOCK)", errno);
}

// This process's environment, then VYRN_FD=3 (protocol.md section 1), then spec.env; a later value replaces an
// earlier one with the same name.
std::vector<std::string> child_environment(const ChildSpec& spec) {
  std::vector<std::string> env;
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) env.emplace_back(*entry);
  set_env_entry(env, "VYRN_FD", "3", false);
  for (const auto& [name, value] : spec.env) set_env_entry(env, name, value, false);
  return env;
}

std::vector<char*> c_strings(std::vector<std::string>& strings) {
  std::vector<char*> pointers;
  pointers.reserve(strings.size() + 1);
  for (std::string& text : strings) pointers.push_back(text.data());
  pointers.push_back(nullptr);
  return pointers;
}

}  // namespace

struct WorkerChild::Impl {
  mutable std::mutex mu;   // pid and reaped: interrupt/freeze/kill/exit_note/alive may come from any thread
  pid_t pid = -1;
  bool reaped = false;
  UniqueFd sock, out_pipe, err_pipe, wake_read, wake_write;

  void signal(int signum) {
    std::lock_guard<std::mutex> lock(mu);
    if (pid > 0 && !reaped) ::kill(pid, signum);
  }
};

WorkerChild::WorkerChild() : impl_(std::make_unique<Impl>()) {}

WorkerChild::~WorkerChild() { kill(); }

void WorkerChild::start(const ChildSpec& spec) {
  Impl& s = *impl_;
  int pair[2];
  if (const int err = posix::socketpair_cloexec(pair); err != 0) fail_setup("socketpair", err);
  UniqueFd sock(pair[0]);
  UniqueFd child_sock(pair[1]);
  UniqueFd out_read, out_write, err_read, err_write, wake_read, wake_write;
  make_pipe(out_read, out_write, "pipe2");
  make_pipe(err_read, err_write, "pipe2");
  make_pipe(wake_read, wake_write, "pipe2");
  child_sock = above_stdio(std::move(child_sock));
  out_write = above_stdio(std::move(out_write));
  err_write = above_stdio(std::move(err_write));
  set_nonblocking(sock);
  set_nonblocking(out_read);
  set_nonblocking(err_read);
  set_nonblocking(wake_read);
  set_nonblocking(wake_write);

  // In the child: fd 0 = /dev/null, 1/2 = our pipes, 3 = its socket end, nothing else.
  SpawnActions actions;
  check_setup(posix_spawn_file_actions_addopen(actions.get(), 0, "/dev/null", O_RDONLY, 0), "addopen");
  check_setup(posix_spawn_file_actions_adddup2(actions.get(), out_write.get(), 1), "adddup2");
  check_setup(posix_spawn_file_actions_adddup2(actions.get(), err_write.get(), 2), "adddup2");
  check_setup(posix_spawn_file_actions_adddup2(actions.get(), child_sock.get(), 3), "adddup2");
  short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
  check_setup(posix::close_fds_from(actions.get(), &flags, 4), "addclosefrom_np");

  // Its own process group (pgid = its pid), no blocked signals, default handling for the signals
  // Vyrn and Python care about (an ignored SIGINT would otherwise stay ignored across exec).
  SpawnAttr attr;
  sigset_t no_signals;
  sigemptyset(&no_signals);
  sigset_t defaults;
  sigemptyset(&defaults);
  for (int signum : {SIGINT, SIGTERM, SIGQUIT, SIGHUP, SIGPIPE, SIGUSR1, SIGUSR2, SIGALRM, SIGCHLD}) {
    sigaddset(&defaults, signum);
  }
  check_setup(posix_spawnattr_setflags(attr.get(), flags), "posix_spawnattr_setflags");
  check_setup(posix_spawnattr_setpgroup(attr.get(), 0), "posix_spawnattr_setpgroup");
  check_setup(posix_spawnattr_setsigmask(attr.get(), &no_signals), "posix_spawnattr_setsigmask");
  check_setup(posix_spawnattr_setsigdefault(attr.get(), &defaults), "posix_spawnattr_setsigdefault");

  std::vector<std::string> args = {spec.program};
  args.insert(args.end(), spec.args.begin(), spec.args.end());
  std::vector<std::string> env = child_environment(spec);
  std::vector<char*> argv = c_strings(args);
  std::vector<char*> envp = c_strings(env);

  pid_t child = -1;
  const int rc = ::posix_spawnp(&child, spec.program.c_str(), actions.get(), attr.get(), argv.data(), envp.data());
  if (rc != 0) throw StartError("", errno_text(rc));
  {
    std::lock_guard<std::mutex> lock(s.mu);
    s.pid = child;   // from here on the destructor kills it
  }
  // The child's ends close when these go out of scope; the parent keeps its own.
  s.sock = std::move(sock);
  s.out_pipe = std::move(out_read);
  s.err_pipe = std::move(err_read);
  s.wake_read = std::move(wake_read);
  s.wake_write = std::move(wake_write);
}

Io WorkerChild::send(const uint8_t* data, size_t size, size_t& n, std::string& /*problem*/) {
  for (;;) {
    const ssize_t sent = posix::send_nosignal(impl_->sock.get(), data, size);
    if (sent > 0) {
      n = static_cast<size_t>(sent);
      return Io::Done;
    }
    if (sent < 0 && errno == EINTR) continue;
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return Io::WouldBlock;
    return Io::Closed;   // EPIPE, ECONNRESET: the worker is gone
  }
}

Io WorkerChild::receive(uint8_t* data, size_t size, size_t& n, std::string& problem) {
  for (;;) {
    const ssize_t got = ::recv(impl_->sock.get(), data, size, 0);
    if (got > 0) {
      n = static_cast<size_t>(got);
      return Io::Done;
    }
    if (got == 0) return Io::End;
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return Io::WouldBlock;
    if (errno != ECONNRESET) problem = errno_text(errno);
    return Io::Closed;
  }
}

void WorkerChild::wait(bool for_write, int timeout_ms) {
  pollfd ready{impl_->sock.get(), static_cast<short>(for_write ? POLLOUT : POLLIN), 0};
  ::poll(&ready, 1, timeout_ms);   // timeout or EINTR: the caller just looks again
}

// SIGINT to the worker pid only: Python raises KeyboardInterrupt inside the stuck user code.
void WorkerChild::interrupt() { impl_->signal(SIGINT); }

// SIGUSR1: the worker's watchdog lets go of every key even while user code runs.
void WorkerChild::freeze() { impl_->signal(SIGUSR1); }

void WorkerChild::kill() {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.mu);
  if (s.pid <= 0 || s.reaped) return;
  // The leader isn't reaped yet, so its pid (= the group id) can't belong to anyone else.
  ::kill(-s.pid, SIGKILL);   // the worker and everything it started
  ::kill(s.pid, SIGKILL);    // in case it left its group
  int status = 0;
  while (::waitpid(s.pid, &status, 0) < 0 && errno == EINTR) {
  }
  s.reaped = true;
}

// How the worker ended, if it already has (it stays waitable: WNOWAIT). "" while it runs.
std::string WorkerChild::exit_note() const {
  const Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.mu);
  if (s.pid <= 0 || s.reaped) return {};
  siginfo_t info{};
  if (::waitid(P_PID, static_cast<id_t>(s.pid), &info, WEXITED | WNOHANG | WNOWAIT) != 0 || info.si_pid == 0) {
    return {};
  }
  if (info.si_code == CLD_EXITED) {
    return info.si_status == 0 ? "it exited" : "exit status " + std::to_string(info.si_status);
  }
  const char* name = ::strsignal(info.si_status);
  return "killed by signal " + std::to_string(info.si_status) + (name ? std::string(": ") + name : std::string());
}

bool WorkerChild::alive() const {
  const Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.mu);
  if (s.pid <= 0 || s.reaped) return false;
  siginfo_t info{};
  if (::waitid(P_PID, static_cast<id_t>(s.pid), &info, WEXITED | WNOHANG | WNOWAIT) != 0) return false;
  return info.si_pid == 0;   // nothing to report: still running
}

int64_t WorkerChild::pid() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->pid;
}

void WorkerChild::pump_output(const std::function<void(int, const char*, size_t)>& on_data,
                              const std::function<void(int)>& on_end) {
  Impl& s = *impl_;
  pollfd fds[3] = {{s.out_pipe.get(), POLLIN, 0}, {s.err_pipe.get(), POLLIN, 0}, {s.wake_read.get(), POLLIN, 0}};
  std::vector<char> chunk(64 * 1024);
  int open_streams = 2;
  while (open_streams > 0) {
    if (::poll(fds, 3, -1) < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (fds[2].revents != 0) break;   // stop_output()
    for (int i = 0; i < 2; ++i) {
      if (fds[i].fd < 0 || fds[i].revents == 0) continue;
      for (int reads = 0; reads < 16; ++reads) {   // then poll again, so a flood can't starve the other stream
        const ssize_t n = ::read(fds[i].fd, chunk.data(), chunk.size());
        if (n > 0) {
          on_data(i, chunk.data(), static_cast<size_t>(n));
          continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        on_end(i);   // EOF (or a read error): this stream is over
        fds[i].fd = -1;
        --open_streams;
        break;
      }
    }
  }
}

void WorkerChild::stop_output() {
  const char one = 1;
  [[maybe_unused]] const ssize_t ignored = ::write(impl_->wake_write.get(), &one, 1);
}

}  // namespace vyrn::platform
