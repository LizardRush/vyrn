// Which system the C++ tests run on (docs/dev/platforms.md section 3): platform-specific tests skip themselves on the
// others with GTEST_SKIP() and a reason; nothing may silently pass.
#pragma once

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#define VYRN_TEST_POSIX 0
#define VYRN_TEST_LINUX 0
#elif defined(__linux__)
#define VYRN_TEST_POSIX 1
#define VYRN_TEST_LINUX 1
#else
#define VYRN_TEST_POSIX 1
#define VYRN_TEST_LINUX 0
#endif

// The few POSIX calls the tests use for temporary folders, the environment and pids, on Windows too.
#if VYRN_TEST_POSIX
#include <stdlib.h>
#include <unistd.h>
#else
#include <io.h>
#include <process.h>
#include <stdlib.h>

#include <random>

// access(), getpid(), chmod() come with the C runtime's POSIX names; _access() only knows these modes.
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef X_OK
#define X_OK 0   // Windows can only tell whether the file is there
#endif
#define popen _popen
#define pclose _pclose
inline int setenv(const char* name, const char* value, int /*overwrite*/) { return _putenv_s(name, value); }
inline int unsetenv(const char* name) { return _putenv_s(name, ""); }
// mkdtemp(): the trailing "XXXXXX" become random letters and the folder is made; nullptr when that fails.
inline char* mkdtemp(char* pattern) {
  std::string text(pattern);
  if (text.size() < 6 || text.compare(text.size() - 6, 6, "XXXXXX") != 0) return nullptr;
  std::random_device random;
  for (int attempt = 0; attempt < 100; ++attempt) {
    for (size_t i = text.size() - 6; i < text.size(); ++i) text[i] = "abcdefghijklmnopqrstuvwxyz0123456789"[random() % 36];
    std::error_code ec;
    if (std::filesystem::create_directory(text, ec) && !ec) {
      text.copy(pattern, text.size());
      return pattern;
    }
  }
  return nullptr;
}
#endif

namespace vyrn_test {

// The variable platform::runtime_dir() starts from (pid files and the panic marker go below it).
#if defined(_WIN32)
inline const char* runtime_env_name() { return "LOCALAPPDATA"; }
#elif defined(__APPLE__)
inline const char* runtime_env_name() { return "TMPDIR"; }
#else
inline const char* runtime_env_name() { return "XDG_RUNTIME_DIR"; }
#endif

// The variable platform::home_dir() reads first.
#if defined(_WIN32)
inline const char* home_env_name() { return "USERPROFILE"; }
#else
inline const char* home_env_name() { return "HOME"; }
#endif

// Windows only lets administrators (or developer mode) make symlinks.
inline bool can_make_symlinks() {
  namespace fs = std::filesystem;
  static const bool can = [] {
    std::error_code ec;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path link = fs::temp_directory_path(ec) / ("vyrn-symlink-probe-" + std::to_string(stamp));
    fs::create_symlink("target", link, ec);
    const bool made = !ec;
    fs::remove(link, ec);
    return made;
  }();
  return can;
}

}  // namespace vyrn_test

#define VYRN_SKIP_WITHOUT_SYMLINKS() \
  if (!vyrn_test::can_make_symlinks()) GTEST_SKIP() << "this system doesn't let Vyrn's tests make symlinks"
