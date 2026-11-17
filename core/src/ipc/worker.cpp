#include "ipc/worker.h"

#include "platform/files.h"
#include "platform/system.h"
#include "platform/worker_child.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace vyrn::ipc {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int64_t kProtocolVersion = 1;
constexpr size_t kMaxLineBytes = 8192;   // longer output lines are passed on in pieces
constexpr size_t kTailKeep = 200;        // stderr lines kept for stderr_tail()
constexpr size_t kDiedTailLines = 20;    // stderr lines in WorkerDied
constexpr size_t kRecvChunk = 256 * 1024;

// ---- small helpers ---------------------------------------------------------------------------

std::string errno_text(int err) { return platform::error_text(err); }

// A point in time to give up waiting. Infinity (or NaN) seconds = never.
class Deadline {
 public:
  explicit Deadline(double seconds) {
    if (!(seconds < 1e9)) {
      never_ = true;
      return;
    }
    at_ = Clock::now() +
          std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(std::max(seconds, 0.0)));
  }
  bool expired() const { return !never_ && Clock::now() >= at_; }
  int poll_ms() const {   // for poll(): -1 = forever; rounded up so we never spin
    if (never_) return -1;
    const auto left = at_ - Clock::now();
    if (left <= Clock::duration::zero()) return 0;
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(left).count();
    return static_cast<int>(std::min<long long>(ms, INT_MAX));
  }

 private:
  bool never_ = false;
  Clock::time_point at_{};
};

std::string seconds_text(double seconds) {
  char text[32];
  std::snprintf(text, sizeof text, "%g", seconds);
  return std::string(text) + (seconds == 1 ? " second" : " seconds");
}

// ---- reading JSON from the worker without trusting its types ----------------------------------

std::string str_field(const json& j, const char* key) {
  if (!j.is_object()) return {};
  const auto found = j.find(key);
  return found != j.end() && found->is_string() ? found->get<std::string>() : std::string();
}

bool int_field(const json& j, const char* key, int64_t& out) {
  if (!j.is_object()) return false;
  const auto found = j.find(key);
  if (found == j.end() || !found->is_number_integer()) return false;   // bools are not integers here
  if (found->is_number_unsigned()) {
    const auto value = found->get<uint64_t>();
    if (value > static_cast<uint64_t>(INT64_MAX)) return false;
    out = static_cast<int64_t>(value);
    return true;
  }
  out = found->get<int64_t>();
  return true;
}

std::string short_json(const json& j) {
  std::string text = j.dump(-1, ' ', false, json::error_handler_t::replace);
  if (text.size() > 120) text = text.substr(0, 117) + "...";
  return text;
}

// "points.py:7: name 'scor' is not defined"
std::string describe_worker_error(const json& error) {
  std::string message = str_field(error, "message");
  if (message.empty()) message = "Python reported an error";
  const std::string script = str_field(error, "script");
  if (script.empty()) return message;
  int64_t line = 0;
  std::string where = script;
  if (int_field(error, "line", line) && line > 0) where += ":" + std::to_string(line);
  return where + ": " + message;
}

bool is_reply_type(const std::string& t) {
  return t == "ok" || t == "result" || t == "spec" || t == "bye" || t == "error";
}

// ---- files: pid files, the panic marker ---------------------------------------------------------

std::string runtime_dir() { return platform::runtime_dir(); }

bool read_small_file(const std::string& path, std::string& text) {
  int err = 0;
  platform::File file = platform::File::open_read(path, false, err);
  if (!file) return false;
  text.clear();
  char chunk[1024];
  for (;;) {
    size_t n = 0;
    if (file.read(chunk, sizeof chunk, n) != 0) return false;
    if (n == 0) return true;
    text.append(chunk, n);
    if (text.size() > 64 * 1024) return false;
  }
}

// Writes <runtime>/<pid>.pid = "<pid> <starttime> <pgid>\n" for scripts/vyrn-panic, through a temporary file and an
// atomic rename so the script never reads half a file. Returns its path, or "" and why not.
std::string write_pid_file(int64_t pid, std::string& problem) {
  const std::string dir = runtime_dir();
  if (const int err = platform::make_dir(dir, 0700); err != 0 && err != EEXIST) {
    problem = "can't create " + dir + " (" + errno_text(err) + ")";
    return {};
  }
  const std::optional<platform::ProcessIdentity> id = platform::process_identity(pid);
  if (!id) {
    problem = "can't tell when process " + std::to_string(pid) + " started";
    return {};
  }
  const std::string path = dir + "/" + std::to_string(pid) + ".pid";
  const std::string temp = path + ".tmp";
  const std::string text = std::to_string(pid) + " " + std::to_string(id->start) + " " + std::to_string(id->group) + "\n";
  int err = 0;
  platform::File file = platform::File::create(temp, platform::File::Create::Truncate, false, 0600, err);
  if (file) err = file.write_all(text.data(), text.size());
  if (const int closed = file.close(); err == 0) err = closed;
  if (err == 0) err = platform::rename_replace(temp, path);
  if (err != 0) {
    problem = "can't write " + path + " (" + errno_text(err) + ")";
    platform::remove_file(temp);
    return {};
  }
  return path;
}

// A pid file "<pid> <starttime> <pgid>" still describes a running process (same pid, same start time).
bool pid_file_matches(const std::string& path) {
  std::string text;
  if (!read_small_file(path, text)) return false;
  std::istringstream fields(text);
  long long pid = 0;
  long long start = 0;
  long long pgid = 0;
  if (!(fields >> pid >> start >> pgid) || pid <= 0 || pid > INT_MAX) return false;
  const std::optional<platform::ProcessIdentity> now = platform::process_identity(pid);
  return now && now->start == start;
}

// When a worker started, by the clock that stamps files (the panic marker must be newer).
struct FileTime {
  int64_t s = 0;
  int32_t ns = 0;
};

// The panic marker counts if it appeared after this worker started (an older one is left over from an
// earlier panic). It is deleted once seen.
bool take_panic_marker(const FileTime& started) {
  const std::string path = runtime_dir() + "/panic";
  platform::FileInfo info;
  if (platform::file_info(path, true, info) != 0) return false;
  if (info.mtime_s < started.s || (info.mtime_s == started.s && info.mtime_ns < started.ns)) return false;
  platform::remove_file(path);
  return true;
}

// ---- spawning ----------------------------------------------------------------------------------

// The worker's variables (protocol.md section 1; the platform adds its transport's: VYRN_FD or VYRN_ADDR and
// VYRN_TOKEN), then options.env; a later value replaces an earlier one with the same name.
std::vector<std::pair<std::string, std::string>> worker_settings(const WorkerOptions& options) {
  std::vector<std::pair<std::string, std::string>> settings = {
      {"OMP_NUM_THREADS", "1"},
      {"OPENBLAS_NUM_THREADS", "1"},
      {"PYTHONDONTWRITEBYTECODE", "1"},
  };
  if (!options.data_dir.empty()) settings.emplace_back("VYRN_DATA", options.data_dir);
  settings.insert(settings.end(), options.env.begin(), options.env.end());
  return settings;
}

// ---- output lines ------------------------------------------------------------------------------

// Splits a byte stream into lines (without the '\n'). A line longer than kMaxLineBytes comes out
// in pieces, cut between UTF-8 characters when possible.
class LineSplitter {
 public:
  template <class Emit>
  void add(const char* data, size_t size, Emit&& emit) {
    const char* end = data + size;
    while (data < end) {
      const auto* newline = static_cast<const char*>(std::memchr(data, '\n', static_cast<size_t>(end - data)));
      partial_.append(data, newline ? newline : end);
      while (partial_.size() > kMaxLineBytes) {
        const size_t cut = utf8_cut(partial_, kMaxLineBytes);
        emit(partial_.substr(0, cut));
        partial_.erase(0, cut);
      }
      if (newline == nullptr) break;
      emit(partial_);
      partial_.clear();
      data = newline + 1;
    }
  }
  // At EOF: pass on a last line that had no '\n'.
  template <class Emit>
  void finish(Emit&& emit) {
    if (!partial_.empty()) emit(partial_);
    partial_.clear();
  }

 private:
  // Largest cut <= max that does not split a UTF-8 character (text.size() > max).
  static size_t utf8_cut(const std::string& text, size_t max) {
    auto continuation = [&](size_t i) { return (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80; };
    size_t cut = max;
    for (int back = 0; back < 3 && cut > 0 && continuation(cut); ++back) --cut;
    return cut == 0 || continuation(cut) ? max : cut;
  }

  std::string partial_;
};

enum class Got { Frame, Timeout, Closed, Broken };
enum class Sent { Ok, Timeout, Closed };
enum class Death { Eof, Broken, Mismatch };

}  // namespace

// ---- WorkerDied / WorkerError / WorkerTimeout -----------------------------------------------------

WorkerError::WorkerError(json err) : std::runtime_error(describe_worker_error(err)), error(std::move(err)) {}

WorkerTimeout::WorkerTimeout(const std::string& message, json err)
    : std::runtime_error(message), error(std::move(err)) {}

WorkerDied::WorkerDied(const std::string& message, std::vector<std::string> tail, bool panic_stop)
    : std::runtime_error(message), stderr_tail(std::move(tail)), panic(panic_stop) {}

// ---- the process state ---------------------------------------------------------------------------

struct WorkerProcess::State {
  explicit State(WorkerOptions o) : options(std::move(o)) {}

  WorkerOptions options;

  // The child: its connection, stdout/stderr and process control (thread-safe where worker_child.h says so).
  platform::WorkerChild child;

  // proc_mu guards these: kill_group may come from any thread.
  mutable std::mutex proc_mu;
  std::string pid_file;   // "" when none was written
  FileTime started;       // just before the start (the panic marker must be newer)

  // The connection's frames. Only the thread inside spawn()/hello()/request() touches these.
  Decoder decoder;
  std::vector<uint8_t> recv_buf;
  int64_t last_id = 0;
  bool spawned = false;
  bool greeted = false;
  bool over = false;   // timed out or died: no more requests

  // stdout/stderr, passed on by the io thread.
  std::thread io;
  mutable std::mutex io_mu;
  std::condition_variable io_cv;
  bool streams_closed = false;
  std::deque<std::string> tail;   // the last kTailKeep stderr lines

  // -- process control --

  void kill_group() {
    child.kill();   // the worker and everything it started (POSIX: SIGKILL to the group; Windows: the job)
    std::lock_guard<std::mutex> lock(proc_mu);
    if (!pid_file.empty()) {
      platform::remove_file(pid_file);
      pid_file.clear();
    }
  }

  // How the worker ended, if it already has. "" while it runs.
  std::string exit_note() { return child.exit_note(); }

  void wait_exit(double seconds) {
    const Deadline deadline(seconds);
    while (exit_note().empty() && !deadline.expired()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // -- output --

  void emit(Stream stream, const std::string& line) {
    if (stream == Stream::Stderr) {
      std::lock_guard<std::mutex> lock(io_mu);
      tail.push_back(line);
      if (tail.size() > kTailKeep) tail.pop_front();
    }
    if (options.on_line) {
      try {
        options.on_line(stream, line);
      } catch (...) {
        // A throwing callback must not end the io thread (the worker would block on a full pipe).
      }
    }
  }

  void io_main(const std::string& note) {
    if (!note.empty()) emit(Stream::Stderr, note);
    LineSplitter splitters[2];
    const Stream streams[2] = {Stream::Stdout, Stream::Stderr};
    child.pump_output(
        [&](int i, const char* data, size_t size) {
          splitters[i].add(data, size, [&](const std::string& line) { emit(streams[i], line); });
        },
        [&](int i) {   // EOF (or a read error): this stream is over
          splitters[i].finish([&](const std::string& line) { emit(streams[i], line); });
        });
    {
      std::lock_guard<std::mutex> lock(io_mu);
      streams_closed = true;
    }
    io_cv.notify_all();
  }

  bool wait_streams_closed(double seconds) {
    if (!io.joinable()) return true;
    std::unique_lock<std::mutex> lock(io_mu);
    return io_cv.wait_for(lock, std::chrono::duration<double>(seconds), [this] { return streams_closed; });
  }

  std::vector<std::string> last_lines(size_t n) const {
    std::lock_guard<std::mutex> lock(io_mu);
    const size_t first = tail.size() > n ? tail.size() - n : 0;
    return {tail.begin() + static_cast<std::ptrdiff_t>(first), tail.end()};
  }

  // -- the connection --

  Sent send_all(const std::vector<uint8_t>& bytes, const Deadline& deadline) {
    size_t done = 0;
    std::string problem;
    while (done < bytes.size()) {
      size_t n = 0;
      switch (child.send(bytes.data() + done, bytes.size() - done, n, problem)) {
        case platform::Io::Done:
          done += n;
          continue;
        case platform::Io::WouldBlock:
          if (deadline.expired()) return Sent::Timeout;
          child.wait(true, deadline.poll_ms());
          continue;
        case platform::Io::End:
        case platform::Io::Closed:
          return Sent::Closed;   // EPIPE, ECONNRESET: the worker is gone
      }
    }
    return Sent::Ok;
  }

  Got read_frame(const Deadline& deadline, Frame& frame, std::string& problem) {
    for (;;) {
      try {
        if (auto next = decoder.next()) {
          frame = std::move(*next);
          return Got::Frame;
        }
      } catch (const ProtocolError& err) {
        problem = err.what();
        return Got::Broken;
      }
      size_t n = 0;
      switch (child.receive(recv_buf.data(), recv_buf.size(), n, problem)) {
        case platform::Io::Done:
          decoder.feed(recv_buf.data(), n);
          continue;
        case platform::Io::End:
          if (decoder.buffered() > 0) problem = "the connection closed in the middle of a message";
          return Got::Closed;
        case platform::Io::Closed:
          return Got::Closed;
        case platform::Io::WouldBlock:
          break;
      }
      if (deadline.expired()) return Got::Timeout;
      child.wait(false, deadline.poll_ms());   // timeout or EINTR: just look again
    }
  }

  // -- endings: each kills the worker and returns the exception to throw --

  WorkerDied died(Death how, const std::string& detail) {
    over = true;
    if (how == Death::Eof) wait_exit(0.1);   // a moment to finish exiting, so we can say how it ended
    const std::string ended = exit_note();
    kill_group();
    wait_streams_closed(1.0);   // its last stderr lines (often a traceback) may still be in the pipe
    FileTime since;
    {
      std::lock_guard<std::mutex> lock(proc_mu);
      since = started;
    }
    const bool panic = take_panic_marker(since);
    std::string message;
    if (panic) {
      message = "Panic stop: the panic key stopped Python.";
    } else if (how == Death::Mismatch) {
      message = detail;
    } else if (how == Death::Broken) {
      message = "Python stopped: it sent a message Vyrn couldn't read (" + detail + ").";
    } else {
      std::string why = ended;
      if (!detail.empty()) why += (why.empty() ? "" : "; ") + detail;
      message = "Python stopped" + (why.empty() ? std::string() : " (" + why + ")") + ".";
    }
    return WorkerDied(message, last_lines(kDiedTailLines), panic);
  }

  WorkerTimeout timed_out(const std::string& type, int64_t id, double timeout_s, double grace_s) {
    over = true;
    // Interrupt the worker only (POSIX: SIGINT to its pid, not its group; Windows: 'I' on the control
    // connection): Python raises KeyboardInterrupt inside the stuck user code and answers with a "timeout" error
    // that names the stuck line.
    child.interrupt();
    json report;
    Frame frame;
    std::string problem;
    if (read_frame(Deadline(grace_s), frame, problem) == Got::Frame && str_field(frame.j, "t") == "error") {
      int64_t reply_id = -1;
      if (int_field(frame.j, "id", reply_id) && (reply_id == id || reply_id == 0)) report = std::move(frame.j);
    }
    kill_group();   // either way
    const std::string message = report.is_null()
                                    ? "Python didn't answer \"" + type + "\" within " + seconds_text(timeout_s) + "."
                                    : describe_worker_error(report);
    return WorkerTimeout(message, std::move(report));
  }

  WorkerDied not_running() const {
    return WorkerDied("Python is not running (it stopped earlier).", last_lines(kDiedTailLines), false);
  }
};

// ---- WorkerProcess ------------------------------------------------------------------------------

WorkerProcess::WorkerProcess(WorkerOptions options) : s_(std::make_unique<State>(std::move(options))) {}

WorkerProcess::~WorkerProcess() {
  State& s = *s_;
  s.kill_group();
  if (s.io.joinable()) {
    // Let the io thread pass on the last lines, then stop it (a process that left the group could
    // keep the pipes open forever).
    s.wait_streams_closed(0.5);
    s.child.stop_output();
    s.io.join();
  }
}

void WorkerProcess::spawn() {
  State& s = *s_;
  if (s.spawned || s.child.pid() > 0) {
    throw std::logic_error("WorkerProcess::spawn(): already started (make a new WorkerProcess)");
  }
  const WorkerOptions& options = s.options;
  const std::string python = options.python.empty() ? platform::default_python() : options.python;
  const std::string script = options.script.empty() ? options.data_dir + "/python/vyrn_worker.py" : options.script;
  if (const int err = platform::check_readable(script); err != 0) {
    throw std::runtime_error("Vyrn can't find its Python worker " + script + " (" + errno_text(err) +
                             "). Is Vyrn installed completely?");
  }

  platform::ChildSpec spec;
  spec.program = python;
  spec.args = {"-u", "-X", "utf8", script, options.project_dir};
  spec.env = worker_settings(options);
  FileTime started;
  platform::file_clock_now(started.s, started.ns);
  try {
    s.child.start(spec);
  } catch (const platform::StartError& e) {
    if (!e.step.empty()) throw std::runtime_error("Vyrn couldn't start Python: " + std::string(e.what()));
    throw std::runtime_error("Vyrn couldn't start Python (" + python + "): " + e.reason +
                             ". Check the Python setting of this project.");
  }
  s.spawned = true;   // from here on the destructor kills it, whatever happens below
  {
    std::lock_guard<std::mutex> lock(s.proc_mu);
    s.started = started;
  }
  std::string problem;
  const std::string pid_file = write_pid_file(s.child.pid(), problem);
  {
    std::lock_guard<std::mutex> lock(s.proc_mu);
    s.pid_file = pid_file;
  }
  s.recv_buf.resize(kRecvChunk);
  const std::string note = problem.empty() ? std::string() : "[vyrn] the panic key can't find this run: " + problem;
  s.io = std::thread([&s, note] { s.io_main(note); });
}

json WorkerProcess::hello(double timeout_s) {
  State& s = *s_;
  if (!s.spawned) throw std::logic_error("WorkerProcess::hello(): call spawn() first");
  if (s.greeted) throw std::logic_error("WorkerProcess::hello(): the hello was already received");
  if (s.over) throw s.not_running();
  Frame frame;
  std::string problem;
  switch (s.read_frame(Deadline(timeout_s), frame, problem)) {
    case Got::Timeout:
      s.over = true;
      s.kill_group();
      throw WorkerTimeout("Python didn't start within " + seconds_text(timeout_s) + ".", nullptr);
    case Got::Closed:
      throw s.died(Death::Eof, problem);
    case Got::Broken:
      throw s.died(Death::Broken, problem);
    case Got::Frame:
      break;
  }
  int64_t id = -1;
  if (str_field(frame.j, "t") != "hello" || !int_field(frame.j, "id", id) || id != 0) {
    throw s.died(Death::Broken, "its first message must be the hello, not " + short_json(frame.j));
  }
  int64_t proto = 0;
  if (!int_field(frame.j, "proto", proto) || proto != kProtocolVersion) {
    const std::string theirs = frame.j.contains("proto") ? short_json(frame.j["proto"]) : "unknown";
    throw s.died(Death::Mismatch, "library/app version mismatch: Vyrn's Python library speaks protocol " + theirs +
                                      " and the app speaks " + std::to_string(kProtocolVersion) +
                                      ". Reinstall Vyrn so both come from the same version.");
  }
  s.greeted = true;
  return std::move(frame.j);
}

Frame WorkerProcess::request(json req, double timeout_s, double grace_s) {
  State& s = *s_;
  if (!s.spawned) throw std::logic_error("WorkerProcess::request(): call spawn() and hello() first");
  if (s.over) throw s.not_running();
  if (!s.greeted) throw std::logic_error("WorkerProcess::request(): call hello() first");
  const std::string type = str_field(req, "t");
  if (type.empty()) throw std::invalid_argument("WorkerProcess::request(): the request needs a \"t\" (its type)");
  const int64_t id = s.last_id + 1;
  req["id"] = id;
  const std::vector<uint8_t> bytes = encode(req);   // ProtocolError if it can't be sent
  s.last_id = id;

  const Deadline deadline(timeout_s);
  switch (s.send_all(bytes, deadline)) {
    case Sent::Ok:
      break;
    case Sent::Timeout:
      throw s.timed_out(type, id, timeout_s, grace_s);
    case Sent::Closed:
      throw s.died(Death::Eof, "");
  }
  Frame frame;
  std::string problem;
  switch (s.read_frame(deadline, frame, problem)) {
    case Got::Timeout:
      throw s.timed_out(type, id, timeout_s, grace_s);
    case Got::Closed:
      throw s.died(Death::Eof, problem);
    case Got::Broken:
      throw s.died(Death::Broken, problem);
    case Got::Frame:
      break;
  }
  const std::string reply_type = str_field(frame.j, "t");
  int64_t reply_id = -1;
  if (!is_reply_type(reply_type) || !int_field(frame.j, "id", reply_id)) {
    throw s.died(Death::Broken, "unexpected message " + short_json(frame.j));
  }
  // An error with id 0 answers a request the worker couldn't read (it then exits).
  if (reply_type == "error" && (reply_id == id || reply_id == 0)) throw WorkerError(std::move(frame.j));
  if (reply_id != id) {
    throw s.died(Death::Broken, "the reply to request " + std::to_string(id) + " has id " + std::to_string(reply_id));
  }
  return frame;
}

void WorkerProcess::freeze() { s_->child.freeze(); }

void WorkerProcess::kill_group() { s_->kill_group(); }

bool WorkerProcess::alive() const { return s_->child.alive(); }

int64_t WorkerProcess::pid() const { return s_->child.pid(); }

std::vector<std::string> WorkerProcess::stderr_tail(size_t n) const { return s_->last_lines(n); }

// ---- stale pid files ----------------------------------------------------------------------------

void sweep_stale_pid_files() {
  namespace fs = std::filesystem;
  std::error_code ec;
  std::vector<fs::path> stale;
  for (fs::directory_iterator it(runtime_dir(), ec), end; !ec && it != end; it.increment(ec)) {
    const fs::path& path = it->path();
    const std::string name = path.filename().string();
    const auto ends_with = [&](std::string_view suffix) {
      return name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if ((ends_with(".pid") || ends_with(".pid.tmp")) && !pid_file_matches(path.string())) stale.push_back(path);
  }
  for (const fs::path& path : stale) fs::remove(path, ec);
}

}  // namespace vyrn::ipc
