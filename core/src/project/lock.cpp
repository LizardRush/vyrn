// One trainer per project: <project>/.vyrn-lock (docs/dev/mcp.md section 4).
//
// The lock file is created exclusively (O_CREAT|O_EXCL; Windows CREATE_NEW), so exactly one process can make it. It
// names its holder; a lock whose holder has ended is stale and may be taken over. Taking over must not let two
// processes win, so takers serialise on a lock of the project folder (platform::FolderLock: flock() on POSIX):
// under it they re-read the file, remove it only if it is still the same stale file, and try once more. Plain
// acquirers never remove a file, and a live holder only removes its own, so the folder lock is needed only between
// takers.
#include "platform/files.h"
#include "platform/system.h"
#include "project/internal.h"
#include "util/clock.h"
#include "vyrn/project.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <ctime>
#include <fstream>
#include <sstream>
#include <thread>
#include <utility>

namespace vyrn {

namespace {

namespace fs = std::filesystem;
using project_detail::pid_running;

constexpr size_t kMaxLockBytes = 4096;
// An empty or garbled lock file this young may still be being written by its creator (it writes right after the
// O_EXCL create), so it counts as held. Older ones are leftovers and count as stale.
constexpr int64_t kUnreadableGraceSeconds = 5;

std::string lock_path(const std::string& project) {
  return (fs::path(project) / project_detail::kLockFileName).string();
}

// This boot's id: a lock written before a restart is stale even when its pid is in use again.
const std::string& boot_id() { return platform::boot_id(); }

// When a process started (platform::process_identity), or nullopt.
std::optional<long long> start_time(int64_t pid) {
  const std::optional<platform::ProcessIdentity> id = platform::process_identity(pid);
  if (!id) return std::nullopt;
  return id->start;
}

struct LockFile {
  std::string text;              // the raw bytes
  bool parsed = false;           // a JSON object with a pid
  int64_t pid = 0;
  std::string by, since, boot;
  std::optional<long long> start;
  int64_t age_s = 0;             // seconds since it was written
  uint64_t dev = 0;              // which file it is (platform::FileInfo device + inode)
  uint64_t ino = 0;
};

enum class ReadResult { Missing, Ok, Error };

void parse(LockFile& f) {
  const nlohmann::json j = nlohmann::json::parse(f.text, nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) return;
  const auto pid = j.find("pid");
  if (pid == j.end() || !pid->is_number_integer()) return;
  const long long value = pid->get<long long>();
  if (value <= 0 || value > INT_MAX) return;
  f.pid = static_cast<int64_t>(value);
  const auto text = [&](const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
  };
  f.by = text("by");
  f.since = text("since");
  f.boot = text("boot");
  const auto start = j.find("start");
  if (start != j.end() && start->is_number_integer()) f.start = start->get<long long>();
  f.parsed = true;
}

int64_t age_of(const platform::FileInfo& st) { return static_cast<int64_t>(std::time(nullptr)) - st.mtime_s; }

ReadResult read_lock(const std::string& path, LockFile& out) {
  out = LockFile{};
  // no_follow: never read through a link; a FIFO in its place can't hang us either (platform::File).
  int err = 0;
  platform::File file = platform::File::open_read(path, /*no_follow=*/true, err);
  if (!file) {
    if (err == ENOENT) return ReadResult::Missing;
    platform::FileInfo st;
    if (err == ELOOP && platform::file_info(path, false, st) == 0 && st.is_symlink()) {
      out.age_s = age_of(st);   // a symlink where the lock should be: unreadable, stale after the grace time
      out.dev = st.device;
      out.ino = st.inode;
      return ReadResult::Ok;
    }
    return ReadResult::Error;
  }
  platform::FileInfo st;
  if (file.info(st) != 0 || !st.is_regular()) return ReadResult::Error;
  out.dev = st.device;
  out.ino = st.inode;
  out.age_s = age_of(st);
  char buf[kMaxLockBytes + 1];
  size_t used = 0;
  while (used < sizeof buf) {
    size_t n = 0;
    if (file.read(buf + used, sizeof buf - used, n) != 0) return ReadResult::Error;
    if (n == 0) break;
    used += n;
  }
  out.text.assign(buf, used);
  if (used <= kMaxLockBytes) parse(out);
  return ReadResult::Ok;
}

bool is_stale(const LockFile& f) {
  if (!f.parsed) return f.age_s > kUnreadableGraceSeconds || f.age_s < -kUnreadableGraceSeconds;
  if (!pid_running(f.pid)) return true;
  if (!f.boot.empty() && !boot_id().empty() && !platform::same_boot(f.boot)) return true;   // before a restart
  if (f.start) {
    const std::optional<long long> now = start_time(f.pid);
    if (now && *now != *f.start) return true;   // the pid now belongs to another program
  }
  return false;
}

// Like read_lock, but gives a creator that is writing the file right now a moment to finish (at most ~0.1 s).
ReadResult read_settled(const std::string& path, LockFile& out) {
  for (int attempt = 0;; ++attempt) {
    const ReadResult r = read_lock(path, out);
    if (r != ReadResult::Ok || out.parsed || attempt >= 20 || is_stale(out)) return r;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

std::string lock_content(const std::string& by) {
  nlohmann::ordered_json j = nlohmann::ordered_json::object();
  j["pid"] = static_cast<long long>(platform::current_pid());
  j["by"] = by;
  j["since"] = util::local_iso_time();
  if (const auto start = start_time(platform::current_pid())) j["start"] = *start;
  if (!boot_id().empty()) j["boot"] = boot_id();
  return j.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
}

// 0, or the errno of the failure (EEXIST: someone else's lock is there).
int create_lock_file(const std::string& path, const std::string& content) {
  int err = 0;
  platform::File file = platform::File::create(path, platform::File::Create::Exclusive, /*no_follow=*/true, 0644, err);
  if (!file) return err;
  err = file.write_all(content.data(), content.size());
  if (const int closed = file.close(); err == 0) err = closed;
  if (err != 0) {
    platform::remove_file(path);   // our own half-written file: never leave it behind
    return err;
  }
  return 0;
}

// An exclusive lock on the project folder for the duration of one takeover (platform::FolderLock: flock() on
// POSIX). Waits at most ~0.5 s.
class TakeoverGuard {
 public:
  explicit TakeoverGuard(const std::string& project) : lock_(project, 500) {}
  bool busy() const { return lock_.busy(); }   // another taker kept the folder locked for too long

 private:
  platform::FolderLock lock_;
};

bool same_file(const LockFile& a, const LockFile& b) {
  return a.text == b.text && a.dev == b.dev && a.ino == b.ino;
}

// Removes the lock file if it is still exactly the one this process wrote.
void release_lock(const std::string& path, const std::string& content) noexcept {
  if (path.empty() || content.empty()) return;
  try {
    LockFile f;
    if (read_lock(path, f) == ReadResult::Ok && f.parsed && f.pid == platform::current_pid() && f.text == content) {
      platform::remove_file(path);
    }
  } catch (...) {
    // Out of memory while reading a tiny file: leave it; it turns stale when this process ends.
  }
}

}  // namespace

ProjectLock::ProjectLock(std::string path) : path_(std::move(path)) {}

ProjectLock::ProjectLock(ProjectLock&& other) noexcept
    : path_(std::move(other.path_)), content_(std::move(other.content_)) {
  other.path_.clear();
  other.content_.clear();
}

ProjectLock& ProjectLock::operator=(ProjectLock&& other) noexcept {
  if (this != &other) {
    release_lock(path_, content_);
    path_ = std::move(other.path_);
    content_ = std::move(other.content_);
    other.path_.clear();
    other.content_.clear();
  }
  return *this;
}

ProjectLock::~ProjectLock() { release_lock(path_, content_); }

std::optional<ProjectLock> ProjectLock::acquire(const std::string& project, const std::string& by) {
  try {
    if (project.empty()) return std::nullopt;
    const std::string path = lock_path(project);
    const std::string content = lock_content(by);
    // Everything that allocates happens before the file exists; arming the lock afterwards can't throw, so a
    // file this call creates always has an owner that removes it.
    std::optional<ProjectLock> lock(ProjectLock{path});
    std::string armed = content;
    const auto made = [&]() noexcept {
      lock->content_.swap(armed);
      return std::move(lock);
    };

    const int err = create_lock_file(path, content);
    if (err == 0) return made();
    if (err != EEXIST) return std::nullopt;   // no lock can be made here (missing or read-only folder)

    LockFile seen;
    const ReadResult first = read_settled(path, seen);
    if (first == ReadResult::Error) return std::nullopt;
    if (first == ReadResult::Ok && !is_stale(seen)) return std::nullopt;   // a live holder

    // It was released meanwhile, or its holder has ended: try once more, one taker at a time.
    TakeoverGuard guard(project);
    if (guard.busy()) return std::nullopt;
    if (first == ReadResult::Ok) {
      LockFile now;
      const ReadResult again = read_lock(path, now);
      if (again == ReadResult::Error) return std::nullopt;
      if (again == ReadResult::Ok) {
        if (!same_file(now, seen)) return std::nullopt;   // someone else took it over first
        if (const int err = platform::remove_file(path); err != 0 && err != ENOENT) return std::nullopt;
      }
    }
    if (create_lock_file(path, content) == 0) return made();
    return std::nullopt;
  } catch (const std::exception&) {
    return std::nullopt;   // out of memory: treat as "not acquired"
  }
}

std::optional<ProjectLock::Holder> ProjectLock::holder(const std::string& project) {
  try {
    if (project.empty()) return std::nullopt;
    LockFile f;
    if (read_settled(lock_path(project), f) != ReadResult::Ok || !f.parsed || is_stale(f)) return std::nullopt;
    Holder h;
    h.pid = static_cast<int>(f.pid);
    h.by = f.by.empty() ? "cli" : f.by;   // the app and the MCP server always say who they are
    h.since = f.since;
    return h;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

}  // namespace vyrn
