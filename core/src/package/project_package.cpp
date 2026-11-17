// .vrlp files: reading and checking one, and writing one from a project folder (docs/dev/package.md section 5).
#include "brain/checkpoint.h"
#include "package/internal.h"
#include "platform/files.h"
#include "package/json_strict.h"
#include "package/text.h"
#include "util/files.h"
#include "util/settings.h"
#include "vyrn/project.h"


#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>

#ifndef VYRN_VERSION
#define VYRN_VERSION "0.0.0"
#endif

namespace vyrn::package_detail {

namespace fs = std::filesystem;

namespace {

constexpr const char* kProjectPrefix = "project/";
constexpr const char* kBrainsPrefix = "brains/";

bool starts_with(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

bool ends_with(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::string in_quotes(const std::string& name) { return "\"" + shown_name(name) + "\""; }

// ---- reading a project folder for export ----

const char* verb_of(Purpose purpose) { return purpose == Purpose::Compile ? "compile" : "share"; }
const char* doing_of(Purpose purpose) { return purpose == Purpose::Compile ? "compiling" : "sharing"; }

// A project file's bytes (no symlinks, at most kMaxProjectFile bytes).
std::string read_project_file(const fs::path& path, const std::string& rel, Purpose purpose) {
  int err = 0;
  platform::File file = platform::File::open_read(path.string(), /*no_follow=*/true, err);
  if (!file) fail("Vyrn can't read " + rel + " (" + platform::error_text(err) + ").");
  platform::FileInfo st;
  if (file.info(st) != 0 || !st.is_regular()) fail("Vyrn can't read " + rel + ".");
  if (st.size > kMaxProjectFile) {
    fail("The file " + rel + " is too big to " + verb_of(purpose) + " (" + human_size(st.size) +
         "; each file can be at most " + human_size(kMaxProjectFile) + ").");
  }
  std::string out(static_cast<size_t>(st.size), '\0');
  size_t got = 0;
  while (got < out.size()) {
    size_t n = 0;
    if ((err = file.read(out.data() + got, out.size() - got, n)) != 0) {
      fail("Vyrn can't read " + rel + " (" + platform::error_text(err) + ").");
    }
    if (n == 0) break;
    got += n;
  }
  out.resize(got);
  return out;
}

// Every file that goes into a package (section 5), sorted by path. Never follows symlinks.
void walk(const fs::path& dir, const std::string& prefix, int depth, std::vector<SharedFile>& out) {
  std::vector<fs::directory_entry> entries;
  std::error_code ec;
  for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
       it.increment(ec)) {
    entries.push_back(*it);
  }
  std::sort(entries.begin(), entries.end(),
            [](const fs::directory_entry& a, const fs::directory_entry& b) { return a.path() < b.path(); });
  for (const fs::directory_entry& entry : entries) {
    const std::string name = entry.path().filename().string();
    const std::string rel = prefix + name;
    std::error_code type_ec;
    const fs::file_status st = entry.symlink_status(type_ec);
    if (type_ec || name.empty() || name[0] == '.') continue;   // hidden files and folders are never shared
    if (fs::is_directory(st)) {
      const std::string lower = lower_ascii(name);
      if ((depth == 0 && (lower == "brains" || lower == "runs")) || name == "__pycache__") continue;
      if (depth + 1 > kMaxProjectDepth) continue;
      if (!name_problem(kProjectPrefix + rel + "/x").empty()) continue;
      walk(entry.path(), rel + "/", depth + 1, out);
    } else if (fs::is_regular_file(st)) {
      if (!name_problem(kProjectPrefix + rel).empty()) continue;     // a name other systems can't use
      if (!project_file_skip_reason(rel).empty()) continue;
      SharedFile f;
      f.rel = rel;
      f.abs = entry.path();
      out.push_back(std::move(f));
    }
    // symlinks, sockets, devices and pipes are never shared
  }
}

void check_project_limits(const std::vector<SharedFile>& files, Purpose purpose) {
  const std::string verb = verb_of(purpose);
  if (files.size() > kMaxProjectFiles) {
    fail("This project has too many files to " + verb + " (" + std::to_string(files.size()) + "; at most " +
         std::to_string(kMaxProjectFiles) + ").");
  }
  uint64_t total = 0;
  for (const SharedFile& f : files) {
    if (f.size > kMaxProjectFile) {
      fail("The file " + f.rel + " is too big to " + verb + " (" + human_size(f.size) +
           "; each file can be at most " + human_size(kMaxProjectFile) +
           "). Move it out of the project folder or make it smaller.");
    }
    total += f.size;
  }
  if (total > kMaxProjectBytes) {
    std::vector<const SharedFile*> biggest;
    for (const SharedFile& f : files) biggest.push_back(&f);
    std::sort(biggest.begin(), biggest.end(),
              [](const SharedFile* a, const SharedFile* b) { return a->size > b->size; });
    std::string names;
    for (size_t i = 0; i < biggest.size() && i < 3; ++i) {
      if (i) names += i + 1 == std::min<size_t>(3, biggest.size()) ? " and " : ", ";
      names += biggest[i]->rel + " (" + human_size(biggest[i]->size) + ")";
    }
    fail("This project is too big to " + verb + " (" + human_size(total) + "; at most " +
         human_size(kMaxProjectBytes) + " together). The biggest files are " + names + ".");
  }
}

}  // namespace

// ---- a project's shared files (section 5) ---------------------------------------------------------------------

std::vector<SharedFile> shared_files(const std::string& project_dir, Purpose purpose) {
  // First pass: names, sizes and checksums (vyrn.json is made by shared_settings()).
  std::vector<SharedFile> files;
  walk(project_dir, "", 0, files);
  files.erase(std::remove_if(files.begin(), files.end(), [](const SharedFile& f) { return f.rel == "vyrn.json"; }),
              files.end());
  {
    // Names that only differ in case (possible on Linux) can't both be shared: keep the first.
    std::set<std::string> folded;
    std::vector<SharedFile> kept;
    folded.insert("vyrn.json");
    for (SharedFile& f : files) {
      const std::string key = fold_case(f.rel);
      bool clash = !folded.insert(key).second;
      for (size_t slash = key.find('/'); !clash && slash != std::string::npos; slash = key.find('/', slash + 1)) {
        clash = folded.count(key.substr(0, slash)) > 0;
      }
      if (!clash) kept.push_back(std::move(f));
    }
    files = std::move(kept);
  }
  for (SharedFile& f : files) {
    std::error_code ec;
    f.size = fs::file_size(f.abs, ec);
    if (ec) fail("Vyrn can't read " + f.rel + " (" + ec.message() + ").");
  }
  check_project_limits(files, purpose);
  for (SharedFile& f : files) {
    const std::string data = read_project_file(f.abs, f.rel, purpose);
    f.size = data.size();
    f.sha256 = sha256_hex(data);
  }
  check_project_limits(files, purpose);
  std::sort(files.begin(), files.end(), [](const SharedFile& a, const SharedFile& b) { return a.rel < b.rel; });
  return files;
}

std::string read_shared_file(const SharedFile& f, Purpose purpose) {
  std::string data = read_project_file(f.abs, f.rel, purpose);
  if (sha256_hex(data) != f.sha256) {
    fail("The file " + f.rel + " changed while Vyrn was " + doing_of(purpose) + " the project. Please try again.");
  }
  return data;
}

ordered_json shared_settings(const util::ProjectSettings& settings, const std::string& project_name,
                             const std::string& brain_id) {
  ordered_json vyrn_json = allowed_settings(settings.exists ? settings.raw : ordered_json::object(), nullptr);
  if (!vyrn_json.contains("name")) vyrn_json["name"] = clean_text(project_name, 200);
  if (brain_id.empty()) {
    vyrn_json.erase("brain");
  } else {
    vyrn_json["brain"] = brain_id;
  }
  return vyrn_json;
}

// ---- reading a .vrlp ----------------------------------------------------------------------------------------

ProjectPackage parse_project_package(const ZipReader& zip, std::string_view bytes, const std::string& label) {
  const std::string mime = mimetype_of(zip, label);
  if (mime == kBrainMime) {
    fail(label + " is a single brain (.vrl), not a whole project (.vrlp). Open a project and import the brain "
                 "into it instead.");
  }
  if (mime != kProjectMime) fail("Vyrn can't open " + label + ": it isn't a Vyrn project file.");

  ProjectPackage p;
  p.file_size = bytes.size();
  std::map<std::string, const ZipEntry*> project_entries, brain_entries;
  for (const ZipEntry& e : zip.entries()) {
    if (e.name == "mimetype" || e.name == kProjectManifest || e.name == kPreviewFile) continue;
    if (starts_with(e.name, kProjectPrefix) && e.name.size() > std::strlen(kProjectPrefix)) {
      project_entries[e.name.substr(std::strlen(kProjectPrefix))] = &e;
    } else if (starts_with(e.name, kBrainsPrefix) && ends_with(e.name, kBrainExt) &&
               e.name.find('/', std::strlen(kBrainsPrefix)) == std::string::npos) {
      brain_entries[e.name] = &e;
    } else {
      fail("Vyrn can't open " + label + ": it holds a file a project file can't have (" + in_quotes(e.name) + ").");
    }
  }

  const ZipEntry* manifest_entry = zip.find(kProjectManifest);
  if (!manifest_entry) fail("Vyrn can't open " + label + ": its description (vyrn-project.json) is missing.");
  if (manifest_entry->usize > kMaxManifest) fail("Vyrn can't open " + label + ": its description is too big.");
  json m;
  try {
    m = parse_json_strict<json>(zip.read(*manifest_entry));
  } catch (const JsonError& e) {
    bad_manifest(label, e.what());
  }
  if (!m.is_object()) bad_manifest(label, "it isn't a JSON object");
  if (m.value("format", json()) != "vyrn-project") bad_manifest(label, "it isn't a project description");
  check_format_version(m, label);
  static const std::set<std::string> kKnown = {"format",   "format_version", "vyrn",       "name",
                                               "description", "author",      "created",    "template",
                                               "mode",     "uses_link",      "start_program", "files",
                                               "brains"};
  for (const auto& item : m.items()) {
    if (!kKnown.count(item.key())) p.ignored_keys.push_back(item.key());
  }
  p.vyrn_version = manifest_text(m, "vyrn", 40, label);
  p.name = manifest_text(m, "name", 200, label);
  if (p.name.empty()) p.name = "Shared project";
  p.description = manifest_text(m, "description", 2000, label, true);
  p.author = manifest_text(m, "author", 100, label);
  p.created = manifest_text(m, "created", 40, label);
  if (!valid_utc_iso(p.created)) p.created.clear();
  p.template_id = manifest_text(m, "template", 64, label);
  if (!util::is_safe_name(p.template_id)) p.template_id.clear();
  p.start_program = manifest_text(m, "start_program", 500, label);

  // files: exactly the project/ entries, with their sizes and checksums.
  const auto files = m.find("files");
  if (files == m.end() || !files->is_array()) bad_manifest(label, "\"files\" is missing");
  std::set<std::string> listed;
  for (const json& f : *files) {
    int64_t size = 0;
    if (!f.is_object() || !f.value("path", json()).is_string() || !f.value("sha256", json()).is_string() ||
        !int_in(f.value("size", json()), 0, int64_t{1} << 40, size)) {
      bad_manifest(label, "\"files\" is wrong");
    }
    const std::string path = f["path"].get<std::string>();
    const auto entry = project_entries.find(path);
    if (entry == project_entries.end() || !listed.insert(path).second) {
      bad_manifest(label, "\"files\" doesn't match what is in the file");
    }
    if (static_cast<uint64_t>(size) != entry->second->usize) {
      fail("Vyrn can't open " + label + ": the size of " + in_quotes(path) + " doesn't match its description.");
    }
    const std::string data = zip.read(*entry->second);
    if (sha256_hex(data) != lower_ascii(f["sha256"].get<std::string>())) {
      fail("Vyrn can't open " + label + ": the file " + in_quotes(path) + " doesn't match its checksum.");
    }
    ProjectFileEntry pf;
    pf.path = path;
    pf.size = static_cast<uint64_t>(size);
    pf.sha256 = sha256_hex(data);
    pf.skip_reason = project_file_skip_reason(path);
    if (path == "vyrn.json" && pf.skip_reason.empty()) {
      try {
        const ordered_json settings = parse_json_strict<ordered_json>(data);
        if (!settings.is_object()) throw JsonError("not an object");
        p.settings = allowed_settings(settings, &p.dropped_settings);
      } catch (const JsonError&) {
        p.settings_damaged = true;
      }
    }
    p.files.push_back(std::move(pf));
  }
  if (listed.size() != project_entries.size()) bad_manifest(label, "\"files\" doesn't match what is in the file");
  std::sort(p.files.begin(), p.files.end(),
            [](const ProjectFileEntry& a, const ProjectFileEntry& b) { return a.path < b.path; });

  // brains: exactly the brains/ entries, each a complete .vrl checked with the same reader and limits.
  const auto brains = m.find("brains");
  std::set<std::string> brain_files;
  if (brains != m.end() && !brains->is_null()) {
    if (!brains->is_array() || brains->size() > kMaxBrainsInProject) bad_manifest(label, "\"brains\" is wrong");
    int active = 0;
    for (const json& b : *brains) {
      if (!b.is_object() || !b.value("file", json()).is_string()) bad_manifest(label, "\"brains\" is wrong");
      ProjectBrainEntry entry;
      entry.file = b["file"].get<std::string>();
      const auto zip_entry = brain_entries.find(entry.file);
      if (zip_entry == brain_entries.end() || !brain_files.insert(entry.file).second) {
        bad_manifest(label, "\"brains\" doesn't match what is in the file");
      }
      const std::string id = manifest_text(b, "id", 64, label);
      entry.id = valid_brain_id(id) ? id : "";
      const json active_value = b.value("active", json(false));
      if (!active_value.is_boolean()) bad_manifest(label, "\"brains\" is wrong");
      entry.active = active_value.get<bool>();
      active += entry.active ? 1 : 0;
      const std::string nested = zip.read(*zip_entry->second);
      entry.brain = parse_brain_package(nested, label + " (" + entry.file + ")");
      p.brains.push_back(std::move(entry));
    }
    if (active > 1) bad_manifest(label, "more than one brain is marked active");
  }
  if (brain_files.size() != brain_entries.size()) bad_manifest(label, "\"brains\" doesn't match what is in the file");
  std::stable_sort(p.brains.begin(), p.brains.end(),
                   [](const ProjectBrainEntry& a, const ProjectBrainEntry& b) { return a.active && !b.active; });

  // What an import computes for itself.
  p.mode = "realtime";
  for (const ProjectFileEntry& f : p.files) {
    if (f.path == "world.py" && f.skip_reason.empty()) p.mode = "sim";
  }
  const auto link = p.settings.find("link");
  p.uses_link = link != p.settings.end() && !link->is_null() && *link != false;
  if (p.template_id.empty()) {
    const auto t = p.settings.find("template");
    if (t != p.settings.end() && t->is_string() && util::is_safe_name(t->get<std::string>())) {
      p.template_id = t->get<std::string>();
    }
  }

  if (const ZipEntry* preview = zip.find(kPreviewFile)) {
    if (preview->usize > kMaxPreview) fail("Vyrn can't open " + label + ": its picture is bigger than 4 MB.");
    p.preview_png = zip.read(*preview);
    if (!plausible_png(p.preview_png)) fail("Vyrn can't open " + label + ": its picture isn't a PNG Vyrn can show.");
  }
  return p;
}

// ---- writing a .vrlp ------------------------------------------------------------------------------------------

void write_project_package(PartFile& part, const std::string& project, const ExportOptions& options, int64_t now) {
  ProjectInfo info;
  try {
    info = read_project(project);
  } catch (const std::exception&) {
    fail("There is no Vyrn project at " + project + ".");
  }
  const util::ProjectSettings settings = util::read_settings(info.path);
  if (!settings.readable) {
    fail("This project's settings file (vyrn.json) isn't valid, so Vyrn can't share the project. Fix it first.");
  }
  if (!options.preview_png.empty() &&
      (options.preview_png.size() > kMaxPreview ||
       !plausible_png(std::string_view(reinterpret_cast<const char*>(options.preview_png.data()),
                                       options.preview_png.size())))) {
    fail("The preview picture must be a PNG of at most 4 MB (and at most 4096 pixels wide and high).");
  }

  // The project's files (vyrn.json is rewritten below), first pass: sizes and checksums.
  const std::vector<SharedFile> files = shared_files(info.path, Purpose::Share);

  // The brain, as a complete .vrl.
  std::string brain_id, brain_bytes;
  if (options.include_brain) {
    const std::string wanted = options.brain_id.empty() ? settings.brain : options.brain_id;
    bool available = !wanted.empty();
    if (available && options.brain_id.empty()) {
      // The active brain goes in only when it has a save; a project without one is shared without a brain.
      const std::string dir = (fs::path(info.path) / "brains" / wanted).string();
      brain::BrainMeta meta;
      try {
        available = util::is_safe_name(wanted) &&
                    (brain::read_checkpoint_meta(dir, "best", meta) || brain::read_checkpoint_meta(dir, "latest", meta));
      } catch (const std::exception&) {
        available = false;
      }
    }
    if (available) {
      brain_bytes = build_brain_package(info.path, wanted, options.which, options, now, nullptr);
      brain_id = wanted;
    }
  }

  // vyrn.json: the allow-list, the included brain, no machine-specific settings.
  const ordered_json vyrn_json = shared_settings(settings, info.name, brain_id);
  const std::string vyrn_text = util::dump_settings(vyrn_json);

  struct Item {
    std::string path, sha256;
    uint64_t size;
  };
  std::vector<Item> items;
  for (const SharedFile& f : files) items.push_back({f.rel, f.sha256, f.size});
  items.push_back({"vyrn.json", sha256_hex(vyrn_text), vyrn_text.size()});
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.path < b.path; });

  ordered_json m = ordered_json::object();
  m["format"] = "vyrn-project";
  m["format_version"] = 1;
  m["vyrn"] = VYRN_VERSION;
  m["name"] = clean_text(info.name, 200);
  m["description"] = clean_text(options.description, 2000, true);
  m["author"] = clean_text(options.author, 100);
  m["created"] = utc_iso(now);
  m["template"] = util::is_safe_name(settings.template_id) ? settings.template_id : "";
  m["mode"] = info.mode;
  m["uses_link"] = vyrn_json.contains("link") && !vyrn_json["link"].is_null() && vyrn_json["link"] != false;
  m["start_program"] = clean_text(shareable_text(settings.start_program), 500);   // no home folder, no paths
  ordered_json listed = ordered_json::array();
  for (const Item& item : items) {
    listed.push_back({{"path", item.path}, {"size", item.size}, {"sha256", item.sha256}});
  }
  m["files"] = listed;
  ordered_json brains = ordered_json::array();
  if (!brain_id.empty()) {
    brains.push_back({{"file", std::string(kBrainsPrefix) + brain_id + kBrainExt}, {"id", brain_id}, {"active", true}});
  }
  m["brains"] = brains;

  // Second pass: write. Entries after the manifest go in byte order of their names.
  ZipWriter zip([&](const char* data, size_t size) { part.write(data, size); }, now);
  zip.add("mimetype", kProjectMime, false);
  zip.add(kProjectManifest, m.dump(2) + "\n", true);
  if (!brain_id.empty()) zip.add(std::string(kBrainsPrefix) + brain_id + kBrainExt, brain_bytes, false);
  if (!options.preview_png.empty()) {
    zip.add(kPreviewFile,
            std::string_view(reinterpret_cast<const char*>(options.preview_png.data()), options.preview_png.size()),
            false);
  }
  std::map<std::string, const SharedFile*> by_path;
  for (const SharedFile& f : files) by_path[f.rel] = &f;
  for (const Item& item : items) {
    const std::string data =
        item.path == "vyrn.json" ? vyrn_text : read_shared_file(*by_path.at(item.path), Purpose::Share);
    const std::string lower = lower_ascii(item.path);
    const bool packed = ends_with(lower, ".png") || ends_with(lower, ".jpg") || ends_with(lower, ".jpeg") ||
                        ends_with(lower, ".gif") || ends_with(lower, ".webp") || ends_with(lower, ".ogg");
    zip.add(kProjectPrefix + item.path, data, !packed);
  }
  zip.finish();
}

}  // namespace vyrn::package_detail
