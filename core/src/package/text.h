// Text, checksum and size helpers for the package code. Standard library only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace vyrn::package_detail {

// Strict UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF.
bool valid_utf8(std::string_view text);

// Number of characters (code points) in valid UTF-8.
size_t utf8_length(std::string_view text);

// True when the text holds a C0 control character, DEL, or a C1 control character (U+0080..U+009F).
bool has_control_chars(std::string_view text);

// Text that is safe to show: invalid UTF-8 replaced by U+FFFD; control characters (newlines too unless
// allow_newlines), and the invisible direction overrides that can disguise text, removed; surrounding whitespace
// trimmed; cut to at most max_chars characters.
std::string clean_text(std::string_view text, size_t max_chars, bool allow_newlines = false);

// A case-folded copy for "equal ignoring case" checks: ASCII, Latin-1, Latin Extended-A, Greek and Cyrillic.
std::string fold_case(std::string_view utf8);

std::string lower_ascii(std::string text);

// "412 KB", "3.1 MB", "32 MB", "120 bytes" (1 KB = 1024 bytes, like the limits).
std::string human_size(uint64_t bytes);

// SHA-256 as 64 lower-case hex digits.
std::string sha256_hex(std::string_view data);

class Sha256 {
 public:
  Sha256();
  void update(const void* data, size_t size);
  std::string hex();   // finishes; call once

 private:
  void block(const uint8_t* p);
  uint32_t h_[8];
  uint8_t buf_[64];
  size_t buf_len_ = 0;
  uint64_t total_ = 0;
};

}  // namespace vyrn::package_detail
