#include "util/files.h"

#include "platform/files.h"
#include "platform/system.h"

#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace vyrn::util {

namespace fs = std::filesystem;

namespace {

[[noreturn]] void fail(const std::string& what, const std::string& path, int err) {
  throw std::runtime_error("Vyrn can't " + what + " " + path + ": " + platform::error_text(err) + ".");
}

}  // namespace

bool read_text_file(const std::string& path, std::string& out, std::string* why) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (why) *why = std::string("can't open it (") + std::strerror(errno) + ")";
    return false;
  }
  std::ostringstream text;
  text << in.rdbuf();
  if (in.bad()) {
    if (why) *why = "reading it failed";
    return false;
  }
  out = text.str();
  return true;
}

void write_text_file_atomic(const std::string& path, const std::string& text) {
  const std::string temp = path + ".tmp-" + std::to_string(platform::current_pid());
  int err = 0;
  platform::File file = platform::File::create(temp, platform::File::Create::Truncate, false, 0644, err);
  if (!file) fail("write", temp, err);
  if (const int write_err = file.write_all(text.data(), text.size()); write_err != 0) {
    file.close();
    platform::remove_file(temp);
    fail("write", temp, write_err);
  }
  err = file.sync();
  if (const int close_err = file.close(); err == 0) err = close_err;
  if (err != 0) {
    platform::remove_file(temp);
    fail("save", temp, err);
  }
  if (const int rename_err = platform::rename_replace(temp, path); rename_err != 0) {
    platform::remove_file(temp);
    fail("replace", path, rename_err);
  }
  // Make the rename itself durable (best effort: some file systems refuse fsync on folders).
  const std::string folder = fs::path(path).has_parent_path() ? fs::path(path).parent_path().string() : ".";
  platform::sync_path(folder, true);
}

std::string unique_child_name(const std::string& parent, const std::string& base) {
  std::error_code ec;
  std::string name = base;
  for (int n = 2; fs::exists(fs::path(parent) / name, ec); ++n) name = base + "-" + std::to_string(n);
  return name;
}

bool is_safe_name(const std::string& name) {
  if (name.empty() || name.size() > 64) return false;
  for (const char c : name) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return false;
  }
  return true;
}

}  // namespace vyrn::util
