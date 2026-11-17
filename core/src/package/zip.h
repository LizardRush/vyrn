// The strict ZIP subset of .vrl / .vrlp files (docs/dev/package.md section 2): a reader that checks a whole
// archive held in memory before anything is extracted, and the writer Vyrn uses. Standard library + zlib only.
#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vyrn::package_detail {

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "Vyrn's package code works on little-endian hosts only");

// An archive that breaks the rules. what() completes the sentence "Vyrn can't open <file>: ...", in plain English
// and ending with a period, e.g. "it is encrypted."
struct ZipError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct ZipLimits {
  uint64_t max_file = 1ull << 30;     // the archive itself
  size_t max_entries = 4096;          // central directory records, folder entries included
  uint64_t max_entry = 64ull << 20;   // one entry, uncompressed
  uint64_t max_total = 512ull << 20;  // all entries together, uncompressed
};

struct ZipEntry {
  std::string name;
  uint16_t method = 0;                // 0 stored, 8 deflated
  uint16_t flags = 0;
  uint32_t crc = 0;
  uint64_t csize = 0, usize = 0;
  uint64_t local_offset = 0, data_offset = 0;
};

// Checks the whole container on construction (end record, every central directory record and local header, names,
// limits, overlaps). Entry data is checked when read(). `bytes` is not copied and must outlive the reader.
class ZipReader {
 public:
  ZipReader(std::string_view bytes, const ZipLimits& limits);

  const std::vector<ZipEntry>& entries() const { return entries_; }   // central directory order, no folder entries
  const std::string& first_name() const { return first_name_; }       // the first record's name (folders included)
  size_t record_count() const { return records_; }                    // central directory records (folders included)
  const ZipEntry* find(std::string_view name) const;
  // The entry's data: inflated (bounded by its declared size) and CRC-checked. Throws ZipError.
  std::string read(const ZipEntry& entry) const;

 private:
  std::string_view bytes_;
  std::vector<ZipEntry> entries_;
  std::string first_name_;
  size_t records_ = 0;
};

// "" when `name` is an acceptable entry name (section 2), else the reason ("... ." sentence fragment). Folder
// entries ("a/b/") are checked without their final '/'.
std::string name_problem(std::string_view name);

// `name` with control characters shown as '?' and cut to about 60 bytes, for messages.
std::string shown_name(std::string_view name);

// Writes exactly what section 2 says Vyrn writes. The sink gets the archive bytes in order; it throws on errors.
class ZipWriter {
 public:
  using Sink = std::function<void(const char* data, size_t size)>;
  // `unix_time`: the export time (UTC), stored as every entry's DOS date and time.
  ZipWriter(Sink sink, int64_t unix_time);

  // Adds one entry. deflate=false stores it. Names must already be valid (they are checked again here).
  void add(const std::string& name, std::string_view data, bool deflate);
  // Writes the central directory and the end record. No add() after this.
  void finish();
  uint64_t size() const { return offset_; }

 private:
  struct Record {
    std::string name;
    uint16_t method;
    uint32_t crc;
    uint32_t csize, usize, offset;
  };
  void put(const std::string& bytes);

  Sink sink_;
  uint16_t dos_time_ = 0, dos_date_ = 0;
  uint64_t offset_ = 0;
  std::vector<Record> records_;
  bool finished_ = false;
};

// zlib's CRC-32 of `data`.
uint32_t crc32_of(std::string_view data);

}  // namespace vyrn::package_detail
