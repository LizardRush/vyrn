// vyrn-cli's sharing commands (docs/dev/package.md section 8): pack, export-brain, inspect, import and trust; and
// compile (docs/dev/standalone.md). Uses only the core's public headers (no torch).
#include "package_cmd.h"

#include "vyrn/package.h"
#include "vyrn/project.h"
#include "vyrn/session.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace vyrn::cli {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitPackage = 5;
constexpr auto kCheckTimeLimit = std::chrono::seconds(90);

constexpr const char* kUsage =
    "Sharing (.vrl = one trained brain, .vrlp = a whole project with its brain):\n"
    "  vyrn-cli pack <project> [--out FILE] [--brain best|latest] [--brain-id ID] [--no-brain]\n"
    "                [--author NAME] [--description TEXT] [--force] [--json]\n"
    "  vyrn-cli export-brain <project> [--out FILE] [--brain best|latest] [--brain-id ID]\n"
    "                [--author NAME] [--description TEXT] [--force] [--json]\n"
    "  vyrn-cli inspect <file.vrl|file.vrlp> [--json]\n"
    "  vyrn-cli import <file.vrlp> [--projects-dir DIR] [--json]\n"
    "  vyrn-cli import <file.vrl> --into <project> [--no-activate] [--json]\n"
    "  vyrn-cli trust <project>\n"
    "  vyrn-cli compile <project> [--out DIR] [--brain best|latest] [--no-wheel] [--force] [--json]\n"
    "\n"
    "  pack          save a project (scripts, pictures, settings and its trained AI) as a .vrlp file\n"
    "  export-brain  save a project's trained AI as a .vrl file\n"
    "  inspect       show what a .vrl or .vrlp file holds (nothing is run or written)\n"
    "  import        open a .vrlp as a new project, or add a .vrl brain to a project with --into\n"
    "  trust         let Vyrn run an imported project's Python code (read its scripts first)\n"
    "  compile       turn the trained AI into a Python program that runs without Vyrn: a folder with\n"
    "                play.py (run: python3 play.py), the brain, the scripts and the vyrn package\n"
    "  --out FILE    where to save (default: a name made from the project's name, in this folder;\n"
    "                compile: a new folder, default ./<Name>-play)\n"
    "  --brain       which saved brain to share: best (default) or latest\n"
    "  --brain-id    share this brain instead of the active one   --no-brain  leave the trained AI out\n"
    "  --force       replace an existing file (compile: a folder an earlier compile made)\n"
    "  --no-activate keep the active brain                         --no-wheel  leave out the vyrn package (.whl)\n"
    "  --json        print one JSON object\n"
    "\n"
    "Exit status: 0 ok, 2 usage, 5 the file was refused or couldn't be written or read (or the AI couldn't be\n"
    "compiled).\n";

struct UsageError {
  std::string message;
};

struct Args {
  std::string command;
  std::vector<std::string> positional;
  std::optional<std::string> out, brain, brain_id, author, description, projects_dir, into;
  bool no_brain = false, force = false, json = false, no_activate = false, no_wheel = false, help = false;
};

bool takes(const std::string& command, const std::string& option) {
  const bool exporting = command == "pack" || command == "export-brain";
  if (option == "json") return command != "trust";
  if (command == "compile") return option == "out" || option == "brain" || option == "force" || option == "no-wheel";
  if (option == "out" || option == "brain" || option == "brain-id" || option == "author" ||
      option == "description" || option == "force") {
    return exporting;
  }
  if (option == "no-brain") return command == "pack";
  if (option == "projects-dir" || option == "into" || option == "no-activate") return command == "import";
  return false;
}

bool has_value(const std::string& option) {
  return option == "out" || option == "brain" || option == "brain-id" || option == "author" ||
         option == "description" || option == "projects-dir" || option == "into";
}

Args parse_args(int argc, char** argv) {
  Args a;
  a.command = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      a.help = true;
      continue;
    }
    if (arg.size() > 2 && arg.compare(0, 2, "--") == 0) {
      std::string name = arg.substr(2);
      std::optional<std::string> value;
      if (const size_t eq = name.find('='); eq != std::string::npos) {
        value = name.substr(eq + 1);
        name.resize(eq);
      }
      if (!takes(a.command, name)) throw UsageError{a.command + " doesn't know the option --" + name + "."};
      if (has_value(name)) {
        if (!value) {
          if (i + 1 >= argc) throw UsageError{"--" + name + " needs a value."};
          value = argv[++i];
        }
      } else if (value) {
        throw UsageError{"--" + name + " doesn't take a value."};
      }
      if (name == "out") a.out = value;
      else if (name == "brain") a.brain = value;
      else if (name == "brain-id") a.brain_id = value;
      else if (name == "author") a.author = value;
      else if (name == "description") a.description = value;
      else if (name == "projects-dir") a.projects_dir = value;
      else if (name == "into") a.into = value;
      else if (name == "no-brain") a.no_brain = true;
      else if (name == "force") a.force = true;
      else if (name == "json") a.json = true;
      else if (name == "no-activate") a.no_activate = true;
      else if (name == "no-wheel") a.no_wheel = true;
      continue;
    }
    a.positional.push_back(arg);
  }
  if (a.help) return a;
  const char* what = a.command == "inspect" ? "a .vrl or .vrlp file" : a.command == "import" ? "a file to import"
                                                                                                : "a project folder";
  if (a.positional.empty()) throw UsageError{a.command + " needs " + what + "."};
  if (a.positional.size() > 1) throw UsageError{a.command + " takes only one " + std::string(what).substr(2) + "."};
  if (a.brain && *a.brain != "best" && *a.brain != "latest") throw UsageError{"--brain must be best or latest."};
  if (a.no_brain && (a.brain || a.brain_id)) throw UsageError{"--no-brain can't be used with --brain or --brain-id."};
  if (a.command == "import") {
    std::string lower = fs::path(a.positional[0]).extension().string();
    for (char& c : lower) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    if (a.into && a.projects_dir) throw UsageError{"--into and --projects-dir can't be used together."};
    if (a.into && lower == kProjectExt) {
      throw UsageError{"--into is for brain files (.vrl). A project file (.vrlp) becomes a new project."};
    }
    if (!a.into && lower == kBrainExt) {
      throw UsageError{"A brain file (.vrl) goes into a project: add --into <project>."};
    }
    if (a.no_activate && !a.into) throw UsageError{"--no-activate is only for brain files (with --into)."};
  }
  return a;
}

// ---- output ----

std::string size_text(uint64_t bytes) {
  char buf[48];
  if (bytes < 1024) {
    std::snprintf(buf, sizeof buf, "%llu byte%s", static_cast<unsigned long long>(bytes), bytes == 1 ? "" : "s");
  } else if (bytes < (1u << 20)) {
    std::snprintf(buf, sizeof buf, "%llu KB", static_cast<unsigned long long>((bytes + 1023) / 1024));
  } else {
    std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / (1u << 20));
  }
  return buf;
}

std::string number_text(double v) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.3g", v);
  return buf;
}

std::string duration_text(double seconds) {
  char buf[48];
  if (seconds < 90) {
    std::snprintf(buf, sizeof buf, "%.0f s", seconds);
  } else if (seconds < 5400) {
    std::snprintf(buf, sizeof buf, "%.0f min", seconds / 60);
  } else {
    std::snprintf(buf, sizeof buf, "%.1f h", seconds / 3600);
  }
  return buf;
}

std::string created_text(const std::string& iso) {
  if (iso.size() != 20) return iso;
  return iso.substr(0, 10) + " " + iso.substr(11, 5) + " UTC";
}

std::string join(const std::vector<std::string>& items, const char* sep = ", ") {
  std::string out;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i) out += sep;
    out += items[i];
  }
  return out;
}

json brain_json(const BrainSummary& b) {
  json history = json::array();
  for (const HistoryPoint& p : b.history) history.push_back({p.round, p.mean20});
  return json{{"id", b.id},
              {"name", b.name},
              {"which", b.which},
              {"input", b.input},
              {"input_shape", b.input_shape},
              {"input_names", b.input_names},
              {"choices", b.choices},
              {"net", b.net},
              {"net_description", b.net_description},
              {"has_best", b.has_best},
              {"best_mean20", b.has_best ? json(b.best_mean20) : json(nullptr)},
              {"time_trained_s", b.time_trained_s},
              {"ticks", b.ticks},
              {"rounds", b.rounds},
              {"history", history}};
}

json info_json(const PackageInfo& info, const std::string& file) {
  json files = json::array();
  for (const PackageFile& f : info.files) files.push_back({{"path", f.path}, {"size", f.size}});
  json brains = json::array();
  for (const BrainSummary& b : info.brains) brains.push_back(brain_json(b));
  std::error_code ec;
  const fs::path absolute = fs::absolute(file, ec);
  return json{{"file", ec ? file : absolute.lexically_normal().string()},
              {"kind", info.kind},
              {"format_version", info.format_version},
              {"vyrn_version", info.vyrn_version},
              {"name", info.name},
              {"description", info.description},
              {"author", info.author},
              {"created", info.created},
              {"template_id", info.template_id},
              {"mode", info.mode},
              {"uses_link", info.uses_link},
              {"start_program", info.start_program},
              {"files", files},
              {"brains", brains},
              {"preview_png_size", info.preview_png.size()},
              {"file_size", info.file_size}};
}

void print_json(const json& j) {
  const std::string text = j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
  std::fputs(text.c_str(), stdout);
  std::fputc('\n', stdout);
}

// "  Label:        value", with the values in one column.
void line(const std::string& label, const std::string& value) {
  std::string padded = label + ":";
  if (padded.size() < 14) padded.resize(14, ' ');
  std::printf("  %s %s\n", padded.c_str(), value.c_str());
}

void more(const std::string& value) { std::printf("  %14s %s\n", "", value.c_str()); }

void print_brain_lines(const BrainSummary& b) {
  std::string sees = b.input == "picture" && b.input_shape.size() == 2
                         ? "a " + std::to_string(b.input_shape[1]) + "x" + std::to_string(b.input_shape[0]) + " picture"
                         : std::to_string(b.input_shape.empty() ? 0 : b.input_shape[0]) + " numbers";
  if (!b.input_names.empty()) sees += " (" + join(b.input_names) + ")";
  line("Trained AI", (b.id.empty() ? b.name : b.id) + " (" + b.which + " brain), " + b.net_description);
  more("sees " + sees + "; choices: " + join(b.choices));
  std::string stats = b.has_best ? "best 20-round mean " + number_text(b.best_mean20) + ", " : "no best score yet, ";
  stats += std::to_string(b.rounds) + " rounds, " + std::to_string(b.ticks) + " ticks, " +
           duration_text(b.time_trained_s) + " of training";
  more(stats);
  if (!b.history.empty()) {
    more("points chart: " + std::to_string(b.history.size()) + " points, rounds " +
         std::to_string(b.history.front().round) + " to " + std::to_string(b.history.back().round));
  }
}

void print_info(const PackageInfo& info, const std::string& file) {
  const bool project = info.kind == "project";
  std::printf("%s: %s (%s)\n", fs::path(file).filename().string().c_str(),
              project ? "a Vyrn project" : "a trained Vyrn brain", size_text(info.file_size).c_str());
  line(project ? "Name" : "Project", info.name);
  if (!info.author.empty()) line("Author", info.author);
  if (!info.description.empty()) line("Description", info.description);
  std::string made = created_text(info.created);
  if (!info.vyrn_version.empty()) made += (made.empty() ? "" : " ") + std::string("with Vyrn ") + info.vyrn_version;
  if (!made.empty()) line("Made", made);
  if (!info.template_id.empty()) line("Template", info.template_id);
  if (!info.mode.empty()) line("Plays", info.mode == "sim" ? "a simulation (world.py)" : "a real program in real time");
  if (info.uses_link) line("Game link", "yes (a game connects over Vyrn Link)");
  if (!info.start_program.empty()) line("Start program", info.start_program + " (a hint: never run by itself)");
  if (project) {
    std::vector<std::string> names;
    for (const PackageFile& f : info.files) names.push_back(f.path + " (" + size_text(f.size) + ")");
    line("Files (" + std::to_string(info.files.size()) + ")", join(names));
    if (info.brains.empty()) line("Trained AI", "none");
  }
  for (const BrainSummary& b : info.brains) print_brain_lines(b);
  if (!info.preview_png.empty()) line("Picture", "included (" + size_text(info.preview_png.size()) + ")");
}

void print_notes(const std::vector<std::string>& notes) {
  for (const std::string& n : notes) std::printf("  - %s\n", n.c_str());
}

// ---- the Check that gives `import <file.vrl> --into P` its spec ----

std::string check_spec(const std::string& project, const char* argv0, std::vector<std::string>& notes) {
  RunConfig cfg;
  cfg.action = RunConfig::Action::Check;
  cfg.project_dir = project;
  cfg.data_dir = find_data_dir(argv0);
  if (cfg.data_dir.empty()) {
    notes.push_back("Vyrn couldn't find its Python helper to run a Check, so it compared the brain with the "
                    "project's active brain instead.");
    return "";
  }
  std::string spec, error;
  {
    Session session(cfg);
    session.start();
    auto deadline = std::chrono::steady_clock::now() + kCheckTimeLimit;
    bool stopping = false;
    const auto take = [&](const std::vector<std::string>& events) {
      for (const std::string& text : events) {
        const nlohmann::json e = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (!e.is_object()) continue;
        const std::string type = e.value("type", "");
        if (type == "spec" && spec.empty()) spec = text;
        if (type == "error" && error.empty()) error = e.value("message", "");
      }
    };
    for (;;) {
      take(session.drain(1000));
      if (session.finished()) {
        take(session.drain(1u << 30));
        break;
      }
      if (std::chrono::steady_clock::now() > deadline) {
        if (stopping) break;
        stopping = true;
        session.stop();
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  if (spec.empty()) {
    notes.push_back("The Check didn't finish" + (error.empty() ? std::string() : " (" + error + ")") +
                    ", so Vyrn compared the brain with the project's active brain instead.");
  }
  return spec;
}

// ---- commands ----

int cmd_export(const Args& a) {
  const bool project = a.command == "pack";
  const std::string& dir = a.positional[0];
  ExportOptions options;
  if (a.author) options.author = *a.author;
  if (a.description) options.description = *a.description;
  if (a.brain_id) options.brain_id = *a.brain_id;
  if (a.brain) options.which = *a.brain;
  options.include_brain = !a.no_brain;
  options.overwrite = a.force;
  const std::string out = a.out ? *a.out : suggested_file_name(dir, project ? "project" : "brain");
  PackageInfo info;
  try {
    info = project ? export_project(dir, out, options) : export_brain(dir, out, options);
  } catch (const PackageError& e) {
    const std::string message = e.what();
    if (message.rfind("There is already a file called", 0) == 0) {
      throw PackageError(message + " (Use --out FILE for another name, or --force to replace it.)");
    }
    throw;
  }
  if (a.json) {
    print_json(info_json(info, out));
    return kExitOk;
  }
  if (project) {
    std::string brain = "no trained AI";
    if (!info.brains.empty()) {
      brain = "its trained AI (the " + info.brains[0].which + " brain " + info.brains[0].id + ")";
    }
    std::printf("Saved %s (%s): the project \"%s\" with %zu files and %s.\n", out.c_str(),
                size_text(info.file_size).c_str(), info.name.c_str(), info.files.size(), brain.c_str());
    std::printf("Anyone with Vyrn can open it to get the project%s.\n",
                info.brains.empty() ? "" : " and watch the AI play");
  } else {
    const BrainSummary& b = info.brains.at(0);
    std::printf("Saved %s (%s): the %s brain %s of \"%s\" (%s).\n", out.c_str(), size_text(info.file_size).c_str(),
                b.which.c_str(), b.id.c_str(), info.name.c_str(), b.net_description.c_str());
    std::printf("It can be added to any project whose scripts see and choose the same things.\n");
  }
  return kExitOk;
}

int cmd_compile(const Args& a) {
  const std::string& project = a.positional[0];
  StandaloneOptions options;
  if (a.brain) options.which = *a.brain;
  options.include_wheel = !a.no_wheel;
  options.overwrite = a.force;
  const std::string out = a.out ? *a.out : suggested_file_name(project, "standalone");
  StandaloneResult r;
  try {
    r = export_standalone(project, out, options);
  } catch (const PackageError& e) {
    const std::string message = e.what();
    if (message.rfind("There is already a folder called", 0) == 0) {
      throw PackageError(message + " (Use --out DIR for another folder, or --force to replace it.)");
    }
    if (message.find(wheel_file_name()) != std::string::npos && !a.no_wheel) {
      throw PackageError(message + " (Add --no-wheel for that.)");
    }
    throw;
  }
  if (a.json) {
    json files = json::array();
    for (const PackageFile& f : r.files) files.push_back({{"path", f.path}, {"size", f.size}});
    print_json(json{{"folder", r.folder},
                    {"files", files},
                    {"brain", brain_json(r.brain)},
                    {"wheel", r.wheel},
                    {"mode", r.mode},
                    {"uses_link", r.uses_link},
                    {"run", "python3 play.py"},
                    {"notes", r.notes}});
    return kExitOk;
  }
  const BrainSummary& b = r.brain;
  std::printf("Compiled the %s brain %s (%s) to %s\n", b.which.c_str(), b.id.c_str(), b.net_description.c_str(),
              r.folder.c_str());
  std::printf("It plays without Vyrn (it doesn't learn). Run it with:\n");
  std::printf("  cd \"%s\"\n", r.folder.c_str());
  if (!r.wheel.empty()) std::printf("  pip install %s     (once per computer)\n", r.wheel.c_str());
  std::printf("  python3 play.py                (Ctrl+C stops it; --help shows the options)\n");
  print_notes(r.notes);
  return kExitOk;
}

int cmd_inspect(const Args& a) {
  const std::string& file = a.positional[0];
  const PackageInfo info = inspect_package(file);
  if (a.json) {
    print_json(info_json(info, file));
  } else {
    print_info(info, file);
    std::printf("Everything in the file checks out. Nothing in it runs until you import it%s.\n",
                info.kind == "project" ? " and trust it" : "");
  }
  return kExitOk;
}

int cmd_import(const Args& a, const char* argv0) {
  const std::string& file = a.positional[0];
  if (!a.into) {
    const ImportResult r = import_project(file, a.projects_dir ? *a.projects_dir : "");
    if (a.json) {
      print_json(json{{"project", r.project}, {"brain_id", r.brain_id}, {"notes", r.notes}, {"trusted", false}});
      return kExitOk;
    }
    std::printf("Imported %s as a new project: %s\n", fs::path(file).filename().string().c_str(), r.project.c_str());
    if (!r.brain_id.empty()) std::printf("Its trained AI is the brain %s.\n", r.brain_id.c_str());
    std::printf("It isn't trusted yet: its Python code was written by someone else. Read its scripts, then run\n"
                "  vyrn-cli trust \"%s\"\n",
                r.project.c_str());
    print_notes(r.notes);
    return kExitOk;
  }
  std::vector<std::string> notes;
  std::string spec;
  std::error_code ec;
  const std::string project = fs::absolute(*a.into, ec).lexically_normal().string();
  if (fs::is_directory(project, ec)) {
    if (is_trusted(project)) {
      inspect_package(file);   // a damaged or hostile file is refused before any script runs
      if (!a.json) {
        std::printf("Running a Check of the project to see whether the brain fits...\n");
        std::fflush(stdout);
      }
      spec = check_spec(project, argv0, notes);
    } else {
      notes.push_back("This project isn't trusted yet, so Vyrn compared the brain with the project's active brain "
                      "instead of running its scripts.");
    }
  }
  ImportResult r = import_brain(file, project, spec, !a.no_activate);
  notes.insert(notes.end(), r.notes.begin(), r.notes.end());
  if (a.json) {
    print_json(json{{"project", r.project}, {"brain_id", r.brain_id}, {"notes", notes}, {"active", !a.no_activate}});
    return kExitOk;
  }
  std::printf("Added the brain %s to %s%s.\n", r.brain_id.c_str(), r.project.c_str(),
              a.no_activate ? "" : " and made it the active brain");
  print_notes(notes);
  return kExitOk;
}

int cmd_trust(const Args& a) {
  std::error_code ec;
  const std::string project = fs::absolute(a.positional[0], ec).lexically_normal().string();
  const std::optional<ImportOrigin> origin = import_origin(project);
  if (!fs::is_directory(project, ec)) throw PackageError("There is no project folder at " + project + ".");
  if (!origin) {
    if (!not_trusted_message(project).empty()) trust_project(project);   // inside an imported project: refused
    std::printf("This project wasn't imported from a shared file, so Vyrn already runs it.\n");
    return kExitOk;
  }
  if (origin->trusted) {
    std::printf("You already trust this project.\n");
    return kExitOk;
  }
  trust_project(project);
  std::printf("You now trust %s%s%s. Vyrn will run its Python code when you press Check, Train or Watch.\n",
              origin->name.empty() ? "this project" : ("\"" + origin->name + "\"").c_str(),
              origin->file.empty() ? "" : " from ", origin->file.c_str());
  return kExitOk;
}

}  // namespace

bool is_package_command(std::string_view command) {
  return command == "pack" || command == "export-brain" || command == "inspect" || command == "import" ||
         command == "trust" || command == "compile";
}

const char* package_usage() { return kUsage; }

int run_package_command(int argc, char** argv) {
  if (argc < 2 || !is_package_command(argv[1])) {
    std::fputs(kUsage, stderr);
    return kExitUsage;
  }
  Args args;
  try {
    args = parse_args(argc, argv);
  } catch (const UsageError& e) {
    std::fprintf(stderr, "vyrn-cli: %s\n\n%s", e.message.c_str(), kUsage);
    return kExitUsage;
  }
  if (args.help) {
    std::fputs(kUsage, stdout);
    return kExitOk;
  }
  try {
    if (args.command == "pack" || args.command == "export-brain") return cmd_export(args);
    if (args.command == "inspect") return cmd_inspect(args);
    if (args.command == "compile") return cmd_compile(args);
    if (args.command == "import") return cmd_import(args, argc > 0 ? argv[0] : nullptr);
    return cmd_trust(args);
  } catch (const std::exception& e) {
    if (args.json) print_json(json{{"error", e.what()}});
    std::fprintf(stderr, "vyrn-cli: %s\n", e.what());
    return kExitPackage;
  }
}

}  // namespace vyrn::cli
