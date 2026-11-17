// Frame codec tests (protocol.md section 2): golden bytes, incremental decoding, broken frames.
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "ipc/proto.h"

namespace {

using vyrn::ipc::append_array;
using vyrn::ipc::ArrayView;
using vyrn::ipc::Decoder;
using vyrn::ipc::encode;
using vyrn::ipc::encode_text;
using vyrn::ipc::Frame;
using vyrn::ipc::json;
using vyrn::ipc::ProtocolError;

// The golden message, serialized the way protocol.md section 2 says (compact, keys in this order).
const std::string kGoldenJson =
    R"({"t":"result","id":7,"obs":{"$a":[0,"u8",[2,2,3]]},"points":1.5,"game_over":false})";

std::vector<uint8_t> bytes_0_to_11() {
  std::vector<uint8_t> blob(12);
  for (size_t i = 0; i < blob.size(); ++i) blob[i] = static_cast<uint8_t>(i);
  return blob;
}

void put_u32le(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

// A frame put together by hand: magic, json_len and bin_len (u32 LE), JSON, blob.
std::vector<uint8_t> raw_frame(const std::string& json_text, const std::vector<uint8_t>& blob,
                               const char* magic = "VYRN") {
  std::vector<uint8_t> frame(magic, magic + 4);
  put_u32le(frame, static_cast<uint32_t>(json_text.size()));
  put_u32le(frame, static_cast<uint32_t>(blob.size()));
  frame.insert(frame.end(), json_text.begin(), json_text.end());
  frame.insert(frame.end(), blob.begin(), blob.end());
  return frame;
}

std::vector<uint8_t> header_only(uint32_t json_len, uint32_t bin_len) {
  std::vector<uint8_t> header = {'V', 'Y', 'R', 'N'};
  put_u32le(header, json_len);
  put_u32le(header, bin_len);
  return header;
}

// tests/golden/frame_result.bin; built from protocol.md section 2 when the file is missing.
std::vector<uint8_t> golden_frame(bool* from_file = nullptr) {
  std::ifstream file(std::string(VYRN_SOURCE_DIR) + "/tests/golden/frame_result.bin", std::ios::binary);
  if (from_file) *from_file = static_cast<bool>(file);
  if (!file) return raw_frame(kGoldenJson, bytes_0_to_11());
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

Frame decode_one(const std::vector<uint8_t>& bytes) {
  Decoder decoder;
  decoder.feed(bytes.data(), bytes.size());
  std::optional<Frame> frame = decoder.next();
  EXPECT_TRUE(frame.has_value());
  EXPECT_EQ(decoder.buffered(), 0u);
  return frame ? std::move(*frame) : Frame{};
}

void expect_broken(const std::vector<uint8_t>& bytes, const std::string& what) {
  Decoder decoder;
  decoder.feed(bytes.data(), bytes.size());
  EXPECT_THROW(decoder.next(), ProtocolError) << what;
}

void expect_broken(const std::string& json_text, const std::vector<uint8_t>& blob, const std::string& what) {
  expect_broken(raw_frame(json_text, blob), what + ": " + json_text);
}

// ---- golden vector ------------------------------------------------------------------------------

TEST(Golden, FileMatchesTheProtocol) {
  bool from_file = false;
  const std::vector<uint8_t> golden = golden_frame(&from_file);
  if (!from_file) GTEST_SKIP() << "tests/golden/frame_result.bin is missing (the other golden tests use hand-built bytes)";
  EXPECT_EQ(golden, raw_frame(kGoldenJson, bytes_0_to_11()));
  EXPECT_EQ(golden.size(), 12u + kGoldenJson.size() + 12u);
}

TEST(Golden, EncodeProducesTheGoldenBytes) {
  const std::vector<uint8_t> golden = golden_frame();
  // nlohmann::json sorts keys, so the byte-exact frame is built from an ordered_json in the
  // protocol's key order; append_array makes the blob and the reference.
  std::vector<uint8_t> bin;
  const std::vector<uint8_t> pixels = bytes_0_to_11();
  nlohmann::ordered_json message;
  message["t"] = "result";
  message["id"] = 7;
  message["obs"] = append_array(bin, "u8", {2, 2, 3}, pixels.data());
  message["points"] = 1.5;
  message["game_over"] = false;
  EXPECT_EQ(message.dump(), kGoldenJson);
  EXPECT_EQ(bin, pixels);
  EXPECT_EQ(encode_text(message.dump(), bin), golden);
}

TEST(Golden, EncodeOfSortedJsonDecodesToTheSameMessage) {
  const Frame golden = decode_one(golden_frame());
  const json message = {{"t", "result"}, {"id", 7}, {"obs", {{"$a", {0, "u8", {2, 2, 3}}}}}, {"points", 1.5},
                        {"game_over", false}};
  const std::vector<uint8_t> bytes = encode(message, bytes_0_to_11());
  EXPECT_EQ(bytes.size(), golden_frame().size());   // same content, only the key order differs
  const Frame again = decode_one(bytes);
  EXPECT_EQ(again.j, golden.j);
  EXPECT_EQ(again.bin, golden.bin);
}

TEST(Golden, DecodesFieldsAndArray) {
  const Frame frame = decode_one(golden_frame());
  EXPECT_EQ(frame.j["t"], "result");
  EXPECT_EQ(frame.j["id"], 7);
  EXPECT_EQ(frame.j["points"], 1.5);
  EXPECT_EQ(frame.j["game_over"], false);
  EXPECT_EQ(frame.j["obs"], json::parse(R"({"$a":[0,"u8",[2,2,3]]})"));   // refs stay refs
  ASSERT_TRUE(frame.has_array("obs"));
  EXPECT_FALSE(frame.has_array("points"));
  EXPECT_FALSE(frame.has_array("missing"));
  const ArrayView obs = frame.array(frame.j["obs"]);
  EXPECT_EQ(obs.dtype, "u8");
  EXPECT_EQ(obs.shape, (std::vector<int64_t>{2, 2, 3}));
  ASSERT_EQ(obs.bytes, 12u);
  EXPECT_EQ(std::vector<uint8_t>(obs.data, obs.data + obs.bytes), bytes_0_to_11());
}

// ---- incremental decoding ---------------------------------------------------------------------

TEST(Decoder, FedByteByByte) {
  const std::vector<uint8_t> golden = golden_frame();
  Decoder decoder;
  for (size_t i = 0; i + 1 < golden.size(); ++i) {
    decoder.feed(&golden[i], 1);
    ASSERT_FALSE(decoder.next().has_value()) << "a frame came out after only " << i + 1 << " bytes";
  }
  decoder.feed(&golden.back(), 1);
  std::optional<Frame> frame = decoder.next();
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->j["id"], 7);
  EXPECT_EQ(frame->bin, bytes_0_to_11());
  EXPECT_FALSE(decoder.next().has_value());
  EXPECT_EQ(decoder.buffered(), 0u);
}

TEST(Decoder, SeveralFramesInRandomChunks) {
  std::vector<uint8_t> pictures;
  std::vector<uint8_t> pixels(84 * 84 * 3);
  for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<uint8_t>(i * 7);
  const std::vector<float> numbers = {1.5f, -2.0f, 1e-3f};
  json big = {{"t", "result"}, {"id", 8}};
  big["obs"] = append_array(pictures, "u8", {84, 84, 3}, pixels.data());
  big["show"] = {{"speed", 3.5}, {"view", append_array(pictures, "f32", {3}, numbers.data())}};

  std::vector<uint8_t> stream = golden_frame();
  const std::vector<uint8_t> second = encode(big, pictures);
  const std::vector<uint8_t> third = encode({{"t", "ok"}, {"id", 9}});
  stream.insert(stream.end(), second.begin(), second.end());
  stream.insert(stream.end(), third.begin(), third.end());

  std::mt19937 rng(12345);
  for (int round = 0; round < 20; ++round) {
    Decoder decoder;
    std::vector<Frame> frames;
    size_t pos = 0;
    while (pos < stream.size()) {
      const size_t n = std::min<size_t>(stream.size() - pos, 1 + rng() % 9000);
      decoder.feed(stream.data() + pos, n);
      pos += n;
      while (std::optional<Frame> frame = decoder.next()) frames.push_back(std::move(*frame));
    }
    ASSERT_EQ(frames.size(), 3u);
    EXPECT_EQ(frames[0].j["id"], 7);
    EXPECT_EQ(frames[1].j, big);
    EXPECT_EQ(frames[2].j["t"], "ok");
    const ArrayView obs = frames[1].array(frames[1].j["obs"]);
    ASSERT_EQ(obs.bytes, pixels.size());
    EXPECT_EQ(std::memcmp(obs.data, pixels.data(), pixels.size()), 0);
    const ArrayView view = frames[1].array(frames[1].j["show"]["view"]);
    EXPECT_EQ(view.dtype, "f32");
    ASSERT_EQ(view.bytes, 12u);
    EXPECT_EQ(static_cast<size_t>(view.data - frames[1].bin.data()) % 64, 0u);
    float back[3];
    std::memcpy(back, view.data, sizeof back);
    EXPECT_EQ(std::vector<float>(back, back + 3), numbers);
    EXPECT_EQ(decoder.buffered(), 0u);
  }
}

// ---- broken frames ----------------------------------------------------------------------------

TEST(Decoder, BadMagic) {
  expect_broken(raw_frame(kGoldenJson, bytes_0_to_11(), "VYRX"), "wrong last magic byte");
  expect_broken(raw_frame(kGoldenJson, bytes_0_to_11(), "vyrn"), "lower-case magic");

  Decoder decoder;   // garbage is caught on its first byte, without waiting for a whole header
  const uint8_t junk = 'H';
  decoder.feed(&junk, 1);
  EXPECT_THROW(decoder.next(), ProtocolError);
  EXPECT_THROW(decoder.next(), ProtocolError);   // and the stream stays broken
}

TEST(Decoder, BadLengthsAreRejectedFromTheHeaderAlone) {
  expect_broken(header_only(0, 0), "empty JSON");
  expect_broken(header_only(vyrn::ipc::kMaxJsonBytes + 1, 0), "JSON over 1 MiB");
  expect_broken(header_only(10, vyrn::ipc::kMaxBinBytes + 1), "blob over 64 MiB");
  expect_broken(header_only(0xffffffffu, 0xffffffffu), "huge lengths");

  Decoder decoder;   // the largest allowed sizes just wait for more bytes
  const std::vector<uint8_t> header = header_only(vyrn::ipc::kMaxJsonBytes, vyrn::ipc::kMaxBinBytes);
  decoder.feed(header.data(), header.size());
  EXPECT_FALSE(decoder.next().has_value());
}

TEST(Decoder, BadJson) {
  const std::vector<uint8_t> none;
  expect_broken("{", none, "unfinished object");
  expect_broken(R"({"t":"ok",})", none, "trailing comma");
  expect_broken(R"({"t":"ok"} extra)", none, "text after the object");
  expect_broken(R"([1,2,3])", none, "not an object");
  expect_broken(R"("text")", none, "not an object");
  expect_broken(R"({"t":"ok","x":NaN})", none, "NaN is not JSON");
  expect_broken(R"({"t":"ok","x":1e999})", none, "a number that overflows to infinity");
  expect_broken("{\"t\":\"\xff\xfe\"}", none, "invalid UTF-8");
  std::string deep = R"({"t":"ok","x":)" + std::string(1000, '[') + std::string(1000, ']') + "}";
  expect_broken(deep, none, "nesting 1000 deep");
}

TEST(Decoder, BadArrayReferences) {
  const std::vector<uint8_t> twelve = bytes_0_to_11();
  auto with = [](const std::string& ref) { return R"({"t":"result","id":1,"obs":)" + ref + "}"; };
  expect_broken(with(R"({"$a":[64,"u8",[1]]})"), twelve, "offset past the blob");
  expect_broken(with(R"({"$a":[0,"u8",[4,4,3]]})"), twelve, "longer than the blob");
  expect_broken(with(R"({"$a":[0,"f32",[4]]})"), twelve, "f32 items are 4 bytes");
  expect_broken(with(R"({"$a":[3,"u8",[1]]})"), twelve, "offset not a multiple of 64");
  expect_broken(with(R"({"$a":[0,"i32",[1]]})"), twelve, "unknown dtype");
  expect_broken(with(R"({"$a":[0,"u8",[-1]]})"), twelve, "negative dimension");
  expect_broken(with(R"({"$a":[0,"u8",3]})"), twelve, "shape not a list");
  expect_broken(with(R"({"$a":[0,"u8",[true]]})"), twelve, "bool dimension");
  expect_broken(with(R"({"$a":[0.0,"u8",[1]]})"), twelve, "float offset");
  expect_broken(with(R"({"$a":[-64,"u8",[1]]})"), twelve, "negative offset");
  expect_broken(with(R"({"$a":[0,"u8"]})"), twelve, "two items instead of three");
  expect_broken(with(R"({"$a":[0,"u8",[1]],"x":1})"), twelve, "extra key next to $a");
  expect_broken(with(R"({"$a":"nope"})"), twelve, "not a list");
  expect_broken(with(R"({"$a":[0,"u8",[4611686018427387904,4611686018427387904]]})"), twelve,
                "dimensions whose product overflows");
  expect_broken(with(R"({"$a":[18446744073709551552,"u8",[1]]})"), twelve, "offset near 2^64");
  expect_broken(R"({"t":"result","id":1,"show":{"view":{"$a":[64,"u8",[10,10,3]]}}})", twelve, "nested deeper");
  expect_broken(R"({"t":"result","id":1,"list":[1,{"$a":[0,"u8",[13]]}]})", twelve, "inside a list");

  // Valid edge cases.
  const Frame empty = decode_one(raw_frame(with(R"({"$a":[0,"u8",[0,5]]})"), {}));
  EXPECT_EQ(empty.array(empty.j["obs"]).bytes, 0u);
  EXPECT_EQ(empty.array(empty.j["obs"]).shape, (std::vector<int64_t>{0, 5}));
  const Frame f32 = decode_one(raw_frame(with(R"({"$a":[0,"f32",[3]]})"), twelve));
  EXPECT_EQ(f32.array(f32.j["obs"]).bytes, 12u);
  std::vector<uint8_t> sixty_five(65, 9);
  const Frame last_byte = decode_one(raw_frame(with(R"({"$a":[64,"u8",[1]]})"), sixty_five));
  EXPECT_EQ(*last_byte.array(last_byte.j["obs"]).data, 9);
}

TEST(Frame, ArrayChecksReferencesItIsGiven) {
  Frame frame;
  frame.j = {{"t", "ok"}, {"id", 1}, {"a", {{"$a", {0, "u8", json::array({4})}}}}, {"b", 3}};
  frame.bin = {1, 2, 3};
  EXPECT_TRUE(frame.has_array("a"));
  EXPECT_FALSE(frame.has_array("b"));
  EXPECT_THROW(frame.array(frame.j["a"]), ProtocolError);   // 4 bytes, blob has 3
  EXPECT_THROW(frame.array(frame.j["b"]), ProtocolError);   // not a reference
  frame.bin.push_back(4);
  EXPECT_EQ(frame.array(frame.j["a"]).bytes, 4u);
}

// ---- encoding ---------------------------------------------------------------------------------

TEST(Encode, RejectsWhatTheReaderWouldReject) {
  EXPECT_THROW(encode(json::array({1, 2})), ProtocolError);
  EXPECT_THROW(encode(json("text")), ProtocolError);
  EXPECT_THROW(encode({{"t", "ok"}, {"x", std::nan("")}}), ProtocolError);
  EXPECT_THROW(encode({{"t", "ok"}, {"x", std::numeric_limits<double>::infinity()}}), ProtocolError);
  EXPECT_THROW(encode({{"t", "ok"}, {"x", "\xff"}}), ProtocolError);   // invalid UTF-8
  EXPECT_THROW(encode({{"t", "ok"}, {"obs", {{"$a", {0, "u8", json::array({13})}}}}}, bytes_0_to_11()), ProtocolError);

  // JSON size: up to exactly 1 MiB.
  const std::string skeleton = json{{"s", ""}, {"t", "ok"}}.dump();
  const size_t room = vyrn::ipc::kMaxJsonBytes - skeleton.size();
  EXPECT_NO_THROW(encode({{"t", "ok"}, {"s", std::string(room, 'x')}}));
  EXPECT_THROW(encode({{"t", "ok"}, {"s", std::string(room + 1, 'x')}}), ProtocolError);

  // Blob size: up to exactly 64 MiB.
  std::vector<uint8_t> blob(vyrn::ipc::kMaxBinBytes);
  EXPECT_EQ(encode({{"t", "ok"}}, blob).size(), 12u + 10u + blob.size());   // {"t":"ok"} is 10 bytes
  blob.push_back(0);
  EXPECT_THROW(encode({{"t", "ok"}}, blob), ProtocolError);
  EXPECT_THROW(encode_text("{}", blob), ProtocolError);
  EXPECT_THROW(encode_text("", {}), ProtocolError);
}

TEST(Encode, EncodeTextFramesTextAsIs) {
  const std::vector<uint8_t> frame = encode_text("not json", {});
  EXPECT_EQ(frame, raw_frame("not json", {}));
  expect_broken(frame, "framing is fine, the JSON is not");
}

TEST(Encode, AppendArrayPadsToMultiplesOf64) {
  std::vector<uint8_t> bin;
  const std::vector<uint8_t> small = bytes_0_to_11();
  const std::vector<float> numbers = {0.25f, -1.0f, 3.0f, 1e6f};
  const json first = append_array(bin, "u8", {12}, small.data());
  const json second = append_array(bin, "f32", {2, 2}, numbers.data());
  const json empty = append_array(bin, "u8", {0}, nullptr);
  EXPECT_EQ(first, json::parse(R"({"$a":[0,"u8",[12]]})"));
  EXPECT_EQ(second, json::parse(R"({"$a":[64,"f32",[2,2]]})"));
  EXPECT_EQ(empty, json::parse(R"({"$a":[128,"u8",[0]]})"));
  ASSERT_EQ(bin.size(), 128u);
  for (size_t i = 12; i < 64; ++i) EXPECT_EQ(bin[i], 0) << "padding byte " << i;

  const Frame frame = decode_one(encode({{"t", "ok"}, {"id", 1}, {"a", first}, {"b", second}, {"c", empty}}, bin));
  const ArrayView b = frame.array(frame.j["b"]);
  ASSERT_EQ(b.bytes, 16u);
  float back[4];
  std::memcpy(back, b.data, sizeof back);
  EXPECT_EQ(std::vector<float>(back, back + 4), numbers);
  EXPECT_THROW(append_array(bin, "i32", {1}, small.data()), std::invalid_argument);
  EXPECT_THROW(append_array(bin, "u8", {-1}, small.data()), std::invalid_argument);
}

}  // namespace
