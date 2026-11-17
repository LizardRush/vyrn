// The portable pieces of the worker transport (transport.h).
#include "platform/transport.h"

#include "platform/worker_child.h"

#include <algorithm>
#include <cctype>

namespace vyrn::platform {

namespace {

bool is_lower_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

char lower_ascii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool same_name(std::string_view a, std::string_view b, bool ignore_case) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (ignore_case ? lower_ascii(a[i]) != lower_ascii(b[i]) : a[i] != b[i]) return false;
  }
  return true;
}

// The name part of "NAME=value". Windows keeps per-drive folders as "=C:=C:\..." : the name starts with '='.
std::string_view entry_name(std::string_view entry) {
  const size_t eq = entry.find('=', entry.empty() || entry[0] != '=' ? 0 : 1);
  return eq == std::string_view::npos ? entry : entry.substr(0, eq);
}

}  // namespace

StartError::StartError(std::string what, std::string why)
    : std::runtime_error(what.empty() ? why : what + " failed (" + why + ")"),
      step(std::move(what)),
      reason(std::move(why)) {}

std::string token_from_bytes(const uint8_t (&bytes)[kTokenBytes]) {
  static const char kHex[] = "0123456789abcdef";
  std::string token;
  token.reserve(kTokenBytes * 2);
  for (const uint8_t b : bytes) {
    token += kHex[b >> 4];
    token += kHex[b & 0x0f];
  }
  return token;
}

bool valid_token(std::string_view token) {
  return token.size() == kTokenBytes * 2 && std::all_of(token.begin(), token.end(), is_lower_hex);
}

std::string worker_address(int port) { return "127.0.0.1:" + std::to_string(port); }

std::string connection_line(std::string_view token, ConnectionRole role) {
  std::string line(token);
  line += role == ConnectionRole::Main ? " main\n" : role == ConnectionRole::Ctl ? " ctl\n" : " ?\n";
  return line;
}

ConnectionRole parse_connection_line(std::string_view line, std::string_view token) {
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  if (!valid_token(token) || line.size() <= token.size() + 1 || line.substr(0, token.size()) != token ||
      line[token.size()] != ' ') {
    return ConnectionRole::None;
  }
  const std::string_view role = line.substr(token.size() + 1);
  if (role == "main") return ConnectionRole::Main;
  if (role == "ctl") return ConnectionRole::Ctl;
  return ConnectionRole::None;
}

ConnectionGreeting::State ConnectionGreeting::add(const char* data, size_t size) {
  if (state_ != State::Waiting) {
    if (state_ == State::Accepted) leftover_.append(data, size);
    return state_;
  }
  const std::string_view bytes(data, size);
  const size_t newline = bytes.find('\n');
  line_.append(bytes.substr(0, newline == std::string_view::npos ? bytes.size() : newline));
  if (line_.size() > kMaxConnectionLine) {
    state_ = State::Refused;
    return state_;
  }
  if (newline == std::string_view::npos) return state_;
  role_ = parse_connection_line(line_, token_);
  state_ = role_ == ConnectionRole::None ? State::Refused : State::Accepted;
  if (state_ == State::Accepted) leftover_.assign(bytes.substr(newline + 1));
  return state_;
}

bool ConnectionSlots::take(ConnectionRole role) {
  bool& slot = role == ConnectionRole::Main ? main_ : ctl_;
  if (role == ConnectionRole::None || slot) return false;
  slot = true;
  return true;
}

std::string quote_windows_arg(const std::string& arg) {
  if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) return arg;
  std::string out = "\"";
  for (size_t i = 0;; ++i) {
    size_t backslashes = 0;
    while (i < arg.size() && arg[i] == '\\') {
      ++backslashes;
      ++i;
    }
    if (i == arg.size()) {   // before the closing quote: every backslash doubled
      out.append(backslashes * 2, '\\');
      break;
    }
    if (arg[i] == '"') {   // a quote: its backslashes doubled, and the quote escaped
      out.append(backslashes * 2 + 1, '\\');
    } else {
      out.append(backslashes, '\\');
    }
    out += arg[i];
  }
  out += '"';
  return out;
}

std::string windows_command_line(const std::vector<std::string>& args) {
  std::string line;
  for (const std::string& arg : args) {
    if (!line.empty()) line += ' ';
    line += quote_windows_arg(arg);
  }
  return line;
}

void set_env_entry(std::vector<std::string>& entries, const std::string& name, const std::string& value,
                   bool ignore_case) {
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [&](const std::string& entry) { return same_name(entry_name(entry), name, ignore_case); }),
                entries.end());
  entries.push_back(name + "=" + value);
}

}  // namespace vyrn::platform
