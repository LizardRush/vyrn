// CSV logs and the console line throttle.
#include "util/csv.h"
#include "util/throttle.h"

#include <gtest/gtest.h>

#include "platform_test.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

namespace fs = std::filesystem;
using namespace vyrn::util;

TEST(Csv, CellsAreQuotedOnlyWhenNeeded) {
  EXPECT_EQ(csv_cell("plain"), "plain");
  EXPECT_EQ(csv_cell(""), "");
  EXPECT_EQ(csv_cell("a,b"), "\"a,b\"");
  EXPECT_EQ(csv_cell("say \"hi\""), "\"say \"\"hi\"\"\"");
  EXPECT_EQ(csv_cell("two\nlines"), "\"two\nlines\"");
}

TEST(Csv, Numbers) {
  EXPECT_EQ(csv_number(0.25), "0.25");
  EXPECT_EQ(csv_number(1240.0), "1240");
  EXPECT_EQ(csv_number(-0.0), "0");
  EXPECT_EQ(csv_number(1.0 / 3.0), "0.333333");
  EXPECT_EQ(csv_number(2.5e-4), "0.00025");
  EXPECT_EQ(csv_number(std::numeric_limits<double>::quiet_NaN()), "");
  EXPECT_EQ(csv_number(std::numeric_limits<double>::infinity()), "");
  EXPECT_EQ(csv_number(123456789LL), "123456789");
}

TEST(Csv, WriterWritesHeaderAndRows) {
  std::string pattern = (fs::temp_directory_path() / "vyrn-csv-XXXXXX").string();
  const std::string dir = ::mkdtemp(pattern.data());
  const std::string path = dir + "/rounds.csv";
  {
    CsvWriter w;
    std::string why;
    ASSERT_TRUE(w.open(path, {"round", "points", "note, with comma"}, &why)) << why;
    w.row({csv_number(1LL), csv_number(-1.0), csv_cell("missed")});
    w.row({csv_number(2LL), csv_number(1.0), csv_cell("caught \"it\"")});
    w.flush();
    std::ifstream in(path);
    std::ostringstream os;
    os << in.rdbuf();
    EXPECT_EQ(os.str(), "round,points,\"note, with comma\"\n1,-1,missed\n2,1,\"caught \"\"it\"\"\"\n");
  }
  CsvWriter bad;
  std::string why;
  EXPECT_FALSE(bad.open(dir + "/no/such/dir/x.csv", {"a"}, &why));
  EXPECT_FALSE(why.empty());
  bad.row({"ignored"});   // harmless when not open
  fs::remove_all(dir);
}

TEST(Throttle, ShowsUpToTheLimitPerSecondAndCountsTheRest) {
  LineThrottler t(200);
  int shown = 0;
  for (int i = 0; i < 1000; ++i) {
    const auto v = t.offer(10.0 + i * 0.0005);   // 1000 lines in half a second
    shown += v.show;
    EXPECT_EQ(v.dropped_before, 0);
  }
  EXPECT_EQ(shown, 200);
  EXPECT_EQ(t.total_dropped(), 800);
  // The next window reports the drops before its first line.
  const auto v = t.offer(11.1);
  EXPECT_TRUE(v.show);
  EXPECT_EQ(v.dropped_before, 800);
  EXPECT_EQ(t.flush(20.0), 0);   // already reported
}

TEST(Throttle, FlushReportsAFloodThatStoppedSuddenly) {
  LineThrottler t(3);
  for (int i = 0; i < 10; ++i) t.offer(5.0);
  EXPECT_EQ(t.flush(5.5), 0);   // the window isn't over yet
  EXPECT_EQ(t.flush(6.0), 7);
  EXPECT_EQ(t.flush(9.0), 0);   // reported once
  const auto v = t.offer(9.0);
  EXPECT_TRUE(v.show);
  EXPECT_EQ(v.dropped_before, 0);
}

TEST(Throttle, QuietOutputIsNeverDropped) {
  LineThrottler t(200);
  for (int i = 0; i < 2000; ++i) EXPECT_TRUE(t.offer(i * 0.01).show);   // 100 lines a second
  EXPECT_EQ(t.total_dropped(), 0);
}
