// release_all_keys() on Linux: nothing to do. Workers press keys through a uinput device, and the kernel releases
// every key of that device when the worker's process ends (docs/dev/platforms.md section 2).
#include "vyrn/platform.h"

namespace vyrn::platform {

void release_all_keys() {}

}  // namespace vyrn::platform
