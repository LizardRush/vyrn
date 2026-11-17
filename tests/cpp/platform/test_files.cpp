// platform/files.h and platform/system.h on this system: exclusive create, atomic replace, no-replace moves, file
// identity, the folder lock, process identity and the public helpers of vyrn/platform.h.
#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "platform/files.h"
#include "platform/system.h"
#include "platform_test.h"
#include "vyrn/platform.h"

namespace {

namespace fs = std::filesystem;
namespace platform = vyrn::platform;

class TempDir {
 public:
  TempDir() {
    uint8_t bytes[8];
    platform::random_bytes(bytes, sizeof bytes);
    std::string name = "vyrn-platform-";
    for (const uint8_t b : bytes) name += "0123456789abcdef"[b & 15];
    path_ = fs::temp_directory_path() / name;
    fs::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  std::string file(const std::string& name) const { return (path_ / name).string(); }

 private:
  fs::path path_;
};

std::string read(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

void write(const std::string& path, const std::string& text) { std::ofstream(path, std::ios::binary) << text; }

TEST(PlatformFiles, ExclusiveCreateRefusesAnExistingName) {
  TempDir tmp;
  int err = 0;
  platform::File f = platform::File::create(tmp.file("a"), platform::File::Create::Exclusive, true, 0644, err);
  ASSERT_TRUE(f) << platform::error_text(err);
  EXPECT_EQ(f.write_all("hello", 5), 0);
  EXPECT_EQ(f.sync(), 0);
  EXPECT_EQ(f.close(), 0);
  platform::File again = platform::File::create(tmp.file("a"), platform::File::Create::Exclusive, true, 0644, err);
  EXPECT_FALSE(again);
  EXPECT_EQ(err, EEXIST);
  EXPECT_EQ(read(tmp.file("a")), "hello");
}

TEST(PlatformFiles, ReadAndInfo) {
  TempDir tmp;
  write(tmp.file("r"), "0123456789");
  int err = 0;
  platform::File f = platform::File::open_read(tmp.file("r"), true, err);
  ASSERT_TRUE(f);
  platform::FileInfo info;
  ASSERT_EQ(f.info(info), 0);
  EXPECT_TRUE(info.is_regular());
  EXPECT_EQ(info.size, 10u);
  EXPECT_NEAR(static_cast<double>(info.mtime_s), static_cast<double>(std::time(nullptr)), 120.0);
  char buf[32];
  size_t got = 0;
  ASSERT_EQ(f.read(buf, sizeof buf, got), 0);
  EXPECT_EQ(std::string(buf, got), "0123456789");
  ASSERT_EQ(f.read(buf, sizeof buf, got), 0);
  EXPECT_EQ(got, 0u);   // the end
  platform::File missing = platform::File::open_read(tmp.file("nothing"), false, err);
  EXPECT_FALSE(missing);
  EXPECT_EQ(err, ENOENT);
  platform::FileInfo same, other;
  ASSERT_EQ(platform::file_info(tmp.file("r"), true, same), 0);
  EXPECT_EQ(same.device, info.device);
  EXPECT_EQ(same.inode, info.inode);
  write(tmp.file("s"), "x");
  ASSERT_EQ(platform::file_info(tmp.file("s"), true, other), 0);
  EXPECT_FALSE(other.device == info.device && other.inode == info.inode);
}

TEST(PlatformFiles, NoFollowRefusesASymlink) {
  VYRN_SKIP_WITHOUT_SYMLINKS();
  TempDir tmp;
  write(tmp.file("target"), "secret");
  fs::create_symlink(tmp.file("target"), tmp.file("link"));
  int err = 0;
  EXPECT_FALSE(platform::File::open_read(tmp.file("link"), true, err));
  EXPECT_EQ(err, ELOOP);
  EXPECT_TRUE(platform::File::open_read(tmp.file("link"), false, err));
  platform::FileInfo info;
  ASSERT_EQ(platform::file_info(tmp.file("link"), false, info), 0);
  EXPECT_TRUE(info.is_symlink());
  ASSERT_EQ(platform::file_info(tmp.file("link"), true, info), 0);
  EXPECT_TRUE(info.is_regular());
}

TEST(PlatformFiles, RenameReplaceAndNoReplace) {
  TempDir tmp;
  write(tmp.file("new"), "new");
  write(tmp.file("old"), "old");
  EXPECT_EQ(platform::rename_replace(tmp.file("new"), tmp.file("old")), 0);
  EXPECT_EQ(read(tmp.file("old")), "new");
  EXPECT_FALSE(fs::exists(tmp.file("new")));

  write(tmp.file("a"), "a");
  write(tmp.file("b"), "b");
  EXPECT_EQ(platform::rename_no_replace(tmp.file("a"), tmp.file("b"), false), EEXIST);
  EXPECT_EQ(read(tmp.file("b")), "b");
  EXPECT_EQ(read(tmp.file("a")), "a");
  EXPECT_EQ(platform::rename_no_replace(tmp.file("a"), tmp.file("c"), false), 0);
  EXPECT_EQ(read(tmp.file("c")), "a");
  EXPECT_FALSE(fs::exists(tmp.file("a")));

  ASSERT_EQ(platform::make_dir(tmp.file("d1"), 0755), 0);
  EXPECT_EQ(platform::make_dir(tmp.file("d1"), 0755), EEXIST);
  write(tmp.file("d1") + "/inside", "x");
  ASSERT_EQ(platform::make_dir(tmp.file("d2"), 0755), 0);
  write(tmp.file("d2") + "/kept", "y");
  const int taken = platform::rename_no_replace(tmp.file("d1"), tmp.file("d2"), true);
  EXPECT_TRUE(taken == EEXIST || taken == ENOTEMPTY) << taken;
  EXPECT_TRUE(fs::exists(tmp.file("d2") + "/kept"));
  EXPECT_EQ(platform::rename_no_replace(tmp.file("d1"), tmp.file("d3"), true), 0);
  EXPECT_TRUE(fs::exists(tmp.file("d3") + "/inside"));
  EXPECT_EQ(platform::sync_path(tmp.file("d3"), true), 0);
  EXPECT_EQ(platform::sync_path(tmp.file("c"), false), 0);
  EXPECT_EQ(platform::remove_file(tmp.file("c")), 0);
  EXPECT_EQ(platform::remove_file(tmp.file("c")), ENOENT);
}

TEST(PlatformFiles, CreateUniqueMakesFreshNames) {
  TempDir tmp;
  std::string first, second;
  int err = 0;
  platform::File a = platform::File::create_unique(tmp.file("code.tmp-"), first, err);
  platform::File b = platform::File::create_unique(tmp.file("code.tmp-"), second, err);
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  EXPECT_NE(first, second);
  EXPECT_EQ(first.rfind(tmp.file("code.tmp-"), 0), 0u);
  EXPECT_EQ(first.size(), tmp.file("code.tmp-").size() + 6);
}

TEST(PlatformFiles, WriteFileAtomicReplacesAndCleansUp) {
  TempDir tmp;
  write(tmp.file("script.py"), "old");
  write(tmp.file(".script.py.new"), "leftover");
  EXPECT_EQ(platform::write_file_atomic(tmp.file("script.py"), tmp.file(".script.py.new"), "print(1)\n", 0644), 0);
  EXPECT_EQ(read(tmp.file("script.py")), "print(1)\n");
  EXPECT_FALSE(fs::exists(tmp.file(".script.py.new")));
  EXPECT_TRUE(platform::is_writable(tmp.file("script.py")));
  EXPECT_NE(platform::write_file_atomic(tmp.file("no-such-dir/x.py"), tmp.file("no-such-dir/.x"), "x", 0644), 0);
}

TEST(PlatformFiles, FolderLockIsExclusiveBetweenHolders) {
  TempDir tmp;
  const std::string folder = tmp.file("");
  std::atomic<int> state{0};   // 1: the first holder has it; 2: the second looked
  bool second_busy = false;
  std::thread first([&] {
    platform::FolderLock lock(folder, 500);
    EXPECT_FALSE(lock.busy());
    state = 1;
    while (state != 2) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  });
  while (state != 1) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  std::thread second([&] {
    platform::FolderLock lock(folder, 50);
    second_busy = lock.busy();
  });
  second.join();
  state = 2;
  first.join();
  EXPECT_TRUE(second_busy) << "a second holder must wait while the first holds the folder";
  platform::FolderLock after(folder, 50);
  EXPECT_FALSE(after.busy());
}

TEST(PlatformSystem, ThisProcessIsRunningAndKnown) {
  const int64_t me = platform::current_pid();
  EXPECT_GT(me, 0);
  EXPECT_TRUE(platform::process_running(me));
  const auto id = platform::process_identity(me);
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(platform::process_identity(me)->start, id->start);   // stable
  EXPECT_FALSE(platform::process_running(0));
  EXPECT_FALSE(platform::process_identity(-5).has_value());
  EXPECT_TRUE(platform::same_boot(platform::boot_id()) || platform::boot_id().empty());
  EXPECT_FALSE(platform::same_boot("not a boot"));
}

TEST(PlatformSystem, PlacesAndTime) {
  EXPECT_FALSE(platform::executable_path().empty());
  EXPECT_TRUE(fs::exists(platform::executable_path()));
  EXPECT_FALSE(platform::runtime_dir().empty());
  EXPECT_FALSE(platform::default_python().empty());
  const auto candidates = platform::data_dir_candidates(platform::executable_path());
  ASSERT_FALSE(candidates.empty());
  for (const auto& c : candidates) EXPECT_NE(c.find("vyrn"), std::string::npos) << c;
  std::tm utc{};
  ASSERT_TRUE(platform::utc_time(0, utc));
  EXPECT_EQ(utc.tm_year, 70);
  EXPECT_EQ(utc.tm_mday, 1);
  std::tm local{};
  EXPECT_TRUE(platform::local_time(std::time(nullptr), local));
  int64_t s = 0;
  int32_t ns = 0;
  platform::file_clock_now(s, ns);
  EXPECT_NEAR(static_cast<double>(s), static_cast<double>(std::time(nullptr)), 5.0);
  EXPECT_GE(ns, 0);
  EXPECT_LT(ns, 1000000000);
}

TEST(PlatformSystem, EnvironmentChangesAreSeen) {
  ASSERT_TRUE(platform::set_env("VYRN_PLATFORM_TEST", "yes"));
  ASSERT_NE(std::getenv("VYRN_PLATFORM_TEST"), nullptr);
  EXPECT_STREQ(std::getenv("VYRN_PLATFORM_TEST"), "yes");
  ASSERT_TRUE(platform::unset_env("VYRN_PLATFORM_TEST"));
  EXPECT_EQ(std::getenv("VYRN_PLATFORM_TEST"), nullptr);
}

TEST(PlatformSystem, RunCaptureCollectsStdoutAndReportsFailures) {
  const std::string python = platform::default_python();
  std::string out, why;
  if (!platform::run_capture({python, "-c", "print('ok')"}, 30, 1 << 16, out, why)) {
    GTEST_SKIP() << "no Python to run here (" << why << ")";
  }
  EXPECT_EQ(out.substr(0, 2), "ok");
  EXPECT_FALSE(platform::run_capture({python, "-c", "import sys; sys.stderr.write('bad\\n'); sys.exit(3)"}, 30, 100,
                                     out, why));
  EXPECT_NE(why.find("bad"), std::string::npos) << why;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(platform::run_capture({python, "-c", "import time; time.sleep(30)"}, 0.5, 100, out, why));
  EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), 10.0);
  EXPECT_NE(why.find("took longer"), std::string::npos) << why;
  EXPECT_FALSE(platform::run_capture({"vyrn-no-such-program-xyz"}, 5, 100, out, why));
  EXPECT_NE(why.find("can't start"), std::string::npos) << why;
}

}  // namespace
