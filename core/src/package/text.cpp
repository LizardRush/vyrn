#include "package/text.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vyrn::package_detail {

namespace {

constexpr char32_t kReplacement = 0xFFFD;

// Decodes one code point at text[i]. Returns false (and advances by one byte) on an invalid sequence.
bool decode_one(std::string_view text, size_t& i, char32_t& cp) {
  const auto byte = [&](size_t k) { return static_cast<unsigned char>(text[k]); };
  const unsigned char c = byte(i);
  if (c < 0x80) {
    cp = c;
    ++i;
    return true;
  }
  int extra = 0;
  char32_t min = 0;
  if (c >= 0xC2 && c <= 0xDF) {
    extra = 1;
    cp = c & 0x1F;
    min = 0x80;
  } else if (c >= 0xE0 && c <= 0xEF) {
    extra = 2;
    cp = c & 0x0F;
    min = 0x800;
  } else if (c >= 0xF0 && c <= 0xF4) {
    extra = 3;
    cp = c & 0x07;
    min = 0x10000;
  } else {
    ++i;
    return false;
  }
  if (i + static_cast<size_t>(extra) >= text.size()) {
    ++i;
    return false;
  }
  for (int k = 1; k <= extra; ++k) {
    const unsigned char cc = byte(i + static_cast<size_t>(k));
    if ((cc & 0xC0) != 0x80) {
      ++i;
      return false;
    }
    cp = (cp << 6) | (cc & 0x3F);
  }
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    ++i;
    return false;
  }
  i += static_cast<size_t>(extra) + 1;
  return true;
}

void append_utf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

bool is_control(char32_t cp) { return cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F); }

// Invisible characters that change the direction or joining of text (used to disguise names).
bool is_disguise(char32_t cp) {
  return (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069) || cp == 0x200E || cp == 0x200F ||
         cp == 0x061C || cp == 0xFEFF;
}

bool is_space(char32_t cp) { return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0xA0; }

char32_t fold(char32_t c) {
  if (c >= 'A' && c <= 'Z') return c + 32;
  if (c < 0x80) return c;
  if ((c >= 0xC0 && c <= 0xDE && c != 0xD7)) return c + 32;
  if (c >= 0x100 && c <= 0x137) return c | 1;                  // Latin Extended-A pairs: even upper, odd lower
  if (c >= 0x139 && c <= 0x148) return (c & 1) ? c + 1 : c;    // odd upper, even lower
  if (c >= 0x14A && c <= 0x177) return c | 1;
  if (c == 0x178) return 0xFF;
  if (c >= 0x179 && c <= 0x17E) return (c & 1) ? c + 1 : c;
  if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;   // Greek
  if (c == 0x3C2) return 0x3C3;                                // final sigma
  if (c >= 0x400 && c <= 0x40F) return c + 80;                 // Cyrillic
  if (c >= 0x410 && c <= 0x42F) return c + 32;
  return c;
}

}  // namespace

bool valid_utf8(std::string_view text) {
  size_t i = 0;
  char32_t cp = 0;
  while (i < text.size()) {
    if (!decode_one(text, i, cp)) return false;
  }
  return true;
}

size_t utf8_length(std::string_view text) {
  size_t n = 0;
  for (const char c : text) {
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
  }
  return n;
}

bool has_control_chars(std::string_view text) {
  size_t i = 0;
  char32_t cp = 0;
  while (i < text.size()) {
    if (decode_one(text, i, cp) && is_control(cp)) return true;
  }
  return false;
}

std::string clean_text(std::string_view text, size_t max_chars, bool allow_newlines) {
  std::u32string chars;
  size_t i = 0;
  while (i < text.size()) {
    char32_t cp = 0;
    if (!decode_one(text, i, cp)) cp = kReplacement;
    if (cp == '\r') continue;
    if (cp == '\t') cp = ' ';
    if (cp == '\n' && allow_newlines) {
      chars += cp;
      continue;
    }
    if (is_control(cp) || is_disguise(cp)) continue;
    chars += cp;
  }
  size_t begin = 0, end = chars.size();
  while (begin < end && is_space(chars[begin])) ++begin;
  while (end > begin && is_space(chars[end - 1])) --end;
  if (end - begin > max_chars) end = begin + max_chars;
  while (end > begin && is_space(chars[end - 1])) --end;
  std::string out;
  for (size_t k = begin; k < end; ++k) append_utf8(out, chars[k]);
  return out;
}

std::string fold_case(std::string_view utf8) {
  std::string out;
  size_t i = 0;
  while (i < utf8.size()) {
    char32_t cp = 0;
    if (!decode_one(utf8, i, cp)) cp = kReplacement;
    append_utf8(out, fold(cp));
  }
  return out;
}

std::string lower_ascii(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  }
  return text;
}

std::string human_size(uint64_t bytes) {
  constexpr uint64_t kKiB = 1024, kMiB = kKiB * 1024, kGiB = kMiB * 1024;
  char buf[48];
  if (bytes < kKiB) {
    std::snprintf(buf, sizeof buf, "%llu byte%s", static_cast<unsigned long long>(bytes), bytes == 1 ? "" : "s");
  } else if (bytes < kMiB) {
    std::snprintf(buf, sizeof buf, "%llu KB", static_cast<unsigned long long>((bytes + kKiB - 1) / kKiB));
  } else if (bytes < kGiB) {
    if (bytes % kMiB == 0) {
      std::snprintf(buf, sizeof buf, "%llu MB", static_cast<unsigned long long>(bytes / kMiB));
    } else {
      std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / static_cast<double>(kMiB));
    }
  } else if (bytes % kGiB == 0) {
    std::snprintf(buf, sizeof buf, "%llu GB", static_cast<unsigned long long>(bytes / kGiB));
  } else {
    std::snprintf(buf, sizeof buf, "%.2f GB", static_cast<double>(bytes) / static_cast<double>(kGiB));
  }
  return buf;
}

// ---- SHA-256 (FIPS 180-4) ------------------------------------------------------------------------------------

namespace {

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

}  // namespace

Sha256::Sha256()
    : h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::block(const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (uint32_t{p[4 * i]} << 24) | (uint32_t{p[4 * i + 1]} << 16) | (uint32_t{p[4 * i + 2]} << 8) |
           uint32_t{p[4 * i + 3]};
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t t1 = h + s1 + ch + kK[i] + w[i];
    const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h_[0] += a;
  h_[1] += b;
  h_[2] += c;
  h_[3] += d;
  h_[4] += e;
  h_[5] += f;
  h_[6] += g;
  h_[7] += h;
}

void Sha256::update(const void* data, size_t size) {
  const auto* p = static_cast<const uint8_t*>(data);
  total_ += size;
  if (buf_len_ > 0) {
    const size_t take = std::min(size, sizeof buf_ - buf_len_);
    std::memcpy(buf_ + buf_len_, p, take);
    buf_len_ += take;
    p += take;
    size -= take;
    if (buf_len_ == sizeof buf_) {
      block(buf_);
      buf_len_ = 0;
    }
  }
  while (size >= 64) {
    block(p);
    p += 64;
    size -= 64;
  }
  if (size > 0) {
    std::memcpy(buf_, p, size);
    buf_len_ = size;
  }
}

std::string Sha256::hex() {
  const uint64_t bits = total_ * 8;
  const uint8_t one = 0x80, zero = 0;
  update(&one, 1);
  while (buf_len_ != 56) update(&zero, 1);
  uint8_t len[8];
  for (int i = 0; i < 8; ++i) len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
  update(len, 8);
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (const uint32_t v : h_) {
    for (int shift = 28; shift >= 0; shift -= 4) out += digits[(v >> shift) & 0xF];
  }
  return out;
}

std::string sha256_hex(std::string_view data) {
  Sha256 sha;
  sha.update(data.data(), data.size());
  return sha.hex();
}

}  // namespace vyrn::package_detail
