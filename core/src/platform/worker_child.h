// The Python worker as a child process with its frame connection (docs/dev/platforms.md section 1, protocol.md
// section 1). The transport is chosen by platform:
//   POSIX:   a socketpair end as fd 3 (VYRN_FD=3), the worker leads its own process group, SIGINT = interrupt,
//            SIGUSR1 = freeze, SIGKILL to the group = kill.
//   Windows: 127.0.0.1:<port> + a token (VYRN_ADDR, VYRN_TOKEN); the worker connects twice, "main" (the frames)
//            and "ctl" (control bytes 'I' interrupt, 'F' freeze); a job object with KILL_ON_JOB_CLOSE, so kill =
//            TerminateJobObject.
// start() and the connection calls belong to one thread at a time; interrupt(), freeze(), kill(), exit_note(),
// alive() and pid() may be called from any thread; pump_output() runs on its own thread until stop_output().
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace vyrn::platform {

struct ChildSpec {
  std::string program;                                    // a path, or a name looked up in PATH
  std::vector<std::string> args;                          // after the program
  std::vector<std::pair<std::string, std::string>> env;   // set on top of this process's environment; later wins
};

// start() failed. step: the system call that failed while setting up ("" = the program itself couldn't be
// started); reason: the system's words for it.
struct StartError : std::runtime_error {
  StartError(std::string step, std::string reason);
  std::string step, reason;
};

enum class Io {
  Done,         // `n` bytes moved (at least one)
  WouldBlock,   // nothing now: wait() and try again
  End,          // the worker closed the connection (or exited before connecting)
  Closed,       // the connection broke; `problem` says how ("" for a reset)
};

class WorkerChild {
 public:
  WorkerChild();
  ~WorkerChild();   // kill(), then everything is closed (stop the pump_output() thread first)
  WorkerChild(const WorkerChild&) = delete;
  WorkerChild& operator=(const WorkerChild&) = delete;

  // Starts the child with stdin from the null device, stdout and stderr into pipes, and the frame connection.
  // Throws StartError (nothing is left running).
  void start(const ChildSpec& spec);

  // ---- the frame connection (never blocks) ----
  Io send(const uint8_t* data, size_t size, size_t& n, std::string& problem);
  Io receive(uint8_t* data, size_t size, size_t& n, std::string& problem);
  // Returns when the connection may be ready (for_write: to send), after timeout_ms (-1: no limit), or early;
  // callers try again. Windows: first accepts the worker's connections (main and ctl) meanwhile.
  void wait(bool for_write, int timeout_ms);

  // ---- process control (any thread) ----
  void interrupt();   // SIGINT to the worker's pid / 'I'
  void freeze();      // SIGUSR1 to the worker's pid / 'F'
  void kill();        // the worker and everything it started, then waits for it. Safe to call again.
  std::string exit_note() const;   // how it ended ("it exited", "exit status 2", ...); "" while it runs
  bool alive() const;              // started and not ended yet
  int64_t pid() const;             // -1 before start()

  // ---- stdout (stream 0) and stderr (stream 1), on the io thread ----
  // Passes on what the worker writes until both streams end (on_end(stream) for each) or stop_output() is called
  // (from any thread).
  void pump_output(const std::function<void(int stream, const char* data, size_t size)>& on_data,
                   const std::function<void(int stream)>& on_end);
  void stop_output();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vyrn::platform
