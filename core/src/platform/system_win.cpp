// system.h and the "places" of vyrn/platform.h on Windows.
#include "platform/system.h"

#include "platform/files.h"
#include "platform/win_util.h"

#include <bcrypt.h>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <vector>

namespace vyrn::platform {

namespace fs = std::filesystem;
using win::narrow;
using win::widen;

namespace {

std::string env_text(const char* name) {
  const DWORD n = GetEnvironmentVariableW(widen(name).c_str(), nullptr, 0);
  if (n == 0) return {};
  std::vector<wchar_t> buf(n);
  const DWORD got = GetEnvironmentVariableW(widen(name).c_str(), buf.data(), n);
  return got > 0 && got < n ? narrow(std::wstring(buf.data(), got)) : std::string();
}

std::string without_trailing_separators(std::string path) {
  while (path.size() > 3 && (path.back() == '\\' || path.back() == '/')) path.pop_back();
  return path;
}

// The boot time in seconds since 1970: now minus the time since boot.
long long boot_seconds() {
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  ULARGE_INTEGER t;
  t.LowPart = now.dwLowDateTime;
  t.HighPart = now.dwHighDateTime;
  const long long now_s = static_cast<long long>((t.QuadPart - 116444736000000000ULL) / 10000000ULL);
  return now_s - static_cast<long long>(GetTickCount64() / 1000);
}

}  // namespace

bool local_time(std::time_t t, std::tm& out) { return localtime_s(&out, &t) == 0; }

bool utc_time(std::time_t t, std::tm& out) { return gmtime_s(&out, &t) == 0; }

// Both the C runtime's copy (getenv) and the process's own block (child processes) change.
bool set_env(const std::string& name, const std::string& value) {
  const bool crt = _putenv_s(name.c_str(), value.c_str()) == 0;
  return SetEnvironmentVariableW(widen(name).c_str(), widen(value).c_str()) && crt;
}

bool unset_env(const std::string& name) {
  const bool crt = _putenv_s(name.c_str(), "") == 0;   // an empty value removes it
  return SetEnvironmentVariableW(widen(name).c_str(), nullptr) && crt;
}

int64_t current_pid() { return static_cast<int64_t>(GetCurrentProcessId()); }

bool process_running(int64_t pid) {
  if (pid <= 0 || pid > 0xFFFFFFFFLL) return false;
  win::Handle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid)));
  if (!h) return GetLastError() == ERROR_ACCESS_DENIED;   // it exists, but isn't ours to look at
  DWORD code = 0;
  return GetExitCodeProcess(h.get(), &code) && code == STILL_ACTIVE;
}

std::optional<ProcessIdentity> process_identity(int64_t pid) {
  if (pid <= 0 || pid > 0xFFFFFFFFLL) return std::nullopt;
  win::Handle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid)));
  if (!h) return std::nullopt;
  DWORD code = 0;
  if (!GetExitCodeProcess(h.get(), &code) || code != STILL_ACTIVE) return std::nullopt;   // ended (a handle keeps it)
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(h.get(), &created, &exited, &kernel, &user)) return std::nullopt;
  ULARGE_INTEGER t;
  t.LowPart = created.dwLowDateTime;
  t.HighPart = created.dwHighDateTime;
  ProcessIdentity id;
  id.start = static_cast<long long>(t.QuadPart);   // 100 ns since 1601: unique across boots
  id.group = pid;
  return id;
}

const std::string& boot_id() {
  static const std::string id = std::to_string(boot_seconds());
  return id;
}

// Computed from two clocks, the boot time wobbles by a second or two (and by clock corrections): allow two minutes.
bool same_boot(const std::string& recorded) {
  char* end = nullptr;
  const long long then = std::strtoll(recorded.c_str(), &end, 10);
  if (recorded.empty() || end == nullptr || *end != '\0') return false;
  const long long now = boot_seconds();
  return then - now <= 120 && now - then <= 120;
}

std::vector<std::string> home_dirs() {
  std::vector<std::string> homes;
  for (const char* name : {"USERPROFILE", "HOME"}) {
    const std::string home = without_trailing_separators(env_text(name));
    if (!home.empty() && (homes.empty() || homes.front() != home)) homes.push_back(home);
  }
  return homes;
}

std::string home_dir() {
  if (std::string profile = env_text("USERPROFILE"); !profile.empty()) return profile;
  const std::string drive = env_text("HOMEDRIVE"), path = env_text("HOMEPATH");
  if (!drive.empty() && !path.empty()) return drive + path;
  return env_text("HOME");
}

std::string executable_path() {
  std::vector<wchar_t> buf(MAX_PATH);
  for (int attempt = 0; attempt < 6; ++attempt) {
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return {};
    if (n < buf.size()) return narrow(std::wstring(buf.data(), n));
    buf.resize(buf.size() * 2);   // cut off: try a bigger buffer
  }
  return {};
}

std::vector<std::string> data_dir_candidates(const std::string& exe) {
  const fs::path dir = fs::path(widen(exe)).parent_path();
  return {narrow((dir / L"share" / L"vyrn").wstring()), narrow((dir.parent_path() / L"share" / L"vyrn").wstring())};
}

// %LOCALAPPDATA%\Vyrn\run (its parent is made here: the caller only makes the last folder).
std::string runtime_dir() {
  std::string base = env_text("LOCALAPPDATA");
  if (base.empty()) {
    wchar_t temp[MAX_PATH + 1];
    const DWORD n = GetTempPathW(MAX_PATH + 1, temp);
    base = n > 0 && n <= MAX_PATH ? narrow(std::wstring(temp, n)) : std::string(".");
  }
  base = without_trailing_separators(base) + "\\Vyrn";
  CreateDirectoryW(widen(base).c_str(), nullptr);   // fails harmlessly when it exists
  return base + "\\run";
}

// python.exe on PATH, else the py launcher (python.org installs it without touching PATH), else the Microsoft Store
// alias, else "python" (the error then names it).
std::string default_python() {
  const std::wstring python = win::find_program("python");
  const bool store_alias = python.find(L"\\WindowsApps\\") != std::wstring::npos;
  if (!python.empty() && !store_alias) return narrow(python);
  const std::wstring py = win::find_program("py");
  if (!py.empty()) return narrow(py);
  return python.empty() ? "python" : narrow(python);
}

void lower_thread_priority(int /*nice_delta*/) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }

bool random_bytes(void* out, size_t size) {
  return BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(size),
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;   // STATUS_SUCCESS
}

// The OpenMP runtime here is not re-executed into: the variables reach child processes (docs/dev/platforms.md).
void ensure_passive_openmp(char** /*argv*/) {
  if (env_text("OMP_WAIT_POLICY").empty()) set_env("OMP_WAIT_POLICY", "PASSIVE");
  if (env_text("KMP_BLOCKTIME").empty()) set_env("KMP_BLOCKTIME", "0");
}

}  // namespace vyrn::platform
