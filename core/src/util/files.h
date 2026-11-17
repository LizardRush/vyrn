// Whole-file reads and crash-safe writes. No torch in this header.
#pragma once

#include <string>

namespace vyrn::util {

// Reads a whole file into `out`. Returns false (with a plain-English reason in *why) if it can't.
bool read_text_file(const std::string& path, std::string& out, std::string* why = nullptr);

// Replaces a file atomically: writes <path>.tmp-<pid>, fsyncs it, renames it over <path> (platform::rename_replace),
// fsyncs the folder.
// Readers see the old or the new content, never half of it. Throws std::runtime_error (plain English).
void write_text_file_atomic(const std::string& path, const std::string& text);

// A folder name that doesn't exist yet inside `parent`: `base`, else base-2, base-3, ...
std::string unique_child_name(const std::string& parent, const std::string& base);

// True for names made only of letters, digits, '-' and '_' (1 to 64 characters): safe as one folder name.
bool is_safe_name(const std::string& name);

}  // namespace vyrn::util
