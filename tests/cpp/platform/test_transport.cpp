// The portable pieces of the Windows worker transport (platform/transport.h, docs/dev/platforms.md section 1): the
// token, the connection lines, the control bytes, and how a child's command line and environment are built.
#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "platform/system.h"
#include "platform/transport.h"

namespace {

using vyrn::platform::ConnectionGreeting;
using vyrn::platform::ConnectionRole;
using vyrn::platform::ConnectionSlots;
using vyrn::platform::Control;

const std::string kToken = "0123456789abcdef0123456789abcdef";

TEST(Transport, ControlBytesAreTheProtocolsLetters) {
  EXPECT_EQ(vyrn::platform::control_byte(Control::Interrupt), 'I');
  EXPECT_EQ(vyrn::platform::control_byte(Control::Freeze), 'F');
}

TEST(Transport, TokenIs32LowercaseHexCharacters) {
  uint8_t bytes[vyrn::platform::kTokenBytes];
  for (size_t i = 0; i < sizeof bytes; ++i) bytes[i] = static_cast<uint8_t>(i * 17);
  EXPECT_EQ(vyrn::platform::token_from_bytes(bytes), "00112233445566778899aabbccddeeff");
  EXPECT_TRUE(vyrn::platform::valid_token(kToken));
  EXPECT_FALSE(vyrn::platform::valid_token("0123456789ABCDEF0123456789ABCDEF"));   // lowercase only
  EXPECT_FALSE(vyrn::platform::valid_token(kToken.substr(1)));
  EXPECT_FALSE(vyrn::platform::valid_token(kToken + "0"));
  EXPECT_FALSE(vyrn::platform::valid_token("0123456789abcdef0123456789abcdeg"));
}

TEST(Transport, RandomTokensAreValidAndDiffer) {
  std::set<std::string> seen;
  for (int i = 0; i < 20; ++i) {
    uint8_t bytes[vyrn::platform::kTokenBytes];
    ASSERT_TRUE(vyrn::platform::random_bytes(bytes, sizeof bytes));
    const std::string token = vyrn::platform::token_from_bytes(bytes);
    EXPECT_TRUE(vyrn::platform::valid_token(token)) << token;
    seen.insert(token);
  }
  EXPECT_EQ(seen.size(), 20u);
}

TEST(Transport, WorkerAddressIsLoopbackAndPort) {
  EXPECT_EQ(vyrn::platform::worker_address(50123), "127.0.0.1:50123");
}

TEST(Transport, ConnectionLinesNameTheirRole) {
  EXPECT_EQ(vyrn::platform::connection_line(kToken, ConnectionRole::Main), kToken + " main\n");
  EXPECT_EQ(vyrn::platform::connection_line(kToken, ConnectionRole::Ctl), kToken + " ctl\n");
  using vyrn::platform::parse_connection_line;
  EXPECT_EQ(parse_connection_line(kToken + " main", kToken), ConnectionRole::Main);
  EXPECT_EQ(parse_connection_line(kToken + " ctl", kToken), ConnectionRole::Ctl);
  EXPECT_EQ(parse_connection_line(kToken + " ctl\r", kToken), ConnectionRole::Ctl);   // a CRLF line
  EXPECT_EQ(parse_connection_line("ffffffffffffffffffffffffffffffff main", kToken), ConnectionRole::None);
  EXPECT_EQ(parse_connection_line(kToken + " MAIN", kToken), ConnectionRole::None);
  EXPECT_EQ(parse_connection_line(kToken + "  main", kToken), ConnectionRole::None);
  EXPECT_EQ(parse_connection_line(kToken + " main ", kToken), ConnectionRole::None);
  EXPECT_EQ(parse_connection_line(kToken + "main", kToken), ConnectionRole::None);
  EXPECT_EQ(parse_connection_line(kToken, kToken), ConnectionRole::None);
  EXPECT_EQ(parse_connection_line("", kToken), ConnectionRole::None);
  EXPECT_EQ(parse_connection_line(" main", ""), ConnectionRole::None);   // an empty token never matches
}

TEST(Transport, GreetingWaitsForTheWholeLineAndKeepsWhatFollows) {
  ConnectionGreeting g(kToken);
  const std::string line = kToken + " main\n";
  for (size_t i = 0; i + 1 < line.size(); ++i) {
    EXPECT_EQ(g.add(&line[i], 1), ConnectionGreeting::State::Waiting) << "after " << i + 1 << " bytes";
  }
  const std::string last = "\nVYRN frame bytes";
  EXPECT_EQ(g.add(last.data(), last.size()), ConnectionGreeting::State::Accepted);
  EXPECT_EQ(g.role(), ConnectionRole::Main);
  EXPECT_EQ(g.leftover(), "VYRN frame bytes");
  const std::string more = "+more";
  EXPECT_EQ(g.add(more.data(), more.size()), ConnectionGreeting::State::Accepted);
  EXPECT_EQ(g.leftover(), "VYRN frame bytes+more");
}

TEST(Transport, GreetingRefusesWrongTokensAndEndlessLines) {
  ConnectionGreeting wrong(kToken);
  const std::string bad = "ffffffffffffffffffffffffffffffff ctl\n";
  EXPECT_EQ(wrong.add(bad.data(), bad.size()), ConnectionGreeting::State::Refused);
  EXPECT_EQ(wrong.role(), ConnectionRole::None);

  ConnectionGreeting endless(kToken);
  const std::string junk(vyrn::platform::kMaxConnectionLine + 1, 'x');
  EXPECT_EQ(endless.add(junk.data(), junk.size()), ConnectionGreeting::State::Refused);
  EXPECT_EQ(endless.add("\n", 1), ConnectionGreeting::State::Refused);   // stays refused

  ConnectionGreeting http(kToken);
  const std::string get = "GET / HTTP/1.1\r\n";
  EXPECT_EQ(http.add(get.data(), get.size()), ConnectionGreeting::State::Refused);
}

TEST(Transport, EachRoleIsTakenOnce) {
  ConnectionSlots slots;
  EXPECT_FALSE(slots.complete());
  EXPECT_FALSE(slots.take(ConnectionRole::None));
  EXPECT_TRUE(slots.take(ConnectionRole::Ctl));
  EXPECT_FALSE(slots.take(ConnectionRole::Ctl));   // a second "ctl" is refused
  EXPECT_FALSE(slots.complete());
  EXPECT_TRUE(slots.take(ConnectionRole::Main));
  EXPECT_FALSE(slots.take(ConnectionRole::Main));
  EXPECT_TRUE(slots.complete());
}

// What CommandLineToArgvW (and the MSVC runtime) turn back into the same argument.
TEST(Transport, WindowsArgumentsAreQuotedOnlyWhenNeeded) {
  using vyrn::platform::quote_windows_arg;
  EXPECT_EQ(quote_windows_arg("python"), "python");
  EXPECT_EQ(quote_windows_arg("C:\\Python312\\python.exe"), "C:\\Python312\\python.exe");   // lone \ stay
  EXPECT_EQ(quote_windows_arg(""), "\"\"");
  EXPECT_EQ(quote_windows_arg("My Game"), "\"My Game\"");
  EXPECT_EQ(quote_windows_arg("tab\there"), "\"tab\there\"");
  EXPECT_EQ(quote_windows_arg("say \"hi\""), "\"say \\\"hi\\\"\"");
  EXPECT_EQ(quote_windows_arg("C:\\My Projects\\"), "\"C:\\My Projects\\\\\"");   // doubled before the quote
  EXPECT_EQ(quote_windows_arg("a\\\\\"b"), "\"a\\\\\\\\\\\"b\"");                 // a\\"b -> "a\\\\\"b"
  EXPECT_EQ(quote_windows_arg("Ünïcödé dir"), "\"Ünïcödé dir\"");                  // UTF-8 passes through
}

TEST(Transport, WindowsCommandLineJoinsQuotedArguments) {
  EXPECT_EQ(vyrn::platform::windows_command_line({"py", "-u", "-X", "utf8", "C:\\Vyrn data\\worker.py", ""}),
            "py -u -X utf8 \"C:\\Vyrn data\\worker.py\" \"\"");
}

TEST(Transport, EnvironmentEntriesAreReplacedByName) {
  using vyrn::platform::set_env_entry;
  std::vector<std::string> posix = {"PATH=/bin", "Path=x", "OMP_NUM_THREADS=8", "VYRN_FDX=1"};
  set_env_entry(posix, "PATH", "/usr/bin", false);
  set_env_entry(posix, "OMP_NUM_THREADS", "1", false);
  set_env_entry(posix, "VYRN_FD", "3", false);   // not a prefix match: VYRN_FDX stays
  EXPECT_EQ(posix, (std::vector<std::string>{"Path=x", "VYRN_FDX=1", "PATH=/usr/bin", "OMP_NUM_THREADS=1",
                                             "VYRN_FD=3"}));

  std::vector<std::string> windows = {"=C:=C:\\Users", "Path=C:\\Windows", "PATH=dup", "TEMP=C:\\Temp"};
  set_env_entry(windows, "PATH", "C:\\Python", true);   // names ignore case on Windows
  set_env_entry(windows, "C:", "nothing", true);        // "=C:" is another name
  EXPECT_EQ(windows, (std::vector<std::string>{"=C:=C:\\Users", "TEMP=C:\\Temp", "PATH=C:\\Python", "C:=nothing"}));
}

}  // namespace
