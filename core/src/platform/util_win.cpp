// Windows helpers (win_util.h). Named *_win.cpp so that only Windows builds it.
#include "platform/win_util.h"

#include "platform/transport.h"

#include <algorithm>
#include <cerrno>
#include <cwchar>

namespace vyrn::platform::win {

std::wstring widen(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (n <= 0) return {};
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
  return out;
}

std::string narrow(const std::wstring& utf16) {
  if (utf16.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), nullptr, 0, nullptr,
                                    nullptr);
  if (n <= 0) return {};
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), out.data(), n, nullptr, nullptr);
  return out;
}

std::string narrow(const wchar_t* utf16) { return utf16 ? narrow(std::wstring(utf16)) : std::string(); }

int errno_from(DWORD error) {
  switch (error) {
    case ERROR_SUCCESS: return 0;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
    case ERROR_INVALID_NAME: return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_WRITE_PROTECT:
    case ERROR_PRIVILEGE_NOT_HELD: return EACCES;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: return EEXIST;
    case ERROR_DIR_NOT_EMPTY: return ENOTEMPTY;
    case ERROR_DIRECTORY: return ENOTDIR;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL: return ENOSPC;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY: return ENOMEM;
    case ERROR_NOT_SAME_DEVICE: return EXDEV;
    case ERROR_TOO_MANY_OPEN_FILES: return EMFILE;
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA: return EPIPE;
    case ERROR_NOT_SUPPORTED:
    case ERROR_INVALID_FUNCTION: return ENOTSUP;
    case ERROR_CANT_RESOLVE_FILENAME: return ELOOP;
    case ERROR_FILENAME_EXCED_RANGE: return ENAMETOOLONG;
    case ERROR_INVALID_PARAMETER: return EINVAL;
    case ERROR_BAD_EXE_FORMAT: return ENOEXEC;
    default: return EIO;
  }
}

int errno_from_wsa(int error) {
  switch (error) {
    case 0: return 0;
    case WSAEWOULDBLOCK: return EWOULDBLOCK;
    case WSAEINTR: return EINTR;
    case WSAEADDRINUSE: return EADDRINUSE;
    case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
    case WSAEACCES: return EACCES;
    case WSAECONNRESET: return ECONNRESET;
    case WSAECONNABORTED: return ECONNABORTED;
    case WSAECONNREFUSED: return ECONNREFUSED;
    case WSAETIMEDOUT: return ETIMEDOUT;
    case WSAENOTCONN: return ENOTCONN;
    case WSAESHUTDOWN: return EPIPE;
    case WSAENOBUFS: return ENOBUFS;
    case WSAEMFILE: return EMFILE;
    case WSAEINVAL: return EINVAL;
    case WSAENOTSOCK: return EBADF;
    default: return EIO;
  }
}

std::string message(DWORD error) {
  wchar_t* text = nullptr;
  const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                 nullptr, error, 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
  std::string out = n > 0 && text ? narrow(std::wstring(text, n)) : "Windows error " + std::to_string(error);
  if (text) LocalFree(text);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ' || out.back() == '.')) {
    out.pop_back();
  }
  return out;
}

bool winsock_ready() {
  static const bool ready = [] {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return ready;
}

namespace {

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring search(const std::wstring& name) {
  std::vector<wchar_t> buf(MAX_PATH);
  for (int attempt = 0; attempt < 2; ++attempt) {
    const DWORD n = SearchPathW(nullptr, name.c_str(), L".exe", static_cast<DWORD>(buf.size()), buf.data(), nullptr);
    if (n == 0) return {};
    if (n < buf.size()) return std::wstring(buf.data(), n);
    buf.resize(static_cast<size_t>(n) + 1);   // n: the room it needs
  }
  return {};
}

// The name part of "NAME=value" ("=C:" entries keep their leading '=').
std::wstring entry_name(const std::wstring& entry) {
  const size_t eq = entry.find(L'=', entry.empty() || entry[0] != L'=' ? 0 : 1);
  return eq == std::wstring::npos ? entry : entry.substr(0, eq);
}

// This process's environment plus `settings`, sorted by name without regard to case (as CreateProcessW wants it),
// as a block of NUL-terminated strings ending with an empty one.
std::wstring environment_block(const std::vector<std::pair<std::string, std::string>>& settings) {
  std::vector<std::string> entries;
  if (wchar_t* block = GetEnvironmentStringsW()) {
    for (const wchar_t* p = block; *p != L'\0'; p += std::wcslen(p) + 1) entries.push_back(narrow(p));
    FreeEnvironmentStringsW(block);
  }
  for (const auto& [name, value] : settings) set_env_entry(entries, name, value, /*ignore_case=*/true);
  std::vector<std::wstring> wide;
  wide.reserve(entries.size());
  for (const std::string& e : entries) wide.push_back(widen(e));
  std::stable_sort(wide.begin(), wide.end(), [](const std::wstring& a, const std::wstring& b) {
    const std::wstring na = entry_name(a), nb = entry_name(b);
    return CompareStringOrdinal(na.c_str(), static_cast<int>(na.size()), nb.c_str(), static_cast<int>(nb.size()),
                                TRUE) == CSTR_LESS_THAN;
  });
  std::wstring out;
  for (const std::wstring& e : wide) {
    out += e;
    out += L'\0';
  }
  if (out.empty()) out += L'\0';
  out += L'\0';
  return out;
}

}  // namespace

std::wstring find_program(const std::string& program) {
  const std::wstring w = widen(program);
  if (w.empty()) return {};
  if (w.find_first_of(L"\\/:") != std::wstring::npos) {   // a path
    if (exists(w)) return w;
    return exists(w + L".exe") ? w + L".exe" : std::wstring();
  }
  return search(w);
}

DWORD start_child(const std::string& program, const std::vector<std::string>& args,
                  const std::vector<std::pair<std::string, std::string>>& env, Child& child, std::string& step) {
  step.clear();
  const std::wstring app = find_program(program);
  if (app.empty()) return ERROR_FILE_NOT_FOUND;

  SECURITY_ATTRIBUTES inherit{};
  inherit.nLength = sizeof inherit;
  inherit.bInheritHandle = TRUE;
  HANDLE r = nullptr, w = nullptr;
  if (!CreatePipe(&r, &w, &inherit, 64 * 1024)) {
    step = "CreatePipe";
    return GetLastError();
  }
  Handle out_read(r), out_write(w);
  if (!CreatePipe(&r, &w, &inherit, 64 * 1024)) {
    step = "CreatePipe";
    return GetLastError();
  }
  Handle err_read(r), err_write(w);
  SetHandleInformation(out_read.get(), HANDLE_FLAG_INHERIT, 0);   // the parent's ends stay here
  SetHandleInformation(err_read.get(), HANDLE_FLAG_INHERIT, 0);
  Handle nul(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr));
  if (!nul) {
    step = "CreateFileW(NUL)";
    return GetLastError();
  }
  Handle job(CreateJobObjectW(nullptr, nullptr));
  if (!job) {
    step = "CreateJobObjectW";
    return GetLastError();
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof limits)) {
    step = "SetInformationJobObject";
    return GetLastError();
  }

  // Only the three standard handles reach the child (another thread may be starting a child of its own).
  SIZE_T size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
  std::vector<unsigned char> list_memory(size);
  auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_memory.data());
  if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) {
    step = "InitializeProcThreadAttributeList";
    return GetLastError();
  }
  struct ListGuard {
    LPPROC_THREAD_ATTRIBUTE_LIST list;
    ~ListGuard() { DeleteProcThreadAttributeList(list); }
  } guard{list};
  HANDLE inherited[3] = {nul.get(), out_write.get(), err_write.get()};
  if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof inherited, nullptr,
                                 nullptr)) {
    step = "UpdateProcThreadAttribute";
    return GetLastError();
  }

  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof si;
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = nul.get();
  si.StartupInfo.hStdOutput = out_write.get();
  si.StartupInfo.hStdError = err_write.get();
  si.lpAttributeList = list;

  std::vector<std::string> all = {program};
  all.insert(all.end(), args.begin(), args.end());
  std::wstring command = widen(windows_command_line(all));
  std::wstring environment = environment_block(env);
  PROCESS_INFORMATION pi{};
  // Suspended until it is in the job, so nothing it starts can escape the job.
  const DWORD flags = CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED;
  if (!CreateProcessW(app.c_str(), command.data(), nullptr, nullptr, TRUE, flags, environment.data(), nullptr,
                      &si.StartupInfo, &pi)) {
    return GetLastError();
  }
  Handle process(pi.hProcess), thread(pi.hThread);
  if (!AssignProcessToJobObject(job.get(), process.get())) {
    const DWORD error = GetLastError();
    TerminateProcess(process.get(), 1);
    WaitForSingleObject(process.get(), 5000);
    step = "AssignProcessToJobObject";
    return error;
  }
  ResumeThread(thread.get());
  child.process = std::move(process);
  child.job = std::move(job);
  child.out_read = std::move(out_read);
  child.err_read = std::move(err_read);
  child.pid = pi.dwProcessId;
  return 0;   // the child's ends of the pipes and NUL close here: only the child holds them now
}

}  // namespace vyrn::platform::win
