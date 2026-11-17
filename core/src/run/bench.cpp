// vyrn::run_bench (public, vyrn/bench.h): the brain's benchmark for `vyrn-cli bench`.
#include "vyrn/bench.h"

#include "brain/bench.h"
#include "brain/threads.h"

namespace vyrn {

std::string run_bench() {
  brain::init_torch_process();
  return brain::run_bench();
}

}  // namespace vyrn
