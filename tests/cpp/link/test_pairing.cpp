// A project's Vyrn Link pairing code (<project>/.vyrn-link-code) and the address a game connects to (vyrn/link.h).
#include "vyrn/link.h"

#include "platform_test.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

class TempDir {
 public:
  TempDir() {
    std::string pattern = (fs::temp_directory_path() / "vyrn-pair-XXXXXX").string();
    path_ = ::mkdtemp(pattern.data());
  }
  ~TempDir() {
    std::error_code ec;
    fs::permissions(path_, fs::perms::owner_all, ec);
    fs::remove_all(path_, ec);
  }
  fs::path path() const { return path_; }

 private:
  std::string path_;
};

std::string read(const fs::path& p) {
  std::ifstream in(p);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

bool six_digits(const std::string& code) {
  return code.size() == 6 && code[0] != '0' && code.find_first_not_of("0123456789") == std::string::npos;
}

}  // namespace

TEST(PairingCode, MadeOnceThenKept) {
  TempDir dir;
  bool saved = false;
  const std::string code = vyrn::link_code(dir.path().string(), &saved);
  EXPECT_TRUE(saved);
  EXPECT_TRUE(six_digits(code)) << code;
  const fs::path file = dir.path() / vyrn::kLinkCodeFile;
  EXPECT_EQ(read(file), code + "\n");
  if (VYRN_TEST_POSIX) {   // Windows: the folder's permissions apply
    EXPECT_EQ(fs::status(file).permissions() & fs::perms::all, fs::perms::owner_read | fs::perms::owner_write)
        << "only the owner can read it";
  }
  EXPECT_EQ(vyrn::link_code(dir.path().string()), code) << "the same code every time";
  for (const auto& entry : fs::directory_iterator(dir.path())) {
    EXPECT_EQ(entry.path().filename(), vyrn::kLinkCodeFile) << "no temporary files left";
  }
}

TEST(PairingCode, ATypedCodeIsReadAndABrokenOneReplaced) {
  TempDir dir;
  const fs::path file = dir.path() / vyrn::kLinkCodeFile;
  std::ofstream(file) << " 483920 \n";
  EXPECT_EQ(vyrn::link_code(dir.path().string()), "483920");
  std::ofstream(file) << "012345\n";   // never a leading 0
  const std::string code = vyrn::link_code(dir.path().string());
  EXPECT_TRUE(six_digits(code)) << code;
  EXPECT_EQ(read(file), code + "\n");
}

TEST(PairingCode, NewCodeReplacesTheOldOne) {
  TempDir dir;
  const std::string old = vyrn::link_code(dir.path().string());
  std::string why;
  const std::string fresh = vyrn::new_link_code(dir.path().string(), &why);
  EXPECT_TRUE(six_digits(fresh)) << why;
  EXPECT_NE(fresh, old);
  EXPECT_EQ(vyrn::link_code(dir.path().string()), fresh);
}

TEST(PairingCode, AReadOnlyFolderStillGivesACode) {
  if (::geteuid() == 0) GTEST_SKIP() << "root can write anywhere";
  TempDir dir;
  fs::permissions(dir.path(), fs::perms::owner_read | fs::perms::owner_exec);
  bool saved = true;
  EXPECT_TRUE(six_digits(vyrn::link_code(dir.path().string(), &saved)));
  EXPECT_FALSE(saved);
  std::string why;
  EXPECT_EQ(vyrn::new_link_code(dir.path().string(), &why), "");
  EXPECT_NE(why.find(vyrn::kLinkCodeFile), std::string::npos) << why;
}

TEST(LinkAddress, LanAddressesAndTheUrl) {
  for (const auto& a : vyrn::lan_addresses()) {
    EXPECT_NE(a.rfind("127.", 0), 0u) << a;
    EXPECT_EQ(std::count(a.begin(), a.end(), '.'), 3) << a;
  }
  const std::string url = vyrn::link_url(7457);
  EXPECT_EQ(url.rfind("ws://", 0), 0u) << url;
  EXPECT_EQ(url.substr(url.size() - 6), ":7457/") << url;
  const auto lan = vyrn::lan_addresses();
  EXPECT_EQ(url, "ws://" + (lan.empty() ? std::string("127.0.0.1") : lan.front()) + ":7457/");
}
