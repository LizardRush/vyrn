#include "brain/checkpoint.h"

#include "platform/files.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace vyrn::brain {

namespace fs = std::filesystem;

namespace test_hooks {
std::function<void(const char* stage)> save_stage;
}  // namespace test_hooks

namespace {

constexpr const char* kPointer = "current";
constexpr const char* kPointerTmp = "current.tmp";
constexpr const char* kSavePrefix = "save-";

void stage(const char* name) {
  if (test_hooks::save_stage) test_hooks::save_stage(name);
}

[[noreturn]] void io_error(const std::string& what, const fs::path& p, int err) {
  throw std::system_error(err, std::generic_category(), what + " " + p.string());
}

void fsync_path(const fs::path& p, bool directory) {
  if (const int err = platform::sync_path(p.string(), directory); err != 0) io_error("Vyrn can't save", p, err);
}

void write_file_synced(const fs::path& p, const std::string& text) {
  int err = 0;
  platform::File file = platform::File::create(p.string(), platform::File::Create::Truncate, false, 0644, err);
  if (!file) io_error("Vyrn can't create", p, err);
  if ((err = file.write_all(text.data(), text.size())) != 0) io_error("Vyrn can't write", p, err);
  if ((err = file.sync()) != 0) io_error("Vyrn can't save", p, err);
}

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) throw std::runtime_error("Vyrn can't read " + p.string());
  std::ostringstream os;
  os << in.rdbuf();
  return os.str();
}

// "save-12" -> 12, anything else -> -1.
int64_t save_number(const std::string& name) {
  const std::string prefix = kSavePrefix;
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

// save-* folders, newest (highest number) first.
std::vector<std::pair<int64_t, fs::path>> list_saves(const fs::path& which_dir) {
  std::vector<std::pair<int64_t, fs::path>> saves;
  std::error_code ec;
  for (fs::directory_iterator it(which_dir, ec), end; !ec && it != end; it.increment(ec)) {
    const int64_t n = save_number(it->path().filename().string());
    if (n >= 0 && it->is_directory(ec)) saves.emplace_back(n, it->path());
  }
  std::sort(saves.begin(), saves.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  return saves;
}

// The save named by the `current` file, or "" if there is none (or it is unreadable).
std::string read_pointer(const fs::path& which_dir) {
  std::error_code ec;
  if (!fs::is_regular_file(which_dir / kPointer, ec)) return "";
  try {
    std::string name = read_file(which_dir / kPointer);
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
    return save_number(name) >= 0 ? name : "";
  } catch (const std::exception&) {
    return "";
  }
}

fs::path which_path(const std::string& brain_dir, const std::string& which) {
  const bool ok = !which.empty() && std::all_of(which.begin(), which.end(), [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
  });
  if (!ok) throw std::invalid_argument("Checkpoint names may only use letters, digits, - and _ (got \"" + which + "\").");
  return fs::path(brain_dir) / which;
}

// Folders to try when loading: the live save first, then the others, newest first.
std::vector<fs::path> load_candidates(const fs::path& which_dir) {
  std::vector<fs::path> out;
  const std::string live = read_pointer(which_dir);
  if (!live.empty()) out.push_back(which_dir / live);
  for (const auto& save : list_saves(which_dir)) {
    if (save.second.filename() != live) out.push_back(save.second);
  }
  return out;
}

json read_meta(const fs::path& save_dir) {
  json j = json::parse(read_file(save_dir / "brain.json"));
  if (!j.is_object()) throw std::runtime_error("brain.json is not an object");
  return j;
}

// Writes one complete save folder with `write_weights`, then swaps the pointer (see checkpoint.h).
void write_save(const std::string& brain_dir, const std::string& which,
                const std::function<void(const fs::path&)>& write_weights, const BrainMeta& meta,
                const json& dqn_state) {
  const fs::path which_dir = which_path(brain_dir, which);
  fs::create_directories(which_dir);
  fsync_path(which_dir.has_parent_path() ? which_dir.parent_path() : fs::path("."), true);

  int64_t n = std::max<int64_t>(1, save_number(read_pointer(which_dir)) + 1);
  for (const auto& save : list_saves(which_dir)) n = std::max(n, save.first + 1);
  const std::string name = kSavePrefix + std::to_string(n);
  const fs::path dir = which_dir / name;
  fs::create_directory(dir);
  stage("folder");

  write_weights(dir);
  json j = meta.j.is_object() ? meta.j : json::object();
  j["dqn"] = dqn_state;
  j["checkpoint"] = {{"format", 1}, {"save", name}};
  write_file_synced(dir / "brain.json", j.dump(2));
  stage("meta");

  for (const auto& entry : fs::directory_iterator(dir)) {
    if (entry.is_regular_file()) fsync_path(entry.path(), false);
  }
  fsync_path(dir, true);
  fsync_path(which_dir, true);
  stage("synced");

  write_file_synced(which_dir / kPointerTmp, name + "\n");
  stage("pointer");
  // Atomic: readers see the old or the new save.
  if (const int err = platform::rename_replace((which_dir / kPointerTmp).string(), (which_dir / kPointer).string())) {
    io_error("Vyrn can't save", which_dir / kPointer, err);
  }
  fsync_path(which_dir, true);
  stage("swapped");

  for (const auto& save : list_saves(which_dir)) {
    if (save.second.filename() != name) {
      std::error_code ec;
      fs::remove_all(save.second, ec);   // best effort: a leftover folder is harmless
    }
  }
}

}  // namespace

void save_checkpoint(const std::string& brain_dir, const std::string& which, const Dqn& dqn, const BrainMeta& meta) {
  write_save(
      brain_dir, which,
      [&](const fs::path& dir) {
        save_model_file((dir / "model.pt").string(), dqn.online(), dqn.target(), dqn.state());
        stage("model");
        torch::save(dqn.optimizer(), (dir / "optim.pt").string());
        stage("optim");
      },
      meta, dqn.state_json());
}

void save_checkpoint(const std::string& brain_dir, const std::string& which, const Policy& weights,
                     const BrainMeta& meta) {
  json state = {{"updates", weights.state().updates},
                {"reward_scale", weights.state().reward_scale()},
                {"max_abs_reward", weights.state().max_abs_reward},
                {"reward_scale_frozen", weights.state().reward_frozen},
                {"net", to_string(weights.net()->config().kind)},
                {"choices", weights.net()->config().n_actions},
                {"weights_only", true}};
  write_save(
      brain_dir, which,
      [&](const fs::path& dir) {
        save_model_file((dir / "model.pt").string(), weights.net(), weights.net(), weights.state());
        stage("model");
      },
      meta, state);
}

bool load_checkpoint(const std::string& brain_dir, const std::string& which, Dqn& dqn, BrainMeta& meta) {
  const fs::path which_dir = which_path(brain_dir, which);
  std::error_code ec;
  if (!fs::is_directory(which_dir, ec)) return false;
  for (const fs::path& dir : load_candidates(which_dir)) {
    try {
      json j = read_meta(dir);
      dqn.load(dir.string());   // all or nothing
      meta.j = std::move(j);
      return true;
    } catch (const BrainMismatch&) {
      throw;   // a real brain for another network: never skip it silently
    } catch (const std::exception&) {
      // Half-written or damaged: try the next one.
    }
  }
  return false;
}

bool read_checkpoint_meta(const std::string& brain_dir, const std::string& which, BrainMeta& meta) {
  const fs::path which_dir = which_path(brain_dir, which);
  std::error_code ec;
  if (!fs::is_directory(which_dir, ec)) return false;
  for (const fs::path& dir : load_candidates(which_dir)) {
    try {
      json j = read_meta(dir);
      if (!fs::is_regular_file(dir / "model.pt", ec)) continue;
      meta.j = std::move(j);
      return true;
    } catch (const std::exception&) {
    }
  }
  return false;
}

}  // namespace vyrn::brain
