#include "util/csv.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vyrn::util {

std::string csv_cell(const std::string& text) {
  if (text.find_first_of(",\"\r\n") == std::string::npos) return text;
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"') out += '"';
    out += c;
  }
  out += '"';
  return out;
}

std::string csv_number(double v) {
  if (!std::isfinite(v)) return "";
  if (v == 0) return "0";   // also turns -0 into 0
  char text[32];
  std::snprintf(text, sizeof text, "%.6g", v);
  return text;
}

std::string csv_number(long long v) { return std::to_string(v); }

bool CsvWriter::open(const std::string& path, const std::vector<std::string>& header, std::string* why) {
  close();
  out_.open(path, std::ios::out | std::ios::trunc);
  if (!out_) {
    if (why) *why = "Vyrn can't create " + path + " (" + std::strerror(errno) + ")";
    return false;
  }
  std::vector<std::string> cells;
  cells.reserve(header.size());
  for (const auto& name : header) cells.push_back(csv_cell(name));
  row(cells);
  flush();
  return true;
}

void CsvWriter::row(const std::vector<std::string>& cells) {
  if (!out_.is_open()) return;
  for (size_t i = 0; i < cells.size(); ++i) {
    if (i) out_ << ',';
    out_ << cells[i];
  }
  out_ << '\n';
}

void CsvWriter::flush() {
  if (out_.is_open()) out_.flush();
}

void CsvWriter::close() {
  if (out_.is_open()) out_.close();
}

}  // namespace vyrn::util
