// What the agent reads: the server's instructions, the guide (vyrn_guide, vyrn://guide) and the vyrn library
// reference (vyrn://api), made from the library's docstrings by python/tools/gen_api.py's collector.
//
// C++17 (the unit tests include this header).
#pragma once

#include <string>
#include <vector>

namespace vyrn::mcp {

// The initialize / server/discover "instructions": the three-file contract in short, and "call vyrn_guide first".
std::string instructions_text();

// The hand-written part of the guide (everything but the library reference).
std::string guide_text();

class Guide {
 public:
  Guide(std::string python, std::string data_dir);
  std::string guide();   // guide_text() followed by api()
  std::string api();     // the library reference (made once, then kept), or a short fallback

 private:
  std::string python_, data_dir_;
  std::string api_;
  bool made_ = false;
  double failed_at_ = -1;   // a failure is retried after a minute
};

// Runs a program (stdin /dev/null, no inherited descriptors, default signals) and collects its stdout (at most
// `max_bytes`). False + a plain reason when it can't start, runs longer than `timeout_s` (it is killed) or fails.
bool run_capture(const std::vector<std::string>& argv, double timeout_s, size_t max_bytes, std::string& out,
                 std::string& why);

}  // namespace vyrn::mcp
