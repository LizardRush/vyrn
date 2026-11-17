#include "ipc/proto.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace vyrn::ipc {
namespace {

constexpr uint8_t kMagic[4] = {'V', 'Y', 'R', 'N'};

void put_u32le(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

uint32_t get_u32le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}

// Bytes per item of a protocol dtype, 0 for anything else.
size_t item_size(const std::string& dtype) {
  if (dtype == "u8") return 1;
  if (dtype == "f32") return 4;
  return 0;
}

// A JSON value for an error message: never throws (bad UTF-8 is replaced), cut to a readable length.
std::string brief(const json& value) {
  std::string text = value.dump(-1, ' ', false, json::error_handler_t::replace);
  if (text.size() > 80) text = text.substr(0, 77) + "...";
  return text;
}

std::string hex_bytes(const uint8_t* p, size_t n) {
  std::string out;
  char piece[4];
  for (size_t i = 0; i < n; ++i) {
    std::snprintf(piece, sizeof piece, i ? " %02x" : "%02x", p[i]);
    out += piece;
  }
  return out;
}

// A non-negative JSON integer (nlohmann keeps bools apart from numbers, as Python's reader does).
bool as_count(const json& value, uint64_t& out) {
  if (value.is_number_unsigned()) {
    out = value.get<uint64_t>();
    return true;
  }
  if (value.is_number_integer()) {
    const int64_t signed_value = value.get<int64_t>();
    if (signed_value < 0) return false;
    out = static_cast<uint64_t>(signed_value);
    return true;
  }
  return false;
}

// Validates one array reference against the blob (same rules as python/vyrn/_proto.py).
ArrayView check_array(const json& ref, const std::vector<uint8_t>& bin) {
  const auto found = ref.is_object() ? ref.find("$a") : ref.end();
  if (!ref.is_object() || ref.size() != 1 || found == ref.end() || !found->is_array() || found->size() != 3) {
    throw ProtocolError("bad array reference " + brief(ref));
  }
  const json& spec = *found;
  uint64_t offset = 0;
  if (!as_count(spec[0], offset) || offset % kArrayAlign != 0) {
    throw ProtocolError("bad array offset " + brief(spec[0]) + " (it must be a multiple of 64)");
  }
  const size_t item = spec[1].is_string() ? item_size(spec[1].get_ref<const std::string&>()) : 0;
  if (item == 0) throw ProtocolError("bad array dtype " + brief(spec[1]) + " (expected \"u8\" or \"f32\")");
  if (!spec[2].is_array()) throw ProtocolError("bad array shape " + brief(spec[2]));

  ArrayView view;
  view.dtype = spec[1].get<std::string>();
  bool empty = false;
  for (const json& dim : spec[2]) {
    uint64_t n = 0;
    if (!as_count(dim, n) || n > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      throw ProtocolError("bad array shape " + brief(spec[2]));
    }
    view.shape.push_back(static_cast<int64_t>(n));
    empty = empty || n == 0;
  }
  // Byte length = prod(shape) * itemsize, compared against the blob without overflowing.
  const uint64_t blob = bin.size();
  uint64_t bytes = empty ? 0 : item;
  bool fits = offset <= blob;
  for (int64_t n : view.shape) {
    if (empty || !fits) break;
    if (static_cast<uint64_t>(n) > blob / bytes) {
      fits = false;
    } else {
      bytes *= static_cast<uint64_t>(n);
    }
  }
  if (!fits || bytes > blob - offset) {
    throw ProtocolError("array " + brief(ref) + " is outside the " + std::to_string(blob) + "-byte blob");
  }
  view.data = bin.data() + offset;
  view.bytes = static_cast<size_t>(bytes);
  return view;
}

// Checks a whole message: every array reference (any object with a "$a" key, like the Python reader),
// only finite numbers, bounded nesting.
void check_tree(const json& value, const std::vector<uint8_t>& bin, int depth) {
  if (depth > kMaxJsonDepth) {
    throw ProtocolError("the JSON is nested more than " + std::to_string(kMaxJsonDepth) + " levels deep");
  }
  switch (value.type()) {
    case json::value_t::object:
      if (value.contains("$a")) {
        check_array(value, bin);
        return;
      }
      for (const json& item : value) check_tree(item, bin, depth + 1);
      return;
    case json::value_t::array:
      for (const json& item : value) check_tree(item, bin, depth + 1);
      return;
    case json::value_t::number_float:
      if (!std::isfinite(value.get<double>())) {
        throw ProtocolError("the JSON contains a number that is not finite (NaN or infinity)");
      }
      return;
    default:
      return;
  }
}

json parse_json(const uint8_t* text, size_t size) {
  const auto* begin = reinterpret_cast<const char*>(text);
  json parsed;
  try {
    // The callback only guards the nesting depth (deep JSON would overflow the stack later on).
    parsed = json::parse(begin, begin + size, [](int depth, json::parse_event_t, json&) {
      if (depth > kMaxJsonDepth) {
        throw ProtocolError("the JSON is nested more than " + std::to_string(kMaxJsonDepth) + " levels deep");
      }
      return true;
    });
  } catch (const json::exception& err) {
    throw ProtocolError(std::string("bad JSON in frame: ") + err.what());
  }
  if (!parsed.is_object()) throw ProtocolError("a frame's JSON must be an object, not " + brief(parsed));
  return parsed;
}

}  // namespace

ArrayView Frame::array(const json& ref) const { return check_array(ref, bin); }

bool Frame::has_array(const char* key) const {
  if (!j.is_object()) return false;
  const auto found = j.find(key);
  return found != j.end() && found->is_object() && found->contains("$a");
}

std::vector<uint8_t> encode(const json& j, const std::vector<uint8_t>& bin) {
  if (!j.is_object()) throw ProtocolError("a message must be a JSON object, not " + brief(j));
  check_tree(j, bin, 0);
  std::string text;
  try {
    text = j.dump();   // compact; throws on strings that are not valid UTF-8
  } catch (const json::exception& err) {
    throw ProtocolError(std::string("the message can't be written as JSON: ") + err.what());
  }
  return encode_text(text, bin);
}

std::vector<uint8_t> encode_text(const std::string& json_text, const std::vector<uint8_t>& bin) {
  if (json_text.empty() || json_text.size() > kMaxJsonBytes) {
    throw ProtocolError("message JSON is " + std::to_string(json_text.size()) + " bytes (allowed: 1 to " +
                        std::to_string(kMaxJsonBytes) + ")");
  }
  if (bin.size() > kMaxBinBytes) {
    throw ProtocolError("message data is " + std::to_string(bin.size()) + " bytes (the limit is " +
                        std::to_string(kMaxBinBytes) + ")");
  }
  std::vector<uint8_t> frame;
  frame.reserve(kHeaderBytes + json_text.size() + bin.size());
  frame.insert(frame.end(), std::begin(kMagic), std::end(kMagic));
  put_u32le(frame, static_cast<uint32_t>(json_text.size()));
  put_u32le(frame, static_cast<uint32_t>(bin.size()));
  frame.insert(frame.end(), json_text.begin(), json_text.end());
  frame.insert(frame.end(), bin.begin(), bin.end());
  return frame;
}

json append_array(std::vector<uint8_t>& bin, const std::string& dtype, const std::vector<int64_t>& shape,
                  const void* data) {
  const size_t item = item_size(dtype);
  if (item == 0) throw std::invalid_argument("append_array: dtype must be \"u8\" or \"f32\", not \"" + dtype + "\"");
  uint64_t count = 1;
  for (int64_t n : shape) {
    if (n < 0) throw std::invalid_argument("append_array: negative dimension in the shape");
    const auto dim = static_cast<uint64_t>(n);
    if (dim != 0 && count > kMaxBinBytes / dim) throw ProtocolError("append_array: the array is bigger than 64 MiB");
    count *= dim;
  }
  const uint64_t bytes = count * item;
  const size_t offset = (bin.size() + kArrayAlign - 1) / kArrayAlign * kArrayAlign;
  if (offset + bytes > kMaxBinBytes) throw ProtocolError("append_array: the blob would be bigger than 64 MiB");
  bin.resize(offset, 0);   // zero padding up to the aligned offset
  if (bytes != 0) {
    const auto* first = static_cast<const uint8_t*>(data);
    bin.insert(bin.end(), first, first + bytes);
  }
  return json{{"$a", json::array({offset, dtype, shape})}};
}

void Decoder::feed(const uint8_t* data, size_t size) {
  if (size == 0) return;
  if (pos_ == buf_.size()) {
    buf_.clear();
    pos_ = 0;
  } else if (pos_ >= (64u << 10) && pos_ * 2 >= buf_.size()) {
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));   // drop consumed bytes
    pos_ = 0;
  }
  buf_.insert(buf_.end(), data, data + size);
}

std::optional<Frame> Decoder::next() {
  const size_t avail = buffered();
  if (avail == 0) return std::nullopt;
  const uint8_t* p = buf_.data() + pos_;
  // Check the magic as soon as its first byte is here, so garbage is caught right away.
  const size_t magic_seen = avail < sizeof kMagic ? avail : sizeof kMagic;
  if (std::memcmp(p, kMagic, magic_seen) != 0) {
    throw ProtocolError("bad magic " + hex_bytes(p, magic_seen) + " (a frame starts with \"VYRN\")");
  }
  if (avail < kHeaderBytes) return std::nullopt;
  const uint32_t json_len = get_u32le(p + 4);
  const uint32_t bin_len = get_u32le(p + 8);
  if (json_len < 1 || json_len > kMaxJsonBytes) {
    throw ProtocolError("bad JSON length " + std::to_string(json_len) + " (allowed: 1 to " +
                        std::to_string(kMaxJsonBytes) + ")");
  }
  if (bin_len > kMaxBinBytes) {
    throw ProtocolError("bad data length " + std::to_string(bin_len) + " (the limit is " +
                        std::to_string(kMaxBinBytes) + ")");
  }
  const size_t total = kHeaderBytes + json_len + static_cast<size_t>(bin_len);
  if (avail < total) return std::nullopt;

  Frame frame;
  frame.j = parse_json(p + kHeaderBytes, json_len);
  frame.bin.assign(p + kHeaderBytes + json_len, p + total);
  check_tree(frame.j, frame.bin, 0);
  pos_ += total;   // only now: a broken frame stays in place and keeps failing
  if (pos_ == buf_.size()) {
    buf_.clear();
    pos_ = 0;
  }
  return frame;
}

}  // namespace vyrn::ipc
