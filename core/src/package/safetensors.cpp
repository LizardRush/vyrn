#include "package/safetensors.h"

#include "package/json_strict.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace vyrn::package_detail {

namespace {

using json = nlohmann::json;

constexpr int64_t kMaxDim = 16'777'216;
constexpr const char* kFormat = "vyrn-qnet";

[[noreturn]] void bad(const std::string& why) { throw WeightsError(why); }

// 4 x product(shape), or a WeightsError when the shape is out of bounds or bigger than `limit` bytes.
uint64_t byte_size(const std::string& name, const std::vector<int64_t>& shape, uint64_t limit) {
  if (shape.empty() || shape.size() > 4) bad("tensor " + name + " has rank " + std::to_string(shape.size()));
  uint64_t bytes = 4;
  for (const int64_t dim : shape) {
    if (dim < 1 || dim > kMaxDim) bad("tensor " + name + " has a dimension of " + std::to_string(dim));
    bytes *= static_cast<uint64_t>(dim);
    if (bytes > limit) bad("tensor " + name + " is bigger than the data");
  }
  return bytes;
}

}  // namespace

bool valid_tensor_name(std::string_view name) {
  if (name.empty() || name.size() > 128) return false;
  return std::all_of(name.begin(), name.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
  });
}

std::string encode_safetensors(std::vector<TensorView> tensors, const std::string& net) {
  if (tensors.size() > kMaxTensors) bad("too many tensors");
  std::sort(tensors.begin(), tensors.end(), [](const TensorView& a, const TensorView& b) { return a.name < b.name; });
  json header = json::object();
  header["__metadata__"] = {{"format", kFormat}, {"net", net}};
  uint64_t offset = 0;
  for (size_t i = 0; i < tensors.size(); ++i) {
    const TensorView& t = tensors[i];
    if (!valid_tensor_name(t.name) || t.name == "__metadata__") bad("bad tensor name " + t.name);
    if (i > 0 && tensors[i - 1].name == t.name) bad("tensor " + t.name + " appears twice");
    const uint64_t bytes = byte_size(t.name, t.shape, 1ull << 40);
    if (bytes != t.data.size()) bad("tensor " + t.name + " has " + std::to_string(t.data.size()) + " bytes");
    header[t.name] = {{"dtype", "F32"}, {"shape", t.shape}, {"data_offsets", {offset, offset + bytes}}};
    offset += bytes;
  }
  std::string text = header.dump();
  while (text.size() % 8 != 0) text += ' ';
  if (text.size() > kMaxHeaderBytes) bad("the header is too big");
  std::string out;
  out.reserve(8 + text.size() + offset);
  const uint64_t n = text.size();
  for (int i = 0; i < 8; ++i) out += static_cast<char>((n >> (8 * i)) & 0xFF);
  out += text;
  for (const TensorView& t : tensors) out.append(t.data.data(), t.data.size());
  return out;
}

std::vector<TensorView> decode_safetensors(std::string_view file, const std::string& expected_net) {
  if (file.size() < 8) bad("the file is shorter than 8 bytes");
  uint64_t header_len = 0;
  for (int i = 7; i >= 0; --i) header_len = (header_len << 8) | static_cast<unsigned char>(file[static_cast<size_t>(i)]);
  if (header_len > kMaxHeaderBytes) bad("the header is longer than 1 MiB");
  if (header_len > file.size() - 8) bad("the header is longer than the file");
  const std::string_view text = file.substr(8, static_cast<size_t>(header_len));
  const std::string_view data = file.substr(8 + static_cast<size_t>(header_len));

  json header;
  try {
    header = parse_json_strict<json>(text, 8);
  } catch (const JsonError& e) {
    bad(std::string("the header: ") + e.what());
  }
  if (!header.is_object()) bad("the header is not a JSON object");

  const auto meta = header.find("__metadata__");
  if (meta == header.end() || !meta->is_object()) bad("the header has no __metadata__");
  for (const auto& [key, value] : meta->items()) {
    if (!value.is_string()) bad("__metadata__." + key + " is not text");
  }
  if (meta->value("format", "") != kFormat) bad("__metadata__.format is not " + std::string(kFormat));
  if (meta->value("net", "") != expected_net) bad("__metadata__.net is not " + expected_net);
  if (header.size() - 1 > kMaxTensors) bad("more than 64 tensors");

  std::vector<TensorView> out;
  uint64_t expected_begin = 0;
  for (const auto& [name, entry] : header.items()) {   // sorted by name
    if (name == "__metadata__") continue;
    if (!valid_tensor_name(name)) bad("bad tensor name");
    if (!entry.is_object() || entry.size() != 3 || !entry.contains("dtype") || !entry.contains("shape") ||
        !entry.contains("data_offsets")) {
      bad("tensor " + name + " needs exactly dtype, shape and data_offsets");
    }
    if (entry["dtype"] != "F32") bad("tensor " + name + " is not F32");
    const json& shape_json = entry["shape"];
    if (!shape_json.is_array()) bad("tensor " + name + " has no shape list");
    TensorView t;
    t.name = name;
    for (const json& dim : shape_json) {
      if (!dim.is_number_integer()) bad("tensor " + name + " has a dimension that is not a whole number");
      t.shape.push_back(dim.get<int64_t>());
    }
    const uint64_t bytes = byte_size(name, t.shape, data.size());
    const json& offsets = entry["data_offsets"];
    if (!offsets.is_array() || offsets.size() != 2 || !offsets[0].is_number_unsigned() ||
        !offsets[1].is_number_unsigned()) {
      bad("tensor " + name + " has bad data_offsets");
    }
    const uint64_t begin = offsets[0].get<uint64_t>(), end = offsets[1].get<uint64_t>();
    if (begin != expected_begin) bad("tensor " + name + " doesn't start where the previous one ends");
    if (end < begin || end - begin != bytes) bad("tensor " + name + " has the wrong byte size");
    if (end > data.size()) bad("tensor " + name + " ends after the data");
    expected_begin = end;
    t.data = data.substr(static_cast<size_t>(begin), static_cast<size_t>(bytes));
    for (size_t at = 0; at < t.data.size(); at += 4) {
      float v;
      std::memcpy(&v, t.data.data() + at, 4);
      if (!std::isfinite(v)) bad("tensor " + name + " holds a value that is not a finite number");
    }
    out.push_back(std::move(t));
  }
  if (expected_begin != data.size()) bad("the data has bytes no tensor uses");
  return out;
}

}  // namespace vyrn::package_detail
