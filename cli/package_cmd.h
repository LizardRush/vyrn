// vyrn-cli's sharing commands: pack, export-brain, inspect, import and trust (.vrl = one trained brain,
// .vrlp = a whole project with its brain; docs/dev/package.md section 8), and compile (the trained AI as a Python
// program that runs without Vyrn; docs/dev/standalone.md). cli/main.cpp hands them over first:
//
//   if (argc > 1 && vyrn::cli::is_package_command(argv[1])) return vyrn::cli::run_package_command(argc, argv);
//
// Standard library only (no torch, Qt or JSON library in this header). Implemented in cli/package_cmd.cpp, which
// links vyrn_package.
#pragma once

#include <string_view>

namespace vyrn::cli {

// True for "pack", "export-brain", "inspect", "import", "trust" and "compile".
bool is_package_command(std::string_view command);

// Runs one sharing command. argv[1] is the command and argv[2...] are its arguments; argv[0] is the program (used
// to find Vyrn's data folder for the Check that `import <file.vrl> --into P` runs). Prints the result (plain text,
// or one JSON object with --json) and returns the exit status: 0 ok, 2 usage, 5 the package was refused or
// couldn't be written or read.
int run_package_command(int argc, char** argv);

// The sharing commands' usage lines, for vyrn-cli's own --help.
const char* package_usage();

}  // namespace vyrn::cli
