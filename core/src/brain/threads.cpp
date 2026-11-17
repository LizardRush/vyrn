#include "brain/threads.h"

#include <ATen/Parallel.h>
#include <torch/torch.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace vyrn::brain {

void init_torch_process() {
  static std::once_flag once;
  std::call_once(once, [] {
    try {
      at::set_num_interop_threads(1);
    } catch (const std::exception&) {
      // Already set, or inter-op work has started: keep what is there.
    }
    const char* env = std::getenv("VYRN_ONEDNN");
    bool onednn = kOneDnnDefault;
    if (env != nullptr && std::strcmp(env, "1") == 0) onednn = true;
    if (env != nullptr && std::strcmp(env, "0") == 0) onednn = false;
    set_onednn_enabled(onednn);
  });
}

void set_torch_threads(int k) {
  init_torch_process();
  (void)at::get_num_threads();   // libtorch's per-thread lazy init happens here, not on the first op
  at::set_num_threads(std::max(1, k));
}

int torch_threads() { return at::get_num_threads(); }

bool onednn_enabled() { return at::globalContext().userEnabledMkldnn(); }

void set_onednn_enabled(bool on) { at::globalContext().setUserEnabledMkldnn(on); }

}  // namespace vyrn::brain
