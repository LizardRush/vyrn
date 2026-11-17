// The Python worker process: spawn, handshake, requests with timeouts, killing it with everything it started
// (docs/dev/protocol.md sections 1, 2 and 7; docs/dev/core-internals.md section 2). The transport and the process
// control are the platform's (platform/worker_child.h, docs/dev/platforms.md section 1).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ipc/proto.h"

namespace vyrn::ipc {

enum class Stream { Stdout, Stderr };

struct WorkerOptions {
  std::string python;                        // interpreter (vyrn.json "python"); empty = platform::default_python()
  std::string data_dir;                      // the folder that contains python/vyrn_worker.py
  std::string project_dir;                   // the user's project (the worker's only argument)
  std::vector<std::pair<std::string, std::string>> env;            // extra/override env vars
  std::function<void(Stream, const std::string& line)> on_line;    // called from the io thread, full speed
  std::string script;   // the Python script to run; empty = <data_dir>/python/vyrn_worker.py
};

// The worker replied "error" (protocol.md section 3.7). what() = "<script>:<line>: <message>".
struct WorkerError : std::runtime_error {
  explicit WorkerError(json error);
  json error;   // the whole error reply
};

// A request (or the hello) took too long. The worker and everything it started have been killed.
struct WorkerTimeout : std::runtime_error {
  WorkerTimeout(const std::string& message, json error);
  json error;   // the worker's reply to the interrupt (normally kind "timeout"), or null if none came
};

// The worker is gone: EOF on its connection, or it broke the protocol. It has been killed with what it started.
struct WorkerDied : std::runtime_error {
  WorkerDied(const std::string& message, std::vector<std::string> stderr_tail, bool panic);
  std::vector<std::string> stderr_tail;   // its last stderr lines (at most 20)
  bool panic = false;                     // the panic key (scripts/vyrn-panic, the app's hotkey) stopped it
};

// One Python worker. spawn(), hello() and request() belong to one thread at a time; freeze(),
// kill_group(), alive(), pid() and stderr_tail() may be called from any thread (e.g. to stop a
// worker while another thread waits in request(), which then throws WorkerDied).
// A WorkerProcess runs one worker once: after a timeout or death, make a new one to respawn.
class WorkerProcess {
 public:
  explicit WorkerProcess(WorkerOptions options);
  ~WorkerProcess();   // kill_group() if still running, join the io thread, remove the pid file
  WorkerProcess(const WorkerProcess&) = delete;
  WorkerProcess& operator=(const WorkerProcess&) = delete;

  // The connection (POSIX: socketpair -> fd 3; Windows: 127.0.0.1 + token), pipes -> stdout/stderr, the null
  // device -> stdin, its own process group (Windows: job object), pid file. Throws std::runtime_error (plain
  // English) if Python can't be started.
  void spawn();
  // Waits for the worker's unprompted hello and returns it. Throws WorkerTimeout, or WorkerDied
  // (also for a hello whose "proto" is not 1: "library/app version mismatch").
  json hello(double timeout_s = 15);
  // Sends req with the next "id" (1, 2, ...) and returns the matching reply.
  //   error reply      -> WorkerError (the worker keeps running)
  //   no reply in time -> interrupt the worker (SIGINT / 'I'), wait grace_s for its report, kill it with what it
  //                       started (SIGKILL to the group / TerminateJobObject), WorkerTimeout
  //   EOF / bad frame  -> kill, WorkerDied (panic = the panic marker appeared)
  Frame request(json req, double timeout_s, double grace_s);
  void freeze();       // out-of-band key release: SIGUSR1 to the worker pid / 'F'
  void kill_group();   // kill the worker with everything it started, wait for it, remove the pid file
  bool alive() const;  // spawned and not exited yet
  int64_t pid() const; // the worker's pid (POSIX: also its process group id); -1 before spawn()
  std::vector<std::string> stderr_tail(size_t n) const;   // the last n stderr lines

 private:
  struct State;
  std::unique_ptr<State> s_;
};

// Deletes pid files in platform::runtime_dir() whose process no longer matches (at app/CLI start).
void sweep_stale_pid_files();

}  // namespace vyrn::ipc
