// The public sharing API (vyrn/package.h): export, inspect and import (docs/dev/package.md sections 4-6).
#include "brain/checkpoint.h"
#include "package/internal.h"
#include "package/text.h"
#include "util/files.h"
#include "util/settings.h"
#include "vyrn/project.h"
#include "vyrn/session.h"

#include "platform/files.h"
#include "platform/system.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>

namespace vyrn {

using namespace package_detail;
namespace fs = std::filesystem;

namespace {

// Runs `body`; anything but a PackageError becomes one, in plain words.
template <class F>
auto guarded(const std::string& doing, F&& body) -> decltype(body()) {
  try {
    return body();
  } catch (const PackageError&) {
    throw;
  } catch (const std::bad_alloc&) {
    fail("Vyrn ran out of memory while " + doing + ".");
  } catch (const fs::filesystem_error& e) {
    fail("Vyrn had a problem with a file while " + doing + " (" + e.code().message() + ": " + e.path1().string() +
         ").");
  } catch (const std::exception& e) {
    fail("Vyrn had a problem while " + doing + ": " + clean_text(e.what(), 300));
  }
}

int64_t now_seconds() { return static_cast<int64_t>(std::time(nullptr)); }

void stage(const char* name) {
  if (test_hooks::export_stage) test_hooks::export_stage(name);
}

PackageInfo info_of(const BrainPackage& b) {
  PackageInfo info;
  info.kind = "brain";
  info.format_version = b.format_version;
  info.vyrn_version = b.vyrn_version;
  info.name = !b.project.empty() ? b.project : b.name;
  info.description = b.description;
  info.author = b.author;
  info.created = b.created;
  info.template_id = b.template_id;
  info.mode = b.mode;
  info.brains.push_back(summary_of(b));
  info.preview_png.assign(b.preview_png.begin(), b.preview_png.end());
  info.file_size = b.file_size;
  return info;
}

PackageInfo info_of(const ProjectPackage& p) {
  PackageInfo info;
  info.kind = "project";
  info.format_version = p.format_version;
  info.vyrn_version = p.vyrn_version;
  info.name = p.name;
  info.description = p.description;
  info.author = p.author;
  info.created = p.created;
  info.template_id = p.template_id;
  info.mode = p.mode;
  info.uses_link = p.uses_link;
  info.start_program = p.start_program;
  for (const ProjectFileEntry& f : p.files) {
    if (f.skip_reason.empty()) info.files.push_back({f.path, f.size});
  }
  for (const ProjectBrainEntry& b : p.brains) {
    BrainSummary s = summary_of(b.brain);
    if (!b.id.empty()) s.id = b.id;
    info.brains.push_back(std::move(s));
  }
  info.preview_png.assign(p.preview_png.begin(), p.preview_png.end());
  info.file_size = p.file_size;
  return info;
}

// What a file holds, by its first entry ("brain" | "project"), for names without a Vyrn extension.
std::string kind_by_content(std::string_view bytes, const std::string& label) {
  try {
    const ZipReader zip(bytes, ZipLimits{});
    const std::string mime = mimetype_of(zip, label);
    if (mime == kBrainMime) return "brain";
    if (mime == kProjectMime) return "project";
  } catch (const ZipError& e) {
    fail("Vyrn can't open " + label + ": " + e.what());
  }
  fail("Vyrn can't open " + label + ": it isn't a Vyrn file.");
}

ProjectPackage parse_project_bytes(std::string_view bytes, const std::string& label,
                                   std::unique_ptr<ZipReader>* keep = nullptr) {
  try {
    auto zip = std::make_unique<ZipReader>(bytes, ZipLimits{});
    ProjectPackage p = parse_project_package(*zip, bytes, label);
    if (keep) *keep = std::move(zip);
    return p;
  } catch (const ZipError& e) {
    fail("Vyrn can't open " + label + ": " + e.what());
  } catch (const WeightsError&) {
    fail("Vyrn can't open " + label + ": " + kWeightsMessage);
  }
}

// Checks a written .part file exactly as inspect_package() would read it.
PackageInfo check_written(const PartFile& part, const std::string& out_file, const std::string& kind) {
  const std::string bytes = read_package_bytes(part.part_path(), kMaxPackageFile);
  const std::string label = base_name(out_file);
  return kind == "brain" ? info_of(parse_brain_package(bytes, label)) : info_of(parse_project_bytes(bytes, label));
}

std::string project_path_or_fail(const std::string& project) {
  try {
    return read_project(project).path;
  } catch (const std::exception&) {
    fail("There is no Vyrn project at " + project + ".");
  }
}

// ---- import helpers ----

bool taken(const fs::path& p) {
  std::error_code ec;
  const fs::file_type type = fs::symlink_status(p, ec).type();
  return type != fs::file_type::not_found && type != fs::file_type::none;
}

// Removes .import-<pid>-* folders left by an import that was killed: its process is gone and the folder is older
// than ten minutes.
void sweep_abandoned_imports(const fs::path& root) {
  const auto now = fs::file_time_type::clock::now();
  std::error_code ec;
  for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.rfind(".import-", 0) != 0) continue;
    char* stop = nullptr;
    const long pid = std::strtol(name.c_str() + 8, &stop, 10);
    std::error_code type_ec;
    if (stop == name.c_str() + 8 || *stop != '-' || pid <= 0 || !it->is_directory(type_ec) || it->is_symlink(type_ec)) {
      continue;
    }
    if (platform::process_running(pid)) continue;   // still running, or can't tell
    const auto modified = fs::last_write_time(it->path(), type_ec);
    if (type_ec || now - modified < std::chrono::minutes(10)) continue;
    fs::remove_all(it->path(), type_ec);
  }
}

// root/.import-<pid>-<random>/, removed unless it was moved into place.
class Staging {
 public:
  explicit Staging(const fs::path& root) {
    sweep_abandoned_imports(root);
    std::random_device random;
    for (int attempt = 0; attempt < 100 && path_.empty(); ++attempt) {
      char suffix[32];
      std::snprintf(suffix, sizeof suffix, "%d-%08x", static_cast<int>(platform::current_pid()), random());
      const fs::path p = root / (std::string(".import-") + suffix);
      if (const int err = platform::make_dir(p.string(), 0755); err == 0) {
        path_ = p;
        platform::set_mode(p.string(), 0755);
      } else if (err != EEXIST) {
        fail("Vyrn can't create a folder in " + root.string() + " (" + platform::error_text(err) + ").");
      }
    }
    if (path_.empty()) fail("Vyrn can't create a folder in " + root.string() + ".");
  }
  ~Staging() {
    if (path_.empty()) return;
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  Staging(const Staging&) = delete;
  Staging& operator=(const Staging&) = delete;
  const fs::path& path() const { return path_; }

  // Moves the folder to `target`. False (nothing changed) when that name is taken.
  bool publish(const fs::path& target) {
    const int err = platform::rename_no_replace(path_.string(), target.string(), /*directory=*/true);
    if (err == 0) {
      path_.clear();
      return true;
    }
    if (err == ENOTEMPTY || err == EEXIST) return false;
    fail("Vyrn can't create " + target.string() + " (" + platform::error_text(err) + ").");
  }

 private:
  fs::path path_;
};

// "<name> 2" -> its folder; long names keep their number.
std::string folder_for(const std::string& base, int n) {
  if (n == 1) return folder_name_for(base);
  const std::string suffix = "-" + std::to_string(n);
  std::string folder = folder_name_for(base + " " + std::to_string(n));
  if (folder.size() >= suffix.size() && folder.compare(folder.size() - suffix.size(), suffix.size(), suffix) == 0) {
    return folder;
  }
  folder = folder_name_for(base);
  if (folder.size() > 64 - suffix.size()) folder.resize(64 - suffix.size());
  while (!folder.empty() && folder.back() == '-') folder.pop_back();
  return (folder.empty() ? "project" : folder) + suffix;
}

std::string join_names(const std::vector<std::string>& names) {
  std::string out;
  for (size_t i = 0; i < names.size(); ++i) {
    if (i) out += ", ";
    out += shown_name(names[i]);
  }
  return out;
}

void copy_agent_files(const fs::path& staging, const std::string& template_id, std::vector<std::string>& notes) {
  const std::string data = find_data_dir(nullptr);
  if (data.empty()) return;
  const fs::path templates = fs::path(data) / "templates";
  std::error_code ec;
  fs::path dir = templates / "my-game";
  if (!template_id.empty() && util::is_safe_name(template_id) && fs::is_directory(templates / template_id, ec)) {
    dir = templates / template_id;
  } else if (!template_id.empty()) {
    notes.push_back("This project was made from the template \"" + template_id +
                    "\", which this Vyrn doesn't have. It may need a newer Vyrn.");
  }
  for (const char* name : {"AGENTS.md", "CLAUDE.md", "GEMINI.md"}) {
    const fs::path src = dir / name;
    if (!fs::is_regular_file(src, ec) || fs::file_size(src, ec) > (1u << 20)) continue;
    std::string text;
    if (!util::read_text_file(src.string(), text)) continue;
    write_new_file(staging, name, text);
  }
}

std::string marker_text(const ProjectPackage& p, const std::string& file, int64_t now) {
  const json marker = {{"file", clean_text(file, 255)}, {"name", p.name},
                       {"author", p.author},            {"created", p.created},
                       {"imported", local_minutes(now)}, {"start_program", p.start_program},
                       {"trusted", false}};
  return marker.dump(2) + "\n";
}

// The fit check of section 6, step 2. Returns notes; throws PackageError when the brain doesn't fit.
std::string strip_new_brain(std::string why) {
  const std::string tail = " This change starts a new brain.";
  if (why.size() >= tail.size() && why.compare(why.size() - tail.size(), tail.size(), tail) == 0) {
    why.resize(why.size() - tail.size());
  }
  if (why.size() > 1 && why[0] >= 'A' && why[0] <= 'Z' && why[1] >= 'a' && why[1] <= 'z') {
    why[0] = static_cast<char>(why[0] - 'A' + 'a');
  }
  return why;
}

std::string list_text(const std::vector<std::string>& names) {
  std::string out;
  for (size_t i = 0; i < names.size(); ++i) {
    if (i) out += ", ";
    out += names[i];
  }
  return out.empty() ? "(none)" : out;
}

std::string shape_text(const brain::BrainShape& s) {
  if (s.kind == brain::ObsSpec::Kind::Numbers) {
    return std::to_string(s.d) + (s.d == 1 ? " number" : " numbers");
  }
  return "a " + std::to_string(s.in_w) + "x" + std::to_string(s.in_h) + " picture (" + std::to_string(s.frames) +
         (s.frames == 1 ? " frame)" : " frames)");
}

// Exact comparison with the project's active brain: "" when they are the same, else why not.
std::string difference(const brain::BrainShape& have, const brain::BrainShape& brain) {
  if (have.kind != brain.kind || have.d != brain.d || have.in_c != brain.in_c || have.in_h != brain.in_h ||
      have.in_w != brain.in_w || have.small_picture != brain.small_picture || have.frames != brain.frames) {
    return "this project's brain sees " + shape_text(have) + " and this one sees " + shape_text(brain) + ".";
  }
  if (have.inputs != brain.inputs) {
    return "the numbers have different names (this project: " + list_text(have.inputs) +
           "; this brain: " + list_text(brain.inputs) + ").";
  }
  if (have.choices != brain.choices) {
    return "the choices are different (this project: " + list_text(have.choices) +
           "; this brain: " + list_text(brain.choices) + ").";
  }
  return "";
}

std::vector<std::string> fit_check(const BrainPackage& b, const std::string& project, const std::string& spec_json) {
  std::vector<std::string> notes;
  if (!spec_json.empty()) {
    const json j = json::parse(spec_json, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) fail("Vyrn couldn't read what the project's scripts produce (from Check).");
    brain::Spec spec;
    try {
      spec = brain::parse_spec(j);
    } catch (const std::exception& e) {
      fail(std::string("Vyrn couldn't read what the project's scripts produce: ") + e.what());
    }
    std::string why;
    switch (brain::compare(b.shape, spec, &why)) {
      case brain::Compat::Same:
        if (!why.empty()) notes.push_back(why);
        break;
      case brain::Compat::AppendedChoices:
        notes.push_back(why + " This brain was trained without them.");
        break;
      case brain::Compat::Incompatible:
        fail("This brain doesn't fit this project: " + strip_new_brain(why));
    }
    return notes;
  }
  const util::ProjectSettings settings = util::read_settings(project);
  if (!settings.brain.empty() && util::is_safe_name(settings.brain)) {
    const std::string dir = (fs::path(project) / "brains" / settings.brain).string();
    brain::BrainMeta meta;
    bool have = false;
    try {
      have = brain::read_checkpoint_meta(dir, "latest", meta) || brain::read_checkpoint_meta(dir, "best", meta);
    } catch (const std::exception&) {
      have = false;
    }
    brain::BrainShape active;
    bool readable = false;
    if (have) {
      try {
        active = meta.j.at("shape").get<brain::BrainShape>();
        readable = true;
      } catch (const std::exception&) {
        readable = false;
      }
    }
    if (readable) {
      const std::string diff = difference(active, b.shape);
      if (!diff.empty()) fail("This brain doesn't fit this project: " + diff);
      return notes;
    }
  }
  notes.push_back("Vyrn couldn't check that this brain fits your scripts yet: press Check.");
  return notes;
}

std::string ignored_note() {
  return "Some details in this file come from a newer Vyrn and were left out.";
}

}  // namespace

// ---- export --------------------------------------------------------------------------------------------------

PackageInfo export_brain(const std::string& project, const std::string& out_file, const ExportOptions& options) {
  return guarded("saving the brain file", [&] {
    PartFile part(out_file, options.overwrite);
    const int64_t now = now_seconds();
    const std::string bytes = build_brain_package(project, options.brain_id, options.which, options, now, nullptr);
    part.write(bytes.data(), bytes.size());
    part.sync();
    stage("part-written");
    PackageInfo info = check_written(part, out_file, "brain");
    part.commit();
    return info;
  });
}

PackageInfo export_project(const std::string& project, const std::string& out_file, const ExportOptions& options) {
  return guarded("saving the project file", [&] {
    PartFile part(out_file, options.overwrite);
    write_project_package(part, project, options, now_seconds());
    part.sync();
    stage("part-written");
    PackageInfo info = check_written(part, out_file, "project");
    part.commit();
    return info;
  });
}

StandaloneResult export_standalone(const std::string& project, const std::string& out_dir,
                                   const StandaloneOptions& options) {
  return guarded("compiling the project", [&] { return write_standalone(project, out_dir, options, now_seconds()); });
}

std::string suggested_file_name(const std::string& project, const std::string& kind) {
  return guarded("choosing a file name", [&] {
    if (kind != "brain" && kind != "project" && kind != "standalone") {
      fail("The kind of file must be \"brain\", \"project\" or \"standalone\".");
    }
    ProjectInfo info;
    try {
      info = read_project(project);
    } catch (const std::exception&) {
      fail("There is no Vyrn project at " + project + ".");
    }
    const std::string base = folder_name_for(info.name);
    if (kind == "standalone") return base + "-play";
    return kind == "brain" ? base + "-brain" + kBrainExt : base + kProjectExt;
  });
}

// ---- inspect -------------------------------------------------------------------------------------------------

PackageInfo inspect_package(const std::string& file) {
  return guarded("reading " + base_name(file), [&] {
    const std::string label = base_name(file);
    const std::string bytes = read_package_bytes(file, kMaxPackageFile);
    std::string kind = kind_for_extension(file);
    if (kind.empty()) kind = kind_by_content(bytes, label);
    return kind == "brain" ? info_of(parse_brain_package(bytes, label)) : info_of(parse_project_bytes(bytes, label));
  });
}

// ---- import --------------------------------------------------------------------------------------------------

ImportResult import_project(const std::string& file, const std::string& root) {
  return guarded("importing " + base_name(file), [&] {
    const std::string label = base_name(file);
    const std::string bytes = read_package_bytes(file, kMaxPackageFile);
    if (kind_for_extension(file) == "brain") {
      fail(label + " is a single brain (.vrl), not a whole project (.vrlp). Open a project and import the brain "
                   "into it instead.");
    }
    std::unique_ptr<ZipReader> zip;
    const ProjectPackage p = parse_project_bytes(bytes, label, &zip);   // everything is checked before writing

    const fs::path base = fs::absolute(root.empty() ? projects_root() : root).lexically_normal();
    std::error_code ec;
    fs::create_directories(base, ec);
    if (!fs::is_directory(base, ec)) fail("Vyrn can't create the projects folder " + base.string() + ".");
    const int64_t now = now_seconds();
    ImportResult result;
    std::vector<std::string>& notes = result.notes;

    Staging staging(base);
    // The trust marker comes first: the folder is never a project Vyrn would run, not even while it is built.
    write_new_file(staging.path(), kTrustMarker, marker_text(p, label, now));
    std::vector<std::string> skipped, agent_files;
    for (const ProjectFileEntry& f : p.files) {
      if (!f.skip_reason.empty()) {
        if (is_agent_file(f.path)) {
          agent_files.push_back(f.path);
        } else {
          skipped.push_back("Skipped " + shown_name(f.path) + ": " + f.skip_reason);
        }
        continue;
      }
      if (f.path == "vyrn.json") continue;   // written below, after the allow-list
      write_new_file(staging.path(), f.path, zip->read(*zip->find("project/" + f.path)));
    }
    std::string active_id;
    for (const ProjectBrainEntry& b : p.brains) {
      const std::string id = install_brain(staging.path().string(), b.brain, b.id.empty() ? b.brain.id : b.id, label);
      if (b.active) active_id = id;
    }
    copy_agent_files(staging.path(), p.template_id, notes);

    ordered_json settings = p.settings_damaged ? ordered_json::object() : p.settings;
    if (!p.template_id.empty() && !settings.contains("template")) settings["template"] = p.template_id;
    if (active_id.empty()) {
      settings.erase("brain");
    } else {
      settings["brain"] = active_id;
    }

    // Move into place under "<name>", else "<name> 2", "<name> 3", ...
    std::string display, folder;
    for (int n = 1; n < 1000 && folder.empty(); ++n) {
      const std::string candidate = folder_for(p.name, n);
      if (taken(base / candidate)) continue;
      display = n == 1 ? p.name : p.name + " " + std::to_string(n);
      settings["name"] = display;
      const fs::path settings_file = staging.path() / "vyrn.json";
      fs::remove(settings_file, ec);
      write_new_file(staging.path(), "vyrn.json", util::dump_settings(settings));
      sync_folder(staging.path());
      if (staging.publish(base / candidate)) folder = candidate;
    }
    if (folder.empty()) fail("Vyrn can't find a free folder name for " + p.name + " in " + base.string() + ".");
    sync_folder(base);

    result.project = (base / folder).string();
    result.brain_id = active_id;
    if (!p.dropped_settings.empty()) {
      notes.push_back("Vyrn left out these settings from the shared project: " + join_names(p.dropped_settings) + ".");
    }
    if (p.settings_damaged) notes.push_back("The project's settings file (vyrn.json) was damaged, so Vyrn made a new one.");
    if (!p.start_program.empty()) {
      notes.push_back("The author started `" + p.start_program +
                      "` with this project. To do the same, set it in Settings → Start program.");
    }
    if (!agent_files.empty()) {
      notes.push_back("The AI agent instructions in the file (" + join_names(agent_files) +
                      ") were replaced with Vyrn's own.");
    }
    for (size_t i = 0; i < skipped.size() && i < 5; ++i) notes.push_back(skipped[i]);
    if (skipped.size() > 5) notes.push_back("Vyrn skipped " + std::to_string(skipped.size() - 5) + " more files.");
    bool newer = !p.ignored_keys.empty();
    for (const ProjectBrainEntry& b : p.brains) newer = newer || !b.brain.ignored_keys.empty();
    if (newer) notes.push_back(ignored_note());
    return result;
  });
}

ImportResult import_brain(const std::string& file, const std::string& project, const std::string& current_spec_json,
                          bool make_active) {
  return guarded("importing " + base_name(file), [&] {
    const std::string label = base_name(file);
    const std::string bytes = read_package_bytes(file, kMaxPackageFile);
    if (kind_for_extension(file) == "project") {
      fail(label + " is a whole project (.vrlp), not a single brain (.vrl). Open it as a project instead.");
    }
    const BrainPackage b = parse_brain_package(bytes, label);
    const std::string dir = project_path_or_fail(project);
    ImportResult result;
    result.project = dir;
    result.notes = fit_check(b, dir, current_spec_json);
    result.brain_id = install_brain(dir, b, b.id, label);
    if (make_active) {
      std::string why;
      if (!merge_settings(dir, json{{"brain", result.brain_id}}.dump(), &why)) {
        result.notes.push_back("Vyrn couldn't make it the active brain: " + why);
      }
    }
    if (!b.ignored_keys.empty()) result.notes.push_back(ignored_note());
    return result;
  });
}

}  // namespace vyrn
