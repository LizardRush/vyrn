// Small system services (docs/dev/platforms.md section 3): time, environment, process identity for pid files and
// locks, where things are, thread priority, random bytes. The public part is in vyrn/platform.h.
#pragma once

#include "vyrn/platform.h"

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace vyrn::platform {

bool local_time(std::time_t t, std::tm& out);   // localtime_r / localtime_s
bool utc_time(std::time_t t, std::tm& out);     // gmtime_r / gmtime_s

bool set_env(const std::string& name, const std::string& value);
bool unset_env(const std::string& name);

// Who a pid is right now, to tell a process from a later one that got the same pid.
//   start: Linux clock ticks after boot (/proc/<pid>/stat field 22), macOS microseconds since the epoch
//          (proc_pidinfo), Windows the creation FILETIME (GetProcessTimes).
//   group: its process group (Windows: the pid itself).
struct ProcessIdentity {
  long long start = 0;
  long long group = 0;
};
std::optional<ProcessIdentity> process_identity(int64_t pid);

// This boot ("" when unknown): Linux /proc/sys/kernel/random/boot_id, macOS kern.boottime, Windows the boot time
// from GetTickCount64. A pid file or lock from another boot is stale even when its pid is in use again.
const std::string& boot_id();
// True when `recorded` (an earlier boot_id()) names this boot. Windows allows the boot time to drift a little.
bool same_boot(const std::string& recorded);

// The user's home folders, for hiding them in shared files: HOME and the account's own (POSIX), USERPROFILE and
// HOME (Windows). Without a trailing separator; may be empty.
std::vector<std::string> home_dirs();

// Where an installed Vyrn keeps python/ and templates/, given this program's path (docs/dev/platforms.md table):
// Linux <exe>/../share/vyrn; macOS <exe>/../Resources/vyrn, then <exe>/../share/vyrn; Windows
// <exe dir>/share/vyrn, then <exe>/../share/vyrn.
std::vector<std::string> data_dir_candidates(const std::string& exe);

// Lowers the calling thread below the others (Linux: nice + `nice_delta` for this thread; macOS: QoS utility;
// Windows: THREAD_PRIORITY_BELOW_NORMAL). Threads it starts afterwards inherit it where the system does that.
void lower_thread_priority(int nice_delta);

// Cryptographically random bytes (getentropy / BCryptGenRandom). False if the system refused.
bool random_bytes(void* out, size_t size);

}  // namespace vyrn::platform
