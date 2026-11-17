// The portable pieces of the worker transport (docs/dev/platforms.md section 1), kept free of system calls so that
// every platform's unit tests exercise them: the token and the first line of each Windows worker connection, the
// control bytes, and the command line / environment a child gets.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vyrn::platform {

// ---- control bytes (the "ctl" connection; POSIX uses signals instead) ----
enum class Control : char {
  Interrupt = 'I',   // like SIGINT: stuck user code raises KeyboardInterrupt
  Freeze = 'F',      // like SIGUSR1: release all keys, refuse key actions until "resume"
};
inline char control_byte(Control c) { return static_cast<char>(c); }

// ---- the token and the connection lines ----
constexpr size_t kTokenBytes = 16;        // 32 lowercase hex characters
constexpr size_t kMaxConnectionLine = 80;  // "<token> main\n" is 38 bytes: anything longer is refused

std::string token_from_bytes(const uint8_t (&bytes)[kTokenBytes]);
bool valid_token(std::string_view token);   // 32 lowercase hex characters
std::string worker_address(int port);       // "127.0.0.1:<port>" (VYRN_ADDR)

enum class ConnectionRole { None, Main, Ctl };
// "<token> main\n" / "<token> ctl\n": what a worker sends first on each connection.
std::string connection_line(std::string_view token, ConnectionRole role);
// The role a first line (without its '\n'; a trailing '\r' is allowed) asks for; None for a wrong token or anything
// else.
ConnectionRole parse_connection_line(std::string_view line, std::string_view token);

// Collects the first bytes of a new connection until its line is complete.
class ConnectionGreeting {
 public:
  enum class State { Waiting, Accepted, Refused };
  explicit ConnectionGreeting(std::string token) : token_(std::move(token)) {}
  // Adds received bytes. Accepted: role() is known, and leftover() holds the bytes after the line (frames on the
  // main connection). Refused: a wrong line, or no '\n' within kMaxConnectionLine bytes.
  State add(const char* data, size_t size);
  State state() const { return state_; }
  ConnectionRole role() const { return role_; }
  const std::string& leftover() const { return leftover_; }

 private:
  std::string token_;
  std::string line_, leftover_;
  State state_ = State::Waiting;
  ConnectionRole role_ = ConnectionRole::None;
};

// The worker's two connections: each role is taken once; a second "main" or "ctl" is refused.
class ConnectionSlots {
 public:
  bool take(ConnectionRole role);   // true when this role was still free (and is now taken)
  bool complete() const { return main_ && ctl_; }

 private:
  bool main_ = false, ctl_ = false;
};

// ---- starting a child ----
// One argument quoted for a Windows command line (the CommandLineToArgvW / MSVC CRT rules): unchanged when it
// has no space, tab, newline or quote; otherwise in quotes, with the backslashes before a quote doubled.
std::string quote_windows_arg(const std::string& arg);
std::string windows_command_line(const std::vector<std::string>& args);

// Sets NAME=value in an environment given as "NAME=value" entries: earlier entries with that name are removed and
// the new one is added at the end. ignore_case: Windows names ("Path" == "PATH"; "=C:" drive entries keep their
// leading '=').
void set_env_entry(std::vector<std::string>& entries, const std::string& name, const std::string& value,
                   bool ignore_case);

}  // namespace vyrn::platform
