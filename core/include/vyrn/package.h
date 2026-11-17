// Sharing trained AIs: .vrl files (one trained brain) and .vrlp files (a whole project: scripts, pictures,
// settings and its trained brain). Format, limits and safety rules: docs/dev/package.md (binding). Compile (a
// trained AI as a Python program that runs without Vyrn): docs/dev/standalone.md (binding).
//
// Standard library only: the Qt app, vyrn-cli and the MCP server include this header. Implemented in
// core/src/package/ (library vyrn_package). Every error a person may read is one plain-English sentence or two.
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace vyrn {

// A package that can't be written or read: damaged, unsafe, too big, or a brain that doesn't fit the project.
struct PackageError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

inline constexpr const char* kBrainExt = ".vrl";
inline constexpr const char* kProjectExt = ".vrlp";
inline constexpr const char* kBrainMime = "application/x-vyrn-brain";
inline constexpr const char* kProjectMime = "application/x-vyrn-project";

struct PackageFile {
  std::string path;     // relative to the project folder, '/'-separated, e.g. "pictures/game_over.png"
  uint64_t size = 0;    // bytes
};

struct HistoryPoint {
  int64_t round = 0;    // round number since the brain was created (1-based)
  double mean20 = 0;    // the 20-round mean points at that round
};

struct BrainSummary {
  std::string id;                        // the brain's folder name where it was trained
  std::string name;                      // person-readable name (default: the project's name)
  std::string which;                     // "best" | "latest": which saved weights these are
  std::string input;                     // "picture" | "numbers"
  std::vector<int> input_shape;          // picture: {height, width} the network uses; numbers: {count}
  std::vector<std::string> input_names;  // numbers returned as a dict, in order; else empty
  std::vector<std::string> choices;      // ordered choice names
  std::string net;                       // "mlp" | "small_cnn" | "der"
  std::string net_description;           // e.g. "MLP, 3 numbers -> 3 choices"
  bool has_best = false;
  double best_mean20 = 0;                // best 20-round mean points (when has_best)
  double time_trained_s = 0;
  int64_t ticks = 0, rounds = 0;
  std::vector<HistoryPoint> history;     // the points chart, at most 1000 points, oldest first; may be empty
};

struct PackageInfo {
  std::string kind;                      // "brain" (.vrl) | "project" (.vrlp)
  int format_version = 1;
  std::string vyrn_version;              // the Vyrn that wrote it
  std::string name;                      // the project's name
  std::string description, author;       // may be empty
  std::string created;                   // UTC, "YYYY-MM-DDTHH:MM:SSZ"
  std::string template_id;               // "" when unknown
  std::string mode;                      // "sim" (has world.py) | "realtime" (plays a real program)
  bool uses_link = false;                // project packages: a game connects over Vyrn Link (vyrn.json "link")
  std::string start_program;             // project packages: the author's Start program. A hint only: importing
                                         // never sets it, and nothing ever runs it automatically
  std::vector<PackageFile> files;        // project packages: the project files an import creates, sorted by path
  std::vector<BrainSummary> brains;      // .vrl: exactly one; .vrlp: 0 or more (the active one first)
  std::vector<uint8_t> preview_png;      // a picture the author included (e.g. what the AI sees); may be empty
  uint64_t file_size = 0;                // bytes
};

struct ExportOptions {
  std::string author;                    // optional, at most 100 characters. Never filled in automatically
  std::string description;               // optional, at most 2000 characters
  std::string brain_id;                  // "" = the project's active brain
  std::string which = "best";            // "best" | "latest"; the other one is used when this one doesn't exist
  bool include_brain = true;             // project packages: false = scripts, pictures and settings only
  std::vector<uint8_t> preview_png;      // optional PNG, at most 4 MiB
  bool overwrite = false;                // false: throw PackageError when out_file already exists
};

// Writes a .vrl holding one trained brain of `project`. Writes "<out_file>.part" next to it, fsyncs it and
// renames it into place, so a failed export never leaves a partial file. Returns what was written, as
// inspect_package() would read it. Throws PackageError (e.g. "This project has no trained brain yet.").
PackageInfo export_brain(const std::string& project, const std::string& out_file, const ExportOptions& options = {});

// Writes a .vrlp: the project's scripts, pictures and other small files (docs/dev/package.md section 5 lists
// what is left out and why), its settings without anything machine-specific, and its brain unless
// options.include_brain is false. Same writing rules as export_brain().
PackageInfo export_project(const std::string& project, const std::string& out_file,
                           const ExportOptions& options = {});

// A file name (no folder) for an export: "Catch-pictures.vrlp" / "Catch-pictures-brain.vrl" / "Catch-pictures-play"
// (safe on every system). kind: "brain" | "project" | "standalone" (the folder export_standalone() writes). Throws
// PackageError when `project` isn't a project folder.
std::string suggested_file_name(const std::string& project, const std::string& kind);

// ---- Compile: the trained AI as a Python program that runs without Vyrn (docs/dev/standalone.md) ----

struct StandaloneOptions {
  std::string which = "best";            // "best" | "latest"; the other one is used when this one doesn't exist
  bool include_wheel = true;             // put the vyrn package (wheel_file_name()) into the folder
  bool overwrite = false;                // replace an existing folder at out_dir, but only one Compile made (it
                                         // holds play.py and brain.vrl). false: an existing out_dir is refused
  std::string data_dir;                  // where the templates and the wheel are; "" = find_data_dir()
};

struct StandaloneResult {
  std::string folder;                    // the compiled folder (absolute)
  std::vector<PackageFile> files;        // every file written into it, sorted by path
  BrainSummary brain;                    // what brain.vrl holds
  std::string wheel;                     // the wheel's file name in the folder; "" when it was left out
  std::string mode;                      // "sim" (has world.py) | "realtime" (plays a real program)
  bool uses_link = false;                // a game connects over Vyrn Link (vyrn.json "link")
  std::vector<std::string> notes;        // plain-English things the person should know (e.g. a file left out)
};

// Writes the folder of docs/dev/standalone.md section 1 at out_dir: play.py and README.txt (from the templates in
// <data>/python/vyrn/runtime/templates), brain.vrl (the active brain, as export_brain() writes it), the project's
// files and vyrn.json by the .vrlp rules (docs/dev/package.md section 5: the same allow-list and exclusions, settings
// sanitized the same way, no AI agent instructions) and the wheel. It is built as "<out_dir>.part" and renamed into
// place, so a failed compile leaves nothing behind. Refuses a project that isn't trusted (with Check's sentence),
// one without a trained brain, and an out_dir inside the project. Throws PackageError.
StandaloneResult export_standalone(const std::string& project, const std::string& out_dir,
                                   const StandaloneOptions& options = {});

// The vyrn Python package a compiled folder needs: "vyrn-<this Vyrn's version>-py3-none-any.whl".
std::string wheel_file_name();
// That file in this Vyrn: <data>/wheels/<name>. A build that runs from its source tree (data_dir is the source
// folder it was built from) finds the one the build made in <build>/wheels. "" when there is none.
// data_dir: "" = find_data_dir().
std::string find_wheel(const std::string& data_dir = "");

// Reads and checks a .vrl or .vrlp completely (every entry, size, checksum and weight) without writing anything.
PackageInfo inspect_package(const std::string& file);

struct ImportResult {
  std::string project;                   // absolute folder of the new (or existing) project
  std::string brain_id;                  // the imported active brain ("" when none)
  std::vector<std::string> notes;        // plain-English things the person should know (e.g. a setting removed)
};

// .vrlp -> a new project in `root` (projects_root() when empty), in a folder named after the package
// ("<name> 2", "<name> 3", ... when taken). It is built in a hidden folder and moved into place when complete.
// The new project is untrusted (is_trusted() is false) until a person trusts it.
ImportResult import_project(const std::string& file, const std::string& root = "");

// .vrl -> a new brain in <project>/brains/ (saved as both its latest and its best), made the active brain
// unless make_active is false. current_spec_json: the project's `spec` from a Check (the worker's spec reply or
// the "spec" event, as JSON text); a brain whose input or choices don't fit it is refused with the reason.
// Empty: the brain is compared with the project's active brain instead; with neither, it is imported with a note.
ImportResult import_brain(const std::string& file, const std::string& project,
                          const std::string& current_spec_json = "", bool make_active = true);

// Projects imported from a .vrlp carry <project>/.vyrn-imported until a person trusts them. Check, Train and
// Watch refuse to run an untrusted project, because its Python code was written by someone else.
struct ImportOrigin {
  std::string file;                      // the package's file name (no folder)
  std::string name, author, created;     // from the package
  std::string imported;                  // local time of the import, "YYYY-MM-DD HH:MM"
  std::string start_program;             // the author's Start program (a hint; never set automatically)
  bool trusted = false;
};
std::optional<ImportOrigin> import_origin(const std::string& project);
// True for a project that wasn't imported, or that a person trusted. A damaged marker counts as untrusted.
bool is_trusted(const std::string& project);
// Records a person's decision to trust the project (keeps where it came from). Throws PackageError on I/O errors.
void trust_project(const std::string& project);

}  // namespace vyrn
