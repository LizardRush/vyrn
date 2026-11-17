// Shared helpers for the sharing-file tests (test_package). No torch here: brains are made in helpers.cpp.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pkg_test {

namespace fs = std::filesystem;

// A fresh folder under $TMPDIR (or /tmp), removed with everything in it at the end of the test.
class TempDir {
 public:
  TempDir();
  ~TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  const std::string& path() const { return path_; }
  std::string operator/(const std::string& rel) const { return (fs::path(path_) / rel).string(); }

 private:
  std::string path_;
};

// Sets an environment variable for the lifetime of the object (restores the old value).
class ScopedEnv {
 public:
  ScopedEnv(const char* name, const std::string& value);
  ~ScopedEnv();

 private:
  std::string name_;
  std::optional<std::string> old_;
};

void write_file(const fs::path& path, const std::string& bytes);
std::string read_file(const fs::path& path);
bool present(const fs::path& path);
// Every file and folder below root (relative, sorted), hidden ones included.
std::vector<std::string> tree(const fs::path& root);

enum class Net { MLP, SmallCNN, DER };

// <root>/<folder>: inputs.py, responses.py, points.py, world.py and a vyrn.json with name and template.
std::string make_project(const std::string& root, const std::string& folder, const std::string& name,
                         const std::string& template_id = "catch-numbers");
// The shape a test brain of this kind has (as brain.json "shape" JSON text).
std::string shape_json(Net net, const std::vector<std::string>& choices = {"left", "right", "stay"});
// The worker's spec reply for scripts that fit a test brain of this kind (JSON text).
std::string spec_json(Net net, const std::vector<std::string>& choices = {"left", "right", "stay"});
// Saves a brain with random weights (from `seed`), one learning update and some counters as
// <project>/brains/<id>/latest (+ best when with_best), and makes it the project's active brain.
void add_brain(const std::string& project, const std::string& id, Net net, uint64_t seed = 1,
               bool with_best = true, const std::vector<std::string>& choices = {"left", "right", "stay"});
// The online network's tensors of a saved brain (name -> raw float bytes), loaded with the core's own code.
std::map<std::string, std::string> saved_weights(const std::string& brain_dir, const std::string& which);
// The saved brain's Q-values for a fixed, seeded batch of inputs.
std::vector<float> saved_q_values(const std::string& brain_dir, const std::string& which);
// The saved brain's brain.json (text) and its counters.
std::string saved_meta(const std::string& brain_dir, const std::string& which);
int64_t saved_updates(const std::string& brain_dir, const std::string& which);

// ---- raw ZIP building, for hostile archives ----
uint32_t crc32_of(const std::string& data);
std::string deflate_raw(const std::string& data);

struct RawEntry {
  std::string name;
  std::string data;                        // uncompressed
  bool deflate = false;
  uint16_t flags = 0x0800;
  uint16_t version_needed = 20;
  std::optional<uint16_t> method;          // default: 8 when deflate else 0
  std::optional<uint32_t> crc;             // both headers
  std::optional<uint32_t> cd_csize, cd_usize, local_csize, local_usize;
  std::optional<std::string> local_name;
  std::string local_extra, cd_extra;
  bool descriptor = false;                 // bit 3, zero sizes in the local header, descriptor after the data
  std::optional<uint32_t> cd_offset;       // the local header offset the central directory names
  std::optional<std::string> raw_body;     // the stored bytes instead of data (deflated or not)
  bool cd_only = false;                    // no local header of its own (use cd_offset to point somewhere)
};

struct RawZip {
  std::vector<RawEntry> entries;
  std::string comment;
  bool zip64_locator = false;
  uint16_t disk = 0;
  std::optional<uint16_t> count;
  std::string build() const;
};

// All entries of a valid archive (name, data), in order.
std::vector<std::pair<std::string, std::string>> unzip_all(const std::string& bytes);
// The entries zipped again the way Vyrn writes them.
std::string rezip(const std::vector<std::pair<std::string, std::string>>& entries);
// Replaces (or adds, when missing) one entry and zips again.
std::string with_entry(const std::string& zip_bytes, const std::string& name, const std::string& data);
std::string entry_of(const std::string& zip_bytes, const std::string& name);
std::string sha256_of(const std::string& data);

// A tiny valid PNG (w x h, RGB).
std::string tiny_png(int w = 2, int h = 2);

}  // namespace pkg_test
