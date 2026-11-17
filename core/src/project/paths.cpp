// Path safety for tools that take file and folder names from outside (the MCP server): safe_join, is_inside.
#include "vyrn/project.h"

#include <filesystem>
#include <stdexcept>
#include <system_error>

namespace vyrn {

namespace {

namespace fs = std::filesystem;

constexpr size_t kMaxFileNameBytes = 255;   // NAME_MAX on Linux file systems

// The name as it may appear inside an error message: control characters shown as '?', at most 60 bytes.
std::string shown(const std::string& name) {
  std::string out;
  for (const char c : name) {
    if (out.size() >= 60) {
      out += "...";
      break;
    }
    out += static_cast<unsigned char>(c) < 0x20 || c == 0x7f ? '?' : c;
  }
  return out;
}

// Absolute, symlinks resolved as far as the path exists, the rest normalised. Empty on errors.
fs::path resolved(const fs::path& p) {
  std::error_code ec;
  const fs::path abs = fs::absolute(p, ec);
  if (ec) return {};
  fs::path out = fs::weakly_canonical(abs, ec);
  if (ec) return {};
  return out;
}

// True when every component of `base` starts `p` (so "/a/bc" is not inside "/a/b").
bool starts_with(const fs::path& p, const fs::path& base) {
  auto it = p.begin();
  for (const fs::path& part : base) {
    if (part.empty()) continue;   // a trailing '/'
    while (it != p.end() && it->empty()) ++it;
    if (it == p.end() || *it != part) return false;
    ++it;
  }
  return true;
}

}  // namespace

std::string safe_join(const std::string& project, const std::string& file_name) {
  if (project.empty()) throw std::invalid_argument("No project folder was given.");
  if (file_name.empty()) throw std::invalid_argument("Please give a file name, such as points.py.");
  if (file_name.find('\0') != std::string::npos) {
    throw std::invalid_argument("A file name can't contain a NUL character.");
  }
  if (file_name.find('/') != std::string::npos || file_name.find('\\') != std::string::npos) {
    throw std::invalid_argument("\"" + shown(file_name) +
                                "\" is a path. Please give just the name of a file directly inside the project "
                                "folder, such as points.py.");
  }
  if (file_name.front() == '.') {
    throw std::invalid_argument("\"" + shown(file_name) +
                                "\" starts with a dot. Hidden files and \"..\" can't be used here; please give a "
                                "plain file name such as points.py.");
  }
  for (const char c : file_name) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
      throw std::invalid_argument("\"" + shown(file_name) +
                                  "\" contains a control character (such as a line break). Please give a plain "
                                  "file name such as points.py.");
    }
  }
  if (file_name.size() > kMaxFileNameBytes) {
    throw std::invalid_argument("That file name is too long (at most 255 bytes).");
  }
  return (fs::path(project) / file_name).string();
}

bool is_inside(const std::string& dir, const std::string& path) {
  if (dir.empty() || path.empty()) return false;
  if (dir.find('\0') != std::string::npos || path.find('\0') != std::string::npos) return false;
  const fs::path base = resolved(dir);
  if (base.empty()) return false;
  // Resolve the path as given, and once more after removing "..": both must stay inside. The second catches a
  // missing folder followed by ".." and a symlink ("dir/new/../link-out/x"), which weakly_canonical leaves
  // unresolved because it stops at the first missing part.
  const fs::path as_given = resolved(path);
  std::error_code ec;
  const fs::path normal = resolved(fs::absolute(path, ec).lexically_normal());
  if (ec || as_given.empty() || normal.empty()) return false;
  return starts_with(as_given, base) && starts_with(normal, base);
}

}  // namespace vyrn
