// M0 toolchain proof: prints the torch and Qt versions and times one small training step.
#include <cstdio>
#include <string>

std::string hello_torch();
#ifdef VYRN_HELLO_QT
std::string hello_qt();
#endif

int main() {
  std::printf("%s\n", hello_torch().c_str());
#ifdef VYRN_HELLO_QT
  std::printf("%s\n", hello_qt().c_str());
#endif
  return 0;
}
