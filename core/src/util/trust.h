// Imported projects (docs/dev/package.md section 7): <project>/.vyrn-imported marks a project made from a shared
// .vrlp file. Until a person trusts it, Check, Train and Watch run none of its Python code (run_session enforces
// it). vyrn_package's is_trusted() calls project_trusted(). No torch in this header.
#pragma once

#include <string>

namespace vyrn::util {

constexpr const char* kTrustMarkerName = ".vyrn-imported";

// No marker -> true (the project wasn't imported); a marker whose "trusted" is true -> true; anything else (false,
// a damaged or unreadable marker, a symlink, a folder) -> false. The folders above `dir` count too (as given and
// with symlinks resolved): a folder inside an imported project is untrusted until that project is trusted, so a
// shared file can't hide a runnable "project" in one of its sub-folders.
bool project_trusted(const std::string& dir);

// The folder whose marker makes `dir` untrusted (`dir` itself, or the imported project it is inside); "" when
// `dir` is trusted.
std::string untrusted_project_folder(const std::string& dir);

// "" for a trusted project, else the sentence of the "not_trusted" error event: "This project came from
// Catch.vrlp. Its Python code hasn't run on this computer yet: read its scripts, then trust it in Vyrn (or run:
// vyrn-cli trust <folder>)." <folder> is the imported project's folder (the one to trust), also when `dir` is a
// folder inside it.
std::string not_trusted_message(const std::string& dir);

}  // namespace vyrn::util
