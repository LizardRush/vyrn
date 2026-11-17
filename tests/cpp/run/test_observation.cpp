// Reading observations and vyrn.show() values out of worker results.
#include "ipc/proto.h"
#include "run/observation.h"

#include <gtest/gtest.h>

#include <map>

using namespace vyrn::run;
using vyrn::ipc::Frame;
using nlohmann::json;

TEST(Observation, PicturesAndNumbers) {
  Frame f;
  const std::vector<uint8_t> px = {1, 2, 3, 4, 5, 6};   // 1 x 2 pixels
  const std::vector<float> nums = {0.5f, -2.0f, 7.0f};
  f.j = {{"t", "result"}, {"final_obs", nullptr}};
  f.j["obs"] = vyrn::ipc::append_array(f.bin, "u8", {1, 2, 3}, px.data());
  f.j["numbers"] = vyrn::ipc::append_array(f.bin, "f32", {3}, nums.data());
  f.j["bad"] = vyrn::ipc::append_array(f.bin, "u8", {2, 2}, px.data());

  ObsView o;
  ASSERT_TRUE(read_obs(f, "obs", o));
  EXPECT_TRUE(o.picture);
  EXPECT_EQ(o.h, 1);
  EXPECT_EQ(o.w, 2);
  EXPECT_EQ(o.rgb[5], 6);
  ASSERT_TRUE(read_obs(f, "numbers", o));
  EXPECT_FALSE(o.picture);
  EXPECT_EQ(o.numbers, nums);
  EXPECT_FALSE(read_obs(f, "final_obs", o));   // null: no observation
  EXPECT_FALSE(read_obs(f, "missing", o));
  EXPECT_THROW(read_obs(f, "bad", o), vyrn::ipc::ProtocolError);   // a gray [H, W] array is not a picture here
}

TEST(Observation, ChannelsFirstToRgb) {
  // 2 x 2 frame, planes R, G, B.
  const std::vector<uint8_t> chw = {10, 11, 12, 13, 20, 21, 22, 23, 30, 31, 32, 33};
  const vyrn::Picture p = picture_from_chw(chw.data(), 2, 2);
  EXPECT_EQ(p.w, 2);
  EXPECT_EQ(p.h, 2);
  EXPECT_EQ(p.rgb, (std::vector<uint8_t>{10, 20, 30, 11, 21, 31, 12, 22, 32, 13, 23, 33}));
}

TEST(Observation, ShowValuesAccumulate) {
  ShowValues show;
  Frame first;
  const std::vector<uint8_t> px(2 * 3 * 3, 200);
  first.j = {{"show", {{"speed", 3.5}, {"label", "jumping"}, {"alive", true}}}};
  first.j["show"]["view"] = vyrn::ipc::append_array(first.bin, "u8", {2, 3, 3}, px.data());
  show.update(first);
  Frame second;   // only what changed comes again
  second.j = {{"show", {{"speed", 4.0}}}};
  show.update(second);

  vyrn::LiveView v;
  show.fill(v);
  std::map<std::string, std::string> text(v.show_text.begin(), v.show_text.end());
  EXPECT_EQ(text.size(), 3u);
  EXPECT_EQ(text["speed"], "4");
  EXPECT_EQ(text["label"], "jumping");
  EXPECT_EQ(text["alive"], "True");
  ASSERT_EQ(v.show_pictures.size(), 1u);
  EXPECT_EQ(v.show_pictures[0].first, "view");
  EXPECT_EQ(v.show_pictures[0].second.h, 2);
  EXPECT_EQ(v.show_pictures[0].second.w, 3);
}

TEST(Observation, ShortNumbers) {
  EXPECT_EQ(short_number(3.5), "3.5");
  EXPECT_EQ(short_number(1240), "1240");
  EXPECT_EQ(short_number(-0.0), "0");
  EXPECT_EQ(short_number(0.123456), "0.1235");
  EXPECT_EQ(short_number(1.5e9), "1.5e+09");
}
