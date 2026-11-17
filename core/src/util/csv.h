// CSV logs (runs/<time>/rounds.csv, updates.csv). No torch in this header.
#pragma once

#include <fstream>
#include <string>
#include <vector>

namespace vyrn::util {

// One cell: quoted (with "" for quotes) only when it holds a comma, a quote or a line break.
std::string csv_cell(const std::string& text);
// A number as a cell: up to 6 significant digits ("0.25", "1240", "-3.5e-05"); NaN and infinity give "".
std::string csv_number(double v);
std::string csv_number(long long v);

class CsvWriter {
 public:
  CsvWriter() = default;
  CsvWriter(const CsvWriter&) = delete;
  CsvWriter& operator=(const CsvWriter&) = delete;
  ~CsvWriter() { close(); }

  // Creates (or empties) the file and writes the header row. False with a reason in *why on failure.
  bool open(const std::string& path, const std::vector<std::string>& header, std::string* why = nullptr);
  bool is_open() const { return out_.is_open(); }
  // Cells are written as given: build them with csv_cell / csv_number.
  void row(const std::vector<std::string>& cells);
  void flush();
  void close();

 private:
  std::ofstream out_;
};

}  // namespace vyrn::util
