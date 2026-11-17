// Projects and templates on disk (vyrn/project.h): where they live, listing, reading, creating, duplicating.
//
// New folders are built under a hidden staging name next to their final place and then moved there with
// renameat2(RENAME_NOREPLACE): a project never appears half-copied, and an existing folder is never replaced.
#include "platform/files.h"
#include "platform/system.h"
#include "project/internal.h"
#include "util/files.h"
#include "util/settings.h"
#include "vyrn/link.h"
#include "vyrn/project.h"
#include "vyrn/session.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace vyrn {

namespace {

namespace fs = std::filesystem;
using project_detail::absolute_clean;

constexpr size_t kMaxFolderName = 64;                    // util::is_safe_name's limit
constexpr const char* kStagingPrefix = ".vyrn-new-";      // hidden, so list_projects never shows it
constexpr int64_t kAbandonedStagingSeconds = 10 * 60;

// ---- small helpers ----------------------------------------------------------------------------------------

// ASCII only: in an 8-bit locale isspace(0xA0) can be true, and 0xA0 also ends UTF-8 letters such as "à".
bool ascii_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

std::string trim(const std::string& text) {
  size_t a = 0, b = text.size();
  while (a < b && ascii_space(text[a])) ++a;
  while (b > a && ascii_space(text[b - 1])) --b;
  return text.substr(a, b - a);
}

// A name as typed, for vyrn.json: trimmed, with line breaks, tabs and other control characters made spaces.
std::string display_name(const std::string& typed) {
  std::string out = typed;
  for (char& c : out) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
  }
  return trim(out);
}

std::string lower(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

std::string home_dir() {
  const std::string home = platform::home_dir();
  return home.empty() ? "." : home;
}

// Something (even a dangling symlink) already has this name.
bool taken(const fs::path& p) {
  std::error_code ec;
  const fs::file_type type = fs::symlink_status(p, ec).type();
  return type != fs::file_type::not_found && type != fs::file_type::none;
}

bool is_dir(const fs::path& p) {
  std::error_code ec;
  return fs::is_directory(p, ec);
}

bool is_file(const fs::path& p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec);
}

[[noreturn]] void fail_io(const std::string& what, int err) {
  throw std::runtime_error(what + " (" + std::strerror(err) + ").");
}

// ---- copying a project folder -----------------------------------------------------------------------------

// Never copied: run results, Python's caches, lock files and half-written settings.
bool skipped(const std::string& name, bool top) {
  if (name == "__pycache__") return true;
  if (name.size() > 4 && name.compare(name.size() - 4, 4, ".pyc") == 0) return true;
  if (!top) return false;
  if (name == "brains" || name == "runs") return true;
  if (name.rfind(project_detail::kLockFileName, 0) == 0) return true;   // .vyrn-lock and leftovers next to it
  if (name.rfind(kLinkCodeFile, 0) == 0) return true;   // each project makes its own Vyrn Link pairing code
  if (name.rfind("vyrn.json.tmp-", 0) == 0) return true;
  return false;
}

void make_owner_writable(const fs::path& p, bool directory) {
  std::error_code ec;
  const fs::perms add = directory ? fs::perms::owner_all : (fs::perms::owner_read | fs::perms::owner_write);
  fs::permissions(p, add, fs::perm_options::add | fs::perm_options::nofollow, ec);
}

// Copies the contents of `from` into the existing folder `to`. Symlinks are copied as links (never followed);
// sockets, FIFOs and devices are left out. Copies are owner-writable even when the template is read-only.
void copy_tree(const fs::path& from, const fs::path& to, bool top) {
  std::error_code ec;
  fs::directory_iterator it(from, ec);
  if (ec) throw std::runtime_error("Vyrn can't read the folder " + from.string() + " (" + ec.message() + ").");
  for (fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
    const fs::path src = it->path();
    const std::string name = src.filename().string();
    if (skipped(name, top)) continue;
    const fs::path dst = to / name;
    std::error_code type_ec;
    const fs::file_type type = it->symlink_status(type_ec).type();
    try {
      if (type == fs::file_type::symlink) {
        fs::copy_symlink(src, dst);
      } else if (type == fs::file_type::directory) {
        fs::create_directory(dst, src);
        make_owner_writable(dst, true);
        copy_tree(src, dst, false);
      } else if (type == fs::file_type::regular) {
        fs::copy_file(src, dst, fs::copy_options::none);
        make_owner_writable(dst, false);
      }
    } catch (const fs::filesystem_error& e) {
      throw std::runtime_error("Vyrn couldn't copy " + src.string() + " (" + e.code().message() + ").");
    }
  }
  if (ec) throw std::runtime_error("Vyrn can't read the folder " + from.string() + " (" + ec.message() + ").");
}

// Staging folders left behind by a program that ended while copying (a crash): removed after a while.
void sweep_abandoned_staging(const fs::path& parent) {
  std::error_code ec;
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  for (fs::directory_iterator it(parent, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.rfind(kStagingPrefix, 0) != 0) continue;
    const char* digits = name.c_str() + std::strlen(kStagingPrefix);
    char* stop = nullptr;
    const long pid = std::strtol(digits, &stop, 10);
    if (stop == digits || *stop != '-' || pid <= 0) continue;
    std::error_code type_ec;
    if (!it->is_directory(type_ec) || it->is_symlink(type_ec)) continue;
    const auto changed = project_detail::mtime_seconds(it->path());
    if (project_detail::pid_running(pid) || !changed || now - *changed < kAbandonedStagingSeconds) {
      continue;
    }
    std::error_code rm_ec;
    fs::remove_all(it->path(), rm_ec);
  }
}

// A hidden folder next to the final place, removed again unless it was published.
class Staging {
 public:
  Staging(const fs::path& parent, const std::string& folder) {
    sweep_abandoned_staging(parent);
    static std::atomic<unsigned> counter{0};
    for (int attempt = 0; attempt < 100; ++attempt) {
      fs::path p = parent / (std::string(kStagingPrefix) + std::to_string(platform::current_pid()) + "-" +
                             std::to_string(counter.fetch_add(1)) + "-" + folder);
      const int err = platform::make_dir(p.string(), 0755);
      if (err == 0) {
        path_ = std::move(p);
        return;
      }
      if (err != EEXIST) fail_io("Vyrn can't create a folder in " + parent.string(), err);
    }
    throw std::runtime_error("Vyrn can't create a folder in " + parent.string() + ".");
  }
  ~Staging() {
    if (path_.empty()) return;
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  Staging(const Staging&) = delete;
  Staging& operator=(const Staging&) = delete;

  const fs::path& path() const { return path_; }

  // Moves the finished folder to `target`. False (and nothing changed) when that name is already taken.
  bool publish(const fs::path& target) {
    const int err = platform::rename_no_replace(path_.string(), target.string(), /*directory=*/true);
    if (err == 0) {
      path_.clear();
      return true;
    }
    if (err == EEXIST || err == ENOTEMPTY) return false;
    fail_io("Vyrn can't create " + target.string(), err);
  }

 private:
  fs::path path_;
};

// `base` shortened so that base + suffix is a valid folder name.
std::string with_suffix(std::string base, const std::string& suffix) {
  const size_t room = kMaxFolderName - suffix.size();
  if (base.size() > room) base.resize(room);
  while (!base.empty() && base.back() == '-') base.pop_back();
  if (base.empty()) base = "project";
  return base + suffix;
}

// ---- reading projects ---------------------------------------------------------------------------------------

// Newest mtime of vyrn.json, the scripts and the brains' saves.
int64_t newest_change(const fs::path& dir) {
  int64_t newest = 0;
  const auto consider = [&](const fs::path& p) {
    if (const auto t = project_detail::mtime_seconds(p)) newest = std::max(newest, *t);
  };
  consider(dir / "vyrn.json");
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code type_ec;
    if (it->path().extension() == ".py" && it->is_regular_file(type_ec)) consider(it->path());
  }
  const fs::path brains = dir / "brains";
  consider(brains);
  for (fs::directory_iterator it(brains, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code type_ec;
    if (!it->is_directory(type_ec)) continue;
    consider(it->path());
    consider(it->path() / "latest" / "current");
    consider(it->path() / "best" / "current");
  }
  if (newest == 0) consider(dir);
  return newest;
}

bool looks_like_project(const fs::path& dir) { return is_file(dir / "vyrn.json") || is_file(dir / "inputs.py"); }

// ---- templates ----------------------------------------------------------------------------------------------

struct KnownTemplate {
  const char* id;
  const char* description;
  bool demo;   // shown apart from the presets in New project
};

// The shipped templates in the order New project shows them: the presets, then the demos.
constexpr KnownTemplate kKnownTemplates[] = {
    {"balance", "A pole on a cart keeps tipping over. Teach the AI to hold it up (learns in a minute or two)", false},
    {"demo-game", "A simple dodge game to try real-game training, no setup needed", false},
    {"my-game", "Train on any game on your screen: pick its area and map keys to choices", false},
    {"catch-pictures", "Catch falling fruit from a 10x10 picture. A quick first look at how it all works", true},
    {"catch-numbers", "The same fruit game, but the AI is handed three numbers instead of a picture", true},
};
constexpr size_t kKnownCount = sizeof kKnownTemplates / sizeof kKnownTemplates[0];

size_t template_rank(const std::string& id) {
  for (size_t i = 0; i < kKnownCount; ++i) {
    if (id == kKnownTemplates[i].id) return i;
  }
  return kKnownCount;
}

// Latin-1 letters (U+00C0..U+00FF) without their accents; "" = left out.
constexpr const char* kLatin1Letters[64] = {
    "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I",  "I",   // C0-CF
    "D", "N", "O", "O", "O", "O", "O",  "",  "O", "U", "U", "U", "U", "Y", "TH", "ss",  // D0-DF
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i",  "i",   // E0-EF
    "d", "n", "o", "o", "o", "o", "o",  "",  "o", "u", "u", "u", "u", "y", "th", "y",   // F0-FF
};

}  // namespace

// ---- public API -------------------------------------------------------------------------------------------------

std::string projects_root() {
  fs::path root;
  if (const char* env = std::getenv("VYRN_PROJECTS"); env && *env) {
    const std::string text = env;
    if (text == "~") {
      root = home_dir();
    } else if (text.rfind("~/", 0) == 0) {
      root = fs::path(home_dir()) / text.substr(2);
    } else {
      root = text;
    }
  } else {
    root = fs::path(home_dir()) / "Vyrn";
  }
  std::error_code ec;
  fs::create_directories(root, ec);   // best effort: an unusable folder simply lists no projects
  return absolute_clean(root.string());
}

std::vector<TemplateInfo> list_templates(const std::string& data_dir) {
  const std::string data = data_dir.empty() ? find_data_dir(nullptr) : data_dir;
  std::vector<TemplateInfo> out;
  if (data.empty()) return out;
  std::error_code ec;
  for (fs::directory_iterator it(fs::path(data) / "templates", ec), end; !ec && it != end; it.increment(ec)) {
    const fs::path folder = it->path();
    const std::string id = folder.filename().string();
    if (id.empty() || id[0] == '.' || !is_dir(folder) || !is_file(folder / "vyrn.json")) continue;
    const util::ProjectSettings s = util::read_settings(folder.string());
    TemplateInfo t;
    t.id = id;
    t.name = s.name.empty() ? id : s.name;
    t.mode = is_file(folder / "world.py") ? "sim" : "realtime";
    const auto description = s.raw.find("description");
    if (description != s.raw.end() && description->is_string()) t.description = trim(description->get<std::string>());
    if (t.description.empty()) {
      const size_t rank = template_rank(id);
      if (rank < kKnownCount) t.description = kKnownTemplates[rank].description;
    }
    if (const size_t rank = template_rank(id); rank < kKnownCount) t.demo = kKnownTemplates[rank].demo;
    t.path = absolute_clean(folder.string());
    out.push_back(std::move(t));
  }
  std::sort(out.begin(), out.end(), [](const TemplateInfo& a, const TemplateInfo& b) {
    const size_t ra = template_rank(a.id), rb = template_rank(b.id);
    if (ra != rb) return ra < rb;
    const std::string na = lower(a.name), nb = lower(b.name);
    if (na != nb) return na < nb;
    return a.id < b.id;
  });
  return out;
}

std::vector<ProjectInfo> list_projects(const std::string& root) {
  std::vector<ProjectInfo> out;
  std::error_code ec;
  for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    const fs::path dir = it->path();
    const std::string name = dir.filename().string();
    // Hidden folders are not projects (Vyrn's own staging folders are hidden while they are being filled).
    if (name.empty() || name[0] == '.' || !is_dir(dir) || !looks_like_project(dir)) continue;
    try {
      out.push_back(read_project(dir.string()));
    } catch (const std::exception&) {
      // Removed or made unreadable while listing: leave it out.
    }
  }
  std::sort(out.begin(), out.end(), [](const ProjectInfo& a, const ProjectInfo& b) {
    if (a.modified != b.modified) return a.modified > b.modified;
    const std::string na = lower(a.name), nb = lower(b.name);
    if (na != nb) return na < nb;
    return a.path < b.path;
  });
  return out;
}

ProjectInfo read_project(const std::string& path) {
  if (path.empty()) throw std::runtime_error("No project folder was given.");
  const fs::path dir = absolute_clean(path);
  if (!is_dir(dir)) throw std::runtime_error("There is no project folder at " + dir.string() + ".");
  if (!looks_like_project(dir)) {
    throw std::runtime_error(dir.string() + " is not a Vyrn project: it has no vyrn.json and no inputs.py.");
  }
  const util::ProjectSettings s = util::read_settings(dir.string());
  ProjectInfo p;
  p.name = trim(s.name).empty() ? dir.filename().string() : s.name;
  p.path = dir.string();
  p.mode = is_file(dir / "world.py") ? "sim" : "realtime";
  p.template_id = s.template_id;
  if (!s.brain.empty()) {
    const project_detail::BrainSummary brain = project_detail::read_brain_summary(dir / "brains" / s.brain);
    p.has_best = brain.has_best;
    p.best_mean20 = brain.best_mean20;
    p.time_trained_s = brain.time_trained_s;
  }
  if (const auto holder = ProjectLock::holder(p.path)) p.training_by = holder->by;
  p.modified = newest_change(dir);
  return p;
}

std::string folder_name_for(const std::string& name) {
  std::string out;
  bool dash = false;   // a '-' is due before the next letter (from spaces and dashes)
  const auto append = [&](const char* piece) {
    if (!*piece) return;
    if (dash && !out.empty()) out += '-';
    dash = false;
    out += piece;
  };
  for (size_t i = 0; i < name.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    const unsigned char next = i + 1 < name.size() ? static_cast<unsigned char>(name[i + 1]) : 0;
    if (c < 0x80) {
      if (std::isalnum(c) || c == '_') {
        const char piece[2] = {static_cast<char>(c), 0};
        append(piece);
      } else if (c == '-' || ascii_space(static_cast<char>(c))) {
        dash = true;
      }
      continue;   // other punctuation is left out
    }
    if (c == 0xC3 && next >= 0x80 && next <= 0xBF) {   // UTF-8 for U+00C0..U+00FF
      append(kLatin1Letters[next - 0x80]);
      ++i;
      continue;
    }
    if (c == 0xC2 && next == 0xA0) {   // no-break space
      dash = true;
      ++i;
      continue;
    }
    while (i + 1 < name.size() && (static_cast<unsigned char>(name[i + 1]) & 0xC0) == 0x80) ++i;   // skip the rest
  }
  if (out.size() > kMaxFolderName) out.resize(kMaxFolderName);
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out.empty() ? "project" : out;
}

std::string create_project(const std::string& root, const std::string& name, const TemplateInfo& tmpl) {
  const std::string display = display_name(name);
  if (display.empty()) throw std::runtime_error("Please give the project a name.");
  if (root.empty()) throw std::runtime_error("No projects folder was given.");
  const std::string label = tmpl.name.empty() ? tmpl.id : tmpl.name;
  const fs::path from = tmpl.path.empty() ? fs::path() : fs::path(absolute_clean(tmpl.path));
  if (from.empty() || !is_dir(from)) {
    throw std::runtime_error("Vyrn can't find the template \"" + label + "\"" +
                             (tmpl.path.empty() ? std::string() : " in " + tmpl.path) + ".");
  }
  const fs::path base = absolute_clean(root);
  std::error_code ec;
  fs::create_directories(base, ec);
  if (!is_dir(base)) {
    throw std::runtime_error("Vyrn can't create the projects folder " + base.string() +
                             (ec ? " (" + ec.message() + ")." : "."));
  }
  const std::string folder = folder_name_for(display);
  const fs::path target = base / folder;
  const std::string exists_message = "There is already a project folder called \"" + folder + "\" in " +
                                     base.string() + ". Please pick another name.";
  if (taken(target)) throw std::runtime_error(exists_message);
  if (is_inside(from.string(), target.string())) {
    throw std::runtime_error("Vyrn can't make a project inside the template's own folder. Please pick another "
                             "projects folder.");
  }

  Staging staging(base, folder);
  copy_tree(from, staging.path(), true);
  util::ordered_json change = util::ordered_json::object();
  change["name"] = display;
  change["template"] = tmpl.id.empty() ? from.filename().string() : tmpl.id;
  std::string why;
  if (!util::update_settings(staging.path().string(), change, &why)) {
    throw std::runtime_error("Vyrn couldn't write the new project's settings (" + why + ").");
  }
  if (!staging.publish(target)) throw std::runtime_error(exists_message);
  return target.string();
}

std::string duplicate_project(const std::string& path) {
  const ProjectInfo source = read_project(path);   // throws when it isn't a project
  const fs::path from = source.path;
  const fs::path parent = from.parent_path();

  Staging staging(parent, from.filename().string());
  copy_tree(from, staging.path(), true);
  const util::ProjectSettings settings = util::read_settings(staging.path().string());
  const std::string base = folder_name_for(source.name);
  for (int n = 1; n < 10000; ++n) {
    const std::string number = n == 1 ? "" : " " + std::to_string(n);
    const fs::path target = parent / with_suffix(base, n == 1 ? "-copy" : "-copy-" + std::to_string(n));
    if (taken(target)) continue;
    if (settings.readable) {   // a vyrn.json Vyrn can't read stays exactly as it was
      util::ordered_json change = util::ordered_json::object();
      change["name"] = source.name + " copy" + number;
      const auto brain = settings.raw.find("brain");
      if (brain != settings.raw.end() && brain->is_string() && !brain->get<std::string>().empty()) {
        change["brain"] = "";   // the copy has no brains/ folder
      }
      std::string why;
      if (!util::update_settings(staging.path().string(), change, &why)) {
        throw std::runtime_error("Vyrn couldn't write the copy's settings (" + why + ").");
      }
    }
    if (staging.publish(target)) return target.string();
  }
  throw std::runtime_error("Vyrn can't find a free folder name for a copy of \"" + source.name + "\".");
}

}  // namespace vyrn
