// Frame codec for the app <-> Python worker protocol (docs/dev/protocol.md section 2).
//
//   offset 0   4 bytes   magic "VYRN"
//   offset 4   u32 LE    json_len   (1 .. 1 MiB)
//   offset 8   u32 LE    bin_len    (0 .. 64 MiB)
//   offset 12  json_len  UTF-8 JSON object
//   then       bin_len   binary blob
//
// Arrays inside the JSON are references into the blob: {"$a": [offset, dtype, shape]} with dtype
// "u8" or "f32" (little-endian), C order, offset a multiple of 64.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace vyrn::ipc {

using json = nlohmann::json;

inline constexpr size_t kHeaderBytes = 12;
inline constexpr uint32_t kMaxJsonBytes = 1u << 20;   // 1 MiB
inline constexpr uint32_t kMaxBinBytes = 64u << 20;   // 64 MiB
inline constexpr size_t kArrayAlign = 64;             // array offsets in the blob are multiples of this
inline constexpr int kMaxJsonDepth = 256;             // deeper nesting is rejected (protocol messages are ~4 deep)

// A frame that breaks the protocol: bad magic, bad lengths, bad JSON, bad array reference...
struct ProtocolError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// One array of a frame, pointing into Frame::bin (valid while the frame lives).
struct ArrayView {
  std::string dtype;            // "u8" or "f32"
  std::vector<int64_t> shape;   // C order
  const uint8_t* data = nullptr;
  size_t bytes = 0;             // prod(shape) * itemsize
};

struct Frame {
  json j;                     // the JSON object (array refs left as {"$a":[off,dtype,shape]})
  std::vector<uint8_t> bin;   // the blob

  // The array a reference points at. Throws ProtocolError if the reference is malformed or
  // reaches outside the blob.
  ArrayView array(const json& ref) const;
  // True if j[key] is an array reference ({"$a": ...}).
  bool has_array(const char* key) const;
};

// Encodes one message. j must be a JSON object whose numbers are finite and whose array refs fit in
// bin; the serialized JSON must be 1 byte .. 1 MiB and bin at most 64 MiB. Throws ProtocolError.
// Note: nlohmann::json keeps object keys sorted, so the key order on the wire is alphabetical.
std::vector<uint8_t> encode(const json& j, const std::vector<uint8_t>& bin = {});

// Frames JSON text that is already serialized (exactly as given, so the key order is the caller's).
// Only the size limits are checked. Used for byte-exact frames (golden tests) and to build bad frames.
std::vector<uint8_t> encode_text(const std::string& json_text, const std::vector<uint8_t>& bin = {});

// Appends an array to a blob (zero padding to the next multiple of 64 first) and returns its
// reference {"$a": [offset, dtype, shape]}. dtype is "u8" or "f32"; data holds prod(shape) items.
json append_array(std::vector<uint8_t>& bin, const std::string& dtype, const std::vector<int64_t>& shape,
                  const void* data);

// Incremental decoder for a byte stream: feed() whatever arrived, then call next() until it returns
// nothing. next() throws ProtocolError on a broken frame (and keeps throwing: the stream is unusable).
class Decoder {
 public:
  void feed(const uint8_t* data, size_t size);
  std::optional<Frame> next();
  // Bytes received but not yet returned as a frame (non-zero at EOF = the stream ended mid-frame).
  size_t buffered() const { return buf_.size() - pos_; }

 private:
  std::vector<uint8_t> buf_;
  size_t pos_ = 0;   // start of the unread bytes in buf_
};

}  // namespace vyrn::ipc
