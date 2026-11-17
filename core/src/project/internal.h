// Private helpers shared by the vyrn_project sources (not installed, not used outside core/src/project/).
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace vyrn::project_detail {

namespace fs = std::filesystem;

inline constexpr const char* kLockFileName = ".vyrn-lock";

// Absolute and lexically normal, without a trailing '/' (symlinks are not resolved).
std::string absolute_clean(const std::string& path);

// The file's modification time in seconds since the epoch (follows symlinks), or nullopt.
std::optional<int64_t> mtime_seconds(const fs::path& path);

// True while a process with this pid exists (platform::process_running).
bool pid_running(int64_t pid);

// What a brain folder's checkpoints say (brains/<id>/{latest,best}/current -> save-<n>/brain.json).
struct BrainSummary {
  bool any_save = false;        // a readable latest or best save exists
  bool has_best = false;        // a best 20-round mean is known
  double best_mean20 = 0;
  double time_trained_s = 0;
  int64_t ticks = 0;
  int64_t updated_epoch = 0;    // when the newest save was written (seconds since the epoch), 0 if unknown
  std::string updated;          // the same as local time "YYYY-MM-DD HH:MM", "" if unknown
};
BrainSummary read_brain_summary(const fs::path& brain_dir);

}  // namespace vyrn::project_detail
