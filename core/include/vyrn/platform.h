// What the app, vyrn-cli and the MCP server need from the platform layer (docs/dev/platforms.md). Standard library
// only. The implementations live in core/src/platform/ (*_posix.cpp, *_linux.cpp, *_mac.cpp, *_win.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vyrn::platform {

// ---- keys -----------------------------------------------------------------------------------------------------
// Sends a key-up for every key a worker may press: macOS CoreGraphics virtual key codes 0-127, Windows SendInput
// scan codes 1-0x58 plus the extended ones. Call it after a worker that used keys was killed, frozen without an
// answer, or on panic. Linux: does nothing (the kernel releases the keys of a uinput device whose process died).
void release_all_keys();

// ---- start-up -------------------------------------------------------------------------------------------------
// libgomp reads OMP_WAIT_POLICY when it loads, so Linux and macOS set it to PASSIVE and run this program again
// (once; if that fails, the default stays). Windows sets OMP_WAIT_POLICY and KMP_BLOCKTIME for child processes
// only. Call it first thing in main().
void ensure_passive_openmp(char** argv);

// ---- stop requests (Ctrl+C) ---------------------------------------------------------------------------------
// POSIX: SIGINT, SIGTERM (and SIGHUP with hangup = true) stop ending the process: call this before any thread
// starts, so every later thread inherits the block, and take them with wait_stop_requests(). SIGPIPE is ignored.
// Windows: SetConsoleCtrlHandler (Ctrl+C, Ctrl+Break and closing the console are stop requests).
// Calling it again does nothing.
void install_stop_handlers(bool hangup);
// Waits up to `ms` milliseconds (0: just looks) for stop requests. Returns how many came (0 = none) and puts the
// last one's number in *last (SIGINT = 2, SIGTERM = 15, SIGHUP = 1; Windows: Ctrl+C/Ctrl+Break = 2, closing the
// console = 15). The exit status after a stop request is 128 + that number.
int wait_stop_requests(int ms, int* last = nullptr);
// "SIGINT", "SIGTERM", "SIGHUP" (Windows: "Ctrl+C", "the console closing").
const char* stop_request_name(int number);

// The MCP server's stdio. open() moves stdout to a private handle for the protocol and points the process's own
// stdout at stderr, so that a stray printf anywhere can't break a message. wait() reads stdin with a timeout and
// watches the stop requests at the same time (install_stop_handlers() first).
class ProtocolStdio {
 public:
  ProtocolStdio();
  ~ProtocolStdio();
  ProtocolStdio(const ProtocolStdio&) = delete;
  ProtocolStdio& operator=(const ProtocolStdio&) = delete;

  bool open(std::string& why);
  struct Wait {
    int stops = 0;          // stop requests that came (wait_stop_requests())
    int last_stop = 0;      // the last one's number
    std::string data;       // bytes read from stdin (at most one read)
    bool end = false;       // stdin is at its end (or broken)
  };
  // Waits up to `ms` milliseconds for a stop request or (read_input) for input on stdin.
  Wait wait(int ms, bool read_input);
  // Writes all of `bytes` to the protocol's stdout. False when the client stopped reading (why says how).
  bool write_all(const std::string& bytes, std::string& why);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ---- processes ------------------------------------------------------------------------------------------------
int64_t current_pid();
bool process_running(int64_t pid);   // also true for a process of another user
// Ends a program another component started (e.g. the app's Start program): POSIX sends SIGTERM (force: SIGKILL)
// to its process group; Windows (force only) terminates the process and everything it started. False when
// nothing was sent (the caller falls back to its own way, e.g. QProcess::terminate()).
bool stop_process_tree(int64_t pid, bool force);
// Runs argv (argv[0] is looked up in PATH) with stdin empty and waits at most timeout_s; keeps up to max_bytes of
// stdout. False (why: plain English) when it couldn't start, timed out (it is killed with what it started) or
// exited with a status other than 0.
bool run_capture(const std::vector<std::string>& argv, double timeout_s, size_t max_bytes, std::string& out,
                 std::string& why);

// ---- files ----------------------------------------------------------------------------------------------------
// Replaces `path` with `content` crash-safely: writes `temp` first (an old file of that name is removed; it is
// created exclusively, never through a symlink, with the POSIX permissions `mode`), fsyncs it, then renames it over
// `path` atomically (Windows: MoveFileExW with MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH). Returns 0 or the
// errno (std::strerror() says what went wrong); `temp` is gone either way.
int write_file_atomic(const std::string& path, const std::string& temp, const std::string& content, int mode);
bool is_writable(const std::string& path);   // access(W_OK); Windows: not read-only

// ---- places ---------------------------------------------------------------------------------------------------
std::string executable_path();   // this program, absolute; "" when unknown
std::string home_dir();          // the user's home folder (HOME / USERPROFILE); "" when unknown
// Pid files and the panic marker: $XDG_RUNTIME_DIR/vyrn (Linux), $TMPDIR/vyrn (macOS), %LOCALAPPDATA%\Vyrn\run.
std::string runtime_dir();
// The Python used when a project names none: /usr/bin/python3 (Linux), python3 from Homebrew or PATH (macOS),
// python.exe from PATH (Windows).
std::string default_python();

}  // namespace vyrn::platform
