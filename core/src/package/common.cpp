// Shared helpers of the package code: errors, time, files (.part writing), names, the vyrn.json allow-list and
// the extra brain-shape bounds (docs/dev/package.md sections 2, 4 and 5).
#include "package/internal.h"
#include "package/text.h"
#include "platform/files.h"
#include "platform/system.h"
#include "util/files.h"
#include "vyrn/project.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <set>

namespace vyrn::package_detail {

namespace fs = std::filesystem;

namespace test_hooks {
std::function<void(const char* stage)> export_stage;
}  // namespace test_hooks

void fail(const std::string& message) { throw PackageError(message); }

// ---- time ----------------------------------------------------------------------------------------------------

std::string utc_iso(int64_t unix_time) {
  const std::time_t t = static_cast<std::time_t>(unix_time);
  std::tm tm{};
  platform::utc_time(t, tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::string local_minutes(int64_t unix_time) {
  const std::time_t t = static_cast<std::time_t>(unix_time);
  std::tm tm{};
  platform::local_time(t, tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
  return buf;
}

bool valid_utc_iso(const std::string& text) {
  if (text.size() != 20) return false;
  static const char* pattern = "dddd-dd-ddTdd:dd:ddZ";
  for (size_t i = 0; i < 20; ++i) {
    const char p = pattern[i];
    if (p == 'd' ? (text[i] < '0' || text[i] > '9') : text[i] != p) return false;
  }
  const int month = std::stoi(text.substr(5, 2)), day = std::stoi(text.substr(8, 2));
  const int hour = std::stoi(text.substr(11, 2)), minute = std::stoi(text.substr(14, 2));
  const int second = std::stoi(text.substr(17, 2));
  return month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour <= 23 && minute <= 59 && second <= 60;
}

// ---- files ---------------------------------------------------------------------------------------------------

std::string base_name(const std::string& path) {
  std::string name = fs::path(path).filename().string();
  return name.empty() ? path : name;
}

std::string kind_for_extension(const std::string& path) {
  const std::string lower = lower_ascii(base_name(path));
  const auto ends = [&](const std::string& ext) {
    return lower.size() > ext.size() && lower.compare(lower.size() - ext.size(), ext.size(), ext) == 0;
  };
  if (ends(kProjectExt)) return "project";
  if (ends(kBrainExt)) return "brain";
  return "";
}

std::string read_package_bytes(const std::string& path, uint64_t max_bytes) {
  const std::string name = base_name(path);
  int err = 0;
  platform::File file = platform::File::open_read(path, false, err);   // a pipe never blocks the reader
  if (!file) {
    if (err == ENOENT || err == ENOTDIR) fail("There is no file at " + path + ".");
    if (err == EACCES || err == EPERM) fail("Vyrn isn't allowed to read " + path + ".");
    fail("Vyrn can't read " + path + " (" + platform::error_text(err) + ").");
  }
  platform::FileInfo st;
  if ((err = file.info(st)) != 0) fail("Vyrn can't read " + path + " (" + platform::error_text(err) + ").");
  if (st.is_directory()) fail(path + " is a folder, not a Vyrn file.");
  if (!st.is_regular()) fail(path + " isn't a regular file, so Vyrn won't open it.");
  if (st.size > max_bytes) {
    fail(name + " is too big to be a Vyrn file (more than " + human_size(max_bytes) + ").");
  }
  std::string bytes(static_cast<size_t>(st.size), '\0');
  size_t got = 0;
  while (got < bytes.size()) {
    size_t n = 0;
    if ((err = file.read(bytes.data() + got, bytes.size() - got, n)) != 0) {
      fail("Vyrn can't read " + path + " (" + platform::error_text(err) + ").");
    }
    if (n == 0) break;
    got += n;
  }
  bytes.resize(got);   // the file shrank while reading: the checks below catch a cut-off archive
  return bytes;
}

PartFile::PartFile(const std::string& out, bool overwrite) : out_(out), part_(out + ".part"), overwrite_(overwrite) {
  if (out.empty()) fail("Please choose where to save the file.");
  platform::FileInfo st;
  if (platform::file_info(out, false, st) == 0) {
    if (st.is_directory()) fail(out + " is a folder. Please choose a file name.");
    if (!overwrite) {
      fail("There is already a file called " + base_name(out) + ". Choose another name, or allow replacing it.");
    }
  }
  int err = 0;
  file_ = platform::File::create(part_, platform::File::Create::Truncate, /*no_follow=*/true, 0644, err);
  if (!file_) fail("Vyrn can't write " + part_ + " (" + platform::error_text(err) + ").");
}

PartFile::~PartFile() {
  file_.close();
  if (!done_) platform::remove_file(part_);
}

void PartFile::write(const char* data, size_t size) {
  if (const int err = file_.write_all(data, size); err != 0) {
    fail("Vyrn can't write " + out_ + " (" + platform::error_text(err) + ").");
  }
}

void PartFile::sync() {
  if (!file_) return;
  const int err = file_.sync();
  file_.close();
  if (err != 0) fail("Vyrn can't save " + out_ + " (" + platform::error_text(err) + ").");
}

void PartFile::commit() {
  sync();
  if (overwrite_) {
    if (const int err = platform::rename_replace(part_, out_); err != 0) {
      fail("Vyrn can't save " + out_ + " (" + platform::error_text(err) + ").");
    }
  } else if (const int err = platform::rename_no_replace(part_, out_, /*directory=*/false); err != 0) {
    if (err == EEXIST) {
      fail("There is already a file called " + base_name(out_) + ". Choose another name, or allow replacing it.");
    }
    fail("Vyrn can't save " + out_ + " (" + platform::error_text(err) + ").");
  }
  done_ = true;
  const fs::path folder = fs::path(out_).has_parent_path() ? fs::path(out_).parent_path() : fs::path(".");
  platform::sync_path(folder.string(), true);
}

// ---- folders and new files ------------------------------------------------------------------------------------

void make_folder(const fs::path& p) {
  if (const int err = platform::make_dir(p.string(), 0755); err != 0) {
    platform::FileInfo st;
    if (err != EEXIST || platform::file_info(p.string(), false, st) != 0 || !st.is_directory()) {
      fail("Vyrn can't create the folder " + p.string() + " (" + platform::error_text(err) + ").");
    }
  }
  platform::set_mode(p.string(), 0755);
}

void write_new_file(const fs::path& root, const std::string& rel, std::string_view data, int mode) {
  fs::path p = root;
  size_t start = 0;
  for (size_t slash = rel.find('/'); slash != std::string::npos; slash = rel.find('/', start)) {
    p /= rel.substr(start, slash - start);
    make_folder(p);
    start = slash + 1;
  }
  p /= rel.substr(start);
  if (!is_inside(root.string(), p.string())) fail("Vyrn refused to write " + shown_name(rel) + " outside its folder.");
  int err = 0;
  platform::File file =
      platform::File::create(p.string(), platform::File::Create::Exclusive, /*no_follow=*/true, mode, err);
  if (!file) fail("Vyrn can't create " + shown_name(rel) + " (" + platform::error_text(err) + ").");
  file.set_mode(mode);
  if ((err = file.write_all(data.data(), data.size())) != 0) {
    fail("Vyrn can't write " + shown_name(rel) + " (" + platform::error_text(err) + ").");
  }
  if ((err = file.sync()) != 0) fail("Vyrn can't save " + shown_name(rel) + " (" + platform::error_text(err) + ").");
}

void sync_folder(const fs::path& dir) { platform::sync_path(dir.string(), true); }

// ---- names ---------------------------------------------------------------------------------------------------

bool valid_brain_id(const std::string& id) {
  if (id.empty() || id.size() > 64) return false;
  const auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
  if (!alnum(id[0])) return false;
  return std::all_of(id.begin(), id.end(), [&](char c) { return alnum(c) || c == '_' || c == '-'; });
}

bool is_agent_file(const std::string& rel_path) {
  const std::string name = lower_ascii(base_name(rel_path));
  return name == "agents.md" || name == "claude.md" || name == "claude.local.md" || name == "gemini.md";
}

std::string project_file_skip_reason(const std::string& rel_path) {
  std::vector<std::string> parts;
  size_t start = 0;
  for (size_t slash = rel_path.find('/'); slash != std::string::npos; slash = rel_path.find('/', start)) {
    parts.push_back(rel_path.substr(start, slash - start));
    start = slash + 1;
  }
  parts.push_back(rel_path.substr(start));
  const std::string file = lower_ascii(parts.back());
  const std::string top = lower_ascii(parts[0]);
  if (parts.size() > 1 && (top == "brains" || top == "runs")) {
    return "brains/ and runs/ are never shared as files (a brain goes in as a .vrl).";
  }
  for (const std::string& part : parts) {
    if (part == "__pycache__") return "Python's cache is never shared.";
    if (!part.empty() && part[0] == '.') return "hidden files are never shared.";
  }
  if (static_cast<int>(parts.size()) - 1 > kMaxProjectDepth) return "it is more than 8 folders deep.";
  if (is_agent_file(rel_path)) return "Vyrn writes its own AI agent instructions instead.";
  static const std::set<std::string> kExtensions = {".py",  ".json", ".txt", ".md",  ".csv", ".tsv", ".png", ".jpg",
                                                    ".jpeg", ".gif", ".bmp", ".webp", ".wav", ".ogg"};
  static const std::set<std::string> kNames = {"license", "readme", "copying"};
  const size_t dot = file.rfind('.');
  if (dot != std::string::npos && dot > 0 && kExtensions.count(file.substr(dot))) return "";
  if (kNames.count(file)) return "";
  return "Vyrn shares only scripts, pictures, sounds and text files.";
}

namespace {

// vyrn.json "advanced" in a shared project: the brain settings Vyrn knows (core/src/brain/dqn.cpp visit_fields)
// with their ranges. The ones that size memory or work per step stop near Vyrn's own defaults (buffer 100,000,
// batch 256, frames 8, 4 updates per tick; the core accepts up to 10^8, 65,536, 16 and 64), so a shared project
// can't make Train allocate gigabytes. A person can still raise them in their own copy.
struct SharedAdvanced {
  const char* name;
  bool whole;
  double lo, hi;
};
constexpr SharedAdvanced kSharedAdvanced[] = {
    {"batch", true, 1, 256},           {"lr", false, 1e-9, 1.0},           {"adam_eps", false, 1e-12, 1.0},
    {"target_every", true, 1, 1e9},    {"n_step", true, 1, 100},           {"gamma", false, 0, 1},
    {"grad_clip", false, 1e-6, 1e6},   {"learning_starts", true, 0, 1e12}, {"updates_per_tick", false, 0, 4},
    {"explore_ticks", true, 0, 1e12},  {"eps_start", false, 0, 1},         {"eps_end", false, 0, 1},
    {"eps_watch", false, 0, 1},        {"buffer", true, 100, 100000},      {"frames", true, 1, 8},
};

ordered_json allowed_advanced(const ordered_json& advanced, std::vector<std::string>* dropped) {
  ordered_json out = ordered_json::object();
  for (auto it = advanced.begin(); it != advanced.end(); ++it) {
    const SharedAdvanced* field = nullptr;
    for (const SharedAdvanced& f : kSharedAdvanced) {
      if (it.key() == f.name) field = &f;
    }
    const ordered_json& v = it.value();
    const double x = v.is_number() ? v.get<double>() : 0.0;
    const bool ok = field && v.is_number() && std::isfinite(x) && x >= field->lo && x <= field->hi &&
                    (!field->whole || x == std::floor(x));
    if (ok) {
      out[it.key()] = v;
    } else if (dropped) {
      dropped->push_back("advanced." + it.key());
    }
  }
  return out;
}

// "link": true/false, or {"port": 1024-65535, "lockstep": bool} (docs/dev/link.md).
ordered_json allowed_link(const ordered_json& link, std::vector<std::string>* dropped) {
  if (link.is_boolean()) return link;
  ordered_json out = ordered_json::object();
  for (auto it = link.begin(); it != link.end(); ++it) {
    const ordered_json& v = it.value();
    const bool ok = (it.key() == "port" && v.is_number_integer() && v.get<int64_t>() >= 1024 &&
                     v.get<int64_t>() <= 65535) ||
                    (it.key() == "lockstep" && v.is_boolean());
    if (ok) {
      out[it.key()] = v;
    } else if (dropped) {
      dropped->push_back("link." + it.key());
    }
  }
  return out;
}

std::vector<std::string> home_folders() { return platform::home_dirs(); }

bool ends_path(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '"' || c == '\''; }

}  // namespace

std::string shareable_text(const std::string& text) {
  std::string out = text;
  for (const std::string& home : home_folders()) {
    if (home.size() <= 1) continue;
    for (size_t at = out.find(home); at != std::string::npos; at = out.find(home, at)) {
      const size_t end = at + home.size();
      if (end == out.size() || out[end] == '/' || ends_path(out[end])) {
        out.replace(at, home.size(), "~");
        at += 1;
      } else {
        at = end;
      }
    }
  }
  // Absolute paths ("/...", "~/...", "~ada/...") that start a word, follow '=' or a quote, or follow "file://".
  std::string result;
  size_t i = 0;
  while (i < out.size()) {
    const char c = out[i];
    const char before = i > 0 ? out[i - 1] : ' ';
    const bool after_file_url = i >= 7 && out.compare(i - 7, 7, "file://") == 0;
    if ((c != '/' && c != '~') || !(ends_path(before) || before == '=' || after_file_url)) {
      result += c;
      ++i;
      continue;
    }
    size_t end = i;
    while (end < out.size() && !ends_path(out[end])) ++end;
    std::string path = out.substr(i, end - i);
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos) {
      result += c == '~' ? "~" : path;   // "~ada" names a home folder too
    } else {
      result += path.size() == 1 ? path : path.substr(slash + 1);
    }
    i = end;
  }
  return result;
}

ordered_json allowed_settings(const ordered_json& settings, std::vector<std::string>* dropped) {
  ordered_json out = ordered_json::object();
  if (!settings.is_object()) return out;
  for (auto it = settings.begin(); it != settings.end(); ++it) {
    const std::string& key = it.key();
    const ordered_json& v = it.value();
    bool keep = false;
    ordered_json value = v;
    if (key == "name" || key == "description") {
      keep = v.is_string();
      if (keep) value = clean_text(v.get<std::string>(), key == "description" ? 2000 : 200, key == "description");
    } else if (key == "template") {
      keep = v.is_string() && (v.get<std::string>().empty() || util::is_safe_name(v.get<std::string>()));
    } else if (key == "brain") {
      keep = v.is_string() && (v.get<std::string>().empty() || valid_brain_id(v.get<std::string>()));
    } else if (key == "decisions_per_second" || key == "round_seconds") {
      keep = v.is_number() && std::isfinite(v.get<double>());
    } else if (key == "round_ticks") {
      keep = v.is_number_integer();
    } else if (key == "link") {
      keep = v.is_object() || v.is_boolean();
      if (keep) value = allowed_link(v, dropped);
    } else if (key == "advanced") {
      keep = v.is_object();
      if (keep) value = allowed_advanced(v, dropped);
    }
    if (keep) {
      out[key] = value;
    } else if (dropped) {
      dropped->push_back(key);
    }
  }
  return out;
}

// ---- brain shapes (section 4) -----------------------------------------------------------------------------

void check_shape_bounds(const brain::BrainShape& s) {
  const auto bad = [](const std::string& why) { throw std::invalid_argument(why); };
  if (s.choices.size() < 2 || s.choices.size() > 256) bad("a brain needs 2 to 256 choices.");
  std::set<std::string> unique;
  for (const std::string& c : s.choices) {
    if (!valid_utf8(c) || has_control_chars(c) || c.empty() || utf8_length(c) > 200) {
      bad("a choice name is empty, too long or has odd characters.");
    }
    if (!unique.insert(c).second) bad("two choices have the same name.");
  }
  if (s.kind == brain::ObsSpec::Kind::Picture) {
    if (s.frames < 1 || s.frames > 8) bad("it stacks " + std::to_string(s.frames) + " frames (1 to 8 are allowed).");
    if (s.in_c != 3 * s.frames) bad("its picture channels don't match its frames.");
    if (s.small_picture ? (s.in_h < 1 || s.in_h > 32 || s.in_w < 1 || s.in_w > 32)
                        : (s.in_h != 84 || s.in_w != 84)) {
      bad("its picture size doesn't fit its network.");
    }
    if (s.d != 0 || !s.inputs.empty()) bad("a picture brain can't also have numbers.");
  } else {
    if (s.d < 1 || s.d > 4096) bad("it sees " + std::to_string(s.d) + " numbers (1 to 4096 are allowed).");
    if (!s.inputs.empty() && static_cast<int>(s.inputs.size()) != s.d) bad("its numbers' names don't match.");
    for (const std::string& n : s.inputs) {
      if (!valid_utf8(n) || has_control_chars(n) || utf8_length(n) > 200) bad("a number's name is too long or odd.");
    }
    if (s.frames != 1 || s.in_c != 0 || s.in_h != 0 || s.in_w != 0 || s.small_picture) {
      bad("a number brain can't also have a picture size.");
    }
  }
}

json checked_obs(const json& obs, const brain::BrainShape& s) {
  const bool picture = s.kind == brain::ObsSpec::Kind::Picture;
  if (obs.is_object() && obs.value("kind", "") == brain::to_string(s.kind)) {
    const auto shape = obs.find("shape");
    if (picture && shape != obs.end() && shape->is_array() && shape->size() == 3 && (*shape)[2] == 3 &&
        (*shape)[0].is_number_integer() && (*shape)[1].is_number_integer()) {
      const int64_t h = (*shape)[0].get<int64_t>(), w = (*shape)[1].get<int64_t>();
      const bool small = std::max(h, w) <= 32;
      if (h >= 1 && w >= 1 && h <= 16384 && w <= 16384 && small == s.small_picture &&
          (!small || (h == s.in_h && w == s.in_w))) {
        return json{{"kind", "picture"}, {"shape", {h, w, 3}}, {"names", json::array()}};
      }
    }
  }
  if (picture) return json{{"kind", "picture"}, {"shape", {s.in_h, s.in_w, 3}}, {"names", json::array()}};
  return json{{"kind", "numbers"}, {"shape", {s.d}}, {"names", s.inputs}};
}

}  // namespace vyrn::package_detail
