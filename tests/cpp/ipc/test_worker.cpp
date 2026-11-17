// WorkerProcess tests against a small fake worker written in plain Python (no numpy, no vyrn library),
// so they don't depend on the real worker: spawning, fds, environment, timeouts, group kills, pid
// files and the panic marker (protocol.md sections 1, 2 and 7).
#include "platform_test.h"

#if VYRN_TEST_LINUX

#include <gtest/gtest.h>

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ipc/worker.h"

namespace {

namespace fs = std::filesystem;
using vyrn::ipc::Frame;
using vyrn::ipc::json;
using vyrn::ipc::Stream;
using vyrn::ipc::WorkerDied;
using vyrn::ipc::WorkerError;
using vyrn::ipc::WorkerOptions;
using vyrn::ipc::WorkerProcess;
using vyrn::ipc::WorkerTimeout;
using Clock = std::chrono::steady_clock;

constexpr const char* kPython = "/usr/bin/python3";

// The fake worker. FAKE_MODE (environment) changes how it starts; requests are handled by type.
const char* const kFakeWorker = R"PY(
import json, os, signal, socket, struct, subprocess, sys, time

MODE = os.environ.get("FAKE_MODE", "")
sock = socket.socket(fileno=3)
frozen = False

def on_usr1(signum, frame):
    global frozen
    frozen = True

signal.signal(signal.SIGUSR1, on_usr1)

def send(msg, blob=b""):
    body = json.dumps(msg, separators=(",", ":")).encode()
    sock.sendall(b"VYRN" + struct.pack("<II", len(body), len(blob)) + body + blob)

def recv_exact(n):
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            return None
        data += chunk
    return data

def recv():
    head = recv_exact(12)
    if head is None:
        return None
    magic, json_len, bin_len = struct.unpack("<4sII", head)
    return json.loads(recv_exact(json_len + bin_len)[:json_len])

def open_fds():
    fds = {}
    for name in os.listdir("/proc/self/fd"):
        try:
            fds[name] = os.readlink("/proc/self/fd/" + name)
        except OSError:
            pass      # the fd that listed the directory, already closed again
    return fds

def status_field(key):
    with open("/proc/self/status") as f:
        for line in f:
            if line.startswith(key + ":"):
                return line.split()[1]
    return ""

GRANDCHILD = """
import signal, sys, time
mark = sys.argv[1]
def on_int(signum, frame):
    with open(mark, "w") as f:
        f.write("the grandchild got SIGINT")
signal.signal(signal.SIGINT, on_int)
open(mark + ".ready", "w").close()
time.sleep(60)
"""

if MODE == "exit_at_start":
    sys.stderr.write("fake worker: something went wrong at start\n")
    sys.exit(3)
if MODE == "no_hello":
    time.sleep(60)

hello = {"t": "hello", "id": 0, "proto": 2 if MODE == "proto2" else 1, "pid": os.getpid()}
if MODE == "report":
    with open("/proc/self/cmdline", "rb") as f:
        cmdline = f.read().decode().split("\0")[:-1]
    names = ("VYRN_FD", "VYRN_DATA", "OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "PYTHONDONTWRITEBYTECODE",
             "VYRN_TEST_EXTRA", "VYRN_TEST_PASSTHROUGH")
    hello.update(fds=open_fds(), pgid=os.getpgid(0), sigblk=status_field("SigBlk"), cmdline=cmdline,
                 env={name: os.environ.get(name) for name in names},
                 sigint_default=signal.getsignal(signal.SIGINT) is signal.default_int_handler)
send(hello)

while True:
    msg = recv()
    if msg is None:
        sys.exit(0)
    t, rid = msg["t"], msg["id"]
    if t == "echo":
        send({"t": "ok", "id": rid, "got": msg})
    elif t == "fail":
        send({"t": "error", "id": rid, "fatal": False, "kind": "exception", "script": "points.py", "line": 7,
              "message": "name 'scor' is not defined"})
    elif t == "write":
        stream = sys.stdout if msg["stream"] == "stdout" else sys.stderr
        stream.write(msg["text"])
        stream.flush()
        send({"t": "ok", "id": rid})
    elif t == "print":
        for i in range(msg["lines"]):
            print("line", i)
        send({"t": "ok", "id": rid})
    elif t == "big":
        n = msg["bytes"]
        send({"t": "ok", "id": rid, "data": {"$a": [0, "u8", [n]]}}, bytes(i % 251 for i in range(n)))
    elif t == "child":
        mark = msg["mark"]
        child = subprocess.Popen([sys.executable, "-c", GRANDCHILD, mark])
        for _ in range(500):
            if os.path.exists(mark + ".ready"):
                break
            time.sleep(0.01)
        send({"t": "ok", "id": rid, "child": child.pid})
    elif t == "hang":
        try:
            while True:
                pass
        except KeyboardInterrupt:
            send({"t": "error", "id": rid, "fatal": False, "kind": "timeout", "script": "points.py", "line": 3,
                  "message": "points() took too long"})
    elif t == "hang_ignoring_sigint":
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        while True:
            time.sleep(0.05)
    elif t == "crash":
        for i in range(30):
            sys.stderr.write("err line %d\n" % i)
        sys.stderr.write("no newline at the end")
        sys.stderr.flush()
        os._exit(1)
    elif t == "garbage":
        os.write(3, b"NOPE, this is not a frame")
        time.sleep(60)
    elif t == "unknown_type":
        send({"t": "surprise", "id": rid})
    elif t == "wrong_id":
        send({"t": "ok", "id": rid + 100})
    elif t == "two_replies":
        send({"t": "ok", "id": rid})
        send({"t": "ok", "id": rid})
    elif t == "status":
        for _ in range(200 if msg.get("wait_for_freeze") else 0):
            if frozen:
                break
            time.sleep(0.01)
        send({"t": "ok", "id": rid, "frozen": frozen})
    elif t == "close":
        send({"t": "bye", "id": rid})
        sys.exit(0)
    else:
        send({"t": "error", "id": rid, "fatal": True, "kind": "protocol", "message": "unknown request " + t})
)PY";

std::string make_temp_dir() {
  std::string pattern = (fs::temp_directory_path() / "vyrn-ipc-test-XXXXXX").string();
  if (::mkdtemp(pattern.data()) == nullptr) throw std::runtime_error("mkdtemp failed");
  return pattern;
}

void write_text(const std::string& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary);
  file << text;
}

std::string read_text(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

bool wait_until(const std::function<bool()>& done, double seconds) {
  const auto end = Clock::now() + std::chrono::duration<double>(seconds);
  while (!done()) {
    if (Clock::now() > end) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}

double seconds_since(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

struct ProcInfo {
  char state = '?';
  long long pgrp = 0;
  unsigned long long start = 0;
};

std::optional<ProcInfo> proc_info(pid_t pid) {
  const std::string text = read_text("/proc/" + std::to_string(pid) + "/stat");
  const auto name_end = text.rfind(')');
  if (name_end == std::string::npos) return std::nullopt;
  std::istringstream fields(text.substr(name_end + 1));
  ProcInfo info;
  long long ppid = 0;
  fields >> info.state >> ppid >> info.pgrp;
  std::string skipped;
  for (int field = 6; field < 22; ++field) fields >> skipped;
  fields >> info.start;
  if (!fields) return std::nullopt;
  return info;
}

// Dead = no such process, or a zombie its (new) parent hasn't collected yet.
bool process_gone(pid_t pid) {
  const auto info = proc_info(pid);
  return !info || info->state == 'Z' || info->state == 'X';
}

// No live process is left in the group.
bool group_gone(pid_t pgid) {
  DIR* dir = ::opendir("/proc");
  if (dir == nullptr) return false;
  bool gone = true;
  while (dirent* entry = ::readdir(dir)) {
    const std::string name = entry->d_name;
    if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) continue;
    const auto info = proc_info(static_cast<pid_t>(std::stol(name)));
    if (info && info->pgrp == pgid && info->state != 'Z' && info->state != 'X') gone = false;
  }
  ::closedir(dir);
  return gone;
}

// Sets an environment variable for the life of the object.
class ScopedEnv {
 public:
  ScopedEnv(const char* name, const std::string& value) : name_(name) {
    if (const char* old = std::getenv(name)) old_ = old;
    ::setenv(name, value.c_str(), 1);
  }
  ~ScopedEnv() {
    if (old_) {
      ::setenv(name_, old_->c_str(), 1);
    } else {
      ::unsetenv(name_);
    }
  }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 private:
  const char* name_;
  std::optional<std::string> old_;
};

class WorkerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (::access(kPython, X_OK) != 0) GTEST_SKIP() << kPython << " is missing";
    tmp_ = make_temp_dir();
    runtime_ = tmp_ + "/runtime";
    fs::create_directories(runtime_);
    fs::create_directories(tmp_ + "/project");
    // Pid files and the panic marker go to a throwaway runtime dir, never the real one.
    xdg_.emplace("XDG_RUNTIME_DIR", runtime_);
    script_ = tmp_ + "/fake_worker.py";
    write_text(script_, kFakeWorker);
  }

  void TearDown() override {
    xdg_.reset();
    if (!tmp_.empty()) {
      std::error_code ec;
      fs::remove_all(tmp_, ec);
    }
  }

  WorkerOptions options(const std::string& mode = "") {
    WorkerOptions o;
    o.python = kPython;
    o.data_dir = tmp_;
    o.project_dir = tmp_ + "/project";
    o.script = script_;
    if (!mode.empty()) o.env.emplace_back("FAKE_MODE", mode);
    o.on_line = [this](Stream stream, const std::string& line) {
      std::lock_guard<std::mutex> lock(lines_mu_);
      lines_.emplace_back(stream, line);
    };
    return o;
  }

  std::vector<std::string> lines(Stream stream) {
    std::lock_guard<std::mutex> lock(lines_mu_);
    std::vector<std::string> out;
    for (const auto& [s, line] : lines_) {
      if (s == stream) out.push_back(line);
    }
    return out;
  }

  std::string pid_file(pid_t pid) const { return runtime_ + "/vyrn/" + std::to_string(pid) + ".pid"; }
  std::string panic_marker() const { return runtime_ + "/vyrn/panic"; }

  std::string tmp_, runtime_, script_;
  std::optional<ScopedEnv> xdg_;
  std::mutex lines_mu_;
  std::vector<std::pair<Stream, std::string>> lines_;
};

// ---- spawning ---------------------------------------------------------------------------------

TEST_F(WorkerTest, ChildSeesOnlyFds0To3AndTheWorkerEnvironment) {
  // Things the child must NOT inherit: fds without CLOEXEC (the lowest free one, maybe 3, and a high
  // one), a blocked signal, an ignored SIGINT.
  const int leaked_low = ::open("/dev/null", O_RDONLY);   // deliberately no O_CLOEXEC
  ASSERT_GE(leaked_low, 0);
  const int leaked_high = ::fcntl(leaked_low, F_DUPFD, 20);   // F_DUPFD leaves CLOEXEC off too
  ASSERT_GE(leaked_high, 20);
  sigset_t block, old_mask;
  sigemptyset(&block);
  sigaddset(&block, SIGUSR2);
  ASSERT_EQ(::pthread_sigmask(SIG_BLOCK, &block, &old_mask), 0);
  struct sigaction ignore {}, old_int {};
  ignore.sa_handler = SIG_IGN;
  ASSERT_EQ(::sigaction(SIGINT, &ignore, &old_int), 0);
  ScopedEnv parent_blas("OPENBLAS_NUM_THREADS", "8");          // the worker's value must win
  ScopedEnv passthrough("VYRN_TEST_PASSTHROUGH", "yes");      // the user's environment passes through

  WorkerOptions o = options("report");
  o.env.emplace_back("VYRN_TEST_EXTRA", "42");
  o.env.emplace_back("OMP_NUM_THREADS", "2");   // options.env overrides the worker's defaults
  WorkerProcess worker(o);
  worker.spawn();
  ::sigaction(SIGINT, &old_int, nullptr);
  ::pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
  ::close(leaked_low);
  ::close(leaked_high);

  const json hello = worker.hello();
  const json& fds = hello["fds"];
  std::vector<std::string> numbers;
  for (const auto& item : fds.items()) numbers.push_back(item.key());
  EXPECT_EQ(numbers, (std::vector<std::string>{"0", "1", "2", "3"})) << fds.dump();
  EXPECT_EQ(fds.value("0", ""), "/dev/null");
  EXPECT_EQ(fds.value("1", "").rfind("pipe:", 0), 0u) << fds.dump();
  EXPECT_EQ(fds.value("2", "").rfind("pipe:", 0), 0u) << fds.dump();
  EXPECT_EQ(fds.value("3", "").rfind("socket:", 0), 0u) << fds.dump();
  EXPECT_NE(fds.value("1", ""), fds.value("2", ""));

  EXPECT_EQ(hello["pid"], worker.pid());
  EXPECT_EQ(hello["pgid"], worker.pid()) << "the worker must lead its own process group";
  EXPECT_EQ(hello["sigblk"], "0000000000000000") << "no blocked signals in the worker";
  EXPECT_EQ(hello["sigint_default"], true) << "SIGINT must reach Python as KeyboardInterrupt";
  EXPECT_EQ(hello["cmdline"],
            json({kPython, "-u", "-X", "utf8", script_, tmp_ + "/project"}));
  const json& env = hello["env"];
  EXPECT_EQ(env["VYRN_FD"], "3");
  EXPECT_EQ(env["VYRN_DATA"], tmp_);
  EXPECT_EQ(env["OMP_NUM_THREADS"], "2");
  EXPECT_EQ(env["OPENBLAS_NUM_THREADS"], "1");
  EXPECT_EQ(env["PYTHONDONTWRITEBYTECODE"], "1");
  EXPECT_EQ(env["VYRN_TEST_EXTRA"], "42");
  EXPECT_EQ(env["VYRN_TEST_PASSTHROUGH"], "yes");
}

TEST_F(WorkerTest, HelloThenRequestIdsCountFromOne) {
  WorkerProcess worker(options());
  EXPECT_EQ(worker.pid(), -1);
  EXPECT_FALSE(worker.alive());
  worker.spawn();
  EXPECT_TRUE(worker.alive());
  const json hello = worker.hello();
  EXPECT_EQ(hello["t"], "hello");
  EXPECT_EQ(hello["id"], 0);
  for (int expected = 1; expected <= 3; ++expected) {
    const Frame reply = worker.request({{"t", "echo"}, {"text", "hi"}}, 5, 1);
    EXPECT_EQ(reply.j["t"], "ok");
    EXPECT_EQ(reply.j["id"], expected);
    EXPECT_EQ(reply.j["got"]["id"], expected);
    EXPECT_EQ(reply.j["got"]["text"], "hi");
  }
  EXPECT_THROW(worker.hello(), std::logic_error);
  EXPECT_THROW(worker.spawn(), std::logic_error);
}

TEST_F(WorkerTest, ErrorReplyThrowsWorkerErrorAndTheWorkerKeepsRunning) {
  WorkerProcess worker(options());
  worker.spawn();
  worker.hello();
  try {
    worker.request({{"t", "fail"}}, 5, 1);
    FAIL() << "expected WorkerError";
  } catch (const WorkerError& e) {
    EXPECT_EQ(e.error["kind"], "exception");
    EXPECT_EQ(e.error["id"], 1);
    EXPECT_STREQ(e.what(), "points.py:7: name 'scor' is not defined");
  }
  EXPECT_TRUE(worker.alive());
  EXPECT_EQ(worker.request({{"t", "echo"}}, 5, 1).j["id"], 2);
}

TEST_F(WorkerTest, RequestBeforeHelloOrSpawnIsAMistake) {
  WorkerProcess worker(options());
  EXPECT_THROW(worker.request({{"t", "echo"}}, 5, 1), std::logic_error);
  EXPECT_THROW(worker.hello(), std::logic_error);
  worker.spawn();
  EXPECT_THROW(worker.request({{"t", "echo"}}, 5, 1), std::logic_error);
  worker.hello();
  EXPECT_THROW(worker.request({{"no_type", 1}}, 5, 1), std::invalid_argument);
}

TEST_F(WorkerTest, MissingPythonOrScriptFailsToSpawnWithAPlainMessage) {
  WorkerOptions bad_python = options();
  bad_python.python = tmp_ + "/no-such-python3";
  WorkerProcess first(bad_python);
  try {
    first.spawn();
    FAIL() << "expected std::runtime_error";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("no-such-python3"), std::string::npos) << e.what();
  }
  EXPECT_FALSE(first.alive());

  WorkerOptions bad_script = options();
  bad_script.script.clear();   // default: <data_dir>/python/vyrn_worker.py, which the temp dir lacks
  WorkerProcess second(bad_script);
  try {
    second.spawn();
    FAIL() << "expected std::runtime_error";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("python/vyrn_worker.py"), std::string::npos) << e.what();
  }
}

// ---- output lines -----------------------------------------------------------------------------

TEST_F(WorkerTest, OutputArrivesAsLinesAndLongLinesAreSplit) {
  WorkerProcess worker(options());
  worker.spawn();
  worker.hello();
  const std::string euros = [] {
    std::string text;
    for (int i = 0; i < 4000; ++i) text += "\xe2\x82\xac";   // U+20AC, 3 bytes each
    return text;
  }();
  worker.request({{"t", "write"}, {"stream", "stdout"}, {"text", "first\nsecond\n"}}, 5, 1);
  worker.request({{"t", "write"}, {"stream", "stdout"}, {"text", std::string(20000, 'x') + "\n"}}, 5, 1);
  worker.request({{"t", "write"}, {"stream", "stdout"}, {"text", std::string(8192, 'y') + "\n"}}, 5, 1);
  worker.request({{"t", "write"}, {"stream", "stdout"}, {"text", euros + "\n"}}, 5, 1);
  worker.request({{"t", "write"}, {"stream", "stderr"}, {"text", "warning: careful\n"}}, 5, 1);

  ASSERT_TRUE(wait_until([&] { return lines(Stream::Stdout).size() >= 8; }, 5));
  const std::vector<std::string> out = lines(Stream::Stdout);
  ASSERT_EQ(out.size(), 8u);
  EXPECT_EQ(out[0], "first");
  EXPECT_EQ(out[1], "second");
  EXPECT_EQ(out[2], std::string(8192, 'x'));
  EXPECT_EQ(out[3], std::string(8192, 'x'));
  EXPECT_EQ(out[4], std::string(20000 - 2 * 8192, 'x'));
  EXPECT_EQ(out[5], std::string(8192, 'y')) << "exactly 8 KiB is not split";
  EXPECT_EQ(out[6].size(), 8190u) << "the cut must not split a 3-byte character";
  EXPECT_EQ(out[6] + out[7], euros);
  ASSERT_TRUE(wait_until([&] { return !lines(Stream::Stderr).empty(); }, 5));
  EXPECT_EQ(lines(Stream::Stderr), (std::vector<std::string>{"warning: careful"}));
  EXPECT_EQ(worker.stderr_tail(5), (std::vector<std::string>{"warning: careful"}));
}

TEST_F(WorkerTest, PrintFloodDoesNotBlockRequests) {
  WorkerProcess worker(options());
  worker.spawn();
  worker.hello();
  constexpr int kLines = 50000;   // far more than a pipe holds
  EXPECT_EQ(worker.request({{"t", "print"}, {"lines", kLines}}, 30, 1).j["t"], "ok");
  ASSERT_TRUE(wait_until([&] { return lines(Stream::Stdout).size() >= kLines; }, 10));
  const std::vector<std::string> out = lines(Stream::Stdout);
  ASSERT_EQ(out.size(), static_cast<size_t>(kLines));
  for (int i = 0; i < kLines; i += 997) EXPECT_EQ(out[static_cast<size_t>(i)], "line " + std::to_string(i));
  EXPECT_EQ(out.back(), "line " + std::to_string(kLines - 1));
}

TEST_F(WorkerTest, BigBlobArrivesWhole) {
  WorkerProcess worker(options());
  worker.spawn();
  worker.hello();
  constexpr int kBytes = 1920 * 1080 * 3;   // a full-HD screen picture
  const Frame reply = worker.request({{"t", "big"}, {"bytes", kBytes}}, 30, 1);
  ASSERT_TRUE(reply.has_array("data"));
  const auto data = reply.array(reply.j["data"]);
  ASSERT_EQ(data.bytes, static_cast<size_t>(kBytes));
  for (size_t i = 0; i < data.bytes; i += 4099) ASSERT_EQ(static_cast<size_t>(data.data[i]), i % 251) << "byte " << i;
  EXPECT_EQ(data.data[data.bytes - 1], (kBytes - 1) % 251);
}

// ---- killing ----------------------------------------------------------------------------------

TEST_F(WorkerTest, GroupKillReachesAGrandchild) {
  WorkerProcess worker(options());
  worker.spawn();
  worker.hello();
  const pid_t pid = worker.pid();
  const Frame reply = worker.request({{"t", "child"}, {"mark", tmp_ + "/mark"}}, 10, 1);
  const pid_t grandchild = reply.j["child"].get<pid_t>();
  ASSERT_FALSE(process_gone(grandchild));
  ASSERT_EQ(proc_info(grandchild)->pgrp, pid) << "the grandchild should be in the worker's group";

  worker.kill_group();
  EXPECT_FALSE(worker.alive());
  EXPECT_TRUE(wait_until([&] { return process_gone(grandchild); }, 5)) << "the grandchild survived the group kill";
  EXPECT_TRUE(wait_until([&] { return group_gone(pid); }, 5));
  EXPECT_FALSE(fs::exists(pid_file(pid)));
  worker.kill_group();   // a second call is harmless
}

TEST_F(WorkerTest, TimeoutSendsSigintToThePidThenKillsTheGroup) {
  WorkerProcess worker(options());
  worker.spawn();
  worker.hello();
  const pid_t pid = worker.pid();
  const std::string mark = tmp_ + "/mark";
  const pid_t grandchild = worker.request({{"t", "child"}, {"mark", mark}}, 10, 1).j["child"].get<pid_t>();

  const auto start = Clock::now();
  try {
    // 1 s: long enough for a busy machine to have the fake worker inside its loop before the SIGINT.
    worker.request({{"t", "hang"}}, 1.0, 5);
    FAIL() << "expected WorkerTimeout";
  } catch (const WorkerTimeout& e) {
    // The worker's KeyboardInterrupt report ends the wait early (well before the 5 s grace).
    EXPECT_LT(seconds_since(start), 5.0);
    EXPECT_GE(seconds_since(start), 0.99);
    EXPECT_EQ(e.error["kind"], "timeout") << e.error.dump();
    EXPECT_NE(std::string(e.what()).find("points() took too long"), std::string::npos) << e.what();
  }
  EXPECT_FALSE(worker.alive());
  EXPECT_TRUE(wait_until([&] { return group_gone(pid); }, 5)) << "the group outlived the timeout";
  EXPECT_TRUE(process_gone(grandchild));
  EXPECT_FALSE(fs::exists(mark)) << "SIGINT must go to the worker pid only, not its group";
  EXPECT_FALSE(fs::exists(pid_file(pid)));
  EXPECT_THROW(worker.request({{"t", "echo"}}, 5, 1), WorkerDied);   // it is over
}

TEST_F(WorkerTest, TimeoutEscalatesToSigkillWhenSigintIsIgnored) {
  WorkerProcess worker(options());
  worker.spawn();
  worker.hello();
  const pid_t pid = worker.pid();
  const std::string mark = tmp_ + "/mark";
  const pid_t grandchild = worker.request({{"t", "child"}, {"mark", mark}}, 10, 1).j["child"].get<pid_t>();

  const auto start = Clock::now();
  try {
    worker.request({{"t", "hang_ignoring_sigint"}}, 1.0, 0.5);
    FAIL() << "expected WorkerTimeout";
  } catch (const WorkerTimeout& e) {
    EXPECT_GE(seconds_since(start), 1.49) << "it must wait the grace period for a report";
    EXPECT_LT(seconds_since(start), 6.0);
    EXPECT_TRUE(e.error.is_null()) << e.error.dump();
    EXPECT_NE(std::string(e.what()).find("didn't answer \"hang_ignoring_sigint\" within 1 second"),
              std::string::npos)
        << e.what();
  }
  EXPECT_FALSE(worker.alive());
  EXPECT_TRUE(wait_until([&] { return group_gone(pid); }, 5));
  EXPECT_TRUE(process_gone(grandchild));
  EXPECT_FALSE(fs::exists(mark)) << "SIGINT must go to the worker pid only, not its group";
}

TEST_F(WorkerTest, KillGroupFromAnotherThreadEndsAWaitingRequest) {
  WorkerProcess worker(options());
  worker.spawn();
  worker.hello();
  std::thread stopper([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    worker.kill_group();
  });
  const auto start = Clock::now();
  EXPECT_THROW(worker.request({{"t", "hang_ignoring_sigint"}}, 60, 1), WorkerDied);
  EXPECT_LT(seconds_since(start), 5.0);
  stopper.join();
  EXPECT_FALSE(worker.alive());
}

// ---- deaths -----------------------------------------------------------------------------------

TEST_F(WorkerTest, EofThrowsWorkerDiedWithTheStderrTail) {
  WorkerProcess worker(options());
  worker.spawn();
  worker.hello();
  const pid_t pid = worker.pid();
  try {
    worker.request({{"t", "crash"}}, 5, 1);
    FAIL() << "expected WorkerDied";
  } catch (const WorkerDied& e) {
    EXPECT_FALSE(e.panic);
    EXPECT_NE(std::string(e.what()).find("Python stopped"), std::string::npos) << e.what();
    EXPECT_NE(std::string(e.what()).find("exit status 1"), std::string::npos) << e.what();
    ASSERT_EQ(e.stderr_tail.size(), 20u);
    EXPECT_EQ(e.stderr_tail.front(), "err line 11");
    EXPECT_EQ(e.stderr_tail[18], "err line 29");
    EXPECT_EQ(e.stderr_tail.back(), "no newline at the end");
  }
  EXPECT_FALSE(worker.alive());
  EXPECT_FALSE(fs::exists(pid_file(pid)));
  EXPECT_THROW(worker.request({{"t", "echo"}}, 5, 1), WorkerDied);
}

TEST_F(WorkerTest, BrokenFrameThrowsWorkerDied) {
  WorkerProcess worker(options());
  worker.spawn();
  worker.hello();
  const pid_t pid = worker.pid();
  try {
    worker.request({{"t", "garbage"}}, 5, 1);
    FAIL() << "expected WorkerDied";
  } catch (const WorkerDied& e) {
    EXPECT_NE(std::string(e.what()).find("couldn't read"), std::string::npos) << e.what();
    EXPECT_NE(std::string(e.what()).find("bad magic"), std::string::npos) << e.what();
  }
  EXPECT_FALSE(worker.alive());
  EXPECT_TRUE(wait_until([&] { return group_gone(pid); }, 5));
}

TEST_F(WorkerTest, UnknownReplyTypeWrongIdAndExtraRepliesAreProtocolErrors) {
  for (const char* request : {"unknown_type", "wrong_id"}) {
    WorkerProcess worker(options());
    worker.spawn();
    worker.hello();
    EXPECT_THROW(worker.request({{"t", request}}, 5, 1), WorkerDied) << request;
    EXPECT_FALSE(worker.alive()) << request;
  }
  WorkerProcess worker(options());
  worker.spawn();
  worker.hello();
  EXPECT_EQ(worker.request({{"t", "two_replies"}}, 5, 1).j["id"], 1);
  EXPECT_THROW(worker.request({{"t", "echo"}}, 5, 1), WorkerDied);   // the extra reply has id 1, not 2
}

TEST_F(WorkerTest, HelloTimeoutKillsTheWorker) {
  WorkerProcess worker(options("no_hello"));
  worker.spawn();
  const pid_t pid = worker.pid();
  const auto start = Clock::now();
  EXPECT_THROW(worker.hello(0.5), WorkerTimeout);
  EXPECT_GE(seconds_since(start), 0.49);
  EXPECT_LT(seconds_since(start), 5.0);
  EXPECT_FALSE(worker.alive());
  EXPECT_FALSE(fs::exists(pid_file(pid)));
  EXPECT_THROW(worker.request({{"t", "echo"}}, 5, 1), WorkerDied);
}

TEST_F(WorkerTest, HelloEofAndVersionMismatchThrowWorkerDied) {
  {
    WorkerProcess worker(options("exit_at_start"));
    worker.spawn();
    try {
      worker.hello();
      FAIL() << "expected WorkerDied";
    } catch (const WorkerDied& e) {
      ASSERT_FALSE(e.stderr_tail.empty());
      EXPECT_EQ(e.stderr_tail.back(), "fake worker: something went wrong at start");
      EXPECT_NE(std::string(e.what()).find("Python stopped"), std::string::npos) << e.what();
    }
  }
  {
    WorkerProcess worker(options("proto2"));
    worker.spawn();
    try {
      worker.hello();
      FAIL() << "expected WorkerDied";
    } catch (const WorkerDied& e) {
      EXPECT_NE(std::string(e.what()).find("library/app version mismatch"), std::string::npos) << e.what();
    }
    EXPECT_FALSE(worker.alive());
  }
}

TEST_F(WorkerTest, CloseGetsByeAndTheDestructorReaps) {
  pid_t pid = -1;
  {
    WorkerProcess worker(options());
    worker.spawn();
    worker.hello();
    pid = worker.pid();
    EXPECT_TRUE(fs::exists(pid_file(pid)));
    EXPECT_EQ(worker.request({{"t", "close"}}, 2, 0).j["t"], "bye");
    EXPECT_TRUE(wait_until([&] { return !worker.alive(); }, 5)) << "the worker should exit after bye";
  }
  EXPECT_FALSE(fs::exists(pid_file(pid)));
  EXPECT_TRUE(process_gone(pid));
}

TEST_F(WorkerTest, FreezeSendsSigusr1) {
  WorkerProcess worker(options());
  worker.spawn();
  worker.hello();
  EXPECT_EQ(worker.request({{"t", "status"}}, 5, 1).j["frozen"], false);
  worker.freeze();
  // The signal is asynchronous: the fake worker waits up to 2 s for its handler to run.
  EXPECT_EQ(worker.request({{"t", "status"}, {"wait_for_freeze", true}}, 5, 1).j["frozen"], true);
}

// ---- pid files and the panic marker -----------------------------------------------------------

TEST_F(WorkerTest, PidFileIsWrittenAfterSpawnAndRemovedAfterWaitpid) {
  WorkerProcess worker(options());
  worker.spawn();
  const pid_t pid = worker.pid();
  const std::string path = pid_file(pid);
  ASSERT_TRUE(fs::exists(path));
  const auto info = proc_info(pid);
  ASSERT_TRUE(info.has_value());
  EXPECT_EQ(read_text(path), std::to_string(pid) + " " + std::to_string(info->start) + " " + std::to_string(pid) + "\n");
  EXPECT_EQ(fs::status(runtime_ + "/vyrn").permissions() & fs::perms::all, fs::perms::owner_all);
  worker.hello();
  worker.kill_group();
  EXPECT_FALSE(fs::exists(path));
}

TEST_F(WorkerTest, SweepRemovesOnlyStalePidFiles) {
  const std::string dir = runtime_ + "/vyrn";
  fs::create_directories(dir);
  const pid_t me = ::getpid();
  const auto mine = proc_info(me);
  ASSERT_TRUE(mine.has_value());
  const std::string live = dir + "/" + std::to_string(me) + ".pid";
  write_text(live, std::to_string(me) + " " + std::to_string(mine->start) + " " + std::to_string(mine->pgrp) + "\n");
  const std::string reused = dir + "/1000000001.pid";   // our pid, but another start time
  write_text(reused, std::to_string(me) + " " + std::to_string(mine->start + 1) + " " + std::to_string(me) + "\n");
  const std::string dead = dir + "/2147483647.pid";      // above pid_max: no such process
  write_text(dead, "2147483647 12345 2147483647\n");
  const std::string junk = dir + "/junk.pid";
  write_text(junk, "this is not a pid file\n");
  const std::string marker = dir + "/panic";
  write_text(marker, "");

  vyrn::ipc::sweep_stale_pid_files();
  EXPECT_TRUE(fs::exists(live));
  EXPECT_FALSE(fs::exists(reused));
  EXPECT_FALSE(fs::exists(dead));
  EXPECT_FALSE(fs::exists(junk));
  EXPECT_TRUE(fs::exists(marker)) << "only pid files are swept";

  // A live worker's own pid file survives the sweep.
  WorkerProcess worker(options());
  worker.spawn();
  vyrn::ipc::sweep_stale_pid_files();
  EXPECT_TRUE(fs::exists(pid_file(worker.pid())));
}

TEST_F(WorkerTest, PanicMarkerMeansPanicStop) {
  WorkerProcess worker(options());
  worker.spawn();
  worker.hello();
  // What scripts/vyrn-panic does: write the marker, then SIGKILL the worker's group.
  write_text(panic_marker(), "");
  ASSERT_EQ(::kill(-worker.pid(), SIGKILL), 0);
  try {
    worker.request({{"t", "echo"}}, 5, 1);
    FAIL() << "expected WorkerDied";
  } catch (const WorkerDied& e) {
    EXPECT_TRUE(e.panic);
    EXPECT_NE(std::string(e.what()).find("Panic stop"), std::string::npos) << e.what();
  }
  EXPECT_FALSE(fs::exists(panic_marker())) << "the marker is deleted once seen";
}

TEST_F(WorkerTest, OldPanicMarkerIsNotThisWorkersPanic) {
  fs::create_directories(runtime_ + "/vyrn");
  write_text(panic_marker(), "");
  const timespec an_hour_ago[2] = {{::time(nullptr) - 3600, 0}, {::time(nullptr) - 3600, 0}};
  ASSERT_EQ(::utimensat(AT_FDCWD, panic_marker().c_str(), an_hour_ago, 0), 0);

  WorkerProcess worker(options());
  worker.spawn();
  worker.hello();
  try {
    worker.request({{"t", "crash"}}, 5, 1);
    FAIL() << "expected WorkerDied";
  } catch (const WorkerDied& e) {
    EXPECT_FALSE(e.panic) << e.what();
  }
}

}  // namespace

#else  // VYRN_TEST_LINUX

TEST(WorkerTest, SkippedOnThisSystem) {
  GTEST_SKIP() << "Linux only: these tests read /proc and drive a fake worker over the POSIX transport (fd 3, signals); the Windows transport is tested in test_platform (WorkerListener).";
}

#endif  // VYRN_TEST_LINUX
