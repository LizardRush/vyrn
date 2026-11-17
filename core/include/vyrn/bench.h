// `vyrn-cli bench`: times the neural network on this computer. Standard library only.
#pragma once

#include <string>

namespace vyrn {

// Times acting and learning for the three networks with oneDNN off/on and 1-3 torch threads, on the calling
// thread. Returns a readable table plus a recommendation. Takes about half a minute.
std::string run_bench();

}  // namespace vyrn
