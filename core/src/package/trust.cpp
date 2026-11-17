// The trust marker of imported projects (docs/dev/package.md section 7): <project>/.vyrn-imported. Whether a
// project is trusted is decided in vyrn_core (util/trust.h: project_trusted), which Session enforces.
#include "package/internal.h"
#include "package/text.h"
#include "util/files.h"
#include "util/trust.h"

#include "platform/files.h"

#include <cerrno>
#include <filesystem>

namespace vyrn {

using namespace package_detail;
namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxMarker = 64u << 10;

}  // namespace

std::optional<ImportOrigin> import_origin(const std::string& project) {
  const std::string path = (fs::path(project) / kTrustMarker).string();
  int err = 0;
  platform::File file = platform::File::open_read(path, /*no_follow=*/true, err);
  if (!file) {
    if (err == ENOENT || err == ENOTDIR) {
      platform::FileInfo st;
      if (platform::file_info(path, false, st) != 0) return std::nullopt;   // no marker: not imported
    }
    return ImportOrigin{};   // a marker Vyrn can't read (or a symlink): untrusted
  }
  platform::FileInfo st;
  if (file.info(st) != 0 || !st.is_regular() || st.size > kMaxMarker) return ImportOrigin{};
  std::string text(static_cast<size_t>(st.size), '\0');
  size_t got = 0;
  while (got < text.size()) {
    size_t n = 0;
    if (file.read(text.data() + got, text.size() - got, n) != 0 || n == 0) break;
    got += n;
  }
  text.resize(got);
  const json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
  ImportOrigin origin;
  if (!j.is_object()) return origin;
  const auto text_of = [&](const char* key, size_t max) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? clean_text(it->get<std::string>(), max) : std::string();
  };
  origin.file = text_of("file", 255);
  origin.name = text_of("name", 200);
  origin.author = text_of("author", 100);
  origin.created = text_of("created", 40);
  origin.imported = text_of("imported", 40);
  origin.start_program = text_of("start_program", 500);
  const auto trusted = j.find("trusted");
  origin.trusted = trusted != j.end() && trusted->is_boolean() && trusted->get<bool>();
  return origin;
}

bool is_trusted(const std::string& project) { return util::project_trusted(project); }

void trust_project(const std::string& project) {
  std::error_code ec;
  if (project.empty() || !fs::is_directory(project, ec)) fail("There is no project folder at " + project + ".");
  const std::optional<ImportOrigin> origin = import_origin(project);
  if (!origin) {
    // Not imported itself, but maybe a folder inside an imported project: that project is what gets trusted.
    const std::string owner = util::untrusted_project_folder(project);
    if (!owner.empty()) {
      fail("This folder is part of the imported project " + owner +
           ", which isn't trusted yet. Read that project's scripts, then trust that project instead.");
    }
    return;   // not imported
  }
  if (origin->trusted) return;   // already trusted
  const json marker = {{"file", origin->file},          {"name", origin->name},
                       {"author", origin->author},      {"created", origin->created},
                       {"imported", origin->imported},  {"start_program", origin->start_program},
                       {"trusted", true}};
  const std::string path = (fs::path(project) / kTrustMarker).string();
  try {
    util::write_text_file_atomic(path, marker.dump(2) + "\n");
  } catch (const std::exception& e) {
    fail(std::string("Vyrn couldn't record that you trust this project: ") + e.what());
  }
}

}  // namespace vyrn
