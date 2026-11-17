// Brains in <project>/brains/ (vyrn::list_brains) and the summary of one brain folder.
//
// The checkpoint layout is the core's (core/src/brain/checkpoint.cpp, read-only here because it needs torch):
// brains/<id>/<which>/current names the live "save-<n>" folder, which holds model.pt (+ optim.pt) and brain.json.
// When `current` is missing or its save is damaged, the newest other save that has both files is used, the same
// fallback the core uses when it loads a brain.
#include "platform/system.h"
#include "project/internal.h"
#include "util/files.h"
#include "util/settings.h"
#include "vyrn/project.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

namespace vyrn {

namespace project_detail {

namespace {

using nlohmann::json;

constexpr size_t kMaxMetaBytes = 16u << 20;   // brain.json is a few KB; anything huge is not ours

// "save-12" -> 12, anything else -> -1.
int64_t save_number(const std::string& name) {
  const std::string prefix = "save-";
  if (name.size() <= prefix.size() || name.size() > prefix.size() + 15 || name.compare(0, prefix.size(), prefix)) {
    return -1;
  }
  int64_t n = 0;
  for (size_t i = prefix.size(); i < name.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(name[i]))) return -1;
    n = n * 10 + (name[i] - '0');
  }
  return n;
}

bool read_small_file(const fs::path& path, std::string& out) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  if (ec || size > kMaxMetaBytes) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream text;
  text << in.rdbuf();
  if (in.bad()) return false;
  out = text.str();
  return true;
}

// The save folders to try, the live one first, then the others newest first.
std::vector<fs::path> save_candidates(const fs::path& which_dir) {
  std::vector<fs::path> out;
  std::string live;
  if (read_small_file(which_dir / "current", live)) {
    while (!live.empty() && std::isspace(static_cast<unsigned char>(live.back()))) live.pop_back();
    if (save_number(live) >= 0) out.push_back(which_dir / live);
  }
  std::vector<std::pair<int64_t, fs::path>> saves;
  std::error_code ec;
  for (fs::directory_iterator it(which_dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    const int64_t n = save_number(name);
    std::error_code type_ec;
    if (n >= 0 && name != live && it->is_directory(type_ec)) saves.emplace_back(n, it->path());
  }
  std::sort(saves.begin(), saves.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  for (auto& save : saves) out.push_back(std::move(save.second));
  return out;
}

struct SaveMeta {
  json j;
  int64_t epoch = 0;     // when it was saved (0 if unknown)
  std::string local;     // "YYYY-MM-DD HH:MM" ("" if unknown)
};

// "2026-11-13T15:30:12" (local time, written by the core) -> seconds since the epoch.
bool parse_local_iso(const std::string& text, int64_t& epoch, std::string& local) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
  char tail = 0;
  if (std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%c", &y, &mo, &d, &h, &mi, &s, &tail) != 6) return false;
  if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) return false;
  std::tm tm{};
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = s;
  tm.tm_isdst = -1;
  const std::time_t t = std::mktime(&tm);
  if (t == static_cast<std::time_t>(-1)) return false;
  epoch = static_cast<int64_t>(t);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d", y, mo, d, h, mi);
  local = buf;
  return true;
}

std::string format_local(int64_t epoch) {
  const std::time_t t = static_cast<std::time_t>(epoch);
  std::tm tm{};
  if (!platform::local_time(t, tm)) return "";
  char buf[32];
  const size_t n = std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
  return std::string(buf, n);
}

// brain.json of the save the core would load first for brains/<id>/<which>, or nullopt.
std::optional<SaveMeta> read_save_meta(const fs::path& which_dir) {
  std::error_code ec;
  if (!fs::is_directory(which_dir, ec)) return std::nullopt;
  for (const fs::path& dir : save_candidates(which_dir)) {
    std::string text;
    if (!fs::is_regular_file(dir / "model.pt", ec) || !read_small_file(dir / "brain.json", text)) continue;
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) continue;
    SaveMeta meta;
    meta.j = std::move(j);
    const auto saved = meta.j.find("saved");
    if (saved == meta.j.end() || !saved->is_string() ||
        !parse_local_iso(saved->get<std::string>(), meta.epoch, meta.local)) {
      if (auto t = mtime_seconds(dir / "brain.json")) {
        meta.epoch = *t;
        meta.local = format_local(*t);
      }
    }
    return meta;
  }
  return std::nullopt;
}

std::optional<double> number_at(const json& j, const char* key) {
  const auto it = j.find(key);
  if (it == j.end() || !it->is_number()) return std::nullopt;
  return it->get<double>();
}

std::optional<double> counter(const json& j, const char* key) {
  const auto it = j.find("counters");
  if (it == j.end() || !it->is_object()) return std::nullopt;
  return number_at(*it, key);
}

}  // namespace

BrainSummary read_brain_summary(const fs::path& brain_dir) {
  BrainSummary out;
  const std::optional<SaveMeta> metas[] = {read_save_meta(brain_dir / "latest"), read_save_meta(brain_dir / "best")};
  for (const auto& meta : metas) {
    if (!meta) continue;
    out.any_save = true;
    if (const auto best = number_at(meta->j, "best_mean20")) {
      out.best_mean20 = out.has_best ? std::max(out.best_mean20, *best) : *best;
      out.has_best = true;
    }
    if (const auto seconds = counter(meta->j, "seconds")) out.time_trained_s = std::max(out.time_trained_s, *seconds);
    if (const auto ticks = counter(meta->j, "ticks")) {
      out.ticks = std::max(out.ticks, static_cast<int64_t>(std::max(0.0, *ticks)));
    }
    if (meta->epoch > out.updated_epoch) {   // 0 = unknown (then `local` is "" too)
      out.updated_epoch = meta->epoch;
      out.updated = meta->local;
    }
  }
  return out;
}

}  // namespace project_detail

std::vector<BrainInfo> list_brains(const std::string& project) {
  namespace fs = std::filesystem;
  using project_detail::BrainSummary;

  const util::ProjectSettings settings = util::read_settings(project);
  struct Item {
    BrainInfo info;
    int64_t when = 0;
  };
  std::vector<Item> items;
  std::error_code ec;
  for (fs::directory_iterator it(fs::path(project) / "brains", ec), end; !ec && it != end; it.increment(ec)) {
    const std::string id = it->path().filename().string();
    std::error_code type_ec;
    if (!util::is_safe_name(id) || !it->is_directory(type_ec)) continue;   // also skips hidden folders
    const BrainSummary summary = project_detail::read_brain_summary(it->path());
    const bool active = id == settings.brain;
    if (!summary.any_save && !active) continue;   // a folder reserved by a run that never saved
    Item item;
    item.info.id = id;
    item.info.active = active;
    item.info.has_best = summary.has_best;
    item.info.best_mean20 = summary.best_mean20;
    item.info.time_trained_s = summary.time_trained_s;
    item.info.ticks = summary.ticks;
    item.info.updated = summary.updated;
    item.when = summary.updated_epoch;
    if (item.when == 0) item.when = project_detail::mtime_seconds(it->path()).value_or(0);
    items.push_back(std::move(item));
  }
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
    if (a.info.active != b.info.active) return a.info.active;
    if (a.when != b.when) return a.when > b.when;
    return a.info.id > b.info.id;   // ids are local times: a later one is newer
  });
  std::vector<BrainInfo> out;
  out.reserve(items.size());
  for (auto& item : items) out.push_back(std::move(item.info));
  return out;
}

}  // namespace vyrn
