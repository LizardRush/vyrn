// worker_child.h on Windows (docs/dev/platforms.md section 1): the worker connects to 127.0.0.1:<port> twice with a
// token (platform::WorkerListener sorts the connections out), "main" carries the frames and "ctl" the control bytes;
// it runs in a job object, so kill() = TerminateJobObject. stdout/stderr are anonymous pipes, read by polling
// (PeekNamedPipe) on the io thread.
#include "platform/worker_child.h"

#include "platform/files.h"
#include "platform/system.h"
#include "platform/transport.h"
#include "platform/win_util.h"
#include "platform/worker_listener.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace vyrn::platform {

namespace {

constexpr UINT kKilledExitCode = 137;   // 128 + SIGKILL, as a shell would show it

void close_socket(SOCKET& s) {
  if (s != INVALID_SOCKET) closesocket(s);
  s = INVALID_SOCKET;
}

}  // namespace

struct WorkerChild::Impl {
  mutable std::mutex mu;   // started/reaped and the control connection: any thread may interrupt/freeze/kill
  win::Child child;
  bool started = false;
  bool reaped = false;
  WorkerListener listener;
  SOCKET main = INVALID_SOCKET;   // the request thread's
  SOCKET ctl = INVALID_SOCKET;    // guarded by mu
  std::string early;              // frame bytes that came together with the main connection's first line
  win::Handle wake;               // stop_output()

  ~Impl() {
    close_socket(main);
    close_socket(ctl);
  }

  bool exited() const { return started && WaitForSingleObject(child.process.get(), 0) == WAIT_OBJECT_0; }

  void send_control(Control c) {
    std::lock_guard<std::mutex> lock(mu);
    if (ctl == INVALID_SOCKET || reaped) return;
    const char byte = control_byte(c);
    ::send(ctl, &byte, 1, 0);   // non-blocking; the worker reads them at once, so the buffer never fills
  }

  // Waits up to timeout_ms for the worker's two connections (main and ctl).
  void connect(int timeout_ms) {
    if (!listener.accept(timeout_ms, [this] { return exited(); })) return;
    main = static_cast<SOCKET>(listener.take_main());
    early = listener.take_early();
    std::lock_guard<std::mutex> lock(mu);
    ctl = static_cast<SOCKET>(listener.take_ctl());
  }
};

WorkerChild::WorkerChild() : impl_(std::make_unique<Impl>()) {}

WorkerChild::~WorkerChild() { kill(); }

void WorkerChild::start(const ChildSpec& spec) {
  Impl& s = *impl_;
  if (!win::winsock_ready()) throw StartError("WSAStartup", "Windows sockets are not available");
  uint8_t bytes[kTokenBytes];
  if (!random_bytes(bytes, sizeof bytes)) throw StartError("BCryptGenRandom", "no random numbers");

  // One listener on 127.0.0.1, any free port; the worker gets its address and the token.
  int err = 0;
  if (!s.listener.open(token_from_bytes(bytes), err)) throw StartError("listen", error_text(err));
  s.wake.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!s.wake) throw StartError("CreateEventW", win::message(GetLastError()));

  std::vector<std::pair<std::string, std::string>> env = {{"VYRN_ADDR", worker_address(s.listener.port())},
                                                          {"VYRN_TOKEN", s.listener.token()}};
  env.insert(env.end(), spec.env.begin(), spec.env.end());
  std::string step;
  const DWORD error = win::start_child(spec.program, spec.args, env, s.child, step);
  if (error != 0) {
    throw StartError(step, error == ERROR_FILE_NOT_FOUND && step.empty() ? error_text(ENOENT) : win::message(error));
  }
  std::lock_guard<std::mutex> lock(s.mu);
  s.started = true;
}

Io WorkerChild::send(const uint8_t* data, size_t size, size_t& n, std::string& /*problem*/) {
  Impl& s = *impl_;
  if (s.main == INVALID_SOCKET) return s.exited() ? Io::End : Io::WouldBlock;
  const int sent = ::send(s.main, reinterpret_cast<const char*>(data), static_cast<int>(std::min<size_t>(size, INT_MAX)), 0);
  if (sent > 0) {
    n = static_cast<size_t>(sent);
    return Io::Done;
  }
  if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) return Io::WouldBlock;
  return Io::Closed;   // reset or aborted: the worker is gone
}

Io WorkerChild::receive(uint8_t* data, size_t size, size_t& n, std::string& problem) {
  Impl& s = *impl_;
  if (!s.early.empty()) {
    n = std::min(size, s.early.size());
    std::memcpy(data, s.early.data(), n);
    s.early.erase(0, n);
    return Io::Done;
  }
  if (s.main == INVALID_SOCKET) return s.exited() ? Io::End : Io::WouldBlock;   // not connected yet
  const int got = recv(s.main, reinterpret_cast<char*>(data), static_cast<int>(std::min<size_t>(size, INT_MAX)), 0);
  if (got > 0) {
    n = static_cast<size_t>(got);
    return Io::Done;
  }
  if (got == 0) return Io::End;
  const int error = WSAGetLastError();
  if (error == WSAEWOULDBLOCK) return Io::WouldBlock;
  if (error != WSAECONNRESET && error != WSAECONNABORTED) problem = error_text(win::errno_from_wsa(error));
  return Io::Closed;
}

void WorkerChild::wait(bool for_write, int timeout_ms) {
  Impl& s = *impl_;
  if (s.main == INVALID_SOCKET) {   // the hello is due: take the connections first
    s.connect(timeout_ms);
    return;
  }
  WSAPOLLFD p{};
  p.fd = s.main;
  p.events = for_write ? POLLWRNORM : POLLRDNORM;
  WSAPoll(&p, 1, timeout_ms);
}

// 'I': the worker's watchdog thread calls _thread.interrupt_main(), like SIGINT.
void WorkerChild::interrupt() { impl_->send_control(Control::Interrupt); }

// 'F': release all keys and refuse key actions until "resume", like SIGUSR1.
void WorkerChild::freeze() { impl_->send_control(Control::Freeze); }

void WorkerChild::kill() {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.mu);
  if (!s.started || s.reaped) return;
  TerminateJobObject(s.child.job.get(), kKilledExitCode);   // the worker and everything it started
  WaitForSingleObject(s.child.process.get(), 5000);
  s.reaped = true;
}

std::string WorkerChild::exit_note() const {
  const Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.mu);
  if (!s.started || s.reaped || WaitForSingleObject(s.child.process.get(), 0) != WAIT_OBJECT_0) return {};
  DWORD code = 0;
  if (!GetExitCodeProcess(s.child.process.get(), &code)) return {};
  if (code == 0) return "it exited";
  if (code >= 0xC0000000u) {   // an NTSTATUS: it crashed (0xC0000005 = access violation, ...)
    char text[48];
    std::snprintf(text, sizeof text, "it crashed (0x%08lX)", static_cast<unsigned long>(code));
    return text;
  }
  return "exit status " + std::to_string(code);
}

bool WorkerChild::alive() const {
  const Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.mu);
  return s.started && !s.reaped && WaitForSingleObject(s.child.process.get(), 0) == WAIT_TIMEOUT;
}

int64_t WorkerChild::pid() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->started ? static_cast<int64_t>(impl_->child.pid) : -1;
}

// Anonymous pipes can't be waited on: look at both about every 10 ms, read whatever is there.
void WorkerChild::pump_output(const std::function<void(int, const char*, size_t)>& on_data,
                              const std::function<void(int)>& on_end) {
  Impl& s = *impl_;
  HANDLE pipes[2] = {s.child.out_read.get(), s.child.err_read.get()};
  bool open[2] = {true, true};
  std::vector<char> chunk(64 * 1024);
  while (open[0] || open[1]) {
    bool got_any = false;
    for (int i = 0; i < 2; ++i) {
      for (int reads = 0; open[i] && reads < 16; ++reads) {   // then the other stream, so a flood can't starve it
        DWORD available = 0;
        if (!PeekNamedPipe(pipes[i], nullptr, 0, nullptr, &available, nullptr)) {
          on_end(i);   // ERROR_BROKEN_PIPE: the worker (and all it started) closed this stream
          open[i] = false;
          break;
        }
        if (available == 0) break;
        DWORD got = 0;
        const DWORD want = std::min<DWORD>(available, static_cast<DWORD>(chunk.size()));
        if (!ReadFile(pipes[i], chunk.data(), want, &got, nullptr) || got == 0) {
          on_end(i);
          open[i] = false;
          break;
        }
        on_data(i, chunk.data(), got);
        got_any = true;
      }
    }
    if (WaitForSingleObject(s.wake.get(), got_any ? 0 : 10) == WAIT_OBJECT_0) return;   // stop_output()
  }
}

void WorkerChild::stop_output() {
  if (impl_->wake) SetEvent(impl_->wake.get());
}

}  // namespace vyrn::platform
