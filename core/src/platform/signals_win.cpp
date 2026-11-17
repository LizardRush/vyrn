// Stop requests and the MCP server's stdio on Windows (vyrn/platform.h). SetConsoleCtrlHandler turns Ctrl+C,
// Ctrl+Break and closing the console into stop requests (the same graceful stop as SIGINT/SIGTERM on POSIX; a
// second one makes the caller exit at once). stdin is read by a thread of its own, since a pipe can't be waited on.
#include "vyrn/platform.h"

#include "platform/win_util.h"

#include <fcntl.h>
#include <io.h>
#include <signal.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vyrn::platform {

namespace {

HANDLE g_stop_event = nullptr;   // auto-reset: set by each stop request
std::atomic<int> g_stops{0};
std::atomic<int> g_last_stop{0};

BOOL WINAPI on_console_event(DWORD type) {
  int number = 0;
  switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT: number = SIGINT; break;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT: number = SIGTERM; break;
    default: return FALSE;
  }
  g_last_stop.store(number);
  g_stops.fetch_add(1);
  if (g_stop_event) SetEvent(g_stop_event);
  // Returning from a close/logoff/shutdown event ends the process at once: wait here instead, so the main thread
  // gets the few seconds Windows allows to save; it ends the process itself when done.
  if (number == SIGTERM) Sleep(INFINITE);
  return TRUE;
}

int take_stops(int* last) {
  const int count = g_stops.exchange(0);
  if (count > 0 && last) *last = g_last_stop.load();
  return count;
}

}  // namespace

void install_stop_handlers(bool /*hangup*/) {
  static std::once_flag once;
  std::call_once(once, [] {
    g_stop_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SetConsoleCtrlHandler(on_console_event, TRUE);
  });
}

int wait_stop_requests(int ms, int* last) {
  if (g_stops.load() == 0) {
    if (g_stop_event == nullptr) {
      if (ms > 0) Sleep(static_cast<DWORD>(ms));
      return 0;
    }
    WaitForSingleObject(g_stop_event, ms < 0 ? INFINITE : static_cast<DWORD>(ms));
  }
  return take_stops(last);
}

const char* stop_request_name(int number) {
  switch (number) {
    case SIGINT: return "Ctrl+C";
    case SIGTERM: return "the console closing";
    default: return "a stop request";
  }
}

// ---- ProtocolStdio ---------------------------------------------------------------------------------------------

namespace {

constexpr size_t kMaxBuffered = size_t{1} << 20;   // the reader waits when this much hasn't been taken yet

struct Input {
  std::mutex mu;
  std::condition_variable room;
  std::string data;
  bool end = false;
  bool closing = false;
  win::Handle ready{CreateEventW(nullptr, FALSE, FALSE, nullptr)};   // auto-reset: data came, or the end
};

void read_stdin(std::shared_ptr<Input> input, HANDLE in) {
  std::vector<char> buf(64 * 1024);
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(input->mu);
      input->room.wait(lock, [&] { return input->data.size() < kMaxBuffered || input->closing; });
      if (input->closing) return;
    }
    DWORD got = 0;
    const BOOL ok = ReadFile(in, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr);
    {
      std::lock_guard<std::mutex> lock(input->mu);
      if (!ok || got == 0) {
        input->end = true;   // EOF (ERROR_BROKEN_PIPE) or a broken stdin
      } else {
        input->data.append(buf.data(), got);
      }
    }
    SetEvent(input->ready.get());
    if (!ok || got == 0) return;
  }
}

}  // namespace

struct ProtocolStdio::Impl {
  HANDLE out = nullptr;   // the protocol's own copy of stdout
  std::shared_ptr<Input> input = std::make_shared<Input>();
};

ProtocolStdio::ProtocolStdio() : impl_(std::make_unique<Impl>()) {}

ProtocolStdio::~ProtocolStdio() {
  {
    std::lock_guard<std::mutex> lock(impl_->input->mu);
    impl_->input->closing = true;
  }
  impl_->input->room.notify_all();   // a reader blocked in ReadFile keeps its own reference and ends with the process
  if (impl_->out) CloseHandle(impl_->out);
}

bool ProtocolStdio::open(std::string& why) {
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  HANDLE copy = nullptr;
  if (out == nullptr || out == INVALID_HANDLE_VALUE ||
      !DuplicateHandle(GetCurrentProcess(), out, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
    why = out == nullptr || out == INVALID_HANDLE_VALUE ? "there is no stdout" : win::message(GetLastError());
    return false;
  }
  impl_->out = copy;
  // The process's own stdout (Win32 and the C runtime) writes to stderr from now on.
  std::fflush(stdout);
  SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE));
  _dup2(_fileno(stderr), _fileno(stdout));
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  if (in != nullptr && in != INVALID_HANDLE_VALUE) {
    std::thread(read_stdin, impl_->input, in).detach();
  } else {
    impl_->input->end = true;
  }
  return true;
}

ProtocolStdio::Wait ProtocolStdio::wait(int ms, bool read_input) {
  Wait w;
  Input& input = *impl_->input;
  const auto has_input = [&] {
    std::lock_guard<std::mutex> lock(input.mu);
    return !input.data.empty() || input.end;
  };
  if (g_stops.load() == 0 && !(read_input && has_input())) {
    HANDLE handles[2];
    DWORD count = 0;
    if (g_stop_event) handles[count++] = g_stop_event;
    if (read_input) handles[count++] = input.ready.get();
    if (count == 0) {
      Sleep(static_cast<DWORD>(ms < 0 ? 0 : ms));
    } else {
      WaitForMultipleObjects(count, handles, FALSE, ms < 0 ? INFINITE : static_cast<DWORD>(ms));
    }
  }
  w.stops = take_stops(&w.last_stop);
  if (read_input) {
    {
      std::lock_guard<std::mutex> lock(input.mu);
      w.data.swap(input.data);
      w.end = input.end;
    }
    input.room.notify_all();
  }
  return w;
}

bool ProtocolStdio::write_all(const std::string& bytes, std::string& why) {
  size_t done = 0;
  while (done < bytes.size()) {
    DWORD wrote = 0;
    const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - done, size_t{1} << 20));
    if (!WriteFile(impl_->out, bytes.data() + done, chunk, &wrote, nullptr) || wrote == 0) {
      why = win::message(GetLastError());
      return false;
    }
    done += wrote;
  }
  return true;
}

}  // namespace vyrn::platform
