// release_all_keys() on macOS: a CoreGraphics key-up for every virtual key code 0-127 (kVK_*), posted at the HID
// level like the worker's key presses. Without the Accessibility permission the system drops them silently.
#include "vyrn/platform.h"

#include <ApplicationServices/ApplicationServices.h>

namespace vyrn::platform {

void release_all_keys() {
  for (CGKeyCode key = 0; key < 128; ++key) {
    CGEventRef up = CGEventCreateKeyboardEvent(nullptr, key, false);
    if (up == nullptr) continue;
    CGEventSetFlags(up, static_cast<CGEventFlags>(0));   // no modifier held while letting go
    CGEventPost(kCGHIDEventTap, up);
    CFRelease(up);
  }
}

}  // namespace vyrn::platform
