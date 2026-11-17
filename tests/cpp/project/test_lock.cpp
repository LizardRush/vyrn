// ProjectLock: one trainer per project (<project>/.vyrn-lock), stale locks, and races between processes.
#include "platform_test.h"

#if VYRN_TEST_POSIX

#include "helpers.h"
#include "platform/system.h"
#include "vyrn/project.h"

#include <gtest/gtest.h>

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace project_test;

namespace {

fs::path lock_file(const std::string& project) { return fs::path(project) / ".vyrn-lock"; }

nlohmann::json lock_json(const std::string& project) { return nlohmann::json::parse(read_file(lock_file(project))); }

std::string first_line(const std::string& path) {
  const std::string text = read_file(path);
  const auto newline = text.find('\n');
  return newline == std::string::npos ? text : text.substr(0, newline);
}

#if VYRN_TEST_LINUX   // straight from /proc, independently of the code under test
std::string boot_id() { return first_line("/proc/sys/kernel/random/boot_id"); }

long long start_time(pid_t pid) {
  const std::string stat = first_line("/proc/" + std::to_string(pid) + "/stat");
  std::istringstream fields(stat.substr(stat.rfind(')') + 1));
  std::string field;
  for (int number = 3; number <= 22; ++number) fields >> field;
  return std::stoll(field);
}
#else
std::string boot_id() { return vyrn::platform::boot_id(); }

long long start_time(pid_t pid) {
  const auto id = vyrn::platform::process_identity(pid);
  return id ? id->start : -1;
}
#endif

// The pid of a process that has ended and been reaped.
pid_t dead_pid() {
  const pid_t pid = ::fork();
  if (pid == 0) ::_exit(0);
  ::waitpid(pid, nullptr, 0);
  return pid;
}

// Reads one byte, waiting at most `timeout_ms`. False on timeout or end of file.
bool read_byte(int fd, char& c, int timeout_ms = 30000) {
  pollfd p{fd, POLLIN, 0};
  if (::poll(&p, 1, timeout_ms) <= 0) return false;
  return ::read(fd, &c, 1) == 1;
}

void wait_for_eof(int fd) {
  char c;
  for (;;) {
    const ssize_t n = ::read(fd, &c, 1);
    if (n == 0 || (n < 0 && errno != EINTR)) return;
  }
}

// A child process: runs `body` (which answers true/false), then stays alive until release() and leaves with
// _exit (no destructors run, so a lock it took stays behind, owned by a pid that is gone).
class Child {
 public:
  template <typename Body>
  explicit Child(Body body) {
    int ready[2], hold[2];
    if (::pipe(ready) != 0 || ::pipe(hold) != 0) throw std::runtime_error("pipe failed");
    pid_ = ::fork();
    if (pid_ == 0) {
      ::close(ready[0]);
      ::close(hold[1]);
      const char answer = body() ? '1' : '0';
      if (::write(ready[1], &answer, 1) != 1) ::_exit(3);
      wait_for_eof(hold[0]);
      ::_exit(0);
    }
    ::close(ready[1]);
    ::close(hold[0]);
    ready_ = ready[0];
    hold_ = hold[1];
  }
  ~Child() {
    release();
    ::close(ready_);
  }
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;

  pid_t pid() const { return pid_; }
  bool answer() {   // waits until body() has run
    char c = 0;
    return read_byte(ready_, c) && c == '1';
  }
  void release() {   // lets the child leave, and reaps it
    if (hold_ >= 0) {
      ::close(hold_);
      hold_ = -1;
    }
    if (pid_ > 0) {
      ::waitpid(pid_, nullptr, 0);
      pid_ = -1;
    }
  }

 private:
  pid_t pid_ = -1;
  int ready_ = -1, hold_ = -1;
};

// In a child: takes the lock and keeps it until the child leaves (the object is never destroyed).
bool take_and_keep(const std::string& project, const std::string& by) {
  auto* kept = new std::optional<vyrn::ProjectLock>(vyrn::ProjectLock::acquire(project, by));
  return kept->has_value();
}

void write_lock(const std::string& project, const nlohmann::json& j) { write_file(lock_file(project), j.dump()); }

// `n` processes call acquire() at the same moment and keep what they got until all have answered.
// Returns how many got the lock (-1 if a child did not answer in time).
int race(const std::string& project, int n) {
  int go[2], results[2], hold[2];
  if (::pipe(go) != 0 || ::pipe(results) != 0 || ::pipe(hold) != 0) return -1;
  std::vector<pid_t> children;
  for (int i = 0; i < n; ++i) {
    const pid_t pid = ::fork();
    if (pid == 0) {
      ::close(go[1]);
      ::close(results[0]);
      ::close(hold[1]);
      wait_for_eof(go[0]);   // everyone starts when the parent closes `go`
      const std::optional<vyrn::ProjectLock> lock = vyrn::ProjectLock::acquire(project, "agent");
      const char answer = lock ? '1' : '0';
      if (::write(results[1], &answer, 1) != 1) ::_exit(3);
      wait_for_eof(hold[0]);   // the winner keeps its lock until everyone has tried
      ::_exit(0);
    }
    if (pid > 0) children.push_back(pid);
  }
  ::close(go[0]);
  ::close(results[1]);
  ::close(hold[0]);
  ::close(go[1]);   // go!
  int winners = 0, answers = 0;
  char c;
  while (answers < n && read_byte(results[0], c)) {
    ++answers;
    if (c == '1') ++winners;
  }
  ::close(hold[1]);
  for (const pid_t pid : children) ::waitpid(pid, nullptr, 0);
  ::close(results[0]);
  return answers == n ? winners : -1;
}

const std::regex kIsoTime(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}$)");

}  // namespace

TEST(ProjectLock, AcquireWritesTheHolderAndReleases) {
  TempDir project;
  {
    std::optional<vyrn::ProjectLock> lock = vyrn::ProjectLock::acquire(project.path(), "app");
    ASSERT_TRUE(lock.has_value());
    const nlohmann::json j = lock_json(project.path());
    EXPECT_EQ(j["pid"], ::getpid());
    EXPECT_EQ(j["by"], "app");
    EXPECT_TRUE(std::regex_match(j["since"].get<std::string>(), kIsoTime)) << j.dump();
    EXPECT_EQ(j["boot"], boot_id());
    EXPECT_EQ(j["start"].get<long long>(), start_time(::getpid()));
    std::vector<std::string> keys;
    const nlohmann::ordered_json ordered = nlohmann::ordered_json::parse(read_file(lock_file(project.path())));
    for (auto it = ordered.begin(); it != ordered.end(); ++it) keys.push_back(it.key());
    EXPECT_EQ(keys, (std::vector<std::string>{"pid", "by", "since", "start", "boot"}));

    const auto holder = vyrn::ProjectLock::holder(project.path());
    ASSERT_TRUE(holder.has_value());
    EXPECT_EQ(holder->pid, ::getpid());
    EXPECT_EQ(holder->by, "app");
    EXPECT_EQ(holder->since, j["since"]);

    // One trainer per project, even inside one process.
    EXPECT_FALSE(vyrn::ProjectLock::acquire(project.path(), "agent").has_value());
    EXPECT_TRUE(fs::exists(lock_file(project.path())));
  }
  EXPECT_FALSE(fs::exists(lock_file(project.path())));
  EXPECT_FALSE(vyrn::ProjectLock::holder(project.path()).has_value());
  EXPECT_TRUE(vyrn::ProjectLock::acquire(project.path(), "cli").has_value());
  EXPECT_FALSE(fs::exists(lock_file(project.path())));   // that temporary released it again
}

TEST(ProjectLock, MovingKeepsOneOwner) {
  TempDir a, b;
  std::optional<vyrn::ProjectLock> first = vyrn::ProjectLock::acquire(a.path(), "app");
  ASSERT_TRUE(first.has_value());
  vyrn::ProjectLock moved = std::move(*first);
  first.reset();   // the moved-from lock owns nothing
  EXPECT_TRUE(fs::exists(lock_file(a.path())));

  std::optional<vyrn::ProjectLock> other = vyrn::ProjectLock::acquire(b.path(), "app");
  ASSERT_TRUE(other.has_value());
  *other = std::move(moved);   // releases b's lock, takes over a's
  EXPECT_FALSE(fs::exists(lock_file(b.path())));
  EXPECT_TRUE(fs::exists(lock_file(a.path())));
  vyrn::ProjectLock& same = *other;
  *other = std::move(same);   // a self-move keeps it
  EXPECT_TRUE(fs::exists(lock_file(a.path())));
  other.reset();
  EXPECT_FALSE(fs::exists(lock_file(a.path())));
}

TEST(ProjectLock, LiveHolderElsewhereBlocksUntilItEnds) {
  TempDir project;
  write_file(fs::path(project.path()) / "inputs.py", "");
  Child child([&] { return take_and_keep(project.path(), "agent"); });
  ASSERT_TRUE(child.answer());
  const pid_t child_pid = child.pid();

  EXPECT_FALSE(vyrn::ProjectLock::acquire(project.path(), "app").has_value());
  const auto holder = vyrn::ProjectLock::holder(project.path());
  ASSERT_TRUE(holder.has_value());
  EXPECT_EQ(holder->pid, child_pid);
  EXPECT_EQ(holder->by, "agent");
  EXPECT_EQ(vyrn::read_project(project.path()).training_by, "agent");

  // The child ends without releasing: its lock is stale now and is taken over.
  child.release();
  EXPECT_TRUE(fs::exists(lock_file(project.path())));
  EXPECT_FALSE(vyrn::ProjectLock::holder(project.path()).has_value());
  EXPECT_EQ(vyrn::read_project(project.path()).training_by, "");
  {
    const auto lock = vyrn::ProjectLock::acquire(project.path(), "app");
    ASSERT_TRUE(lock.has_value());
    EXPECT_EQ(lock_json(project.path())["pid"], ::getpid());
    EXPECT_EQ(lock_json(project.path())["by"], "app");
  }
  EXPECT_FALSE(fs::exists(lock_file(project.path())));
}

TEST(ProjectLock, LockOfAnEndedProgramIsTakenOver) {
  TempDir project;
  write_lock(project.path(), {{"pid", dead_pid()}, {"by", "cli"}, {"since", "2026-11-13T17:05:00"}});
  EXPECT_FALSE(vyrn::ProjectLock::holder(project.path()).has_value());
  const auto lock = vyrn::ProjectLock::acquire(project.path(), "agent");
  ASSERT_TRUE(lock.has_value());
  EXPECT_EQ(lock_json(project.path())["pid"], ::getpid());
  EXPECT_EQ(lock_json(project.path())["by"], "agent");
}

TEST(ProjectLock, ThePlainDocumentedFormatIsUnderstood) {
  TempDir project;
  Child sleeper([] { return true; });
  ASSERT_TRUE(sleeper.answer());
  write_lock(project.path(), {{"pid", sleeper.pid()}, {"by", "cli"}, {"since", "2026-11-13T17:05:00"}});
  const auto holder = vyrn::ProjectLock::holder(project.path());
  ASSERT_TRUE(holder.has_value());
  EXPECT_EQ(holder->pid, sleeper.pid());
  EXPECT_EQ(holder->by, "cli");
  EXPECT_EQ(holder->since, "2026-11-13T17:05:00");
  EXPECT_FALSE(vyrn::ProjectLock::acquire(project.path(), "app").has_value());
  EXPECT_EQ(lock_json(project.path())["pid"], sleeper.pid());   // untouched

  // A live lock that doesn't say who holds it still has a holder.
  write_lock(project.path(), {{"pid", sleeper.pid()}});
  const auto unnamed = vyrn::ProjectLock::holder(project.path());
  ASSERT_TRUE(unnamed.has_value());
  EXPECT_EQ(unnamed->by, "cli");
  EXPECT_EQ(unnamed->since, "");
}

TEST(ProjectLock, APidUsedAgainByAnotherProgramIsStale) {
  TempDir project;
  Child sleeper([] { return true; });
  ASSERT_TRUE(sleeper.answer());
  const long long start = start_time(sleeper.pid());
  const nlohmann::json live = {{"pid", sleeper.pid()}, {"by", "agent"}, {"since", "2026-11-13T17:05:00"},
                               {"start", start}, {"boot", boot_id()}};
  write_lock(project.path(), live);
  EXPECT_TRUE(vyrn::ProjectLock::holder(project.path()).has_value());
  EXPECT_FALSE(vyrn::ProjectLock::acquire(project.path(), "app").has_value());

  nlohmann::json other_start = live;
  other_start["start"] = start + 12345;   // the pid now belongs to a program that started later
  write_lock(project.path(), other_start);
  EXPECT_FALSE(vyrn::ProjectLock::holder(project.path()).has_value());
  {
    const auto lock = vyrn::ProjectLock::acquire(project.path(), "app");
    EXPECT_TRUE(lock.has_value());
  }

  nlohmann::json other_boot = live;
  other_boot["boot"] = "00000000-0000-0000-0000-000000000000";   // written before the computer restarted
  write_lock(project.path(), other_boot);
  EXPECT_FALSE(vyrn::ProjectLock::holder(project.path()).has_value());
  EXPECT_TRUE(vyrn::ProjectLock::acquire(project.path(), "app").has_value());
}

TEST(ProjectLock, UnreadableLocksWaitBrieflyThenCountAsStale) {
  TempDir project;
  const fs::path file = lock_file(project.path());
  const std::vector<std::string> unreadable = {"", "{\"pid\":", "hello", "{\"pid\": 0}", "{\"pid\": -5}",
                                               "{\"pid\": \"12\"}", "[1]"};
  for (const std::string& text : unreadable) {
    write_file(file, text);   // fresh: maybe its creator is still writing it
    EXPECT_FALSE(vyrn::ProjectLock::acquire(project.path(), "app").has_value()) << text;
    EXPECT_FALSE(vyrn::ProjectLock::holder(project.path()).has_value()) << text;
    EXPECT_EQ(read_file(file), text);
    set_mtime(file, now_epoch() - 60);   // a leftover
    const auto lock = vyrn::ProjectLock::acquire(project.path(), "app");
    EXPECT_TRUE(lock.has_value()) << text;
  }
  EXPECT_FALSE(fs::exists(file));
}

TEST(ProjectLock, ASymlinkInPlaceOfTheLockIsNeverFollowed) {
  TempDir project, elsewhere;
  const fs::path target = fs::path(elsewhere.path()) / "precious.txt";
  write_file(target, "keep me");
  const fs::path file = lock_file(project.path());
  fs::create_symlink(target, file);
  EXPECT_FALSE(vyrn::ProjectLock::acquire(project.path(), "app").has_value());
  set_mtime(file, now_epoch() - 60, /*nofollow=*/true);
  {
    const auto lock = vyrn::ProjectLock::acquire(project.path(), "app");
    ASSERT_TRUE(lock.has_value());
    EXPECT_FALSE(fs::is_symlink(file));
    EXPECT_EQ(lock_json(project.path())["pid"], ::getpid());
  }
  EXPECT_EQ(read_file(target), "keep me");
}

TEST(ProjectLock, FoldersWhereNoLockCanBeMade) {
  TempDir root;
  EXPECT_FALSE(vyrn::ProjectLock::acquire(root / "missing", "app").has_value());
  EXPECT_FALSE(vyrn::ProjectLock::holder(root / "missing").has_value());
  EXPECT_FALSE(vyrn::ProjectLock::acquire("", "app").has_value());
  EXPECT_FALSE(vyrn::ProjectLock::holder("").has_value());
  fs::create_directory(fs::path(root / "folder-lock"));
  fs::create_directory(fs::path(root / "folder-lock") / ".vyrn-lock");   // a folder where the file should be
  EXPECT_FALSE(vyrn::ProjectLock::acquire(root / "folder-lock", "app").has_value());
  EXPECT_FALSE(vyrn::ProjectLock::holder(root / "folder-lock").has_value());
  if (::geteuid() != 0) {   // root may write anywhere
    const fs::path read_only = root / "read-only";
    fs::create_directory(read_only);
    fs::permissions(read_only, fs::perms::owner_read | fs::perms::owner_exec);
    EXPECT_FALSE(vyrn::ProjectLock::acquire(read_only.string(), "app").has_value());
    fs::permissions(read_only, fs::perms::owner_all);
  }
}

TEST(ProjectLock, ReleasesOnlyItsOwnFile) {
  TempDir project;
  Child sleeper([] { return true; });
  ASSERT_TRUE(sleeper.answer());
  {
    const auto lock = vyrn::ProjectLock::acquire(project.path(), "app");
    ASSERT_TRUE(lock.has_value());
    // Someone replaced the file meanwhile (by hand): it isn't ours any more, so it stays.
    write_lock(project.path(), {{"pid", sleeper.pid()}, {"by", "agent"}, {"since", "2026-11-13T17:05:00"}});
  }
  EXPECT_TRUE(fs::exists(lock_file(project.path())));
  EXPECT_EQ(lock_json(project.path())["pid"], sleeper.pid());
}

TEST(ProjectLock, AForkedChildNeverReleasesItsParentsLock) {
  TempDir project;
  std::optional<vyrn::ProjectLock> lock = vyrn::ProjectLock::acquire(project.path(), "app");
  ASSERT_TRUE(lock.has_value());
  Child child([&] {
    lock.reset();   // the child's copy of the object goes away: the parent's file must stay
    return !fs::exists(lock_file(project.path()));
  });
  EXPECT_FALSE(child.answer());
  child.release();
  EXPECT_TRUE(fs::exists(lock_file(project.path())));
  const auto holder = vyrn::ProjectLock::holder(project.path());
  ASSERT_TRUE(holder.has_value());
  EXPECT_EQ(holder->pid, ::getpid());
  lock.reset();
  EXPECT_FALSE(fs::exists(lock_file(project.path())));
}

TEST(ProjectLock, ForkRaceExactlyOneWins) {
  TempDir project;
  for (int round = 0; round < 25; ++round) {
    EXPECT_EQ(race(project.path(), 8), 1) << "no lock yet, round " << round;
    // The winner left without releasing: everyone now races to take over its stale lock.
    ASSERT_TRUE(fs::exists(lock_file(project.path())));
    EXPECT_FALSE(vyrn::ProjectLock::holder(project.path()).has_value());
    EXPECT_EQ(race(project.path(), 8), 1) << "stale lock, round " << round;
    fs::remove(lock_file(project.path()));
  }
}

#else  // VYRN_TEST_POSIX

TEST(ProjectLock, SkippedOnThisSystem) {
  GTEST_SKIP() << "POSIX only: these tests race real processes made with fork().";
}

#endif  // VYRN_TEST_POSIX
