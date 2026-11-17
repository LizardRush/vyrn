// Spec parsing and compatibility, picture resizing, frame stacks and the number normalizer.
#include "brain/preprocess.h"
#include "brain/spec.h"

#include <gtest/gtest.h>

#include <cmath>
#include <numeric>
#include <random>

using namespace vyrn::brain;

namespace {

json picture_spec(int h, int w, std::vector<std::string> choices = {"left", "right", "stay"}) {
  return json{{"t", "spec"},
              {"id", 1},
              {"mode", "sim"},
              {"obs", {{"kind", "picture"}, {"shape", {h, w, 3}}, {"names", nullptr}}},
              {"choices", choices},
              {"labels", choices},
              {"effects", std::vector<std::string>(choices.size(), "do nothing")},
              {"uses", {{"screen", false}, {"keys", false}}}};
}

json numbers_spec(int d, json names = nullptr, std::vector<std::string> choices = {"left", "right", "stay"}) {
  return json{{"t", "spec"},
              {"id", 1},
              {"mode", "realtime"},
              {"obs", {{"kind", "numbers"}, {"shape", {d}}, {"names", names}}},
              {"choices", choices},
              {"uses", {{"screen", true}, {"keys", true}}}};
}

}  // namespace

// ---- spec ------------------------------------------------------------------------------------------

TEST(Spec, ParsesPictureAndNumbers) {
  Spec p = parse_spec(picture_spec(10, 12));
  EXPECT_EQ(p.mode, "sim");
  EXPECT_EQ(p.obs.kind, ObsSpec::Kind::Picture);
  EXPECT_EQ(p.obs.h, 10);
  EXPECT_EQ(p.obs.w, 12);
  EXPECT_EQ(p.choices, (std::vector<std::string>{"left", "right", "stay"}));
  EXPECT_FALSE(p.uses_keys);

  Spec n = parse_spec(numbers_spec(3, {"fruit x", "fruit y", "basket"}, {"go_left", "go_right"}));
  EXPECT_EQ(n.mode, "realtime");
  EXPECT_EQ(n.obs.kind, ObsSpec::Kind::Numbers);
  EXPECT_EQ(n.obs.d, 3);
  EXPECT_EQ(n.obs.names, (std::vector<std::string>{"fruit x", "fruit y", "basket"}));
  EXPECT_EQ(n.labels, (std::vector<std::string>{"go left", "go right"}));   // made up when missing
  EXPECT_TRUE(n.uses_keys);
  EXPECT_TRUE(n.uses_screen);
  EXPECT_EQ(n.raw["id"], 1);
}

TEST(Spec, RejectsBrokenMessages) {
  auto bad = [](json m) { EXPECT_THROW(parse_spec(m), SpecError) << m.dump(); };
  json m = picture_spec(10, 10);
  m["obs"]["shape"] = {10, 10};
  bad(m);
  m = picture_spec(10, 10);
  m["obs"]["kind"] = "sound";
  bad(m);
  bad(picture_spec(10, 10, {"only_one"}));
  bad(picture_spec(10, 10, {"a", "a"}));
  m = numbers_spec(2, {"x"});
  bad(m);
  m = numbers_spec(0);
  bad(m);
  m = picture_spec(10, 10);
  m["mode"] = "turbo";
  bad(m);
}

TEST(Spec, BrainShapeAndJsonRoundTrip) {
  BrainShape pic = make_brain_shape(parse_spec(picture_spec(10, 12)), 4);
  EXPECT_EQ(pic.in_c, 12);
  EXPECT_EQ(pic.in_h, 10);
  EXPECT_EQ(pic.in_w, 12);
  EXPECT_TRUE(pic.small_picture);
  BrainShape big = make_brain_shape(parse_spec(picture_spec(480, 640)), 4);
  EXPECT_FALSE(big.small_picture);
  EXPECT_EQ(big.in_h, 84);
  EXPECT_EQ(big.in_w, 84);
  BrainShape num = make_brain_shape(parse_spec(numbers_spec(3, {"a", "b", "c"})), 4);
  EXPECT_EQ(num.frames, 1);   // numbers are never stacked
  EXPECT_EQ(num.d, 3);
  EXPECT_EQ(num.inputs, (std::vector<std::string>{"a", "b", "c"}));

  for (const BrainShape& s : {pic, big, num}) {
    json j = s;
    BrainShape back = j.get<BrainShape>();
    EXPECT_EQ(json(back), j);
  }
  EXPECT_THROW(json({{"kind", "picture"}}).get<BrainShape>(), SpecError);
}

TEST(Spec, CompareChoicesAndInputs) {
  const BrainShape saved = make_brain_shape(parse_spec(picture_spec(10, 10)), 4);
  std::string why;
  EXPECT_EQ(compare(saved, parse_spec(picture_spec(10, 10)), &why), Compat::Same);
  EXPECT_TRUE(why.empty());

  EXPECT_EQ(compare(saved, parse_spec(picture_spec(10, 10, {"left", "right", "stay", "jump"})), &why),
            Compat::AppendedChoices);
  EXPECT_NE(why.find("jump"), std::string::npos);

  EXPECT_EQ(compare(saved, parse_spec(picture_spec(10, 10, {"right", "left", "stay"})), &why), Compat::Incompatible);
  EXPECT_NE(why.find("reordered"), std::string::npos) << why;
  EXPECT_NE(why.find("starts a new brain"), std::string::npos);
  EXPECT_EQ(compare(saved, parse_spec(picture_spec(10, 10, {"left", "right"})), &why), Compat::Incompatible);
  EXPECT_NE(why.find("stay"), std::string::npos) << why;
  EXPECT_EQ(compare(saved, parse_spec(picture_spec(10, 10, {"left", "right", "wait"})), &why), Compat::Incompatible);
  EXPECT_EQ(compare(saved, parse_spec(picture_spec(10, 10, {"left", "jump", "right", "stay"})), &why),
            Compat::Incompatible);

  // Small pictures of another size are resized (with a note); small <-> big needs a new brain.
  EXPECT_EQ(compare(saved, parse_spec(picture_spec(12, 12)), &why), Compat::Same);
  EXPECT_NE(why.find("resizes"), std::string::npos) << why;
  EXPECT_EQ(compare(saved, parse_spec(picture_spec(33, 33)), &why), Compat::Incompatible);
  const BrainShape big = make_brain_shape(parse_spec(picture_spec(480, 640)), 4);
  EXPECT_EQ(compare(big, parse_spec(picture_spec(1080, 1920)), &why), Compat::Same);
  EXPECT_TRUE(why.empty());

  // Kind, count and names of numbers.
  EXPECT_EQ(compare(saved, parse_spec(numbers_spec(3)), &why), Compat::Incompatible);
  const BrainShape nums = make_brain_shape(parse_spec(numbers_spec(2, {"x", "y"})), 1);
  EXPECT_EQ(compare(nums, parse_spec(numbers_spec(2, {"x", "y"})), nullptr), Compat::Same);
  EXPECT_EQ(compare(nums, parse_spec(numbers_spec(3, {"x", "y", "z"})), &why), Compat::Incompatible);
  EXPECT_EQ(compare(nums, parse_spec(numbers_spec(2, {"y", "x"})), &why), Compat::Incompatible);
  EXPECT_NE(why.find("names"), std::string::npos) << why;
}

// ---- geometry and resizing ---------------------------------------------------------------------------

TEST(Preprocess, ChooseGeometry) {
  struct Case { int h, w; bool small; int gh, gw; };
  for (const Case& c : {Case{1, 1, true, 1, 1}, Case{1, 10, true, 1, 10}, Case{4, 4, true, 4, 4},
                        Case{10, 10, true, 10, 10}, Case{32, 32, true, 32, 32}, Case{33, 33, false, 84, 84},
                        Case{480, 640, false, 84, 84}, Case{1080, 1920, false, 84, 84}, Case{1, 100, false, 84, 84}}) {
    const Geometry g = choose_geometry(c.h, c.w);
    EXPECT_EQ(g.small, c.small) << c.h << "x" << c.w;
    EXPECT_EQ(g.h, c.gh);
    EXPECT_EQ(g.w, c.gw);
  }
  EXPECT_THROW(choose_geometry(0, 5), std::invalid_argument);
}

namespace {

// An RGB picture whose pixel (y, x) is (f(y,x), f(y,x)+1, f(y,x)+2).
template <typename F>
std::vector<uint8_t> make_picture(int h, int w, F f) {
  std::vector<uint8_t> rgb(static_cast<size_t>(h) * w * 3);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      for (int c = 0; c < 3; ++c) rgb[(static_cast<size_t>(y) * w + x) * 3 + c] = static_cast<uint8_t>(f(y, x) + c);
  return rgb;
}

uint8_t at(const std::vector<uint8_t>& chw, const Geometry& g, int c, int y, int x) {
  return chw[(static_cast<size_t>(c) * g.h + y) * g.w + x];
}

}  // namespace

TEST(Preprocess, NativeSmallPictureIsReorderedToChw) {
  const auto rgb = make_picture(2, 3, [](int y, int x) { return 10 * y + 3 * x; });
  const Geometry g = choose_geometry(2, 3);
  std::vector<uint8_t> out(3 * 2 * 3);
  to_frame(rgb.data(), 2, 3, g, out.data());
  for (int c = 0; c < 3; ++c)
    for (int y = 0; y < 2; ++y)
      for (int x = 0; x < 3; ++x) EXPECT_EQ(at(out, g, c, y, x), 10 * y + 3 * x + c);
}

TEST(Preprocess, SmallPictureOfAnotherSizeIsNearestResized) {
  // 4x4 -> the saved 2x2: centre-aligned nearest picks rows/cols 1 and 3.
  const auto rgb = make_picture(4, 4, [](int y, int x) { return 10 * y + x; });
  const Geometry g{true, 2, 2};
  std::vector<uint8_t> out(3 * 4);
  to_frame(rgb.data(), 4, 4, g, out.data());
  EXPECT_EQ(at(out, g, 0, 0, 0), 11);
  EXPECT_EQ(at(out, g, 0, 0, 1), 13);
  EXPECT_EQ(at(out, g, 0, 1, 0), 31);
  EXPECT_EQ(at(out, g, 2, 1, 1), 35);
  // 2x2 -> 4x4 repeats each pixel twice.
  const auto small = make_picture(2, 2, [](int y, int x) { return 100 * y + 10 * x; });
  const Geometry g4{true, 4, 4};
  std::vector<uint8_t> big(3 * 16);
  to_frame(small.data(), 2, 2, g4, big.data());
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) EXPECT_EQ(at(big, g4, 1, y, x), 100 * (y / 2) + 10 * (x / 2) + 1);
}

TEST(Preprocess, AreaResizeAveragesBlocks) {
  // 168x168 -> 84x84: every output pixel is the mean of a 2x2 block {0, 10, 20, 30} -> 15.
  const auto rgb = make_picture(168, 168, [](int y, int x) { return 20 * (y % 2) + 10 * (x % 2); });
  const Geometry g = choose_geometry(168, 168);
  std::vector<uint8_t> out(3 * 84 * 84);
  to_frame(rgb.data(), 168, 168, g, out.data());
  for (int y = 0; y < 84; ++y)
    for (int x = 0; x < 84; ++x) {
      ASSERT_EQ(at(out, g, 0, y, x), 15);
      ASSERT_EQ(at(out, g, 2, y, x), 17);
    }
  // A 1.5 ratio (126 wide): output 0 covers pixel 0 fully and half of pixel 1: (0*1 + 30*0.5) / 1.5 = 10.
  const auto stripes = make_picture(84, 126, [](int, int x) { return x % 2 ? 30 : 0; });
  to_frame(stripes.data(), 84, 126, g, out.data());
  EXPECT_EQ(at(out, g, 0, 0, 0), 10);
  EXPECT_EQ(at(out, g, 0, 5, 1), 10);   // [1.5, 3): half of pixel 1 (30) and pixel 2 (0) -> 15 / 1.5
  EXPECT_EQ(at(out, g, 0, 5, 2), 20);   // [3, 4.5): pixel 3 (30) and half of pixel 4 (0) -> 30 / 1.5
  EXPECT_EQ(at(out, g, 1, 5, 3), 21);   // [4.5, 6): half of pixel 4 and pixel 5, green = red + 1
}

TEST(Preprocess, BilinearUpscaleAndConstantPictures) {
  // 42 wide -> 84: output x maps to input (x + 0.5) / 2 - 0.5. Columns alternate 0, 100.
  const auto rgb = make_picture(42, 42, [](int, int x) { return x % 2 ? 100 : 0; });
  const Geometry g = choose_geometry(42, 42);
  std::vector<uint8_t> out(3 * 84 * 84);
  to_frame(rgb.data(), 42, 42, g, out.data());
  EXPECT_EQ(at(out, g, 0, 7, 0), 0);     // -0.25 clamps to pixel 0
  EXPECT_EQ(at(out, g, 0, 7, 1), 25);    // 0.25: 0.75 * 0 + 0.25 * 100
  EXPECT_EQ(at(out, g, 0, 7, 2), 75);    // 0.75: 0.25 * 0 + 0.75 * 100
  EXPECT_EQ(at(out, g, 0, 7, 3), 75);    // 1.25: 0.75 * 100 + 0.25 * 0
  EXPECT_EQ(at(out, g, 0, 7, 4), 25);    // 1.75: 0.25 * 100 + 0.75 * 0
  EXPECT_EQ(at(out, g, 0, 7, 83), 100);  // 41.25 clamps to pixel 41
  // Mixed axes (1 x 100): rows grow 1 -> 84 (copies), columns shrink 100 -> 84 (area).
  const auto line = make_picture(1, 100, [](int, int x) { return x < 49 ? 0 : 84; });
  to_frame(line.data(), 1, 100, choose_geometry(1, 100), out.data());
  for (int y = 0; y < 84; ++y) {
    ASSERT_EQ(at(out, g, 0, y, 40), 0);    // [47.62, 48.81): pixels 47 and 48
    ASSERT_EQ(at(out, g, 0, y, 41), 71);   // [48.81, 50): 0.19 of pixel 48 (0) + pixel 49 (84) -> 84 / 1.19
    ASSERT_EQ(at(out, g, 0, y, 42), 84);
  }
}

TEST(Preprocess, ConstantPicturesStayConstantAtEverySize) {
  for (auto [h, w] : std::vector<std::pair<int, int>>{{1, 1}, {1, 10}, {4, 4}, {10, 10}, {32, 32}, {33, 33},
                                                       {1, 100}, {480, 640}, {1080, 1920}, {85, 300}}) {
    const auto rgb = make_picture(h, w, [](int, int) { return 77; });
    const Geometry g = choose_geometry(h, w);
    std::vector<uint8_t> out(static_cast<size_t>(3) * g.h * g.w);
    to_frame(rgb.data(), h, w, g, out.data());
    for (int c = 0; c < 3; ++c)
      for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x) ASSERT_EQ(at(out, g, c, y, x), 77 + c) << h << "x" << w;
  }
}

// ---- frame stack -------------------------------------------------------------------------------------

TEST(FrameStack, RepeatsTheFirstFrameOfASegment) {
  FrameStack s(4, 2);
  const uint8_t A[2] = {'A', 'a'}, B[2] = {'B', 'b'}, C[2] = {'C', 'c'}, D[2] = {'D', 'd'};
  auto text = [&] {
    std::string t;
    for (size_t i = 0; i < s.stacked().size(); i += 2) t += static_cast<char>(s.stacked()[i]);
    return t;
  };
  s.reset(A);
  EXPECT_EQ(text(), "AAAA");
  s.push(B);
  EXPECT_EQ(text(), "AAAB");
  s.push(C);
  EXPECT_EQ(text(), "AABC");
  EXPECT_EQ(s.newest()[1], 'c');
  s.reset(D);
  EXPECT_EQ(text(), "DDDD");
  EXPECT_EQ(s.stacked().size(), 8u);
}

// ---- normalizer ------------------------------------------------------------------------------------------

TEST(RunningMeanStd, MatchesClosedFormValues) {
  std::mt19937 rng(5);
  std::normal_distribution<float> noise(3.0f, 2.0f);
  const int d = 3;
  std::vector<float> all;
  RunningMeanStd rms(d);
  for (int batch : {1, 7, 64, 2, 300}) {
    std::vector<float> rows(static_cast<size_t>(batch) * d);
    for (size_t i = 0; i < rows.size(); ++i) rows[i] = noise(rng) * (1 + static_cast<float>(i % d)) + 100.0f * (i % d);
    rms.update(rows.data(), batch);
    all.insert(all.end(), rows.begin(), rows.end());
  }
  const size_t n = all.size() / d;
  ASSERT_DOUBLE_EQ(rms.count(), static_cast<double>(n));
  for (int i = 0; i < d; ++i) {
    double mean = 0;
    for (size_t r = 0; r < n; ++r) mean += all[r * d + i];
    mean /= static_cast<double>(n);
    double var = 0;
    for (size_t r = 0; r < n; ++r) var += (all[r * d + i] - mean) * (all[r * d + i] - mean);
    var /= static_cast<double>(n);   // population variance, like numpy.var
    EXPECT_NEAR(rms.mean()[i], mean, 1e-6 * std::max(1.0, std::fabs(mean)));
    EXPECT_NEAR(rms.var()[i], var, 1e-6 * std::max(1.0, var));
  }
  // normalize = clip((x - mean) / sqrt(var + 1e-8), +-10)
  const float x[3] = {3.0f, 100.0f, 1e6f};
  float out[3];
  rms.normalize(x, out);
  for (int i = 0; i < 2; ++i) {
    EXPECT_NEAR(out[i], (x[i] - rms.mean()[i]) / std::sqrt(rms.var()[i] + 1e-8), 1e-5);
  }
  EXPECT_FLOAT_EQ(out[2], 10.0f);
  float rows_out[6];
  const float rows_in[6] = {3.0f, 100.0f, 1e6f, 3.0f, 100.0f, 1e6f};
  rms.normalize_rows(rows_in, rows_out, 2);
  for (int i = 0; i < 3; ++i) {
    EXPECT_FLOAT_EQ(rows_out[i], out[i]);
    EXPECT_FLOAT_EQ(rows_out[3 + i], out[i]);
  }

  // Hand-computed: {1, 2, 3, 4} -> mean 2.5, var 1.25 (in two updates).
  RunningMeanStd small(1);
  const float a[2] = {1, 2}, b[2] = {3, 4};
  small.update(a, 2);
  small.update(b, 2);
  EXPECT_NEAR(small.mean()[0], 2.5, 1e-12);
  EXPECT_NEAR(small.var()[0], 1.25, 1e-12);
}

TEST(RunningMeanStd, FrozenAndJsonRoundTrip) {
  RunningMeanStd rms(2);
  const float rows[4] = {1, 10, 3, 30};
  rms.update(rows, 2);
  const json saved = rms.to_json();
  rms.frozen = true;
  const float more[2] = {1000, 1000};
  rms.update(more, 1);
  EXPECT_DOUBLE_EQ(rms.mean()[0], 2.0);   // unchanged while frozen

  RunningMeanStd back(2);
  back.from_json(json::parse(saved.dump()));
  EXPECT_DOUBLE_EQ(back.count(), 2.0);
  EXPECT_DOUBLE_EQ(back.mean()[1], 20.0);
  EXPECT_DOUBLE_EQ(back.var()[1], 100.0);
  RunningMeanStd wrong(3);
  EXPECT_THROW(wrong.from_json(saved), std::invalid_argument);
}
