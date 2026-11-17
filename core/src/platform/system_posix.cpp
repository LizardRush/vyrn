// The POSIX parts of system.h that Linux and macOS share.
#include "platform/system.h"

#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#if __has_include(<sys/random.h>)
#include <sys/random.h>   // getentropy (macOS; glibc also declares it in unistd.h)
#endif

#include <cerrno>
#include <cstdlib>

namespace vyrn::platform {

bool local_time(std::time_t t, std::tm& out) { return ::localtime_r(&t, &out) != nullptr; }

bool utc_time(std::time_t t, std::tm& out) { return ::gmtime_r(&t, &out) != nullptr; }

bool set_env(const std::string& name, const std::string& value) { return ::setenv(name.c_str(), value.c_str(), 1) == 0; }

bool unset_env(const std::string& name) { return ::unsetenv(name.c_str()) == 0; }

int64_t current_pid() { return static_cast<int64_t>(::getpid()); }

// kill(pid, 0) does not fail with ESRCH, and (where /proc exists) /proc/<pid> exists.
bool process_running(int64_t pid) {
  if (pid <= 0 || pid != static_cast<pid_t>(pid)) return false;
  if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) return false;   // EPERM: another user's process
  // Without /proc (macOS, or unusual Linux) the kill() answer is all there is.
  struct stat st {};
  if (::stat("/proc/self", &st) == 0) {
    const std::string proc = "/proc/" + std::to_string(pid);
    if (::stat(proc.c_str(), &st) != 0) return false;
  }
  return true;
}

std::vector<std::string> home_dirs() {
  std::vector<std::string> homes;
  if (const char* home = std::getenv("HOME"); home && *home) homes.emplace_back(home);
  struct passwd pw {};
  struct passwd* found = nullptr;
  char buf[4096];
  if (::getpwuid_r(::getuid(), &pw, buf, sizeof buf, &found) == 0 && found && found->pw_dir && *found->pw_dir) {
    homes.emplace_back(found->pw_dir);
  }
  for (std::string& h : homes) {
    while (h.size() > 1 && h.back() == '/') h.pop_back();
  }
  return homes;
}

std::string home_dir() {
  if (const char* home = std::getenv("HOME"); home && *home) return home;
  const std::vector<std::string> homes = home_dirs();
  return homes.empty() ? std::string() : homes.front();
}

bool random_bytes(void* out, size_t size) {
  auto* bytes = static_cast<unsigned char*>(out);
  while (size > 0) {
    const size_t chunk = size < 256 ? size : 256;   // getentropy's limit
    if (::getentropy(bytes, chunk) != 0) return false;
    bytes += chunk;
    size -= chunk;
  }
  return true;
}

}  // namespace vyrn::platform
