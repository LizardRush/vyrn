// run_capture() and stop_process_tree() on Windows (vyrn/platform.h).
#include "vyrn/platform.h"

#include "platform/files.h"
#include "platform/win_util.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cerrno>
#include <vector>

namespace vyrn::platform {

namespace {

// The processes `pid` started, their children, and so on (Toolhelp snapshot), deepest first.
void collect_children(const std::vector<PROCESSENTRY32W>& all, DWORD pid, std::vector<DWORD>& out, int depth) {
  if (depth > 32) return;
  for (const PROCESSENTRY32W& p : all) {
    if (p.th32ParentProcessID == pid && p.th32ProcessID != pid && p.th32ProcessID != 0) {
      collect_children(all, p.th32ProcessID, out, depth + 1);
      out.push_back(p.th32ProcessID);
    }
  }
}

}  // namespace

// Windows has no process groups: terminate the program and what it started (force only; a polite stop is the
// caller's, e.g. QProcess::terminate()).
bool stop_process_tree(int64_t pid, bool force) {
  if (!force || pid <= 0 || pid > 0xFFFFFFFFLL) return false;
  std::vector<PROCESSENTRY32W> all;
  win::Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
  if (snapshot) {
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    for (BOOL ok = Process32FirstW(snapshot.get(), &entry); ok; ok = Process32NextW(snapshot.get(), &entry)) {
      all.push_back(entry);
    }
  }
  std::vector<DWORD> victims;
  collect_children(all, static_cast<DWORD>(pid), victims, 0);
  victims.push_back(static_cast<DWORD>(pid));
  bool any = false;
  for (const DWORD victim : victims) {
    win::Handle h(OpenProcess(PROCESS_TERMINATE, FALSE, victim));
    if (h && TerminateProcess(h.get(), 1)) any = true;
  }
  return any;
}

bool run_capture(const std::vector<std::string>& argv, double timeout_s, size_t max_bytes, std::string& out,
                 std::string& why) {
  out.clear();
  if (argv.empty()) {
    why = "nothing to run";
    return false;
  }
  win::Child child;
  std::string step;
  const std::vector<std::string> args(argv.begin() + 1, argv.end());
  if (const DWORD error = win::start_child(argv[0], args, {}, child, step); error != 0) {
    const std::string reason =
        error == ERROR_FILE_NOT_FOUND && step.empty() ? error_text(ENOENT) : win::message(error);
    why = "can't start " + argv[0] + ": " + (step.empty() ? reason : step + " failed (" + reason + ")");
    return false;
  }
  std::string err;
  HANDLE pipes[2] = {child.out_read.get(), child.err_read.get()};
  bool open[2] = {true, true};
  bool timed_out = false;
  const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(std::max(0.0, timeout_s) * 1000);
  char buf[8192];
  while (open[0] || open[1]) {
    if (GetTickCount64() >= deadline) {
      timed_out = true;
      break;
    }
    bool got_any = false;
    for (int i = 0; i < 2; ++i) {
      DWORD available = 0;
      if (!open[i]) continue;
      if (!PeekNamedPipe(pipes[i], nullptr, 0, nullptr, &available, nullptr)) {
        open[i] = false;   // the end of this stream
        continue;
      }
      if (available == 0) continue;
      DWORD got = 0;
      if (!ReadFile(pipes[i], buf, std::min<DWORD>(available, sizeof buf), &got, nullptr) || got == 0) {
        open[i] = false;
        continue;
      }
      std::string& into = i == 0 ? out : err;
      const size_t cap = i == 0 ? max_bytes : 4096;
      if (into.size() < cap) into.append(buf, std::min(static_cast<size_t>(got), cap - into.size()));
      got_any = true;
    }
    if (!got_any) Sleep(10);
  }
  if (!timed_out) {   // both streams ended: give it the rest of the time to exit
    const ULONGLONG now = GetTickCount64();
    const DWORD left = now < deadline ? static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, 0x7FFFFFFF)) : 0;
    timed_out = WaitForSingleObject(child.process.get(), left) == WAIT_TIMEOUT;
  }
  if (timed_out) {
    TerminateJobObject(child.job.get(), 1);   // with everything it started
    WaitForSingleObject(child.process.get(), 5000);
    why = argv[0] + " took longer than " + std::to_string(static_cast<int>(timeout_s)) + " s";
    return false;
  }
  DWORD code = 1;
  GetExitCodeProcess(child.process.get(), &code);
  if (code != 0) {
    while (!err.empty() && (err.back() == '\n' || err.back() == '\r')) err.pop_back();
    const auto last = err.rfind('\n');
    why = argv[0] + " failed" + (err.empty() ? std::string() : ": " + err.substr(last == std::string::npos ? 0 : last + 1));
    return false;
  }
  return true;
}

}  // namespace vyrn::platform
