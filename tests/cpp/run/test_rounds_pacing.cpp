// 20-round means, the best one, and Watch's pacing maths.
#include "run/pacing.h"
#include "run/rounds.h"

#include <gtest/gtest.h>

using namespace vyrn::run;

TEST(Rounds, MeanOfTheLastTwenty) {
  RoundTracker t;
  EXPECT_EQ(t.rounds(), 0);
  EXPECT_DOUBLE_EQ(t.mean20(), 0.0);
  t.add(1);
  t.add(-1);
  t.add(3);
  EXPECT_DOUBLE_EQ(t.mean20(), 1.0);   // fewer than 20: the mean of what there is
  EXPECT_FALSE(t.best_mean20());       // no best before 20 rounds
  for (int i = 0; i < 17; ++i) t.add(0);
  EXPECT_EQ(t.rounds(), 20);
  EXPECT_DOUBLE_EQ(t.mean20(), 3.0 / 20.0);
  ASSERT_TRUE(t.best_mean20());
  EXPECT_DOUBLE_EQ(*t.best_mean20(), 3.0 / 20.0);
  EXPECT_TRUE(t.improved());
  t.add(0);   // the first round (1) leaves the window
  EXPECT_DOUBLE_EQ(t.mean20(), 2.0 / 20.0);
  EXPECT_FALSE(t.improved());
  EXPECT_DOUBLE_EQ(*t.best_mean20(), 3.0 / 20.0);
}

TEST(Rounds, BestRisesOnlyOnAStrictlyBetterWindow) {
  RoundTracker t;
  for (int i = 0; i < 20; ++i) t.add(1);
  EXPECT_TRUE(t.improved());
  t.add(1);   // same mean
  EXPECT_FALSE(t.improved());
  t.add(2);
  EXPECT_TRUE(t.improved());
  EXPECT_DOUBLE_EQ(*t.best_mean20(), 21.0 / 20.0);
  for (int i = 0; i < 40; ++i) t.add(-5);
  EXPECT_DOUBLE_EQ(*t.best_mean20(), 21.0 / 20.0);
  EXPECT_DOUBLE_EQ(t.mean20(), -5.0);
}

TEST(Rounds, ABestFromEarlierRunsMustBeBeaten) {
  RoundTracker t(0.9);
  for (int i = 0; i < 20; ++i) t.add(0.5);
  EXPECT_FALSE(t.improved());
  EXPECT_DOUBLE_EQ(*t.best_mean20(), 0.9);
  for (int i = 0; i < 20; ++i) t.add(1.0);
  EXPECT_TRUE(t.improved());
  EXPECT_DOUBLE_EQ(*t.best_mean20(), 1.0);
}

TEST(Pacing, OnTimeTicksWaitForTheirDeadline) {
  const PaceStep s = pace(10.0, 10.03, 0.1);
  EXPECT_DOUBLE_EQ(s.due, 10.1);
  EXPECT_NEAR(s.wait_s, 0.07, 1e-12);
  EXPECT_EQ(s.late_s, 0.0);
  EXPECT_EQ(s.skipped, 0);
}

TEST(Pacing, ASlightlyLateTickRunsAtOnceAndKeepsTheGrid) {
  const PaceStep s = pace(10.0, 10.13, 0.1);
  EXPECT_DOUBLE_EQ(s.due, 10.1);
  EXPECT_EQ(s.wait_s, 0.0);
  EXPECT_NEAR(s.late_s, 0.03, 1e-12);
  EXPECT_EQ(s.skipped, 0);
  // The tick after it is due one period after the missed deadline, not after the late start.
  const PaceStep next = pace(s.due, 10.15, 0.1);
  EXPECT_DOUBLE_EQ(next.due, 10.2);
  EXPECT_NEAR(next.wait_s, 0.05, 1e-12);
}

TEST(Pacing, MissedTicksAreSkippedNotBursted) {
  const PaceStep s = pace(10.0, 10.55, 0.1);   // deadlines 10.1 .. 10.5 have all passed
  EXPECT_EQ(s.skipped, 4);                     // 10.1, 10.2, 10.3, 10.4 are skipped
  EXPECT_NEAR(s.due, 10.5, 1e-12);
  EXPECT_EQ(s.wait_s, 0.0);
  EXPECT_NEAR(s.late_s, 0.05, 1e-9);
  EXPECT_LT(s.late_s, 0.1);
}

TEST(Pacing, PacerStartsAtOnceAndCountsSkips) {
  Pacer p(0.1);
  ASSERT_TRUE(p.paced());
  PaceStep s = p.next(5.0);
  EXPECT_EQ(s.wait_s, 0.0);
  EXPECT_DOUBLE_EQ(s.due, 5.0);
  s = p.next(5.02);
  EXPECT_NEAR(s.wait_s, 0.08, 1e-12);
  s = p.next(5.43);   // due 5.2; 5.2 and 5.3 are over, 5.4 runs late
  EXPECT_EQ(s.skipped, 2);
  EXPECT_EQ(p.skipped(), 2);
  p.restart();        // after a pause the schedule starts again
  s = p.next(100.0);
  EXPECT_EQ(s.wait_s, 0.0);
  EXPECT_EQ(s.skipped, 0);
  EXPECT_DOUBLE_EQ(s.due, 100.0);
}

TEST(Pacing, ZeroPeriodMeansAsFastAsPossible) {
  Pacer p(0);
  EXPECT_FALSE(p.paced());
  for (int i = 0; i < 3; ++i) EXPECT_EQ(p.next(1.0 + i).wait_s, 0.0);
}

TEST(Pacing, Percentile) {
  EXPECT_EQ(percentile({}, 99), 0.0);
  std::vector<double> v;
  for (int i = 1; i <= 100; ++i) v.push_back(i);
  EXPECT_EQ(percentile(v, 99), 99.0);
  EXPECT_EQ(percentile(v, 100), 100.0);
  EXPECT_EQ(percentile(v, 50), 50.0);
  EXPECT_EQ(percentile({7.0}, 99), 7.0);
  EXPECT_EQ(percentile({3.0, 1.0, 2.0}, 0), 1.0);
}
