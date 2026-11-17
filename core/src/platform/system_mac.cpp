// macOS: proc_pidinfo for process identity, kern.boottime, _NSGetExecutablePath, $TMPDIR, QoS instead of a per-thread
// nice, and the macOS versions of the POSIX primitives in posix_util.h.
#include "platform/files.h"
#include "platform/posix_util.h"
#include "platform/system.h"

#include <fcntl.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdio.h>
#include <sys/proc_info.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <vector>

namespace vyrn::platform {

namespace fs = std::filesystem;

std::optional<ProcessIdentity> process_identity(int64_t pid) {
  if (pid <= 0) return std::nullopt;
  struct proc_bsdinfo info {};
  const int size = static_cast<int>(sizeof info);
  if (::proc_pidinfo(static_cast<int>(pid), PROC_PIDTBSDINFO, 0, &info, size) != size) {
    return std::nullopt;
  }
  ProcessIdentity id;
  id.start = static_cast<long long>(info.pbi_start_tvsec) * 1000000LL + static_cast<long long>(info.pbi_start_tvusec);
  id.group = static_cast<long long>(info.pbi_pgid);
  return id;
}

const std::string& boot_id() {
  static const std::string id = [] {
    struct timeval boot {};
    size_t size = sizeof boot;
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (::sysctl(mib, 2, &boot, &size, nullptr, 0) != 0 || size != sizeof boot) return std::string();
    return std::to_string(static_cast<long long>(boot.tv_sec)) + "." + std::to_string(static_cast<long long>(boot.tv_usec));
  }();
  return id;
}

bool same_boot(const std::string& recorded) { return recorded == boot_id(); }

std::string executable_path() {
  uint32_t size = 0;
  ::_NSGetExecutablePath(nullptr, &size);   // fails, and says how much room the path needs
  std::vector<char> buf(static_cast<size_t>(size) + 1, '\0');
  if (::_NSGetExecutablePath(buf.data(), &size) != 0) return {};
  std::error_code ec;
  const fs::path exe = fs::weakly_canonical(fs::path(buf.data()), ec);
  return ec ? std::string(buf.data()) : exe.string();
}

// Inside Vyrn.app the data is in Contents/Resources/vyrn (the program is Contents/MacOS/vyrn).
std::vector<std::string> data_dir_candidates(const std::string& exe) {
  const fs::path up = fs::path(exe).parent_path().parent_path();
  return {(up / "Resources" / "vyrn").string(), (up / "share" / "vyrn").string()};
}

std::string runtime_dir() {
  std::string base;
  if (const char* tmp = std::getenv("TMPDIR"); tmp && *tmp) {
    base = tmp;
  } else {
    char buf[1024] = "";
    const size_t n = ::confstr(_CS_DARWIN_USER_TEMP_DIR, buf, sizeof buf);
    base = n > 0 && n <= sizeof buf ? std::string(buf) : std::string("/tmp");
  }
  while (base.size() > 1 && base.back() == '/') base.pop_back();
  return base + "/vyrn";
}

// Apps started from the Finder don't get the shell's PATH, and /usr/bin/python3 is Apple's older Python: prefer
// Homebrew's.
std::string default_python() {
  for (const char* candidate : {"/opt/homebrew/bin/python3", "/usr/local/bin/python3"}) {
    if (::access(candidate, X_OK) == 0) return candidate;
  }
  return "python3";
}

void lower_thread_priority(int /*nice_delta*/) { ::pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0); }

void ensure_passive_openmp(char** argv) {
  if (std::getenv("OMP_WAIT_POLICY")) return;
  const std::string exe = executable_path();
  if (exe.empty() || ::setenv("OMP_WAIT_POLICY", "PASSIVE", 1) != 0) return;
  ::execv(exe.c_str(), argv);
}

void file_clock_now(int64_t& seconds, int32_t& nanoseconds) {
  timespec now{};
  ::clock_gettime(CLOCK_REALTIME, &now);
  seconds = static_cast<int64_t>(now.tv_sec);
  nanoseconds = static_cast<int32_t>(now.tv_nsec);
}

namespace posix {

int pipe_cloexec(int fds[2]) {
  if (::pipe(fds) != 0) return errno;
  ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  return 0;
}

int socketpair_cloexec(int fds[2]) {
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return errno;
  const int one = 1;
  for (int i = 0; i < 2; ++i) {
    ::fcntl(fds[i], F_SETFD, FD_CLOEXEC);
    ::setsockopt(fds[i], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
  }
  return 0;
}

ssize_t send_nosignal(int fd, const void* data, size_t size) { return ::send(fd, data, size, 0); }   // SO_NOSIGPIPE

void no_sigpipe(int fd) {
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
}

int close_fds_from(posix_spawn_file_actions_t* /*actions*/, short* flags, int /*first*/) {
  *flags = static_cast<short>(*flags | POSIX_SPAWN_CLOEXEC_DEFAULT);   // only what the actions set up stays open
  return 0;
}

void stat_mtime(const struct stat& st, int64_t& seconds, int32_t& nanoseconds) {
  seconds = static_cast<int64_t>(st.st_mtimespec.tv_sec);
  nanoseconds = static_cast<int32_t>(st.st_mtimespec.tv_nsec);
}

int rename_exclusive(const char* from, const char* to) { return ::renamex_np(from, to, RENAME_EXCL) == 0 ? 0 : errno; }

}  // namespace posix

}  // namespace vyrn::platform
