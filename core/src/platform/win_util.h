// Windows helpers for the *_win.cpp files: UTF-8 <-> UTF-16, Win32 / Winsock errors as errno values, a HANDLE
// owner, and starting a child process inside a job object. Only *_win.cpp files include this header.
#pragma once

#include <winsock2.h>   // before windows.h
#include <windows.h>

#include <string>
#include <utility>
#include <vector>

namespace vyrn::platform::win {

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& utf16);
std::string narrow(const wchar_t* utf16);

int errno_from(DWORD error);   // a Win32 error (GetLastError()) as the nearest errno value (EIO when none fits)
int errno_from_wsa(int error);   // a Winsock error (WSAGetLastError())
std::string message(DWORD error);   // the system's words for a Win32 error, as UTF-8, without the final ".\r\n"

// Owns a HANDLE (nullptr and INVALID_HANDLE_VALUE both mean "none").
class Handle {
 public:
  Handle() = default;
  explicit Handle(HANDLE h) : h_(h) {}
  ~Handle() { reset(); }
  Handle(Handle&& other) noexcept : h_(other.release()) {}
  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;

  HANDLE get() const { return h_; }
  explicit operator bool() const { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }
  HANDLE release() { return std::exchange(h_, nullptr); }
  void reset(HANDLE h = nullptr) {
    if (*this) CloseHandle(h_);
    h_ = h;
  }

 private:
  HANDLE h_ = nullptr;
};

bool winsock_ready();   // WSAStartup(2.2), once per process

// `program` as CreateProcessW needs it: a path is used as it is (".exe" added when only that exists), a bare name
// is searched like a shell would (SearchPathW, ".exe" added). "" when there is no such program.
std::wstring find_program(const std::string& program);

// A child started by start_child(): stdin from NUL, stdout and stderr into new pipes (the parent's read ends here),
// inside a new job object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE (closing `job` kills everything left in it).
struct Child {
  Handle process, job, out_read, err_read;
  DWORD pid = 0;
};
// Starts program + args with this process's environment plus `env` (names ignore case; later wins), no console
// window, inheriting only its three standard handles. Returns 0, or the Win32 error; `step` then names the call
// that failed ("" when the program itself couldn't be started).
DWORD start_child(const std::string& program, const std::vector<std::string>& args,
                  const std::vector<std::pair<std::string, std::string>>& env, Child& child, std::string& step);

}  // namespace vyrn::platform::win
