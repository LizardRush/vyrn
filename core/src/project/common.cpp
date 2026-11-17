// Small helpers shared by the vyrn_project sources.
#include "project/internal.h"

#include "platform/files.h"
#include "vyrn/platform.h"

#include <cerrno>
#include <string>
#include <system_error>

namespace vyrn::project_detail {

std::string absolute_clean(const std::string& path) {
  std::error_code ec;
  fs::path p = fs::absolute(fs::path(path), ec);
  if (ec) p = fs::path(path);
  p = p.lexically_normal();
  std::string text = p.string();
  while (text.size() > 1 && text.back() == '/') text.pop_back();
  return text;
}

std::optional<int64_t> mtime_seconds(const fs::path& path) {
  platform::FileInfo info;
  if (platform::file_info(path.string(), true, info) != 0) return std::nullopt;
  return info.mtime_s;
}

bool pid_running(int64_t pid) { return platform::process_running(pid); }

}  // namespace vyrn::project_detail
