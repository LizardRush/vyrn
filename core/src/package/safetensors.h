// model.safetensors (docs/dev/package.md section 3): u64 header length | JSON header | raw float32 data.
// Standard library + nlohmann_json only; the torch side is in weights.cpp.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vyrn::package_detail {

// Weights that break the rules. what() says what exactly (for tests and logs); people see the one sentence
// "The brain's weights are damaged or don't match its description."
struct WeightsError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct TensorView {
  std::string name;
  std::vector<int64_t> shape;
  std::string_view data;   // little-endian float32, C order: 4 x product(shape) bytes
};

constexpr size_t kMaxTensors = 64;
constexpr uint64_t kMaxHeaderBytes = 1ull << 20;

// Builds the file. Tensors are written in name order. Throws WeightsError on a bad name, shape or size.
std::string encode_safetensors(std::vector<TensorView> tensors, const std::string& net);

// Parses and checks the file (header, names, dtypes, shapes, offsets, finite values). The views point into
// `file`. expected_net: the "net" the metadata must name. Throws WeightsError.
std::vector<TensorView> decode_safetensors(std::string_view file, const std::string& expected_net);

// [A-Za-z0-9_.]{1,128}
bool valid_tensor_name(std::string_view name);

}  // namespace vyrn::package_detail
