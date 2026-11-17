// Shared helpers for the vyrn_project tests (test_project).
#pragma once

#include "platform_test.h"

#if VYRN_TEST_POSIX
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <sys/utime.h>
#endif

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

namespace project_test {

namespace fs = std::filesystem;

// A fresh folder under $TMPDIR (or /tmp), removed with everything in it at the end of the test.
class TempDir {
 public:
  TempDir() {
    std::string pattern = (fs::temp_directory_path() / "vyrn-project-test-XXXXXX").string();
    if (::mkdtemp(pattern.data())) path_ = pattern;
  }
  ~TempDir() {
    if (path_.empty()) return;
    std::error_code ec;
    // Read-only copies made by a test must not stop the clean-up.
    for (fs::recursive_directory_iterator it(path_, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
      std::error_code perm_ec;
      if (!it->is_symlink(perm_ec)) fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, perm_ec);
    }
    fs::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }
  std::string operator/(const std::string& rel) const { return (fs::path(path_) / rel).string(); }

 private:
  std::string path_;
};

inline void write_file(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

inline std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

// Sets a file's modification time (seconds since the epoch). nofollow: change a symlink itself.
inline void set_mtime(const fs::path& path, int64_t epoch, bool nofollow = false) {
#if VYRN_TEST_POSIX
  struct timespec times[2];
  times[0].tv_sec = static_cast<time_t>(epoch);
  times[0].tv_nsec = 0;
  times[1] = times[0];
  ::utimensat(AT_FDCWD, path.c_str(), times, nofollow ? AT_SYMLINK_NOFOLLOW : 0);
#else
  (void)nofollow;   // Windows: the link's target changes (tests that need a symlink skip themselves there)
  __utimbuf64 times{epoch, epoch};
  _wutime64(path.c_str(), &times);
#endif
}

inline int64_t now_epoch() { return static_cast<int64_t>(std::time(nullptr)); }

// "2026-11-13T15:30:12" in local time -> seconds since the epoch.
inline int64_t local_epoch(int y, int mo, int d, int h, int mi, int s) {
  std::tm tm{};
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = s;
  tm.tm_isdst = -1;
  return static_cast<int64_t>(std::mktime(&tm));
}

// One checkpoint save the way the core writes it (core/src/brain/checkpoint.cpp + run/brain_host.cpp):
// brains/<id>/<which>/save-<n>/{model.pt, optim.pt, brain.json} and brains/<id>/<which>/current = "save-<n>\n".
struct SaveSpec {
  std::optional<double> best_mean20;
  double seconds = 0;
  int64_t ticks = 0;
  int64_t rounds = 0;
  std::string saved;          // "2026-11-13T15:30:12" ("" = leave the field out)
  bool model = true;          // write model.pt
  bool pointer = true;        // point `current` at this save
};

inline void write_brain_save(const fs::path& project, const std::string& id, const std::string& which, int n,
                             const SaveSpec& spec) {
  const fs::path which_dir = project / "brains" / id / which;
  const std::string save = "save-" + std::to_string(n);
  const fs::path dir = which_dir / save;
  fs::create_directories(dir);
  nlohmann::json meta = {
      {"format", 1},
      {"vyrn", "0.1.0"},
      {"brain", id},
      {"shape", {{"kind", "picture"}, {"in_c", 3}, {"in_h", 10}, {"in_w", 10}, {"d", 0}, {"small_picture", true},
                 {"frames", 4}, {"choices", {"left", "right", "stay"}}, {"inputs", nlohmann::json::array()}}},
      {"net", "SmallCNN"},
      {"net_description", "small CNN for 10x10 pictures"},
      {"config", {{"batch", 32}, {"lr", 1e-4}}},
      {"obs", {{"kind", "picture"}, {"shape", {10, 10, 3}}}},
      {"choices", {"left", "right", "stay"}},
      {"labels", {"left", "right", "stay"}},
      {"effects", {"function", "function", "nothing"}},
      {"counters",
       {{"ticks", spec.ticks}, {"rounds", spec.rounds}, {"updates", spec.ticks / 4}, {"seconds", spec.seconds}}},
      {"normalizer", nullptr},
      {"best_mean20", spec.best_mean20 ? nlohmann::json(*spec.best_mean20) : nlohmann::json(nullptr)},
      {"dqn", {{"updates", spec.ticks / 4}, {"reward_scale", 1.0}}},
      {"checkpoint", {{"format", 1}, {"save", save}}},
  };
  if (!spec.saved.empty()) meta["saved"] = spec.saved;
  write_file(dir / "brain.json", meta.dump(2));
  if (spec.model) write_file(dir / "model.pt", "not really torch weights");
  if (which == "latest") write_file(dir / "optim.pt", "not really an optimizer");
  if (spec.pointer) write_file(which_dir / "current", save + "\n");
}

}  // namespace project_test
