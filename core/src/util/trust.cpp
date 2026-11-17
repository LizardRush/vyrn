#include "util/trust.h"

#include <nlohmann/json.hpp>

#include "platform/files.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

namespace vyrn::util {

namespace {

namespace fs = std::filesystem;

constexpr size_t kMaxMarkerBytes = 64u << 10;
constexpr size_t kMaxFileName = 255;

struct Marker {
  bool present = false;   // something called .vyrn-imported is there
  bool trusted = false;   // ... and it is a readable JSON object whose "trusted" is true
  std::string file;       // the shared file it came from ("" when unknown)
};

Marker read_marker(const std::string& dir) {
  Marker m;
  const std::string path = (fs::path(dir) / kTrustMarkerName).string();
  int err = 0;
  platform::File in = platform::File::open_read(path, /*no_follow=*/true, err);
  if (!in) {
    platform::FileInfo info;
    // ENOENT: nothing there. Anything else (a symlink, no permission, ...) is a marker Vyrn can't read: untrusted.
    m.present = !((err == ENOENT || err == ENOTDIR) && platform::file_info(path, false, info) != 0);
    return m;
  }
  m.present = true;
  platform::FileInfo info;
  if (in.info(info) != 0 || !info.is_regular() || info.size > kMaxMarkerBytes) return m;
  std::string text(static_cast<size_t>(info.size), '\0');
  size_t got = 0;
  while (got < text.size()) {
    size_t n = 0;
    if (in.read(text.data() + got, text.size() - got, n) != 0 || n == 0) break;
    got += n;
  }
  in.close();
  text.resize(got);
  const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) return m;
  const auto trusted = j.find("trusted");
  m.trusted = trusted != j.end() && trusted->is_boolean() && trusted->get<bool>();
  const auto file = j.find("file");
  if (file != j.end() && file->is_string()) m.file = file->get<std::string>();
  return m;
}

// A file name fit for a sentence: no control characters, none of the invisible characters that change the
// direction of text (which could make "evil.py.vrlp" read as something else; the same set package/text.cpp
// removes), at most 255 bytes cut at a character boundary.
std::string printable_name(const std::string& name) {
  const auto at = [&](size_t i) { return i < name.size() ? static_cast<unsigned char>(name[i]) : 0; };
  std::string out;
  for (size_t i = 0; i < name.size(); ++i) {
    const unsigned char c = at(i), b = at(i + 1), d = at(i + 2);
    if (c < 0x20 || c == 0x7f) continue;
    if (c == 0xC2 && b >= 0x80 && b <= 0x9F) {   // U+0080..U+009F
      ++i;
      continue;
    }
    if (c == 0xD8 && b == 0x9C) {   // U+061C
      ++i;
      continue;
    }
    if ((c == 0xE2 && b == 0x80 && ((d >= 0xAA && d <= 0xAE) || d == 0x8E || d == 0x8F)) ||   // U+202A..E, 200E/F
        (c == 0xE2 && b == 0x81 && d >= 0xA6 && d <= 0xA9) ||                                  // U+2066..U+2069
        (c == 0xEF && b == 0xBB && d == 0xBF)) {                                               // U+FEFF
      i += 2;
      continue;
    }
    out += static_cast<char>(c);
  }
  if (out.size() > kMaxFileName) {
    size_t cut = kMaxFileName;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
    out.resize(cut);
  }
  return out;
}

// A path as one shell word: as it is when that is safe, else in single quotes.
std::string shell_word(const std::string& s) {
  bool plain = !s.empty();
  for (const char ch : s) {
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                    std::strchr("/._-+,:@%", ch) != nullptr;
    if (!ok || ch == '\0') plain = false;
  }
  if (plain) return s;
  std::string out = "'";
  for (const char ch : s) out += ch == '\'' ? std::string("'\\''") : std::string(1, ch);
  return out + "'";
}

// The folder and every folder above it, nearest first: as given (made absolute), then again with symlinks
// resolved, so a folder inside an imported project is found whichever way it was named.
std::vector<fs::path> folder_and_parents(const std::string& dir) {
  std::vector<fs::path> out;
  const auto add_chain = [&](fs::path p) {
    if (p.has_relative_path() && p.filename().empty()) p = p.parent_path();   // "a/b/" -> "a/b"
    for (;;) {
      if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
      const fs::path up = p.parent_path();
      if (up.empty() || up == p) break;
      p = up;
    }
  };
  std::error_code ec;
  const fs::path absolute = fs::absolute(dir, ec);
  if (ec) {
    out.emplace_back(dir);
    return out;
  }
  add_chain(absolute.lexically_normal());
  const fs::path real = fs::weakly_canonical(absolute, ec);
  if (!ec) add_chain(real);
  return out;
}

// The nearest folder (`dir` itself or one above it) whose marker isn't a trusted one. A folder inside an imported
// project belongs to that project: a .vrlp can hold a sub-folder that looks like a project of its own (vyrn.json,
// inputs.py, ...), and it must not run before the person trusts the whole project.
std::optional<std::pair<fs::path, Marker>> untrusted_owner(const std::string& dir) {
  for (const fs::path& folder : folder_and_parents(dir)) {
    Marker m = read_marker(folder.string());
    if (m.present && !m.trusted) return std::make_pair(folder, std::move(m));
  }
  return std::nullopt;
}

}  // namespace

bool project_trusted(const std::string& dir) { return !untrusted_owner(dir).has_value(); }

std::string untrusted_project_folder(const std::string& dir) {
  const auto owner = untrusted_owner(dir);
  return owner ? owner->first.string() : std::string();
}

std::string not_trusted_message(const std::string& dir) {
  const auto owner = untrusted_owner(dir);
  if (!owner) return "";
  const Marker& m = owner->second;
  const std::string folder = owner->first.string();
  const std::string file = printable_name(m.file);
  return "This project came from " + (file.empty() ? std::string("a shared file") : file) +
         ". Its Python code hasn't run on this computer yet: read its scripts, then trust it in Vyrn (or run: "
         "vyrn-cli trust " +
         shell_word(folder) + ").";
}

}  // namespace vyrn::util
