// release_all_keys() on Windows: SendInput key-ups by scan code (KEYEVENTF_SCANCODE, like the worker presses them,
// so games that read DirectInput see them too) for set-1 scan codes 1-0x58 plus the extended (E0) keys: arrows,
// Insert/Delete/Home/End/Page Up/Page Down, right Ctrl/Alt, keypad Enter and /, the Windows and menu keys.
#include "vyrn/platform.h"

#include "platform/win_util.h"

#include <vector>

namespace vyrn::platform {

void release_all_keys() {
  std::vector<INPUT> inputs;
  const auto key_up = [&](WORD scan, bool extended) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = scan;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
    inputs.push_back(in);
  };
  static const WORD kExtended[] = {0x1C, 0x1D, 0x35, 0x38, 0x47, 0x48, 0x49, 0x4B, 0x4D,
                                   0x4F, 0x50, 0x51, 0x52, 0x53, 0x5B, 0x5C, 0x5D};
  for (WORD scan = 0x01; scan <= 0x58; ++scan) key_up(scan, false);
  for (const WORD scan : kExtended) key_up(scan, true);
  SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

}  // namespace vyrn::platform
