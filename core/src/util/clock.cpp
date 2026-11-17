#include "util/clock.h"

#include "platform/system.h"

#include <ctime>

namespace vyrn::util {

namespace {

std::string format_now(const char* pattern) {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  platform::local_time(now, local);
  char text[64];
  const size_t n = std::strftime(text, sizeof text, pattern, &local);
  return std::string(text, n);
}

}  // namespace

std::string local_stamp() { return format_now("%Y%m%d-%H%M%S"); }

std::string local_iso_time() { return format_now("%Y-%m-%dT%H:%M:%S"); }

}  // namespace vyrn::util
