// model.safetensors (docs/dev/package.md section 3): the writer and every refusal of the reader.
#include "brain/spec.h"
#include "helpers.h"
#include "package/internal.h"
#include "package/safetensors.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>

using namespace pkg_test;
namespace pd = vyrn::package_detail;
using json = nlohmann::json;

namespace {

std::string floats(std::initializer_list<float> values) {
  std::string out;
  for (const float v : values) out.append(reinterpret_cast<const char*>(&v), 4);
  return out;
}

// A file with a hand-written header (padded to 8) and data.
std::string file_with(const json& header, const std::string& data) {
  std::string text = header.dump();
  while (text.size() % 8) text += ' ';
  std::string out;
  const uint64_t n = text.size();
  for (int i = 0; i < 8; ++i) out += static_cast<char>((n >> (8 * i)) & 0xFF);
  return out + text + data;
}

json meta() { return {{"format", "vyrn-qnet"}, {"net", "mlp"}}; }

json tensor(std::vector<int64_t> shape, uint64_t begin, uint64_t end, const char* dtype = "F32") {
  return {{"dtype", dtype}, {"shape", shape}, {"data_offsets", {begin, end}}};
}

std::string why_refused(const std::string& file, const std::string& net = "mlp") {
  try {
    pd::decode_safetensors(file, net);
  } catch (const pd::WeightsError& e) {
    return e.what();
  }
  return "";
}

}  // namespace

TEST(PackageSafetensors, RoundTripInNameOrder) {
  const std::string a = floats({1, 2, 3, 4}), b = floats({-0.5f}), c = floats({7, 8});
  const std::string file = pd::encode_safetensors({{"head.weight", {2, 2}, a}, {"fc1.bias", {1}, b}, {"head.bias", {2}, c}}, "mlp");
  uint64_t header_len = 0;
  std::memcpy(&header_len, file.data(), 8);
  EXPECT_EQ(header_len % 8, 0u);
  const json header = json::parse(file.substr(8, header_len));
  EXPECT_EQ(header["__metadata__"]["format"], "vyrn-qnet");
  EXPECT_EQ(header["fc1.bias"]["data_offsets"], json({0, 4}));   // key order: fc1.bias, head.bias, head.weight
  EXPECT_EQ(header["head.bias"]["data_offsets"], json({4, 12}));
  EXPECT_EQ(header["head.weight"]["data_offsets"], json({12, 28}));
  const auto views = pd::decode_safetensors(file, "mlp");
  ASSERT_EQ(views.size(), 3u);
  EXPECT_EQ(views[2].name, "head.weight");
  EXPECT_EQ(views[2].shape, (std::vector<int64_t>{2, 2}));
  EXPECT_EQ(std::string(views[2].data), a);
}

TEST(PackageSafetensors, RefusesBadFiles) {
  const std::string d4 = floats({1, 2, 3, 4});
  json ok = {{"__metadata__", meta()}, {"a.weight", tensor({4}, 0, 16)}};
  EXPECT_EQ(why_refused(file_with(ok, d4)), "");

  EXPECT_NE(why_refused("short"), "");
  std::string huge_header(8, '\0');
  huge_header[3] = 1;   // 16 MiB
  EXPECT_NE(why_refused(huge_header + "{}"), "");
  std::string past_end(8, '\0');
  past_end[0] = 100;
  EXPECT_NE(why_refused(past_end + "{}"), "");
  EXPECT_NE(why_refused(file_with(json::array(), "")), "");

  std::string text = "{\"__metadata__\":{\"format\":\"vyrn-qnet\",\"net\":\"mlp\"},\"a\":{\"dtype\":\"F32\",\"shape\":[4],"
                     "\"data_offsets\":[0,16]},\"a\":{\"dtype\":\"F32\",\"shape\":[4],\"data_offsets\":[0,16]}}";
  while (text.size() % 8) text += ' ';
  std::string dup(8, '\0');
  dup[0] = static_cast<char>(text.size());
  EXPECT_NE(why_refused(dup + text + d4).find("repeats"), std::string::npos);

  json no_meta = {{"a.weight", tensor({4}, 0, 16)}};
  EXPECT_NE(why_refused(file_with(no_meta, d4)), "");
  json other_net = ok;
  other_net["__metadata__"]["net"] = "der";
  EXPECT_NE(why_refused(file_with(other_net, d4)), "");
  json other_format = ok;
  other_format["__metadata__"]["format"] = "pickle";
  EXPECT_NE(why_refused(file_with(other_format, d4)), "");

  const auto refused = [&](json h, const std::string& data) { return why_refused(file_with(h, data)) != ""; };
  json f16 = ok;
  f16["a.weight"] = tensor({4}, 0, 16, "F16");
  EXPECT_TRUE(refused(f16, d4));
  json rank0 = ok;
  rank0["a.weight"] = tensor({}, 0, 16);
  EXPECT_TRUE(refused(rank0, d4));
  json rank5 = ok;
  rank5["a.weight"] = tensor({1, 1, 1, 1, 4}, 0, 16);
  EXPECT_TRUE(refused(rank5, d4));
  json zero_dim = ok;
  zero_dim["a.weight"] = tensor({0, 4}, 0, 0);
  EXPECT_TRUE(refused(zero_dim, ""));
  json giant = ok;
  giant["a.weight"] = tensor({16777217}, 0, 16);
  EXPECT_TRUE(refused(giant, d4));
  json overflow = ok;
  overflow["a.weight"] = tensor({16777216, 16777216, 16777216, 16777216}, 0, 16);
  EXPECT_TRUE(refused(overflow, d4));
  json wrong_size = ok;
  wrong_size["a.weight"] = tensor({3}, 0, 16);
  EXPECT_TRUE(refused(wrong_size, d4));
  json gap = ok;
  gap["a.weight"] = tensor({3}, 4, 16);
  EXPECT_TRUE(refused(gap, d4));
  json overlap = {{"__metadata__", meta()}, {"a", tensor({4}, 0, 16)}, {"b", tensor({4}, 8, 24)}};
  EXPECT_TRUE(refused(overlap, d4 + floats({5, 6})));
  json out_of_order = {{"__metadata__", meta()}, {"a", tensor({2}, 8, 16)}, {"b", tensor({2}, 0, 8)}};
  EXPECT_TRUE(refused(out_of_order, d4));
  EXPECT_TRUE(refused(ok, d4 + floats({9})));   // bytes after the last tensor
  json negative = ok;
  negative["a.weight"]["data_offsets"] = {-16, 0};
  EXPECT_TRUE(refused(negative, d4));
  json extra_key = ok;
  extra_key["a.weight"]["note"] = "hi";
  EXPECT_TRUE(refused(extra_key, d4));
  for (const char* bad_name : {"a/b", "a b", "", "x-y"}) {
    json named = {{"__metadata__", meta()}, {bad_name, tensor({4}, 0, 16)}};
    EXPECT_TRUE(refused(named, d4)) << bad_name;
  }
  json long_name = {{"__metadata__", meta()}, {std::string(129, 'a'), tensor({4}, 0, 16)}};
  EXPECT_TRUE(refused(long_name, d4));
  json many = {{"__metadata__", meta()}};
  std::string many_data;
  for (int i = 0; i < 65; ++i) {
    char name[16];
    std::snprintf(name, sizeof name, "t%02d", i);
    many[name] = tensor({1}, static_cast<uint64_t>(4 * i), static_cast<uint64_t>(4 * i + 4));
    many_data += floats({1});
  }
  EXPECT_TRUE(refused(many, many_data));
  for (const float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity()}) {
    EXPECT_NE(why_refused(file_with(ok, floats({1, bad, 3, 4}))).find("finite"), std::string::npos);
  }
}

TEST(PackageSafetensors, TensorsMustMatchTheNetwork) {
  const vyrn::brain::BrainShape shape = json::parse(shape_json(Net::MLP)).get<vyrn::brain::BrainShape>();
  // The MLP for 3 numbers and 3 choices: fc1 [128,3], fc2 [128,128], head [3,128] and their biases.
  std::vector<std::pair<std::string, std::vector<int64_t>>> expected = {
      {"fc1.weight", {128, 3}}, {"fc1.bias", {128}}, {"fc2.weight", {128, 128}},
      {"fc2.bias", {128}},      {"head.weight", {3, 128}}, {"head.bias", {3}}};
  std::vector<std::string> storage;
  for (const auto& [name, dims] : expected) {
    int64_t n = 1;
    for (const int64_t d : dims) n *= d;
    storage.push_back(std::string(static_cast<size_t>(n) * 4, '\0'));
  }
  const auto views_with = [&](const std::function<void(std::vector<pd::TensorView>&)>& change) {
    std::vector<pd::TensorView> views;
    for (size_t i = 0; i < expected.size(); ++i) views.push_back({expected[i].first, expected[i].second, storage[i]});
    change(views);
    return views;
  };
  int64_t parameters = 0;
  EXPECT_NO_THROW(pd::check_weights_fit(shape, views_with([](auto&) {}), &parameters));
  EXPECT_EQ(parameters, 128 * 3 + 128 + 128 * 128 + 128 + 3 * 128 + 3);
  EXPECT_THROW(pd::check_weights_fit(shape, views_with([](auto& v) { v.pop_back(); }), nullptr), pd::WeightsError);
  EXPECT_THROW(pd::check_weights_fit(shape, views_with([](auto& v) { v[0].shape = {3, 128}; }), nullptr),
               pd::WeightsError);
  EXPECT_THROW(pd::check_weights_fit(shape, views_with([](auto& v) { v[1].name = "fc9.bias"; }), nullptr),
               pd::WeightsError);
  EXPECT_THROW(pd::check_weights_fit(shape, views_with([&](auto& v) { v.push_back({"extra", {1}, storage[5]}); }),
                                     nullptr),
               pd::WeightsError);
}
