#include "package/zip.h"

#include "platform/system.h"

#include "package/text.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <set>
#include <utility>

namespace vyrn::package_detail {

namespace {

constexpr uint32_t kLocalSig = 0x04034b50;
constexpr uint32_t kCentralSig = 0x02014b50;
constexpr uint32_t kEndSig = 0x06054b50;
constexpr uint32_t kZip64LocatorSig = 0x07064b50;
constexpr uint32_t kDescriptorSig = 0x08074b50;
constexpr size_t kEndSize = 22;
constexpr size_t kMaxComment = 65535;
constexpr uint16_t kFlagEncrypted = 1u << 0;
constexpr uint16_t kFlagDescriptor = 1u << 3;
constexpr uint16_t kFlagPatched = 1u << 5;
constexpr uint16_t kFlagStrongEncryption = 1u << 6;
constexpr uint16_t kFlagUtf8 = 1u << 11;
constexpr uint16_t kFlagMaskedHeader = 1u << 13;
constexpr uint16_t kVersionNeeded = 20;
constexpr uint16_t kMadeByUnix = (3u << 8) | 20u;
constexpr uint32_t kRegularFile0644 = 0100644u << 16;

uint16_t u16(std::string_view b, size_t at) {
  return static_cast<uint16_t>(static_cast<unsigned char>(b[at]) | (static_cast<unsigned char>(b[at + 1]) << 8));
}

uint32_t u32(std::string_view b, size_t at) {
  return static_cast<uint32_t>(u16(b, at)) | (static_cast<uint32_t>(u16(b, at + 2)) << 16);
}

void put16(std::string& out, uint16_t v) {
  out += static_cast<char>(v & 0xFF);
  out += static_cast<char>(v >> 8);
}

void put32(std::string& out, uint32_t v) {
  put16(out, static_cast<uint16_t>(v & 0xFFFF));
  put16(out, static_cast<uint16_t>(v >> 16));
}

std::string in_quotes(std::string_view name) { return "\"" + shown_name(name) + "\""; }

[[noreturn]] void damaged(const std::string& why) { throw ZipError(why); }

// Extra fields: (id, size, data)*, exactly filling the area. ZIP64 and AES records are refused.
void check_extra(std::string_view extra, std::string_view name) {
  size_t at = 0;
  while (at < extra.size()) {
    if (extra.size() - at < 4) damaged("the extra information of " + in_quotes(name) + " is damaged.");
    const uint16_t id = u16(extra, at);
    const uint16_t size = u16(extra, at + 2);
    if (extra.size() - at - 4 < size) damaged("the extra information of " + in_quotes(name) + " is damaged.");
    if (id == 0x0001) damaged("it uses ZIP64, which Vyrn files never need.");
    if (id == 0x9901) damaged("it is encrypted.");
    at += 4 + size;
  }
}

void check_flags(uint16_t flags, uint16_t method) {
  if ((flags & (kFlagEncrypted | kFlagStrongEncryption | kFlagMaskedHeader)) || method == 99) {
    damaged("it is encrypted.");
  }
  if (flags & kFlagPatched) damaged("it uses a ZIP feature Vyrn doesn't read (patched data).");
}

struct Range {
  uint64_t begin, end;
};

}  // namespace

std::string shown_name(std::string_view name) {
  std::string out;
  for (const char c : name) {
    if (out.size() >= 60) {
      out += "...";
      break;
    }
    const unsigned char u = static_cast<unsigned char>(c);
    out += (u < 0x20 || u == 0x7F) ? '?' : c;
  }
  if (!valid_utf8(out)) out = clean_text(out, 60);
  return out;
}

std::string name_problem(std::string_view name) {
  if (!name.empty() && name.back() == '/') name.remove_suffix(1);   // a folder entry
  if (name.empty()) return "an entry has no name.";
  if (name.size() > 1024) return "an entry's name is longer than 1024 bytes.";
  if (!valid_utf8(name)) return "an entry's name isn't valid UTF-8 text.";
  if (name.front() == '/') return "the name " + in_quotes(name) + " is an absolute path.";
  if (name.find('\\') != std::string_view::npos) return "the name " + in_quotes(name) + " contains a backslash.";
  if (name.find(':') != std::string_view::npos) return "the name " + in_quotes(name) + " contains a colon.";
  if (name.find('\0') != std::string_view::npos || has_control_chars(name)) {
    return "the name " + in_quotes(name) + " contains a control character.";
  }
  size_t start = 0;
  while (start <= name.size()) {
    size_t end = name.find('/', start);
    if (end == std::string_view::npos) end = name.size();
    const std::string_view part = name.substr(start, end - start);
    if (part.empty()) return "the name " + in_quotes(name) + " has an empty folder name in it.";
    if (part == "..") return "the name " + in_quotes(name) + " leads outside the package.";
    if (part == ".") return "the name " + in_quotes(name) + " contains a \".\" folder.";
    if (part.size() > 255) return "the name " + in_quotes(name) + " has a part longer than 255 bytes.";
    if (part.front() == '.') {
      return "the name " + in_quotes(name) + " is a hidden file or folder (hidden files are never shared).";
    }
    start = end + 1;
  }
  return "";
}

uint32_t crc32_of(std::string_view data) {
  uLong crc = ::crc32(0L, Z_NULL, 0);
  const auto* p = reinterpret_cast<const Bytef*>(data.data());
  size_t left = data.size();
  while (left > 0) {
    const uInt n = static_cast<uInt>(std::min<size_t>(left, 1u << 30));
    crc = ::crc32(crc, p, n);
    p += n;
    left -= n;
  }
  return static_cast<uint32_t>(crc);
}

// ---- reading ----------------------------------------------------------------------------------------------

ZipReader::ZipReader(std::string_view bytes, const ZipLimits& limits) : bytes_(bytes) {
  const std::string_view b = bytes_;
  if (b.size() > limits.max_file) damaged("it is bigger than 1 GB.");
  if (b.size() < kEndSize) damaged("it isn't a ZIP archive.");

  // End of central directory: its comment must end exactly at the end of the file.
  const size_t lowest = b.size() > kEndSize + kMaxComment ? b.size() - kEndSize - kMaxComment : 0;
  size_t end = std::string_view::npos;
  for (size_t pos = b.size() - kEndSize;; --pos) {
    if (u32(b, pos) == kEndSig && pos + kEndSize + u16(b, pos + 20) == b.size()) {
      end = pos;
      break;
    }
    if (pos == lowest) break;
  }
  if (end == std::string_view::npos) damaged("it isn't a ZIP archive (or its end is missing).");
  const uint16_t disk = u16(b, end + 4), cd_disk = u16(b, end + 6);
  const uint16_t count_here = u16(b, end + 8), count = u16(b, end + 10);
  const uint32_t cd_size = u32(b, end + 12), cd_offset = u32(b, end + 16);
  if (count == 0xFFFF || count_here == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_offset == 0xFFFFFFFFu ||
      (end >= 20 && u32(b, end - 20) == kZip64LocatorSig)) {
    damaged("it uses ZIP64, which Vyrn files never need.");
  }
  if (disk != 0 || cd_disk != 0 || count_here != count) damaged("it is split over several disks.");
  if (static_cast<uint64_t>(cd_offset) + cd_size != end) damaged("its table of contents is damaged.");
  records_ = count;
  if (count > limits.max_entries) {
    damaged("it holds more than " + std::to_string(limits.max_entries) + " files.");
  }

  std::vector<Range> ranges;
  uint64_t total = 0;
  size_t at = cd_offset;
  for (uint16_t i = 0; i < count; ++i) {
    if (end - at < 46 || u32(b, at) != kCentralSig) damaged("its table of contents is damaged.");
    const uint16_t version_needed = u16(b, at + 6);
    const uint16_t flags = u16(b, at + 8);
    const uint16_t method = u16(b, at + 10);
    const uint32_t crc = u32(b, at + 16);
    const uint32_t csize = u32(b, at + 20), usize = u32(b, at + 24);
    const uint16_t name_len = u16(b, at + 28), extra_len = u16(b, at + 30), comment_len = u16(b, at + 32);
    const uint16_t disk_start = u16(b, at + 34);
    const uint32_t local = u32(b, at + 42);
    if (end - at - 46 < static_cast<size_t>(name_len) + extra_len + comment_len) {
      damaged("its table of contents is damaged.");
    }
    const std::string_view name = b.substr(at + 46, name_len);
    const std::string_view extra = b.substr(at + 46 + name_len, extra_len);
    at += 46 + static_cast<size_t>(name_len) + extra_len + comment_len;

    check_flags(flags, method);
    if (csize == 0xFFFFFFFFu || usize == 0xFFFFFFFFu || local == 0xFFFFFFFFu) {
      damaged("it uses ZIP64, which Vyrn files never need.");
    }
    check_extra(extra, name);
    if (version_needed > 45) damaged("it needs a newer ZIP reader than Vyrn's.");
    if (disk_start != 0) damaged("it is split over several disks.");
    if (method != 0 && method != 8) {
      damaged("the file " + in_quotes(name) + " is compressed in a way Vyrn doesn't read (only stored and deflated).");
    }
    if (const std::string why = name_problem(name); !why.empty()) damaged(why);
    const bool folder = name.back() == '/';
    if (folder && usize != 0) damaged("the folder entry " + in_quotes(name) + " holds data.");
    if (usize > limits.max_entry) {
      damaged("the file " + in_quotes(name) + " is bigger than " + human_size(limits.max_entry) + ".");
    }
    total += usize;
    if (total > limits.max_total) damaged("its files add up to more than " + human_size(limits.max_total) + ".");
    if (method == 0 && csize != usize) damaged("the sizes of " + in_quotes(name) + " don't add up.");

    // The local header must say the same.
    if (local > cd_offset || cd_offset - local < 30 || u32(b, local) != kLocalSig) {
      damaged("the file " + in_quotes(name) + " is missing or damaged.");
    }
    const uint16_t l_flags = u16(b, local + 6), l_method = u16(b, local + 8);
    const uint32_t l_crc = u32(b, local + 14), l_csize = u32(b, local + 18), l_usize = u32(b, local + 22);
    const uint16_t l_name_len = u16(b, local + 26), l_extra_len = u16(b, local + 28);
    check_flags(l_flags, l_method);
    if (cd_offset - local - 30 < static_cast<size_t>(l_name_len) + l_extra_len) {
      damaged("the file " + in_quotes(name) + " is missing or damaged.");
    }
    check_extra(b.substr(local + 30 + l_name_len, l_extra_len), name);
    if (l_csize == 0xFFFFFFFFu || l_usize == 0xFFFFFFFFu) damaged("it uses ZIP64, which Vyrn files never need.");
    if (b.substr(local + 30, l_name_len) != name || l_method != method ||
        (l_flags & kFlagDescriptor) != (flags & kFlagDescriptor)) {
      damaged("the table of contents doesn't match the file " + in_quotes(name) + ".");
    }
    const bool descriptor = flags & kFlagDescriptor;
    const bool sizes_match = l_crc == crc && l_csize == csize && l_usize == usize;
    const bool sizes_deferred = descriptor && l_crc == 0 && l_csize == 0 && l_usize == 0;
    if (!sizes_match && !sizes_deferred) {
      damaged("the sizes of " + in_quotes(name) + " don't add up.");
    }
    const uint64_t data = static_cast<uint64_t>(local) + 30 + l_name_len + l_extra_len;
    if (data > cd_offset || cd_offset - data < csize) damaged("the sizes of " + in_quotes(name) + " don't add up.");
    uint64_t stop = data + csize;
    if (descriptor) {
      const size_t d = static_cast<size_t>(stop);
      const auto fits = [&](size_t n) { return cd_offset - d >= n; };
      if (fits(16) && u32(b, d) == kDescriptorSig && u32(b, d + 4) == crc && u32(b, d + 8) == csize &&
          u32(b, d + 12) == usize) {
        stop += 16;
      } else if (fits(12) && u32(b, d) == crc && u32(b, d + 4) == csize && u32(b, d + 8) == usize) {
        stop += 12;
      } else {
        damaged("the sizes of " + in_quotes(name) + " don't add up.");
      }
    }
    ranges.push_back({local, stop});
    if (i == 0) first_name_ = std::string(name);
    if (folder) continue;

    ZipEntry entry;
    entry.name = std::string(name);
    entry.method = method;
    entry.flags = flags;
    entry.crc = crc;
    entry.csize = csize;
    entry.usize = usize;
    entry.local_offset = local;
    entry.data_offset = data;
    entries_.push_back(std::move(entry));
  }
  if (at != end) damaged("its table of contents is damaged.");

  // No two entries may share bytes (overlapping entries are a zip bomb technique).
  std::sort(ranges.begin(), ranges.end(), [](const Range& x, const Range& y) { return x.begin < y.begin; });
  for (size_t i = 1; i < ranges.size(); ++i) {
    if (ranges[i].begin < ranges[i - 1].end) damaged("two of its files share the same bytes (a zip bomb trick).");
  }

  // Names must stay different on systems that ignore upper and lower case, and no file may also be a folder.
  std::set<std::string> files;
  for (const ZipEntry& e : entries_) {
    if (!files.insert(fold_case(e.name)).second) {
      damaged("two files are called " + in_quotes(e.name) + " (ignoring upper and lower case).");
    }
  }
  for (const ZipEntry& e : entries_) {
    const std::string folded = fold_case(e.name);
    for (size_t slash = folded.find('/'); slash != std::string::npos; slash = folded.find('/', slash + 1)) {
      if (files.count(folded.substr(0, slash))) {
        damaged("the name " + in_quotes(e.name.substr(0, slash)) + " is used for a file and for a folder.");
      }
    }
  }
}

const ZipEntry* ZipReader::find(std::string_view name) const {
  for (const ZipEntry& e : entries_) {
    if (e.name == name) return &e;
  }
  return nullptr;
}

std::string ZipReader::read(const ZipEntry& e) const {
  const std::string_view src = bytes_.substr(static_cast<size_t>(e.data_offset), static_cast<size_t>(e.csize));
  std::string out;
  if (e.method == 0) {
    out.assign(src.data(), src.size());
  } else {
    // One spare byte: a stream that wants to write more than the declared size is caught, never followed.
    out.resize(static_cast<size_t>(e.usize) + 1);
    z_stream zs{};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) damaged("Vyrn ran out of memory while reading it.");
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(src.data()));
    zs.avail_in = static_cast<uInt>(src.size());
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());
    const int rc = inflate(&zs, Z_FINISH);
    const uLong produced = zs.total_out;
    const uInt left_in = zs.avail_in;
    inflateEnd(&zs);
    if (rc != Z_STREAM_END || produced != e.usize || left_in != 0) {
      damaged("the file " + in_quotes(e.name) + " is damaged (its size or its compressed data is wrong).");
    }
    out.resize(static_cast<size_t>(e.usize));
  }
  if (crc32_of(out) != e.crc) damaged("the file " + in_quotes(e.name) + " is damaged (its checksum doesn't match).");
  return out;
}

// ---- writing ----------------------------------------------------------------------------------------------

ZipWriter::ZipWriter(Sink sink, int64_t unix_time) : sink_(std::move(sink)) {
  std::tm tm{};
  const std::time_t t = static_cast<std::time_t>(unix_time);
  platform::utc_time(t, tm);
  const int year = std::clamp(tm.tm_year + 1900, 1980, 2107);
  dos_date_ = static_cast<uint16_t>(((year - 1980) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
  dos_time_ = static_cast<uint16_t>((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
}

void ZipWriter::put(const std::string& bytes) {
  sink_(bytes.data(), bytes.size());
  offset_ += bytes.size();
}

void ZipWriter::add(const std::string& name, std::string_view data, bool deflate) {
  if (finished_) throw std::logic_error("ZipWriter::add after finish");
  if (const std::string why = name_problem(name); !why.empty() || name.back() == '/') {
    throw ZipError("Vyrn tried to write a bad name into the package (" + shown_name(name) + ").");
  }
  if (data.size() >= 0xFFFFFFFFull || offset_ >= 0xFFFFFFFFull) throw ZipError("it would be bigger than 4 GB.");
  std::string packed;
  std::string_view body = data;
  if (deflate) {
    z_stream zs{};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
      throw ZipError("Vyrn ran out of memory while compressing it.");
    }
    packed.resize(deflateBound(&zs, static_cast<uLong>(data.size())) + 16);
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    zs.avail_in = static_cast<uInt>(data.size());
    zs.next_out = reinterpret_cast<Bytef*>(packed.data());
    zs.avail_out = static_cast<uInt>(packed.size());
    const int rc = ::deflate(&zs, Z_FINISH);
    const uLong n = zs.total_out;
    deflateEnd(&zs);
    if (rc != Z_STREAM_END) throw ZipError("Vyrn couldn't compress " + shown_name(name) + ".");
    packed.resize(n);
    body = packed;
  }
  Record r;
  r.name = name;
  r.method = deflate ? 8 : 0;
  r.crc = crc32_of(data);
  r.csize = static_cast<uint32_t>(body.size());
  r.usize = static_cast<uint32_t>(data.size());
  r.offset = static_cast<uint32_t>(offset_);

  std::string header;
  put32(header, kLocalSig);
  put16(header, kVersionNeeded);
  put16(header, kFlagUtf8);
  put16(header, r.method);
  put16(header, dos_time_);
  put16(header, dos_date_);
  put32(header, r.crc);
  put32(header, r.csize);
  put32(header, r.usize);
  put16(header, static_cast<uint16_t>(name.size()));
  put16(header, 0);
  header += name;
  put(header);
  sink_(body.data(), body.size());
  offset_ += body.size();
  records_.push_back(std::move(r));
}

void ZipWriter::finish() {
  if (finished_) return;
  finished_ = true;
  if (records_.size() > 0xFFFE || offset_ >= 0xFFFFFFFFull) throw ZipError("it would need ZIP64.");
  const uint64_t cd_offset = offset_;
  std::string cd;
  for (const Record& r : records_) {
    put32(cd, kCentralSig);
    put16(cd, kMadeByUnix);
    put16(cd, kVersionNeeded);
    put16(cd, kFlagUtf8);
    put16(cd, r.method);
    put16(cd, dos_time_);
    put16(cd, dos_date_);
    put32(cd, r.crc);
    put32(cd, r.csize);
    put32(cd, r.usize);
    put16(cd, static_cast<uint16_t>(r.name.size()));
    put16(cd, 0);   // extra
    put16(cd, 0);   // comment
    put16(cd, 0);   // disk
    put16(cd, 0);   // internal attributes
    put32(cd, kRegularFile0644);
    put32(cd, r.offset);
    cd += r.name;
  }
  if (cd_offset + cd.size() >= 0xFFFFFFFFull) throw ZipError("it would need ZIP64.");
  put(cd);
  std::string end;
  put32(end, kEndSig);
  put16(end, 0);
  put16(end, 0);
  put16(end, static_cast<uint16_t>(records_.size()));
  put16(end, static_cast<uint16_t>(records_.size()));
  put32(end, static_cast<uint32_t>(cd.size()));
  put32(end, static_cast<uint32_t>(cd_offset));
  put16(end, 0);
  put(end);
}

}  // namespace vyrn::package_detail
