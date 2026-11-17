// Linux: /proc for process identity and this program's path, XDG_RUNTIME_DIR, per-thread nice, and the Linux
// versions of the POSIX primitives in posix_util.h.
#include "platform/files.h"
#include "platform/posix_util.h"
#include "platform/system.h"

#include <fcntl.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace vyrn::platform {

namespace fs = std::filesystem;

namespace {

std::string first_line(const std::string& path) {
  std::ifstream in(path);
  std::string line;
  std::getline(in, line);
  while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
  return line;
}

}  // namespace

// /proc/<pid>/stat. The command name (field 2) may contain spaces and ')', so count from the last ')'.
std::optional<ProcessIdentity> process_identity(int64_t pid) {
  if (pid <= 0) return std::nullopt;
  const std::string text = first_line("/proc/" + std::to_string(pid) + "/stat");
  const auto name_end = text.rfind(')');
  if (name_end == std::string::npos) return std::nullopt;
  std::istringstream fields(text.substr(name_end + 1));
  char state = '?';
  long long ppid = 0;
  ProcessIdentity id;
  if (!(fields >> state >> ppid >> id.group)) return std::nullopt;   // fields 3, 4, 5
  std::string skipped;
  for (int field = 6; field < 22; ++field) {
    if (!(fields >> skipped)) return std::nullopt;
  }
  unsigned long long start = 0;   // field 22: start time in clock ticks after boot
  if (!(fields >> start)) return std::nullopt;
  id.start = static_cast<long long>(start);
  return id;
}

const std::string& boot_id() {
  static const std::string id = first_line("/proc/sys/kernel/random/boot_id");
  return id;
}

bool same_boot(const std::string& recorded) { return recorded == boot_id(); }

std::string executable_path() {
  std::error_code ec;
  const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  return ec ? std::string() : exe.string();
}

std::vector<std::string> data_dir_candidates(const std::string& exe) {
  return {(fs::path(exe).parent_path().parent_path() / "share" / "vyrn").string()};
}

// $XDG_RUNTIME_DIR/vyrn, with the same fallback as scripts/vyrn-panic.
std::string runtime_dir() {
  const char* xdg = std::getenv("XDG_RUNTIME_DIR");
  const std::string base = xdg && *xdg ? std::string(xdg) : "/run/user/" + std::to_string(::getuid());
  return base + "/vyrn";
}

std::string default_python() { return "/usr/bin/python3"; }

// nice is per thread on Linux.
void lower_thread_priority(int nice_delta) {
  const id_t tid = static_cast<id_t>(::syscall(SYS_gettid));
  errno = 0;
  const int nice_now = ::getpriority(PRIO_PROCESS, tid);
  if (errno == 0) ::setpriority(PRIO_PROCESS, tid, std::min(19, nice_now + nice_delta));
}

void ensure_passive_openmp(char** argv) {
  if (std::getenv("OMP_WAIT_POLICY")) return;
  if (::setenv("OMP_WAIT_POLICY", "PASSIVE", 1) != 0) return;
  ::execv("/proc/self/exe", argv);
}

void file_clock_now(int64_t& seconds, int32_t& nanoseconds) {
  timespec now{};
  ::clock_gettime(CLOCK_REALTIME_COARSE, &now);   // the clock file times use
  seconds = static_cast<int64_t>(now.tv_sec);
  nanoseconds = static_cast<int32_t>(now.tv_nsec);
}

namespace posix {

int pipe_cloexec(int fds[2]) { return ::pipe2(fds, O_CLOEXEC) == 0 ? 0 : errno; }

int socketpair_cloexec(int fds[2]) {
  return ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0 ? 0 : errno;
}

ssize_t send_nosignal(int fd, const void* data, size_t size) { return ::send(fd, data, size, MSG_NOSIGNAL); }

void no_sigpipe(int /*fd*/) {}

int close_fds_from(posix_spawn_file_actions_t* actions, short* /*flags*/, int first) {
  return posix_spawn_file_actions_addclosefrom_np(actions, first);
}

void stat_mtime(const struct stat& st, int64_t& seconds, int32_t& nanoseconds) {
  seconds = static_cast<int64_t>(st.st_mtim.tv_sec);
  nanoseconds = static_cast<int32_t>(st.st_mtim.tv_nsec);
}

int rename_exclusive(const char* from, const char* to) {
  return ::renameat2(AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE) == 0 ? 0 : errno;
}

}  // namespace posix

}  // namespace vyrn::platform
