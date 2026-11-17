// .vrl files: building one from a saved brain, reading and checking one (docs/dev/package.md section 4), the
// points history, and installing a checked brain into a project (section 6).
#include "package/internal.h"
#include "platform/files.h"
#include "package/json_strict.h"
#include "package/text.h"
#include "util/clock.h"
#include "util/files.h"
#include "util/settings.h"
#include "vyrn/project.h"


#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#ifndef VYRN_VERSION
#define VYRN_VERSION "0.0.0"
#endif

namespace vyrn::package_detail {

namespace fs = std::filesystem;

namespace {

constexpr int64_t kMaxCounter = 1'000'000'000'000'000;   // 10^15
constexpr int64_t kMaxUpdates = 1'000'000'000'000;       // 10^12
constexpr double kMaxSeconds = 1e9;

std::string without_period(std::string text) {
  while (!text.empty() && text.back() == '.') text.pop_back();
  return text;
}

bool number_in(const json& v, double lo, double hi, double& out) {
  if (!v.is_number()) return false;
  out = v.get<double>();
  return std::isfinite(out) && out >= lo && out <= hi;
}

}  // namespace

void bad_manifest(const std::string& label, const std::string& detail) {
  fail("Vyrn can't open " + label + ": its description is damaged (" + without_period(detail) + ").");
}

bool int_in(const json& v, int64_t lo, int64_t hi, int64_t& out) {
  if (v.is_number_unsigned()) {
    const uint64_t u = v.get<uint64_t>();
    if (u > static_cast<uint64_t>(hi)) return false;
    out = static_cast<int64_t>(u);
    return out >= lo;
  }
  if (v.is_number_integer()) {
    out = v.get<int64_t>();
    return out >= lo && out <= hi;
  }
  return false;
}

std::string manifest_text(const json& m, const char* key, size_t max_chars, const std::string& label,
                          bool newlines) {
  const auto it = m.find(key);
  if (it == m.end() || it->is_null()) return "";
  if (!it->is_string()) bad_manifest(label, std::string("\"") + key + "\" isn't text");
  return clean_text(it->get<std::string>(), max_chars, newlines);
}

void check_format_version(const json& m, const std::string& label) {
  const auto version = m.find("format_version");
  if (version == m.end() || !version->is_number()) bad_manifest(label, "\"format_version\" is missing");
  if (version->get<double>() >= 2) fail(label + " was made by a newer version of Vyrn. Please update Vyrn to open it.");
  if (!(version->get<double>() >= 1)) bad_manifest(label, "\"format_version\" is wrong");
}

std::string mimetype_of(const ZipReader& zip, const std::string& label) {
  const ZipEntry* e = zip.find("mimetype");
  if (!e || zip.first_name() != "mimetype" || e->method != 0 || e->usize > 100) {
    fail("Vyrn can't open " + label + ": it isn't a Vyrn file.");
  }
  return zip.read(*e);
}

bool plausible_png(std::string_view png) {
  static const unsigned char kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  if (png.size() < 33 || std::memcmp(png.data(), kSignature, 8) != 0) return false;
  const auto be32 = [&](size_t at) {
    uint32_t v = 0;
    for (size_t i = 0; i < 4; ++i) v = (v << 8) | static_cast<unsigned char>(png[at + i]);
    return v;
  };
  if (be32(8) != 13 || png.substr(12, 4) != "IHDR") return false;
  const uint32_t w = be32(16), h = be32(20);
  if (w == 0 || h == 0 || w > 4096 || h > 4096) return false;
  const int depth = static_cast<unsigned char>(png[24]), color = static_cast<unsigned char>(png[25]);
  const bool ok_depth = (color == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)) ||
                        (color == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8)) ||
                        ((color == 2 || color == 4 || color == 6) && (depth == 8 || depth == 16));
  return ok_depth && be32(29) == crc32_of(png.substr(12, 17));
}

namespace {

// The shape object, with every number checked before from_json converts it.
brain::BrainShape shape_field(const json& m, const std::string& label) {
  const auto it = m.find("shape");
  if (it == m.end() || !it->is_object()) bad_manifest(label, "\"shape\" is missing");
  for (const char* key : {"in_c", "in_h", "in_w", "d", "frames"}) {
    const auto v = it->find(key);
    int64_t n = 0;
    if (v != it->end() && !int_in(*v, 0, 1'000'000, n)) bad_manifest(label, std::string("shape.") + key + " is wrong");
  }
  const auto small = it->find("small_picture");
  if (small != it->end() && !small->is_boolean()) bad_manifest(label, "shape.small_picture is wrong");
  for (const char* key : {"choices", "inputs"}) {
    const auto v = it->find(key);
    if (v == it->end()) continue;
    if (!v->is_array() || v->size() > 4096) bad_manifest(label, std::string("shape.") + key + " is wrong");
    for (const json& s : *v) {
      if (!s.is_string()) bad_manifest(label, std::string("shape.") + key + " is wrong");
    }
  }
  brain::BrainShape shape;
  try {
    shape = it->get<brain::BrainShape>();
  } catch (const std::exception& e) {
    bad_manifest(label, "its shape can't be read");
  }
  try {
    check_shape_bounds(shape);
  } catch (const std::invalid_argument& e) {
    bad_manifest(label, e.what());
  }
  return shape;
}

json labels_for(const json& v, const std::vector<std::string>& choices) {
  json out = json::array();
  if (v.is_array() && v.size() == choices.size() &&
      std::all_of(v.begin(), v.end(), [](const json& s) { return s.is_string(); })) {
    for (const json& s : v) out.push_back(clean_text(s.get<std::string>(), 200));
    return out;
  }
  for (const std::string& c : choices) out.push_back(c);
  return out;
}

json effects_for(const json& v, size_t n) {
  json out = json::array();
  if (v.is_array() && v.size() == n && std::all_of(v.begin(), v.end(), [](const json& s) { return s.is_string(); })) {
    for (const json& s : v) out.push_back(clean_text(s.get<std::string>(), 200));
    return out;
  }
  for (size_t i = 0; i < n; ++i) out.push_back("");
  return out;
}

// The number statistics: null, or {"count", "mean": [d], "var": [d]} with finite values. False when damaged.
bool normalizer_for(const json& v, const brain::BrainShape& shape, json& out) {
  out = nullptr;
  if (v.is_null()) return true;
  if (shape.kind == brain::ObsSpec::Kind::Picture || !v.is_object()) return false;
  double count = 0;
  const auto c = v.find("count");
  const auto mean = v.find("mean");
  const auto var = v.find("var");
  if (v.size() != 3 || c == v.end() || mean == v.end() || var == v.end()) return false;
  if (!number_in(*c, 0, 1e300, count) || !mean->is_array() || !var->is_array()) return false;
  if (mean->size() != static_cast<size_t>(shape.d) || var->size() != static_cast<size_t>(shape.d)) return false;
  std::vector<double> m, s;
  for (const json& x : *mean) {
    double d = 0;
    if (!number_in(x, -1e300, 1e300, d)) return false;
    m.push_back(d);
  }
  for (const json& x : *var) {
    double d = 0;
    if (!number_in(x, 0, 1e300, d)) return false;
    s.push_back(d);
  }
  out = json{{"count", count}, {"mean", m}, {"var", s}};
  return true;
}

// The "history" of a manifest or of a saved brain.json: at most kMaxHistory [round, mean20] pairs, rounds strictly
// increasing. False (and nothing in out) when it is malformed; a missing or null history is empty.
bool read_history(const json& m, std::vector<HistoryPoint>& out) {
  out.clear();
  if (!m.is_object()) return true;
  const auto it = m.find("history");
  if (it == m.end() || it->is_null()) return true;
  if (!it->is_array() || it->size() > kMaxHistory) return false;
  for (const json& p : *it) {
    HistoryPoint point;
    if (!p.is_array() || p.size() != 2 || !int_in(p[0], 1, kMaxCounter, point.round) ||
        !number_in(p[1], -1e300, 1e300, point.mean20) || (!out.empty() && point.round <= out.back().round)) {
      out.clear();
      return false;
    }
    out.push_back(point);
  }
  return true;
}

std::vector<HistoryPoint> history_field(const json& m, const std::string& label) {
  std::vector<HistoryPoint> out;
  if (!read_history(m, out)) bad_manifest(label, "\"history\" is wrong");
  return out;
}

BrainPackage parse_brain(std::string_view bytes, const std::string& label) {
  const ZipReader zip(bytes, ZipLimits{});
  const std::string mime = mimetype_of(zip, label);   // first, so a project file gets the helpful message
  if (mime == kProjectMime) {
    fail(label + " is a whole Vyrn project (.vrlp), not a single brain (.vrl). Open it as a project instead.");
  }
  if (mime != kBrainMime) fail("Vyrn can't open " + label + ": it isn't a Vyrn brain file.");
  if (zip.record_count() > kMaxBrainEntries) {
    fail("Vyrn can't open " + label + ": it holds more than " + std::to_string(kMaxBrainEntries) + " files.");
  }
  for (const ZipEntry& e : zip.entries()) {
    if (e.name != "mimetype" && e.name != kBrainManifest && e.name != kWeightsFile && e.name != kPreviewFile) {
      fail("Vyrn can't open " + label + ": it holds a file a brain file can't have (\"" + shown_name(e.name) + "\").");
    }
  }

  BrainPackage b;
  b.file_size = bytes.size();
  const ZipEntry* manifest_entry = zip.find(kBrainManifest);
  if (!manifest_entry) fail("Vyrn can't open " + label + ": its description (vyrn-brain.json) is missing.");
  if (manifest_entry->usize > kMaxManifest) fail("Vyrn can't open " + label + ": its description is too big.");
  json m;
  try {
    m = parse_json_strict<json>(zip.read(*manifest_entry));
  } catch (const JsonError& e) {
    bad_manifest(label, e.what());
  }
  if (!m.is_object()) bad_manifest(label, "it isn't a JSON object");
  if (m.value("format", json()) != "vyrn-brain") bad_manifest(label, "it isn't a brain description");
  check_format_version(m, label);

  static const std::set<std::string> kKnown = {
      "format", "format_version", "vyrn", "name", "description", "author", "created", "project", "template",
      "mode", "brain", "shape", "net", "net_description", "obs", "choices", "labels", "effects", "normalizer",
      "model_state", "counters", "best_mean20", "history", "weights"};
  for (const auto& item : m.items()) {
    if (!kKnown.count(item.key())) b.ignored_keys.push_back(item.key());
  }

  b.vyrn_version = manifest_text(m, "vyrn", 40, label);
  b.name = manifest_text(m, "name", 200, label);
  b.description = manifest_text(m, "description", 2000, label, true);
  b.author = manifest_text(m, "author", 100, label);
  b.created = manifest_text(m, "created", 40, label);
  if (!valid_utc_iso(b.created)) b.created.clear();
  b.project = manifest_text(m, "project", 200, label);
  b.template_id = manifest_text(m, "template", 64, label);
  if (!util::is_safe_name(b.template_id)) b.template_id.clear();
  b.mode = manifest_text(m, "mode", 20, label);
  if (b.mode != "sim" && b.mode != "realtime") b.mode.clear();
  if (const auto it = m.find("brain"); it != m.end() && !it->is_null()) {
    if (!it->is_object()) bad_manifest(label, "\"brain\" is wrong");
    const std::string id = manifest_text(*it, "id", 64, label);
    b.id = valid_brain_id(id) ? id : "";
    b.which = manifest_text(*it, "which", 10, label) == "latest" ? "latest" : "best";
  }

  b.shape = shape_field(m, label);
  b.net = net_name_for(b.shape);
  b.net_description = net_description_for(b.shape);
  if (m.value("net", json()) != b.net) bad_manifest(label, "\"net\" doesn't match the brain's shape");
  b.obs = checked_obs(m.value("obs", json()), b.shape);
  if (m.value("choices", json()) != json(b.shape.choices)) bad_manifest(label, "\"choices\" doesn't match the shape");
  b.labels = labels_for(m.value("labels", json()), b.shape.choices);
  b.effects = effects_for(m.value("effects", json()), b.shape.choices.size());
  if (!normalizer_for(m.value("normalizer", json()), b.shape, b.normalizer)) {
    bad_manifest(label, "\"normalizer\" is wrong");
  }

  const auto state = m.find("model_state");
  if (state == m.end() || !state->is_object()) bad_manifest(label, "\"model_state\" is missing");
  if (!int_in(state->value("updates", json()), 0, kMaxUpdates, b.model_updates) ||
      !number_in(state->value("max_abs_reward", json()), 0, 1e300, b.max_abs_reward) ||
      !state->value("reward_frozen", json()).is_boolean()) {
    bad_manifest(label, "\"model_state\" is wrong");
  }
  b.reward_frozen = state->value("reward_frozen", false);

  if (const auto c = m.find("counters"); c != m.end() && !c->is_null()) {
    if (!c->is_object()) bad_manifest(label, "\"counters\" is wrong");
    const auto get_int = [&](const char* key, int64_t& out) {
      const auto v = c->find(key);
      if (v != c->end() && !int_in(*v, 0, kMaxCounter, out)) bad_manifest(label, std::string("counters.") + key);
    };
    get_int("ticks", b.ticks);
    get_int("rounds", b.rounds);
    get_int("updates", b.updates);
    const auto s = c->find("seconds");
    if (s != c->end() && !number_in(*s, 0, kMaxSeconds, b.seconds)) bad_manifest(label, "counters.seconds");
  }
  if (const auto best = m.find("best_mean20"); best != m.end() && !best->is_null()) {
    double v = 0;
    if (!number_in(*best, -1e300, 1e300, v)) bad_manifest(label, "\"best_mean20\" is wrong");
    b.best_mean20 = v;
  }
  b.history = history_field(m, label);

  const auto weights = m.find("weights");
  int64_t tensors = 0, parameters = 0;
  if (weights == m.end() || !weights->is_object() || weights->value("file", json()) != kWeightsFile ||
      !weights->value("sha256", json()).is_string() ||
      !int_in(weights->value("tensors", json()), 0, kMaxTensors, tensors) ||
      !int_in(weights->value("parameters", json()), 0, kMaxCounter, parameters)) {
    bad_manifest(label, "\"weights\" is wrong");
  }
  const ZipEntry* weights_entry = zip.find(kWeightsFile);
  if (!weights_entry) fail("Vyrn can't open " + label + ": its weights (model.safetensors) are missing.");
  b.weights = zip.read(*weights_entry);
  if (lower_ascii(weights->value("sha256", "")) != sha256_hex(b.weights)) {
    throw WeightsError("the sha256 doesn't match model.safetensors");
  }
  const std::vector<TensorView> views = decode_safetensors(b.weights, b.net);
  check_weights_fit(b.shape, views, &b.parameters);
  if (static_cast<int64_t>(views.size()) != tensors || b.parameters != parameters) {
    throw WeightsError("the tensor or parameter count differs from the description");
  }
  b.tensors = static_cast<int>(views.size());

  if (const ZipEntry* preview = zip.find(kPreviewFile)) {
    if (preview->usize > kMaxPreview) fail("Vyrn can't open " + label + ": its picture is bigger than 4 MB.");
    b.preview_png = zip.read(*preview);
    if (!plausible_png(b.preview_png)) fail("Vyrn can't open " + label + ": its picture isn't a PNG Vyrn can show.");
  }
  return b;
}

// ---- history from runs/ ----

bool read_small(const fs::path& path, uint64_t max_bytes, std::string& out) {
  std::error_code ec;
  const fs::file_status st = fs::symlink_status(path, ec);
  if (ec || !fs::is_regular_file(st)) return false;
  const auto size = fs::file_size(path, ec);
  if (ec || size > max_bytes) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream text;
  text << in.rdbuf();
  out = text.str();
  return !in.bad();
}

std::vector<std::string> split(const std::string& line, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  for (size_t at = line.find(sep); at != std::string::npos; at = line.find(sep, start)) {
    out.push_back(line.substr(start, at - start));
    start = at + 1;
  }
  out.push_back(line.substr(start));
  return out;
}

void read_rounds_csv(const std::string& text, std::vector<HistoryPoint>& out) {
  std::istringstream in(text);
  std::string line;
  if (!std::getline(in, line)) return;
  if (!line.empty() && line.back() == '\r') line.pop_back();
  const std::vector<std::string> header = split(line, ',');
  const auto round_col = std::find(header.begin(), header.end(), "round") - header.begin();
  const auto mean_col = std::find(header.begin(), header.end(), "mean20") - header.begin();
  if (round_col >= static_cast<long>(header.size()) || mean_col >= static_cast<long>(header.size())) return;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::vector<std::string> cells = split(line, ',');
    if (static_cast<long>(cells.size()) <= std::max(round_col, mean_col)) continue;
    const std::string& r = cells[static_cast<size_t>(round_col)];
    const std::string& v = cells[static_cast<size_t>(mean_col)];
    char* end = nullptr;
    errno = 0;
    const long long round = std::strtoll(r.c_str(), &end, 10);
    if (r.empty() || *end || errno || round < 1 || round > kMaxCounter) continue;
    const double mean = std::strtod(v.c_str(), &end);
    if (v.empty() || *end || !std::isfinite(mean)) continue;
    out.push_back({round, mean});
  }
}

}  // namespace

BrainPackage parse_brain_package(std::string_view bytes, const std::string& label) {
  try {
    return parse_brain(bytes, label);
  } catch (const ZipError& e) {
    fail("Vyrn can't open " + label + ": " + e.what());
  } catch (const WeightsError&) {
    fail("Vyrn can't open " + label + ": " + kWeightsMessage);
  }
}

BrainSummary summary_of(const BrainPackage& b) {
  BrainSummary s;
  s.id = b.id;
  s.name = !b.name.empty() ? b.name : (!b.project.empty() ? b.project : "Brain");
  s.which = b.which;
  s.input = brain::to_string(b.shape.kind);
  if (b.shape.kind == brain::ObsSpec::Kind::Picture) {
    s.input_shape = {b.shape.in_h, b.shape.in_w};
  } else {
    s.input_shape = {b.shape.d};
    s.input_names = b.shape.inputs;
  }
  s.choices = b.shape.choices;
  s.net = b.net;
  s.net_description = b.net_description;
  s.has_best = b.best_mean20.has_value();
  s.best_mean20 = b.best_mean20.value_or(0.0);
  s.time_trained_s = b.seconds;
  s.ticks = b.ticks;
  s.rounds = b.rounds;
  s.history = b.history;
  return s;
}

std::vector<HistoryPoint> brain_history(const std::string& project, const std::string& brain_id) {
  std::vector<fs::path> runs;
  std::error_code ec;
  for (fs::directory_iterator it(fs::path(project) / "runs", ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code type_ec;
    const std::string name = it->path().filename().string();
    if (!name.empty() && name[0] != '.' && it->is_directory(type_ec) && !it->is_symlink(type_ec)) {
      runs.push_back(it->path());
    }
  }
  std::sort(runs.begin(), runs.end());
  std::vector<HistoryPoint> all;
  for (const fs::path& run : runs) {
    std::string text;
    if (!read_small(run / "run.json", 64u << 10, text)) continue;
    const json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object() || j.value("brain", json()) != brain_id) continue;
    if (!read_small(run / "rounds.csv", 64ull << 20, text)) continue;
    read_rounds_csv(text, all);
  }
  std::stable_sort(all.begin(), all.end(), [](const HistoryPoint& a, const HistoryPoint& b) { return a.round < b.round; });
  std::vector<HistoryPoint> points;
  for (const HistoryPoint& p : all) {
    if (points.empty() || p.round > points.back().round) points.push_back(p);
  }
  if (points.size() <= kMaxHistory) return points;
  std::vector<HistoryPoint> thinned;
  const size_t n = points.size();
  for (size_t k = 0; k < kMaxHistory; ++k) {
    const size_t i = static_cast<size_t>(std::llround(static_cast<double>(k) * static_cast<double>(n - 1) /
                                                      static_cast<double>(kMaxHistory - 1)));
    thinned.push_back(points[i]);
  }
  return thinned;
}

std::string build_brain_package(const std::string& project, const std::string& brain_id, const std::string& which,
                                const ExportOptions& options, int64_t now, std::string* which_used) {
  ProjectInfo info;
  try {
    info = read_project(project);
  } catch (const std::exception&) {
    fail("There is no Vyrn project at " + project + ".");
  }
  const util::ProjectSettings settings = util::read_settings(info.path);
  const std::string id = brain_id.empty() ? settings.brain : brain_id;
  const std::string no_brain = "This project has no trained brain yet. Train it for a little while first.";
  if (id.empty()) fail(no_brain);
  if (!util::is_safe_name(id)) fail("There is no brain called \"" + clean_text(id, 64) + "\" in this project.");
  if (which != "best" && which != "latest") fail("Please choose the best or the latest brain.");
  const fs::path dir = fs::path(info.path) / "brains" / id;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) fail(brain_id.empty() ? no_brain : "There is no brain called \"" + id + "\" in this project.");
  if (!options.preview_png.empty() &&
      (options.preview_png.size() > kMaxPreview ||
       !plausible_png(std::string_view(reinterpret_cast<const char*>(options.preview_png.data()),
                                       options.preview_png.size())))) {
    fail("The preview picture must be a PNG of at most 4 MB (and at most 4096 pixels wide and high).");
  }

  const LocalBrain local = read_local_brain(dir.string(), which);
  if (which_used) *which_used = local.which;
  const json& meta = local.meta;
  json normalizer;
  if (!normalizer_for(meta.value("normalizer", json()), local.shape, normalizer)) {
    fail("This brain's number statistics are damaged, so it can't be shared.");
  }
  const json counters = meta.value("counters", json::object()).is_object() ? meta.value("counters", json::object())
                                                                            : json::object();
  const auto counter = [&](const char* key) {
    int64_t v = 0;
    return int_in(counters.value(key, json()), 0, kMaxCounter, v) ? v : 0;
  };
  double seconds = 0;
  if (!number_in(counters.value("seconds", json()), 0, kMaxSeconds, seconds)) seconds = 0;
  double best = 0;
  const bool has_best = number_in(meta.value("best_mean20", json()), -1e300, 1e300, best);

  ordered_json m = ordered_json::object();
  m["format"] = "vyrn-brain";
  m["format_version"] = 1;
  m["vyrn"] = VYRN_VERSION;
  m["name"] = clean_text(info.name, 200);
  m["description"] = clean_text(options.description, 2000, true);
  m["author"] = clean_text(options.author, 100);
  m["created"] = utc_iso(now);
  m["project"] = clean_text(info.name, 200);
  m["template"] = util::is_safe_name(settings.template_id) ? settings.template_id : "";
  m["mode"] = info.mode;
  m["brain"] = {{"id", id}, {"which", local.which}};
  const brain::BrainShape& s = local.shape;
  m["shape"] = {{"kind", brain::to_string(s.kind)}, {"in_c", s.in_c}, {"in_h", s.in_h}, {"in_w", s.in_w},
                {"d", s.d}, {"small_picture", s.small_picture}, {"frames", s.frames}, {"choices", s.choices},
                {"inputs", s.inputs}};
  m["net"] = net_name_for(s);
  m["net_description"] = net_description_for(s);
  m["obs"] = ordered_json::parse(checked_obs(meta.value("obs", json()), s).dump());
  m["choices"] = s.choices;
  m["labels"] = ordered_json::parse(labels_for(meta.value("labels", json()), s.choices).dump());
  m["effects"] = ordered_json::parse(effects_for(meta.value("effects", json()), s.choices.size()).dump());
  m["normalizer"] = ordered_json::parse(normalizer.dump());
  m["model_state"] = {{"updates", local.updates},
                      {"max_abs_reward", local.max_abs_reward},
                      {"reward_frozen", local.reward_frozen}};
  m["counters"] = {{"ticks", counter("ticks")},
                   {"rounds", counter("rounds")},
                   {"updates", counter("updates")},
                   {"seconds", seconds}};
  m["best_mean20"] = has_best ? ordered_json(best) : ordered_json(nullptr);
  std::vector<HistoryPoint> points = brain_history(info.path, id);
  // No run of this project has points for this brain (it was imported, say): use the chart its brain.json kept
  // (a malformed one leaves the history empty).
  if (points.empty()) read_history(meta, points);
  ordered_json history = ordered_json::array();
  for (const HistoryPoint& p : points) history.push_back({p.round, p.mean20});
  m["history"] = history;
  m["weights"] = {{"file", kWeightsFile},
                  {"sha256", sha256_hex(local.safetensors)},
                  {"tensors", local.tensors},
                  {"parameters", local.parameters}};

  std::string out;
  ZipWriter zip([&](const char* data, size_t size) { out.append(data, size); }, now);
  zip.add("mimetype", kBrainMime, false);
  zip.add(kBrainManifest, m.dump(2) + "\n", true);
  zip.add(kWeightsFile, local.safetensors, false);
  if (!options.preview_png.empty()) {
    zip.add(kPreviewFile,
            std::string_view(reinterpret_cast<const char*>(options.preview_png.data()), options.preview_png.size()),
            false);
  }
  zip.finish();
  return out;
}

json imported_brain_meta(const BrainPackage& b, const std::string& id, const std::string& package_file) {
  json meta = json::object();
  meta["format"] = 1;
  meta["vyrn"] = VYRN_VERSION;
  meta["brain"] = id;
  meta["name"] = !b.name.empty() ? b.name : b.project;
  meta["shape"] = b.shape;
  meta["net"] = b.net;
  meta["net_description"] = b.net_description;
  meta["obs"] = b.obs;
  meta["choices"] = b.shape.choices;
  meta["labels"] = b.labels;
  meta["effects"] = b.effects;
  meta["counters"] = {{"ticks", b.ticks}, {"rounds", b.rounds}, {"updates", b.updates}, {"seconds", b.seconds}};
  meta["normalizer"] = b.normalizer;
  meta["best_mean20"] = b.best_mean20 ? json(*b.best_mean20) : json(nullptr);
  meta["model_state"] = {{"updates", b.model_updates},
                         {"max_abs_reward", b.max_abs_reward},
                         {"reward_frozen", b.reward_frozen}};
  meta["imported"] = {{"file", clean_text(package_file, 255)}, {"author", b.author}, {"created", b.created}};
  if (!b.history.empty()) {   // the points chart it came with: a later export falls back to it (section 4)
    json history = json::array();
    for (const HistoryPoint& p : b.history) history.push_back({p.round, p.mean20});
    meta["history"] = std::move(history);
  }
  meta["saved"] = util::local_iso_time();
  return meta;
}

std::string install_brain(const std::string& project, const BrainPackage& brain, const std::string& wanted_id,
                          const std::string& package_file) {
  const fs::path brains = fs::path(project) / "brains";
  std::error_code ec;
  const bool made = !fs::exists(brains, ec) && fs::create_directories(brains, ec);
  if (!fs::is_directory(brains, ec) || !is_inside(project, brains.string())) {
    fail("Vyrn can't create the brains folder in " + project + ".");
  }
  if (made) fs::permissions(brains, fs::perms(0755), fs::perm_options::replace, ec);
  const auto claim = [&](const std::string& candidate) {
    const int err = platform::make_dir((brains / candidate).string(), 0755);
    if (err == 0) return true;
    if (err != EEXIST) fail("Vyrn can't create a folder in " + brains.string() + " (" + platform::error_text(err) + ").");
    return false;
  };
  std::string id;
  if (valid_brain_id(wanted_id) && claim(wanted_id)) {
    id = wanted_id;
  } else {
    const std::string base = util::local_stamp();
    for (int n = 1; n < 1000 && id.empty(); ++n) {
      const std::string candidate = n == 1 ? base : base + "-" + std::to_string(n);
      if (claim(candidate)) id = candidate;
    }
  }
  if (id.empty()) fail("Vyrn can't find a free name for the new brain in " + brains.string() + ".");
  const fs::path dir = brains / id;
  try {
    write_brain_checkpoints(dir.string(), brain, imported_brain_meta(brain, id, package_file));
  } catch (...) {
    fs::remove_all(dir, ec);
    throw;
  }
  // Folders 0755 and files 0644, whatever the umask (section 6). The checkpoint code made only these.
  fs::permissions(dir, fs::perms(0755), fs::perm_options::replace, ec);
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code type_ec;
    const fs::file_status st = it->symlink_status(type_ec);
    if (fs::is_directory(st)) fs::permissions(it->path(), fs::perms(0755), fs::perm_options::replace, type_ec);
    if (fs::is_regular_file(st)) fs::permissions(it->path(), fs::perms(0644), fs::perm_options::replace, type_ec);
  }
  return id;
}

}  // namespace vyrn::package_detail
