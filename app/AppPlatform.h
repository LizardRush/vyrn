// What the app does differently on Linux, macOS and Windows (docs/dev/app-internals.md section 10). Exactly one of
// platform_linux.cpp, platform_mac.cpp and platform_win.cpp implements this header (app/CMakeLists.txt picks it).
// Everything else in app/ is the same code everywhere: it asks os() or calls these functions, never #ifdef.
// Core matters (processes, keys, places) come from vyrn/platform.h.
#pragma once

#include <QString>

#include <functional>

class QProcess;

namespace app_platform {

enum class Os { Linux, MacOS, Windows };
Os os();

// The panic shortcut as people read it on this system: "Ctrl+Alt+Esc" (Linux, Windows), "Control+Option+Esc"
// (macOS).
QString panicShortcut();

// macOS and Windows: registers the global panic hotkey, the shortcut above and the same with Shift (the AI may be
// holding Shift down). `pressed` runs on the UI thread, from the event loop, each time it is pressed. False with a
// plain sentence in *why when the system refuses (another program took the keys). Linux: labwc's keybinds run
// vyrn-panic instead (SetupDialog checks them), so this does nothing and returns false with *why empty.
bool installPanicHotkey(std::function<void()> pressed, QString* why);

// True when `pid` is a running process this app started itself (its parent is this process). The panic stop ends
// only this app's own Python workers.
bool isOwnChild(qint64 pid);

// Settings → Start program. Linux and macOS start the program in a process group of its own, so that
// vyrn::platform::stop_process_tree() ends it together with what it started. Windows: nothing to do (the tree is
// found through the parent process ids).
void useOwnProcessGroup(QProcess* process);

// The privacy permissions macOS asks for (real games); elsewhere they are always granted.
bool accessibilityAllowed();     // AXIsProcessTrusted(): Python may press keys (CGEventPost)
bool screenRecordingAllowed();   // CGPreflightScreenCaptureAccess(): Python and Pick area may see other windows
void requestScreenRecording();   // CGRequestScreenCaptureAccess(): macOS asks the person (only the first time)

// Updating Vyrn (app/Updater.h): the README's one-line install command of this system, and a terminal window that runs
// it (so the person sees the build). False when no terminal could be started.
QString updateCommand();
bool openUpdateTerminal(const QString& oneLiner);

}  // namespace app_platform
