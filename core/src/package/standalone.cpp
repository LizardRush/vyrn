// Compile (docs/dev/standalone.md, binding): a trained AI as a folder with play.py that runs with plain Python and
// the vyrn package, without the Vyrn app. The project's files follow the .vrlp rules of docs/dev/package.md
// section 5 (shared_files(), shared_settings()); brain.vrl is the brain export (build_brain_package()).
#include "package/internal.h"
#include "package/text.h"
#include "platform/files.h"
#include "platform/system.h"
#include "util/files.h"
#include "util/settings.h"
#include "vyrn/project.h"
#include "vyrn/session.h"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <set>
#include <string_view>
#include <system_error>

#ifndef VYRN_VERSION
#define VYRN_VERSION "0.0.0"
#endif

namespace vyrn {

using namespace package_detail;
namespace fs = std::filesystem;

namespace {

constexpr const char* kPlayFile = "play.py";
constexpr const char* kReadmeFile = "README.txt";
constexpr const char* kBrainFile = "brain.vrl";
constexpr const char* kTemplates = "python/vyrn/runtime/templates";   // in the data folder
constexpr uint64_t kMaxTemplate = 1ull << 20;
constexpr uint64_t kMaxWheel = 64ull << 20;

bool exists_no_follow(const fs::path& p, platform::FileInfo* info = nullptr) {
  platform::FileInfo st;
  const bool there = platform::file_info(p.string(), /*follow=*/false, st) == 0;
  if (info) *info = st;
  return there;
}

bool regular_file_in(const fs::path& folder, const char* name) {
  platform::FileInfo st;
  return exists_no_follow(folder / name, &st) && st.is_regular();
}

#if defined(VYRN_SOURCE_DIR) && defined(VYRN_BUILD_WHEELS_DIR)
// The same folder, symlinks resolved (false when either can't be resolved).
bool same_folder(const fs::path& a, const fs::path& b) {
  std::error_code ea, eb;
  const fs::path ca = fs::weakly_canonical(a, ea), cb = fs::weakly_canonical(b, eb);
  return !ea && !eb && !ca.empty() && ca == cb;
}
#endif

// "/home/ada/Catch-play/" -> "/home/ada/Catch-play" (absolute, normalized, no trailing separator).
fs::path clean_folder(const std::string& path) {
  std::error_code ec;
  fs::path p = fs::absolute(fs::path(path), ec);
  if (ec) p = fs::path(path);
  p = p.lexically_normal();
  if (p.filename().empty() && p.has_parent_path() && p != p.root_path()) p = p.parent_path();
  return p;
}

// A template, read whole (a regular file of at most 1 MB).
std::string read_template(const fs::path& data, const std::string& name) {
  const fs::path file = fs::path(data) / kTemplates / name;
  std::error_code ec;
  if (!fs::is_regular_file(file, ec)) {
    fail("Vyrn can't find the file it compiles with (" + std::string(kTemplates) + "/" + name + " in " +
         data.string() + "). Reinstall Vyrn.");
  }
  return read_package_bytes(file.string(), kMaxTemplate);
}

// Replaces @NAME@, @VERSION@ and @WHEEL@ in one pass (a value is never read again as a placeholder).
std::string fill_template(std::string_view text, const std::string& name, const std::string& wheel) {
  const std::pair<std::string_view, std::string> values[] = {
      {"@NAME@", name}, {"@VERSION@", VYRN_VERSION}, {"@WHEEL@", wheel}};
  std::string out;
  out.reserve(text.size() + 64);
  size_t i = 0;
  while (i < text.size()) {
    bool replaced = false;
    if (text[i] == '@') {
      for (const auto& [key, value] : values) {
        if (text.compare(i, key.size(), key) == 0) {
          out += value;
          i += key.size();
          replaced = true;
          break;
        }
      }
    }
    if (!replaced) out += text[i++];
  }
  return out;
}

// The project's name for Python source (play.py puts it in its docstring): no quotes or backslashes, so it can't end
// a string or start an escape. Quotes become typographic ones; Python reads its source as UTF-8.
std::string python_safe_name(const std::string& name) {
  std::string out;
  for (const char c : name) {
    if (c == '"') {
      out += "\xE2\x80\x9D";   // U+201D
    } else if (c == '\'') {
      out += "\xE2\x80\x99";   // U+2019
    } else if (c == '\\') {
      out += '/';
    } else {
      out += c;
    }
  }
  return out;
}

// <out>.part/: the folder is built here and renamed into place; removed with everything in it unless it was.
class PartFolder {
 public:
  explicit PartFolder(const fs::path& out) : out_(out), part_(out.string() + ".part") {
    std::error_code ec;
    if (exists_no_follow(part_)) fs::remove_all(part_, ec);   // left by a compile that was stopped half-way
    if (const int err = platform::make_dir(part_.string(), 0755); err != 0) {
      fail("Vyrn can't create the folder " + part_.string() + " (" + platform::error_text(err) + ").");
    }
    platform::set_mode(part_.string(), 0755);
    made_ = true;
  }
  ~PartFolder() {
    if (!made_) return;
    std::error_code ec;
    fs::remove_all(part_, ec);
  }
  PartFolder(const PartFolder&) = delete;
  PartFolder& operator=(const PartFolder&) = delete;
  const fs::path& path() const { return part_; }

  // Renames the folder to `out`. replace: a folder already there (checked by the caller) is moved aside first and
  // removed once the new one is in place; it is put back when the rename fails.
  void commit(bool replace) {
    sync_folder(part_);
    const std::string name = out_.filename().string();
    fs::path old;
    std::error_code ec;
    if (replace && exists_no_follow(out_)) {
      old = out_.string() + ".old-" + std::to_string(platform::current_pid());
      if (exists_no_follow(old)) fs::remove_all(old, ec);
      if (const int err = platform::rename_no_replace(out_.string(), old.string(), /*directory=*/true); err != 0) {
        fail("Vyrn can't replace the folder " + out_.string() + " (" + platform::error_text(err) + ").");
      }
    }
    if (const int err = platform::rename_no_replace(part_.string(), out_.string(), /*directory=*/true); err != 0) {
      if (!old.empty()) platform::rename_no_replace(old.string(), out_.string(), /*directory=*/true);
      if (err == EEXIST || err == ENOTEMPTY) {
        fail("There is already a folder called " + name + ". Choose another folder, or allow replacing it.");
      }
      fail("Vyrn can't create the folder " + out_.string() + " (" + platform::error_text(err) + ").");
    }
    made_ = false;
    if (!old.empty()) fs::remove_all(old, ec);
    sync_folder(out_.parent_path());
  }

 private:
  fs::path out_, part_;
  bool made_ = false;
};

// Where the compiled folder may go: a new folder whose parent exists, outside the project (an existing folder only
// with overwrite, and only one Compile made).
void check_out_folder(const fs::path& out, const std::string& project, bool overwrite) {
  if (out.empty() || out == out.root_path() || !out.has_parent_path()) {
    fail("Please choose a new folder for the compiled AI.");
  }
  const std::string name = out.filename().string();
  if (is_inside(project, out.string())) {
    fail("The compiled AI can't go inside the project folder. Choose a folder outside it.");
  }
  std::error_code ec;
  if (!fs::is_directory(out.parent_path(), ec)) {
    fail("The folder " + out.parent_path().string() + " doesn't exist. Choose a folder that does.");
  }
  // Both folders may be removed (an old compiled folder, a .part left behind): never one that holds the project.
  if (is_inside(out.string(), project) || is_inside(out.string() + ".part", project)) {
    fail("The folder " + name + " holds the project itself. Choose a new folder for the compiled AI.");
  }
  platform::FileInfo st;
  if (!exists_no_follow(out, &st)) return;
  if (!st.is_directory()) {
    fail("There is already a file called " + name + " (not a folder). Choose another name for the compiled AI.");
  }
  if (!overwrite) fail("There is already a folder called " + name + ". Choose another folder, or allow replacing it.");
  if (!regular_file_in(out, kPlayFile) || !regular_file_in(out, kBrainFile)) {
    fail("The folder " + name + " isn't one Compile made (it has no play.py and brain.vrl), so Vyrn won't replace "
         "it. Choose another folder.");
  }
}

}  // namespace

// ---- the wheel ----------------------------------------------------------------------------------------------

std::string wheel_file_name() { return std::string("vyrn-") + VYRN_VERSION + "-py3-none-any.whl"; }

std::string find_wheel(const std::string& data_dir) {
  const std::string data = data_dir.empty() ? find_data_dir(nullptr) : data_dir;
  if (data.empty()) return "";
  std::error_code ec;
  const fs::path installed = fs::path(data) / "wheels" / wheel_file_name();
  if (fs::is_regular_file(installed, ec)) return installed.string();
#if defined(VYRN_SOURCE_DIR) && defined(VYRN_BUILD_WHEELS_DIR)
  // A build running from its source tree: the wheel is a build product, so it stays in the build folder.
  if (same_folder(data, VYRN_SOURCE_DIR)) {
    const fs::path built = fs::path(VYRN_BUILD_WHEELS_DIR) / wheel_file_name();
    if (fs::is_regular_file(built, ec)) return built.string();
  }
#endif
  return "";
}

// ---- the folder ---------------------------------------------------------------------------------------------

namespace package_detail {

StandaloneResult write_standalone(const std::string& project, const std::string& out_dir,
                                  const StandaloneOptions& options, int64_t now) {
  ProjectInfo info;
  try {
    info = read_project(project);
  } catch (const std::exception&) {
    fail("There is no Vyrn project at " + project + ".");
  }
  // The compiled program runs the project's Python code outside Vyrn: only a project the person trusts.
  if (const std::string untrusted = not_trusted_message(info.path); !untrusted.empty()) fail(untrusted);
  const util::ProjectSettings settings = util::read_settings(info.path);
  if (!settings.readable) {
    fail("This project's settings file (vyrn.json) isn't valid, so Vyrn can't compile the project. Fix it first.");
  }
  if (options.which != "best" && options.which != "latest") fail("Please choose the best or the latest brain.");

  // The brain first: without one there is nothing to compile.
  std::string which_used;
  const std::string brain_bytes = build_brain_package(info.path, "", options.which, ExportOptions{}, now, &which_used);
  const BrainPackage brain = parse_brain_package(brain_bytes, kBrainFile);

  if (out_dir.empty()) fail("Please choose a folder for the compiled AI.");
  const fs::path out = clean_folder(out_dir);
  check_out_folder(out, info.path, options.overwrite);

  const std::string data = options.data_dir.empty() ? find_data_dir(nullptr) : options.data_dir;
  if (data.empty()) {
    fail("Vyrn can't find its data folder (with python/ and templates/), so it can't compile. Reinstall Vyrn, or "
         "set VYRN_DATA to the folder that has them.");
  }
  const std::string wheel_name = wheel_file_name();
  const std::string name = clean_text(info.name, 200);
  const std::string play = fill_template(read_template(data, "play.py.in"), python_safe_name(name), wheel_name);
  const std::string readme = fill_template(read_template(data, "README.txt.in"), name, wheel_name);
  std::string wheel_bytes;
  if (options.include_wheel) {
    const std::string wheel = find_wheel(data);
    if (wheel.empty()) {
      fail("Vyrn can't find its Python package (" + wheel_name + " in " + (fs::path(data) / "wheels").string() +
           "), so it can't put it next to the AI. Reinstall Vyrn, or compile without the vyrn package.");
    }
    wheel_bytes = read_package_bytes(wheel, kMaxWheel);
  }

  // The project's files by the .vrlp rules. Compile's own files win over project files of the same name.
  std::set<std::string> reserved = {fold_case(kPlayFile), fold_case(kReadmeFile), fold_case(kBrainFile),
                                    fold_case("vyrn.json")};
  if (options.include_wheel) reserved.insert(fold_case(wheel_name));
  std::vector<SharedFile> files = shared_files(info.path, Purpose::Compile);
  std::vector<std::string> left_out;
  files.erase(std::remove_if(files.begin(), files.end(),
                             [&](const SharedFile& f) {
                               const std::string top = f.rel.substr(0, f.rel.find('/'));
                               if (!reserved.count(fold_case(top))) return false;
                               left_out.push_back(top);
                               return true;
                             }),
              files.end());
  const ordered_json shared = shared_settings(settings, info.name, brain.id);
  const std::string vyrn_text = util::dump_settings(shared);

  StandaloneResult result;
  result.folder = out.string();
  const auto put = [&](const fs::path& root, const std::string& rel, std::string_view bytes, int mode = 0644) {
    write_new_file(root, rel, bytes, mode);
    result.files.push_back({rel, bytes.size()});
  };
  {
    PartFolder part(out);
    for (const SharedFile& f : files) put(part.path(), f.rel, read_shared_file(f, Purpose::Compile));
    put(part.path(), "vyrn.json", vyrn_text);
    put(part.path(), kBrainFile, brain_bytes);
    put(part.path(), kPlayFile, play, 0755);   // executable on Linux and macOS: ./play.py works too
    put(part.path(), kReadmeFile, readme);
    if (options.include_wheel) put(part.path(), wheel_name, wheel_bytes);
    if (test_hooks::export_stage) test_hooks::export_stage("part-written");
    part.commit(options.overwrite);
  }
  std::sort(result.files.begin(), result.files.end(),
            [](const PackageFile& a, const PackageFile& b) { return a.path < b.path; });

  result.brain = summary_of(brain);
  result.wheel = options.include_wheel ? wheel_name : "";
  result.mode = info.mode;
  const auto link = shared.find("link");
  result.uses_link = link != shared.end() && !link->is_null() && *link != false;

  std::vector<std::string>& notes = result.notes;
  if (which_used != options.which) {
    notes.push_back("This brain has no " + options.which + " save yet, so Vyrn used the " + which_used + " one.");
  }
  for (const std::string& name_left_out : left_out) {
    notes.push_back("Left out the project's own " + shown_name(name_left_out) +
                    ": the compiled folder has its own file of that name.");
  }
  if (result.uses_link) {
    notes.push_back("The game connects over Vyrn Link: play.py prints the pairing code to type into the game.");
  } else if (info.mode == "realtime") {
    notes.push_back("This AI plays a real game. play.py only presses keys when you add --allow-keys, after a "
                    "3-second countdown; Ctrl+C stops it.");
  }
  if (!settings.start_program.empty()) {
    notes.push_back("play.py doesn't start the game the way Settings → Start program does: start it yourself.");
  }
  if (!options.include_wheel) {
    notes.push_back("The vyrn package isn't in the folder: each computer needs it installed (pip install " +
                    wheel_name + ", from a Vyrn that has it).");
  }
  return result;
}

}  // namespace package_detail

}  // namespace vyrn
