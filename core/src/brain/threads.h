// Torch thread settings (PLAN.md §2). No torch in this header.
#pragma once

namespace vyrn::brain {

// oneDNN's default, chosen from `vyrn-cli bench` on the Raspberry Pi 5 (docs/dev/bench.md): off. Debian's
// oneDNN has no Arm Compute Library and this A76 has no SVE, so with it the picture networks learn and act
// 2-3x slower (the MLP costs the same). VYRN_ONEDNN=1 (on) or VYRN_ONEDNN=0 (off) overrides it.
constexpr bool kOneDnnDefault = false;

// Once per process, before torch work starts (later calls do nothing): one inter-op thread, and oneDNN as
// kOneDnnDefault or the VYRN_ONEDNN override says.
void init_torch_process();

// Call first on every thread that runs torch ops (brain 3, actor 1, learner 2). It runs libtorch's own
// per-thread lazy init (via the exported at::get_num_threads()) and only then sets k, so the lazy init
// can't reset k on the thread's first parallel op. Also makes sure init_torch_process() has run.
void set_torch_threads(int k);

// The torch thread count of the calling thread.
int torch_threads();

bool onednn_enabled();
void set_onednn_enabled(bool on);   // process-wide; switch only between runs (the bench does)

}  // namespace vyrn::brain
