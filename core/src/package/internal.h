// Private helpers shared by the vyrn_package sources (docs/dev/package.md). No torch in this header: the torch side
// (weights.cpp) is reached through plain types.
#pragma once

#include "brain/spec.h"
#include "package/safetensors.h"
#include "package/zip.h"
#include "platform/files.h"
#include "util/settings.h"
#include "vyrn/package.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vyrn::package_detail {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

// ---- limits (section 2, 4, 5) ----
constexpr uint64_t kMaxPackageFile = 1ull << 30;
constexpr size_t kMaxBrainEntries = 16;
constexpr uint64_t kMaxManifest = 4ull << 20;
constexpr uint64_t kMaxPreview = 4ull << 20;
constexpr size_t kMaxBrainsInProject = 8;
constexpr uint64_t kMaxProjectFile = 32ull << 20;
constexpr size_t kMaxProjectFiles = 2000;
constexpr uint64_t kMaxProjectBytes = 256ull << 20;
constexpr int kMaxProjectDepth = 8;   // folders below the project folder
constexpr size_t kMaxHistory = 1000;

constexpr const char* kBrainManifest = "vyrn-brain.json";
constexpr const char* kProjectManifest = "vyrn-project.json";
constexpr const char* kWeightsFile = "model.safetensors";
constexpr const char* kPreviewFile = "preview.png";
constexpr const char* kTrustMarker = ".vyrn-imported";
constexpr const char* kWeightsMessage = "the brain's weights are damaged or don't match its description.";

[[noreturn]] void fail(const std::string& message);   // throws PackageError

// ---- time ----
std::string utc_iso(int64_t unix_time);       // "2026-11-13T19:20:00Z"
std::string local_minutes(int64_t unix_time); // "2026-11-13 19:20"
bool valid_utc_iso(const std::string& text);

// ---- files ----
std::string base_name(const std::string& path);
// The whole file, refusing anything but a regular file of at most max_bytes. Throws PackageError.
std::string read_package_bytes(const std::string& path, uint64_t max_bytes);
// The extension's kind: "brain" for .vrl, "project" for .vrlp (any case), else "".
std::string kind_for_extension(const std::string& path);

// Writes <out>.part next to `out`, then fsync + an atomic rename (fsync the folder). Removes the .part file unless commit()
// succeeded. overwrite=false refuses an existing `out` before anything is written.
class PartFile {
 public:
  PartFile(const std::string& out, bool overwrite);
  ~PartFile();
  PartFile(const PartFile&) = delete;
  PartFile& operator=(const PartFile&) = delete;
  void write(const char* data, size_t size);
  void sync();                                  // fsync + close (before checking the written file)
  const std::string& part_path() const { return part_; }
  void commit();                                // rename into place

 private:
  std::string out_, part_;
  bool overwrite_;
  platform::File file_;
  bool done_ = false;
};

// ---- manifests ----
[[noreturn]] void bad_manifest(const std::string& label, const std::string& detail);
// Optional text field: missing or null -> "", anything but text is damage; the text is cleaned and cut.
std::string manifest_text(const json& m, const char* key, size_t max_chars, const std::string& label,
                          bool newlines = false);
// "format_version": 1 (or 1.x). A newer major version is refused with a plain message.
void check_format_version(const json& m, const std::string& label);
bool int_in(const json& v, int64_t lo, int64_t hi, int64_t& out);
// The first entry must be a stored "mimetype"; returns its content.
std::string mimetype_of(const ZipReader& zip, const std::string& label);
// PNG signature + a sane IHDR (at most 4096 x 4096 pixels): enough to show it safely.
bool plausible_png(std::string_view png);

// ---- folders and new files (imports and compiled folders) ----
// Creates the folder (0755) unless a folder of that name exists. Throws PackageError.
void make_folder(const std::filesystem::path& folder);
// Creates <root>/<rel> (a checked, '/'-separated name) with its folders: never through a symlink, never over an
// existing file; fsyncs it. mode: 0644, or 0755 for a program. Throws PackageError.
void write_new_file(const std::filesystem::path& root, const std::string& rel, std::string_view data, int mode = 0644);
void sync_folder(const std::filesystem::path& folder);   // fsync a folder (POSIX; nothing on Windows)

// ---- names and project files (section 5) ----
bool valid_brain_id(const std::string& id);              // [A-Za-z0-9][A-Za-z0-9_-]{0,63}
bool is_agent_file(const std::string& rel_path);         // AGENTS.md, CLAUDE.md, CLAUDE.local.md, GEMINI.md (any case)
// "" when a project file at rel_path (already a valid name) is shared, else a short plain reason for a note.
std::string project_file_skip_reason(const std::string& rel_path);
// vyrn.json reduced to the allow-list (in file order, with sane value types). dropped: removed keys ("python",
// "game_window", "advanced.buffer", "link.host", ...). "advanced" keeps only the brain settings Vyrn knows, as
// numbers within the limits a shared project may ask for (no huge replay buffers or batches); "link" only "port"
// and "lockstep".
ordered_json allowed_settings(const ordered_json& settings, std::vector<std::string>* dropped);
// The author's start_program as it goes into a shared file: their home folder becomes "~" and every absolute path
// is cut to its last part ("/home/ada/Games/run.sh -d /home/ada/x" -> "run.sh -d x"), so the file doesn't tell
// where things are on their computer or what their user name is.
std::string shareable_text(const std::string& text);

// ---- a project's shared files (section 5): what a .vrlp holds, and a compiled folder (docs/dev/standalone.md) ----
enum class Purpose { Share, Compile };   // what the files are read for: it goes into the messages

struct SharedFile {
  std::string rel;           // relative to the project folder, '/'-separated
  std::filesystem::path abs; // where it is on this computer
  uint64_t size = 0;
  std::string sha256;
};
// Every file of the project that goes into a shared copy, except vyrn.json (shared_settings() makes that one):
// the allow-list and exclusions of section 5 (no hidden files, brains/, runs/, Python caches, AI agent instructions,
// symlinks or other odd files), names that only differ in case resolved (the first is kept), every limit checked.
// Sorted by path. Throws PackageError.
std::vector<SharedFile> shared_files(const std::string& project_dir, Purpose purpose);
// The file's bytes once more, refused when they changed since shared_files() read them.
std::string read_shared_file(const SharedFile& file, Purpose purpose);
// The vyrn.json of a shared copy: the allow-list (allowed_settings), the project's name when it has none, and "brain"
// set to brain_id ("" removes it). Nothing machine-specific (Python, the game window, the start program) is left.
// util::dump_settings() writes it.
ordered_json shared_settings(const util::ProjectSettings& settings, const std::string& project_name,
                             const std::string& brain_id);

// ---- brain packages (.vrl) ----
struct BrainPackage {
  int format_version = 1;
  std::string vyrn_version, name, description, author, created, project, template_id, mode;
  std::string id;      // "" when the package's id isn't a valid id
  std::string which = "best";
  brain::BrainShape shape;
  std::string net, net_description;
  json obs, labels, effects, normalizer;   // validated, ready for brain.json
  int64_t model_updates = 0;
  double max_abs_reward = 0;
  bool reward_frozen = false;
  int64_t ticks = 0, rounds = 0, updates = 0;
  double seconds = 0;
  std::optional<double> best_mean20;
  std::vector<HistoryPoint> history;
  int tensors = 0;
  int64_t parameters = 0;
  std::string weights;                     // model.safetensors (checked)
  std::string preview_png;
  std::vector<std::string> ignored_keys;   // manifest keys this Vyrn doesn't know
  uint64_t file_size = 0;
};

// Reads and checks a whole .vrl held in memory (container, manifest, weights against the network). `label` names
// the file in messages. Throws PackageError.
BrainPackage parse_brain_package(std::string_view bytes, const std::string& label);

// Builds a complete .vrl from a project's saved brain. which_used: "best" or "latest" (whichever was read).
std::string build_brain_package(const std::string& project, const std::string& brain_id, const std::string& which,
                                const ExportOptions& options, int64_t now, std::string* which_used);

BrainSummary summary_of(const BrainPackage& brain);

// ---- project packages (.vrlp) ----
struct ProjectFileEntry {
  std::string path;          // relative to the project folder
  uint64_t size = 0;
  std::string sha256;
  std::string skip_reason;   // "" = an import writes it
};

struct ProjectBrainEntry {
  std::string file;          // "brains/<x>.vrl"
  std::string id;            // the manifest's id ("" when not a valid id)
  bool active = false;
  BrainPackage brain;
};

struct ProjectPackage {
  int format_version = 1;
  std::string vyrn_version, name, description, author, created, template_id, mode, start_program;
  bool uses_link = false;
  std::vector<ProjectFileEntry> files;          // every project/ entry, by path
  std::vector<ProjectBrainEntry> brains;        // the active one first
  std::string preview_png;
  std::vector<std::string> ignored_keys;
  bool settings_damaged = false;                // the packaged vyrn.json wasn't a JSON object
  ordered_json settings = ordered_json::object();   // the packaged vyrn.json after the allow-list
  std::vector<std::string> dropped_settings;
  uint64_t file_size = 0;
};

// Reads and checks a whole .vrlp held in memory (container, manifest, every file's checksum, every nested .vrl).
ProjectPackage parse_project_package(const ZipReader& zip, std::string_view bytes, const std::string& label);

// Writes a complete .vrlp of `project` into `part` (section 5).
void write_project_package(PartFile& part, const std::string& project, const ExportOptions& options, int64_t now);

// ---- compiled folders (standalone.cpp, docs/dev/standalone.md) ----
// Writes the whole folder (export_standalone() is this inside the usual error guard). `now` stamps brain.vrl.
StandaloneResult write_standalone(const std::string& project, const std::string& out_dir,
                                  const StandaloneOptions& options, int64_t now);

// ---- the torch side (weights.cpp) ----
struct LocalBrain {
  json meta;                 // brain.json of the save that was read
  brain::BrainShape shape;   // from meta, checked with check_shape_bounds
  std::string which;         // "best" | "latest"
  std::string safetensors;   // the online network's weights
  int tensors = 0;
  int64_t parameters = 0;
  int64_t updates = 0;
  double max_abs_reward = 0;
  bool reward_frozen = false;
};
// Reads <brain_dir>/<which> (or the other one when it has no save). Throws PackageError.
LocalBrain read_local_brain(const std::string& brain_dir, const std::string& which);
// Throws WeightsError unless the tensors are exactly the network's parameters and buffers (names and shapes).
void check_weights_fit(const brain::BrainShape& shape, const std::vector<TensorView>& tensors, int64_t* parameters);
// Writes <brain_dir>/best and <brain_dir>/latest (weights only) from checked tensors.
void write_brain_checkpoints(const std::string& brain_dir, const BrainPackage& brain, const json& meta);

// ---- shared by export and import ----
// Extra bounds of section 4 on top of from_json(BrainShape). Throws std::invalid_argument with a plain reason.
void check_shape_bounds(const brain::BrainShape& shape);
// The "obs" kept in brain.json: the given one when it agrees with the shape, else one rebuilt from the shape.
json checked_obs(const json& obs, const brain::BrainShape& shape);
std::string net_name_for(const brain::BrainShape& shape);
std::string net_description_for(const brain::BrainShape& shape);
// The brain.json written for an imported brain (section 6 step 4).
json imported_brain_meta(const BrainPackage& brain, const std::string& id, const std::string& package_file);
// Imports a checked brain into <project>/brains/<id> (a fresh id when needed). Returns the id.
std::string install_brain(const std::string& project, const BrainPackage& brain, const std::string& wanted_id,
                          const std::string& package_file);

// history (section 4)
std::vector<HistoryPoint> brain_history(const std::string& project, const std::string& brain_id);

// ---- test hooks ----
namespace test_hooks {
// Called with "part-written" once a .part file is complete, before it is checked and renamed. Throwing from it
// simulates a failure at that point.
extern std::function<void(const char* stage)> export_stage;
}  // namespace test_hooks

}  // namespace vyrn::package_detail
