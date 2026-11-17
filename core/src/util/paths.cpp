// vyrn::find_data_dir (docs/dev/core-internals.md section 1, "Data dir lookup").
#include "vyrn/session.h"

#include "platform/system.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace vyrn {

namespace fs = std::filesystem;

namespace {

bool has_worker(const fs::path& dir) {
  std::error_code ec;
  return !dir.empty() && fs::is_regular_file(dir / "python" / "vyrn_worker.py", ec);
}

std::string clean(const fs::path& dir) {
  std::error_code ec;
  const fs::path canonical = fs::weakly_canonical(dir, ec);
  return (ec ? dir : canonical).string();
}

fs::path executable_path(const char* argv0) {
  const std::string exe = platform::executable_path();
  if (!exe.empty()) return exe;
  std::error_code ec;
  if (argv0 && *argv0) return fs::absolute(argv0, ec);
  return {};
}

}  // namespace

std::string find_data_dir(const char* argv0) {
  if (const char* env = std::getenv("VYRN_DATA"); env && *env && has_worker(env)) return clean(env);
  const fs::path exe = executable_path(argv0);
  if (!exe.empty()) {
    for (const std::string& installed : platform::data_dir_candidates(exe.string())) {
      if (has_worker(installed)) return clean(installed);
    }
  }
#ifdef VYRN_SOURCE_DIR
  if (has_worker(VYRN_SOURCE_DIR)) return clean(VYRN_SOURCE_DIR);
#endif
  return "";
}

}  // namespace vyrn
