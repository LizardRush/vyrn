// What Check concludes from a few results, with hand-made worker replies.
#include "ipc/proto.h"
#include "run/check.h"

#include <gtest/gtest.h>

using namespace vyrn::run;
using vyrn::ipc::Frame;

namespace {

const json kSpec = {{"t", "spec"},
                    {"mode", "sim"},
                    {"obs", {{"kind", "numbers"}, {"shape", {3}}, {"names", {"fruit x", "fruit y", "basket"}}}},
                    {"choices", {"left", "right"}},
                    {"hooks", {{"see", "inputs.py:4"}, {"points", "points.py:2"}, {"game_over", "points.py:9"}}},
                    {"warnings", {{{"script", "responses.py"}, {"line", 0}, {"message", "No do-nothing choice"}}}}};

// A result frame with numbers as the observation (f32 [3]).
Frame numbers_result(std::vector<float> obs, json points, bool game_over, json ms = json::object(),
                     json warnings = json::array()) {
  Frame f;
  f.j = {{"t", "result"}, {"id", 1},           {"points", points},     {"game_over", game_over},
         {"truncated", false}, {"first", game_over}, {"warnings", warnings}, {"ms", ms}};
  f.j["obs"] = vyrn::ipc::append_array(f.bin, "f32", {static_cast<int64_t>(obs.size())}, obs.data());
  return f;
}

Frame picture_result(uint8_t value) {
  Frame f;
  std::vector<uint8_t> px(2 * 2 * 3, value);
  f.j = {{"t", "result"}, {"id", 1}, {"points", 0.0}, {"game_over", false}};
  f.j["obs"] = vyrn::ipc::append_array(f.bin, "u8", {2, 2, 3}, px.data());
  return f;
}

}  // namespace

TEST(Check, LocationOfAHook) {
  EXPECT_EQ(split_location("points.py:9"), (std::pair<std::string, int64_t>{"points.py", 9}));
  EXPECT_EQ(split_location("world.py:12"), (std::pair<std::string, int64_t>{"world.py", 12}));
  EXPECT_EQ(split_location("nonsense"), (std::pair<std::string, int64_t>{"", 0}));
  EXPECT_EQ(split_location("a.py:x"), (std::pair<std::string, int64_t>{"", 0}));
}

TEST(Check, SpecWarningsComeOnce) {
  CheckReport r(kSpec);
  auto w = r.take_new_warnings();
  ASSERT_EQ(w.size(), 1u);
  EXPECT_EQ(w[0].script, "responses.py");
  EXPECT_TRUE(r.take_new_warnings().empty());
  // A runtime warning arrives once, even if the worker repeated it.
  const json pts_none = json::array({{{"script", "points.py"}, {"line", 2}, {"message", "points() returned None"}}});
  r.add_result(numbers_result({1, 0, 5}, nullptr, false, json::object(), pts_none));
  r.add_result(numbers_result({1, 1, 5}, 0.0, false, json::object(), pts_none));
  w = r.take_new_warnings();
  ASSERT_EQ(w.size(), 1u);
  EXPECT_EQ(w[0].message, "points() returned None");
}

TEST(Check, FindsConstantInputsAndAMissingGameOver) {
  CheckReport r(kSpec);
  r.add_result(numbers_result({1, 0, 5}, nullptr, false, {{"see", 0.5}, {"points", 0.1}}));
  for (int i = 1; i <= 4; ++i) {
    r.count_step();
    r.add_result(numbers_result({1, static_cast<float>(i), 5}, i == 4 ? 1.0 : 0.0, false,
                                {{"see", 1.5}, {"points", 0.3}}));
  }
  EXPECT_EQ(r.constant_inputs(), (std::vector<std::string>{"fruit x", "basket"}));
  EXPECT_FALSE(r.game_over_seen());
  const auto found = r.findings(false, 10);
  ASSERT_EQ(found.size(), 3u);
  EXPECT_EQ(found[0].script, "points.py");   // where game_over() is defined
  EXPECT_EQ(found[0].line, 9);
  EXPECT_NE(found[0].message.find("never True in 4 steps"), std::string::npos);
  EXPECT_EQ(found[1].script, "inputs.py");
  EXPECT_NE(found[1].message.find("\"fruit x\" never changed"), std::string::npos);

  const json e = json::parse(event_text(r.event(true, false, 10)));
  EXPECT_EQ(e["type"], "check");
  EXPECT_EQ(e["ok"], true);
  EXPECT_EQ(e["warnings"].size(), 4u);   // the spec's + the three findings
  EXPECT_DOUBLE_EQ(e["ms"]["see"].get<double>(), 1.3);        // (0.5 + 4 * 1.5) / 5
  EXPECT_DOUBLE_EQ(e["mean_points"].get<double>(), 0.25);     // points 0, 0, 0, 1 (null doesn't count)
  EXPECT_EQ(e["game_over_seen"], false);
  EXPECT_EQ(e["constant_inputs"], json::array({"fruit x", "basket"}));
  EXPECT_EQ(e["steps"], 4);
}

TEST(Check, GameOverSeenAndEverythingChanges) {
  CheckReport r(kSpec);
  r.add_result(numbers_result({1, 0, 5}, nullptr, false));
  r.count_step();
  r.add_result(numbers_result({2, 1, 6}, -1.0, true));
  EXPECT_TRUE(r.game_over_seen());
  EXPECT_TRUE(r.constant_inputs().empty());
  EXPECT_TRUE(r.findings(false, 10).empty());
}

TEST(Check, APictureThatNeverChanges) {
  json spec = kSpec;
  spec["obs"] = {{"kind", "picture"}, {"shape", {2, 2, 3}}, {"names", nullptr}};
  spec.erase("hooks");
  CheckReport same(spec);
  same.add_result(picture_result(107));
  same.count_step();
  same.add_result(picture_result(107));
  EXPECT_EQ(same.constant_inputs(), (std::vector<std::string>{"picture"}));
  ASSERT_EQ(same.findings(false, 10).size(), 1u);

  CheckReport moving(spec);
  moving.add_result(picture_result(107));
  moving.count_step();
  moving.add_result(picture_result(108));
  EXPECT_TRUE(moving.constant_inputs().empty());
  EXPECT_TRUE(moving.findings(false, 10).empty());
}

TEST(Check, RealTimeSlowSeeAndDescribedEffects) {
  CheckReport r(kSpec);
  r.add_result(numbers_result({1, 0, 5}, nullptr, false, {{"see", 90.0}}));
  r.add_ok({{"t", "ok"}, {"did", {"would hold A"}}, {"ms", {{"respond", 0.2}}}});
  r.count_step();
  r.add_result(numbers_result({2, 1, 6}, 0.0, true, {{"see", 95.0}}));
  r.add_ok({{"t", "ok"}, {"did", {"would hold A", "would tap Space"}}});
  const auto found = r.findings(true, 10);   // a tick is 100 ms; see() takes ~92 ms
  ASSERT_EQ(found.size(), 1u);
  EXPECT_NE(found[0].message.find("over 80% of a tick"), std::string::npos);
  const json e = json::parse(event_text(r.event(true, true, 10)));
  EXPECT_EQ(e["mode"], "realtime");
  EXPECT_EQ(e["did"], json::array({"would hold A", "would tap Space"}));
}

TEST(Check, RealTimeGameOverIsANoteAndRestartIsReported) {
  CheckReport r(kSpec);
  r.add_restart({{"t", "ok"}, {"did", {"would tap Space"}}, {"ms", {{"restart", 2.0}}}});
  EXPECT_EQ(r.restart_summary(), "restart(): would tap Space (nothing was pressed).");
  r.add_result(numbers_result({1, 0, 5}, nullptr, false));
  for (int i = 1; i <= 5; ++i) {
    r.count_step();
    r.add_result(numbers_result({static_cast<float>(i), static_cast<float>(i), static_cast<float>(i)}, 0.0, false));
  }
  // A real game rarely ends within a 5-tick check: not a warning, a note.
  EXPECT_TRUE(r.findings(true, 10).empty());
  const auto notes = r.notes(true, 10);
  ASSERT_EQ(notes.size(), 1u);
  EXPECT_NE(notes[0].find("game_over() stayed False during this short check (0.5 s)"), std::string::npos);
  EXPECT_TRUE(r.notes(false, 10).empty());   // a simulation warns instead
  ASSERT_EQ(r.findings(false, 10).size(), 1u);
  const json e = json::parse(event_text(r.event(true, true, 10)));
  EXPECT_EQ(e["notes"].size(), 1u);
  EXPECT_EQ(e["restart_did"], json::array({"would tap Space"}));
  EXPECT_DOUBLE_EQ(e["ms"]["restart"].get<double>(), 2.0);
  EXPECT_TRUE(e["warnings"].size() == 1u);   // only the spec's "No do-nothing choice"

  CheckReport quiet(kSpec);
  quiet.add_restart({{"t", "ok"}, {"did", json::array()}});
  EXPECT_EQ(quiet.restart_summary(), "restart(): ran (it has no keys to press).");
  EXPECT_EQ(CheckReport(kSpec).restart_summary(), "");
}

TEST(Check, AnAlmostBlackPictureWarnsInBothModes) {
  json spec = kSpec;
  spec["obs"] = {{"kind", "picture"}, {"shape", {2, 2, 3}}, {"names", nullptr}};
  spec["hooks"] = {{"see", "inputs.py:4"}};
  for (bool realtime : {false, true}) {
    CheckReport dark(spec);
    dark.add_result(picture_result(3));
    dark.count_step();
    dark.add_result(picture_result(15));
    const auto found = dark.findings(realtime, 10);
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].message, "The picture is almost black (brightest value 15 of 255): the AI can hardly see anything.");
    EXPECT_EQ(found[0].script, "inputs.py");

    CheckReport bright(spec);
    bright.add_result(picture_result(3));
    bright.count_step();
    bright.add_result(picture_result(16));
    EXPECT_TRUE(bright.findings(realtime, 10).empty());
  }
}
