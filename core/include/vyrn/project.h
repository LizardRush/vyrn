// Projects on disk: templates, project folders, settings, brains, locks, PNG (docs/dev/mcp.md section 6).
//
// Standard library only: the Qt app, vyrn-cli and the MCP server include this header. Implemented in
// core/src/project/ (library vyrn_project). Errors that a person may read are plain English.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vyrn {

struct TemplateInfo {
  std::string id;            // folder name, e.g. "catch-pictures"
  std::string name;          // vyrn.json "name", e.g. "Catch (pictures)"
  std::string mode;          // "sim" (has world.py) | "realtime"
  std::string description;   // one line: vyrn.json "description", else a built-in default for known templates
  std::string path;          // absolute folder
  bool demo = false;         // a demo (catch-pictures, catch-numbers): New project lists it after the presets
};

struct BrainInfo {
  std::string id;            // folder name under <project>/brains/
  bool active = false;       // it is vyrn.json "brain"
  bool has_best = false;
  double best_mean20 = 0;    // best 20-round mean points
  double time_trained_s = 0;
  int64_t ticks = 0;
  std::string updated;       // local time of the latest save, "YYYY-MM-DD HH:MM" ("" if unknown)
};

struct ProjectInfo {
  std::string name;          // vyrn.json "name", else the folder name
  std::string path;          // absolute folder
  std::string mode;          // "sim" (has world.py) | "realtime"
  std::string template_id;   // vyrn.json "template"
  bool has_best = false;     // of the active brain
  double best_mean20 = 0;
  double time_trained_s = 0;
  std::string training_by;   // "" | "app" | "agent" | "cli": who holds a live lock right now
  int64_t modified = 0;      // newest mtime of vyrn.json, the scripts and brains (seconds since the epoch)
};

// $VYRN_PROJECTS if set, else ~/Vyrn. Created when missing.
std::string projects_root();

// Every folder in <data_dir>/templates with a vyrn.json, sorted: balance, demo-game, my-game, then the demos
// catch-pictures and catch-numbers (in that order), then the rest by name. An empty data_dir means find_data_dir() (vyrn/session.h).
std::vector<TemplateInfo> list_templates(const std::string& data_dir);

// Every folder in root with a vyrn.json or an inputs.py, newest first (hidden folders are skipped: Vyrn builds new
// projects under a hidden name and moves them into place when they are complete).
std::vector<ProjectInfo> list_projects(const std::string& root);

// Throws std::runtime_error when path is not a project folder.
ProjectInfo read_project(const std::string& path);

// "My Game!" -> "My-Game" (letters, digits, '-', '_'; spaces become '-'; never empty: "project").
std::string folder_name_for(const std::string& name);

// Copies the template into <root>/<folder_name_for(name)>, skipping brains/, runs/, __pycache__ and lock files,
// then sets "name" and "template" in its vyrn.json. Never overwrites: throws std::runtime_error when the folder
// already exists or the name is empty. Returns the new absolute path.
std::string create_project(const std::string& root, const std::string& name, const TemplateInfo& tmpl);

// Copies a project (without brains/, runs/ and the lock) to a new folder next to it named "<name> copy"
// (then "copy 2", ...). Returns the new path. That name goes into the copy's vyrn.json; the folder is
// folder_name_for(it), e.g. "Catch (pictures)" -> "Catch (pictures) copy" in "Catch-pictures-copy". The copy's
// "brain" is cleared (it has no brains/). Throws std::runtime_error (e.g. when path is not a project).
std::string duplicate_project(const std::string& path);

// vyrn.json as text ("{}" when missing).
std::string read_settings_text(const std::string& project);
// Merges the top-level keys of a JSON object into vyrn.json, keeping every other key and the key order; creates
// the file when missing. Returns false + a plain-English reason instead of overwriting a file that is not a JSON
// object, or when the text is not a JSON object.
bool merge_settings(const std::string& project, const std::string& json_object_text, std::string* why);

// Brains in <project>/brains/: the active one first, then newest first (by their latest save). A folder counts once
// it has a readable save in latest/ or best/ (or when it is the active one).
std::vector<BrainInfo> list_brains(const std::string& project);

// One trainer per project: <project>/.vyrn-lock holds {"pid":123,"by":"app","since":"2026-11-13T17:05:00"}
// (plus "start" and "boot": the holder's start time and the boot id, so a pid reused by another program or after a
// restart never looks like a live holder).
class ProjectLock {
 public:
  struct Holder {
    int pid = 0;
    std::string by;      // "app" | "agent" | "cli"
    std::string since;
  };
  // Creates the lock atomically (O_CREAT|O_EXCL). A lock whose pid is no longer running is stale and is taken
  // over. Returns nullopt when another live process holds it, and also when the lock file can't be created at
  // all (e.g. a missing or read-only folder: holder() then returns nullopt as well). Never throws.
  static std::optional<ProjectLock> acquire(const std::string& project, const std::string& by);
  // The live holder, or nullopt (no lock, or a stale one).
  static std::optional<Holder> holder(const std::string& project);

  ProjectLock(ProjectLock&& other) noexcept;
  ProjectLock& operator=(ProjectLock&& other) noexcept;
  ProjectLock(const ProjectLock&) = delete;
  ProjectLock& operator=(const ProjectLock&) = delete;
  ~ProjectLock();   // removes the lock file if it still belongs to this process

 private:
  explicit ProjectLock(std::string path);
  std::string path_;
  std::string content_;   // the exact text this lock wrote: the destructor removes only that file
};

// <project>/<file_name>, refusing anything but a plain file name directly inside the folder (no '/', no '..',
// no leading '.', no NUL; also no backslash, no control characters, at most 255 bytes). Throws std::invalid_argument
// with a plain-English message. Lexical only: check is_inside() too before writing through a possible symlink.
std::string safe_join(const std::string& project, const std::string& file_name);

// True when `path`, after resolving symlinks (or the nearest existing parent), is `dir` or inside it.
bool is_inside(const std::string& dir, const std::string& path);

// 8-bit RGB PNG (zlib). `rgb` is height*width*3 bytes, row by row. scale >= 1 enlarges with nearest-neighbour
// (handy for tiny pictures such as a 10x10 canvas).
std::vector<uint8_t> png_encode(int width, int height, const std::vector<uint8_t>& rgb, int scale = 1);

}  // namespace vyrn
