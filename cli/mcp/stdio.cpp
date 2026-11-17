// run_mcp_server(): the stdio side of the MCP server (docs/dev/mcp.md sections 1 and 4).
//
// One thread does everything: it reads newline-delimited JSON-RPC from stdin, answers on stdout (logs go to
// stderr), follows the trainings and watches for stop requests (SIGINT/SIGTERM/SIGHUP; Windows: Ctrl+C,
// Ctrl+Break, closing the console). The system side is vyrn::platform::ProtocolStdio (vyrn/platform.h). A long tool call (Check, Watch, Stop)
// waits through StdioLoop::wait(), which keeps reading: a ping is answered at once, notifications/cancelled
// cancels the call (or drops a queued request), anything else waits its turn. stdin EOF or a signal stops every
// training gracefully (the brains are saved) before the server exits; a second signal quits at once.
#include "mcp_server.h"

#include "server.h"

#include "vyrn/platform.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <utility>

namespace vyrn::mcp {

namespace {

constexpr size_t kMaxLineBytes = size_t{8} << 20;   // a script is at most 100 KB; this leaves plenty of room
constexpr double kShutdownSeconds = 120;            // saving the brains; a second signal quits at once

const char* signal_name(int sig) { return vyrn::platform::stop_request_name(sig); }

class StdioLoop : public Waiter {
 public:
  StdioLoop(const Options& options, vyrn::platform::ProtocolStdio& io) : server_(options), io_(io) {}

  int run() {
    std::fprintf(stderr, "vyrn mcp: ready; projects in %s\n", server_.context().workspace.allowed_text().c_str());
    while (!stop_) {
      if (!pending_.empty()) {
        Pending p = std::move(pending_.front());
        pending_.pop_front();
        const std::string reply = server_.protocol().handle_line(p.line, *this);
        if (!reply.empty()) write_line(reply);
        continue;
      }
      if (eof_) break;
      poll_input(0.1, false);
      server_.pump();
    }
    return shutdown();
  }

  // Waiter: lets `seconds` pass while the server keeps reading and following trainings.
  bool wait(double seconds) override {
    const double end = now_s() + std::max(0.0, seconds);
    do {
      poll_input(std::max(0.0, std::min(0.05, end - now_s())), true);
      server_.pump();
    } while (now_s() < end);
    return !stop_ && !server_.protocol().current_cancelled();
  }

 private:
  struct Pending {
    std::string line;
    std::string id;   // compact JSON of the request id ("" for notifications and unreadable lines)
  };

  void poll_input(double seconds, bool in_call) {
    const bool read_stdin = !eof_ && !stop_;
    vyrn::platform::ProtocolStdio::Wait got = io_.wait(static_cast<int>(seconds * 1000), read_stdin);
    if (got.stops > 0) note_signal(got.last_stop);
    if (read_stdin && (!got.data.empty() || got.end)) read_input(got, in_call);
  }

  void note_signal(int sig) {
    ++signals_;
    if (stop_) return;
    signal_ = sig;
    stop_ = true;
    std::fprintf(stderr, "vyrn mcp: %s: stopping.\n", signal_name(sig));
  }

  // What one wait() read: bytes (split into lines), and/or the end of stdin.
  void read_input(const vyrn::platform::ProtocolStdio::Wait& got, bool in_call) {
    const char* buf = got.data.data();
    size_t start = 0;
    const size_t n = got.data.size();
    for (size_t i = 0; i < n; ++i) {
      if (buf[i] != '\n') continue;
      if (discarding_) {
        discarding_ = false;
      } else {
        partial_.append(buf + start, i - start);
        on_line(std::move(partial_), in_call);
        partial_.clear();
      }
      start = i + 1;
    }
    if (start < n && !discarding_) {
      partial_.append(buf + start, n - start);
      if (partial_.size() > kMaxLineBytes) {
        partial_.clear();
        discarding_ = true;
        write_line(dump_line(json{{"jsonrpc", "2.0"},
                                  {"id", nullptr},
                                  {"error", json{{"code", kInvalidRequest},
                                                 {"message", "Invalid Request: a message longer than 8 MiB."}}}}));
      }
    }
    if (got.end) eof_ = true;
    if (eof_) {
      if (!partial_.empty() && !discarding_) on_line(std::move(partial_), in_call);   // a last line without '\n'
      partial_.clear();
    }
  }

  void on_line(std::string line, bool in_call) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.find_first_not_of(" \t\r") == std::string::npos) return;
    std::string id;
    const json msg = json::parse(line, nullptr, false);
    if (!msg.is_discarded() && msg.is_object()) {
      const auto m = msg.find("method");
      const std::string method = m != msg.end() && m->is_string() ? m->get<std::string>() : "";
      const bool request = msg.contains("id");
      if (method == "notifications/cancelled" && !request) {
        const auto params = msg.find("params");
        if (params != msg.end() && params->is_object() && params->contains("requestId")) {
          const std::string target = Protocol::id_text((*params)["requestId"]);
          if (!target.empty()) {
            if (in_call && target == server_.protocol().current_request()) {
              server_.protocol().cancel_current();
            } else {   // not started yet: drop it (cancelled requests get no reply)
              pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                            [&](const Pending& p) { return p.id == target; }),
                             pending_.end());
            }
          }
        }
        return;
      }
      if (in_call && request && method == "ping") {   // answered at once, even during a long call
        const std::string reply = server_.protocol().handle(msg, *this);
        if (!reply.empty()) write_line(reply);
        return;
      }
      if (request) id = Protocol::id_text(msg["id"]);
    }
    pending_.push_back({std::move(line), std::move(id)});
  }

  void write_line(const std::string& text) {
    if (broken_) return;
    std::string out = text;
    out += '\n';
    std::string why;
    if (!io_.write_all(out, why)) {
      broken_ = true;
      stop_ = true;
      std::fprintf(stderr, "vyrn mcp: the client stopped reading (%s); stopping.\n", why.c_str());
    }
  }

  int shutdown() {
    const int running = server_.context().runs.running();
    if (running > 0) {
      std::fprintf(stderr, "vyrn mcp: stopping %d training%s and saving %s...\n", running, running == 1 ? "" : "s",
                   running == 1 ? "its brain" : "their brains");
    }
    server_.begin_shutdown();
    const double deadline = now_s() + kShutdownSeconds;
    while (!server_.all_finished() && now_s() < deadline) {
      int sig = 0;
      if (vyrn::platform::wait_stop_requests(100, &sig) > 0) {
        note_signal(sig);
        if (signals_ >= 2) {
          std::fprintf(stderr, "vyrn mcp: %s again: quitting without waiting for the saves.\n", signal_name(sig));
          std::_Exit(128 + sig);   // Python notices that we are gone and exits on its own
        }
      }
      server_.pump();
    }
    if (!server_.all_finished()) std::fprintf(stderr, "vyrn mcp: a training didn't stop in time.\n");
    std::fprintf(stderr, "vyrn mcp: bye.\n");
    return signal_ != 0 ? 128 + signal_ : 0;
  }

  Server server_;
  vyrn::platform::ProtocolStdio& io_;
  std::deque<Pending> pending_;
  std::string partial_;
  bool discarding_ = false;   // inside an over-long line, until its '\n'
  bool eof_ = false, stop_ = false, broken_ = false;
  int signal_ = 0, signals_ = 0;
};

}  // namespace

}  // namespace vyrn::mcp

int run_mcp_server(int argc, char** argv) {
  using namespace vyrn::mcp;
  Options options;
  std::string message;
  const int parsed = parse_options(argc, argv, options, message);
  if (parsed == 1) {
    std::fputs(message.c_str(), stdout);
    std::fflush(stdout);
    return 0;
  }
  if (parsed != 0) {
    std::fprintf(stderr, "vyrn mcp: %s", message.c_str());
    return 2;
  }

  // Stop requests: taken here before any thread exists, so every Session thread inherits the block (POSIX) and
  // only this loop sees them. Python workers get a clean signal mask from the core.
  vyrn::platform::install_stop_handlers(/*hangup=*/true);

  // stdout carries only the protocol: it moves to a private handle, and the process's own stdout becomes stderr so
  // that a stray printf anywhere in the process can't break a message.
  vyrn::platform::ProtocolStdio io;
  std::string why;
  if (!io.open(why)) {
    std::fprintf(stderr, "vyrn mcp: can't use stdout: %s\n", why.c_str());
    return 2;
  }

  int code = 0;
  {
    StdioLoop loop(options, io);
    code = loop.run();
  }   // the Server's destructor ends what is left (everything has finished by now)
  return code;
}
