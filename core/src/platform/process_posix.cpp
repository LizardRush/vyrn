// run_capture() and stop_process_tree() on POSIX (vyrn/platform.h).
#include "vyrn/platform.h"

#include "platform/posix_util.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

extern char** environ;

namespace vyrn::platform {

namespace {

double now_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

bool stop_process_tree(int64_t pid, bool force) {
  if (pid <= 0 || pid != static_cast<pid_t>(pid)) return false;
  return ::kill(-static_cast<pid_t>(pid), force ? SIGKILL : SIGTERM) == 0;   // the program's whole process group
}

bool run_capture(const std::vector<std::string>& argv, double timeout_s, size_t max_bytes, std::string& out,
                 std::string& why) {
  out.clear();
  if (argv.empty()) {
    why = "nothing to run";
    return false;
  }
  int out_pipe[2], err_pipe[2];
  if (const int err = posix::pipe_cloexec(out_pipe); err != 0) {
    why = std::strerror(err);
    return false;
  }
  if (const int err = posix::pipe_cloexec(err_pipe); err != 0) {
    why = std::strerror(err);
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    return false;
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, out_pipe[1], 1);
  posix_spawn_file_actions_adddup2(&actions, err_pipe[1], 2);
  short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP;
  posix::close_fds_from(&actions, &flags, 3);
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  sigset_t none, defaults;
  sigemptyset(&none);
  sigemptyset(&defaults);
  for (const int s : {SIGINT, SIGTERM, SIGHUP, SIGPIPE}) sigaddset(&defaults, s);
  posix_spawnattr_setsigmask(&attr, &none);
  posix_spawnattr_setsigdefault(&attr, &defaults);
  posix_spawnattr_setpgroup(&attr, 0);
  posix_spawnattr_setflags(&attr, flags);
  std::vector<char*> args;
  for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);
  pid_t pid = -1;
  const int rc = ::posix_spawnp(&pid, args[0], &actions, &attr, args.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attr);
  ::close(out_pipe[1]);
  ::close(err_pipe[1]);
  if (rc != 0) {
    why = "can't start " + argv[0] + ": " + std::strerror(rc);
    ::close(out_pipe[0]);
    ::close(err_pipe[0]);
    return false;
  }
  std::string err;
  bool out_open = true, err_open = true, timed_out = false;
  const double deadline = now_s() + timeout_s;
  char buf[8192];
  while (out_open || err_open) {
    const double left = deadline - now_s();
    if (left <= 0) {
      timed_out = true;
      break;
    }
    pollfd fds[2] = {{out_open ? out_pipe[0] : -1, POLLIN, 0}, {err_open ? err_pipe[0] : -1, POLLIN, 0}};
    const int n = ::poll(fds, 2, static_cast<int>(std::min(left, 1.0) * 1000) + 1);
    if (n < 0 && errno != EINTR) break;
    for (int i = 0; i < 2; ++i) {
      if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      const ssize_t got = ::read(fds[i].fd, buf, sizeof buf);
      if (got > 0) {
        std::string& into = i == 0 ? out : err;
        const size_t cap = i == 0 ? max_bytes : 4096;
        if (into.size() < cap) into.append(buf, std::min(static_cast<size_t>(got), cap - into.size()));
      } else if (got == 0 || (errno != EINTR && errno != EAGAIN)) {
        (i == 0 ? out_open : err_open) = false;
      }
    }
  }
  ::close(out_pipe[0]);
  ::close(err_pipe[0]);
  if (timed_out) ::kill(-pid, SIGKILL);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (timed_out) {
    why = argv[0] + " took longer than " + std::to_string(static_cast<int>(timeout_s)) + " s";
    return false;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    while (!err.empty() && (err.back() == '\n' || err.back() == '\r')) err.pop_back();
    const auto last = err.rfind('\n');
    why = argv[0] + " failed" + (err.empty() ? std::string() : ": " + err.substr(last == std::string::npos ? 0 : last + 1));
    return false;
  }
  return true;
}

}  // namespace vyrn::platform
