// vyrn::png_encode: 8-bit RGB PNG with zlib, optionally enlarged with nearest-neighbour.
//
// Each row gets the PNG filter (None, Sub, Up, Average, Paeth) with the smallest sum of absolute differences, the
// usual heuristic from the PNG specification; enlarged rows that repeat become all zeros under "Up", so scaled
// pictures stay small. Rows are compressed as they are made, so memory stays at a few rows plus the output.
#include "vyrn/project.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace vyrn {

namespace {

constexpr uint64_t kMaxSide = 1u << 16;            // PNG allows more, but nothing in Vyrn is near this
constexpr uint64_t kMaxPixels = uint64_t{1} << 26;  // 64 M pixels (192 MB of raw RGB)
constexpr size_t kIdatChunk = 256 * 1024;           // split the compressed data into chunks of this size
constexpr size_t kChannels = 3;

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v >> 24));
  out.push_back(static_cast<uint8_t>(v >> 16));
  out.push_back(static_cast<uint8_t>(v >> 8));
  out.push_back(static_cast<uint8_t>(v));
}

void put_chunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* data, size_t size) {
  put_u32(out, static_cast<uint32_t>(size));
  const size_t type_at = out.size();
  out.insert(out.end(), type, type + 4);
  if (size > 0) out.insert(out.end(), data, data + size);
  uLong crc = crc32(0L, Z_NULL, 0);
  crc = crc32(crc, out.data() + type_at, static_cast<uInt>(4 + size));
  put_u32(out, static_cast<uint32_t>(crc));
}

uint8_t paeth(int a, int b, int c) {
  const int p = a + b - c;
  const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
  if (pa <= pb && pa <= pc) return static_cast<uint8_t>(a);
  if (pb <= pc) return static_cast<uint8_t>(b);
  return static_cast<uint8_t>(c);
}

// Filters `row` against `prior` (the row above, zeros for the first row) into the best of the five filters.
// `out` gets the filter type byte followed by the filtered bytes.
void filter_row(const uint8_t* row, const uint8_t* prior, size_t n, std::array<std::vector<uint8_t>, 5>& tries,
                std::vector<uint8_t>& out) {
  uint64_t best_sum = UINT64_MAX;
  int best = 0;
  for (int type = 0; type < 5; ++type) {
    std::vector<uint8_t>& t = tries[static_cast<size_t>(type)];
    t.resize(n + 1);
    t[0] = static_cast<uint8_t>(type);
    uint64_t sum = 0;
    for (size_t i = 0; i < n; ++i) {
      const int x = row[i];
      const int a = i >= kChannels ? row[i - kChannels] : 0;
      const int b = prior[i];
      const int c = i >= kChannels ? prior[i - kChannels] : 0;
      int predicted = 0;
      switch (type) {
        case 1: predicted = a; break;
        case 2: predicted = b; break;
        case 3: predicted = (a + b) / 2; break;
        case 4: predicted = paeth(a, b, c); break;
        default: break;
      }
      const uint8_t v = static_cast<uint8_t>(x - predicted);
      t[i + 1] = v;
      sum += static_cast<uint64_t>(std::abs(static_cast<int>(static_cast<int8_t>(v))));
    }
    if (sum < best_sum) {
      best_sum = sum;
      best = type;
    }
  }
  out.swap(tries[static_cast<size_t>(best)]);
}

// Owns a zlib deflate stream.
class Deflater {
 public:
  Deflater() {
    if (deflateInit(&zs_, Z_DEFAULT_COMPRESSION) != Z_OK) throw std::runtime_error("Vyrn couldn't start zlib.");
  }
  ~Deflater() { deflateEnd(&zs_); }
  Deflater(const Deflater&) = delete;
  Deflater& operator=(const Deflater&) = delete;

  void feed(const uint8_t* data, size_t size, bool finish, std::vector<uint8_t>& out) {
    zs_.next_in = const_cast<Bytef*>(data);
    zs_.avail_in = static_cast<uInt>(size);
    uint8_t buf[64 * 1024];
    for (;;) {
      zs_.next_out = buf;
      zs_.avail_out = sizeof buf;
      const int rc = deflate(&zs_, finish ? Z_FINISH : Z_NO_FLUSH);
      if (rc == Z_STREAM_ERROR) throw std::runtime_error("zlib failed while making a PNG.");
      out.insert(out.end(), buf, buf + (sizeof buf - zs_.avail_out));
      if (finish ? rc == Z_STREAM_END : (zs_.avail_in == 0 && zs_.avail_out != 0)) break;
    }
  }

 private:
  z_stream zs_{};
};

}  // namespace

std::vector<uint8_t> png_encode(int width, int height, const std::vector<uint8_t>& rgb, int scale) {
  if (width <= 0 || height <= 0) {
    throw std::invalid_argument("A picture needs a width and a height of at least 1 pixel.");
  }
  if (scale < 1) throw std::invalid_argument("The scale of a picture must be 1 or more.");
  const uint64_t need = uint64_t(width) * uint64_t(height) * kChannels;
  if (rgb.size() != need) {
    throw std::invalid_argument("The picture has " + std::to_string(rgb.size()) + " bytes, but a " +
                                std::to_string(width) + "x" + std::to_string(height) + " colour picture needs " +
                                std::to_string(need) + ".");
  }
  const uint64_t out_w = uint64_t(width) * uint64_t(scale);
  const uint64_t out_h = uint64_t(height) * uint64_t(scale);
  if (out_w > kMaxSide || out_h > kMaxSide || out_w * out_h > kMaxPixels) {
    throw std::invalid_argument("That picture would be too big to save (" + std::to_string(out_w) + "x" +
                                std::to_string(out_h) + " pixels).");
  }

  const size_t row_bytes = static_cast<size_t>(out_w) * kChannels;
  std::vector<uint8_t> row(row_bytes), prior(row_bytes, 0), filtered;
  std::array<std::vector<uint8_t>, 5> tries;
  std::vector<uint8_t> idat;
  Deflater deflater;
  for (int y = 0; y < height; ++y) {
    const uint8_t* src = rgb.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * kChannels;
    if (scale == 1) {
      std::memcpy(row.data(), src, row_bytes);
    } else {
      uint8_t* dst = row.data();
      for (int x = 0; x < width; ++x, src += kChannels) {
        for (int k = 0; k < scale; ++k, dst += kChannels) std::memcpy(dst, src, kChannels);
      }
    }
    for (int k = 0; k < scale; ++k) {   // the same row `scale` times: "Up" makes the repeats all zeros
      filter_row(row.data(), prior.data(), row_bytes, tries, filtered);
      deflater.feed(filtered.data(), filtered.size(), false, idat);
      prior = row;
    }
  }
  deflater.feed(nullptr, 0, true, idat);

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  png.reserve(png.size() + idat.size() + 64 + 12 * (idat.size() / kIdatChunk + 1));
  std::vector<uint8_t> ihdr;
  put_u32(ihdr, static_cast<uint32_t>(out_w));
  put_u32(ihdr, static_cast<uint32_t>(out_h));
  ihdr.push_back(8);   // bit depth
  ihdr.push_back(2);   // colour type: RGB
  ihdr.push_back(0);   // compression: deflate
  ihdr.push_back(0);   // filter method 0 (adaptive, per row)
  ihdr.push_back(0);   // no interlace
  put_chunk(png, "IHDR", ihdr.data(), ihdr.size());
  for (size_t at = 0; at < idat.size(); at += kIdatChunk) {
    put_chunk(png, "IDAT", idat.data() + at, std::min(kIdatChunk, idat.size() - at));
  }
  put_chunk(png, "IEND", nullptr, 0);
  return png;
}

}  // namespace vyrn
