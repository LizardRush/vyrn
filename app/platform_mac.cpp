// AppPlatform.h on macOS. The panic hotkey is a Carbon hot key (RegisterEventHotKey: global, needs no permission).
// Real games need two privacy permissions: Accessibility (Python presses keys with CGEventPost) and Screen Recording
// (pictures of other apps' windows). Python, started by the app, is covered by the app's permissions.

// AssertMacros.h (Carbon includes it) must not define macros called check(), verify() and require().
#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0

#include "AppPlatform.h"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryFile>

#include <QCoreApplication>
#include <QProcess>
#include <QTimer>

#include <climits>
#include <cstdint>
#include <utility>

#include <libproc.h>
#include <unistd.h>

#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>

namespace app_platform {

namespace {

constexpr OSType kSignature = (OSType('V') << 24) | (OSType('Y') << 16) | (OSType('R') << 8) | OSType('N');
constexpr UInt32 kPlainId = 1;
constexpr UInt32 kShiftedId = 2;

struct Hotkey {
  std::function<void()> pressed;
  EventHandlerRef handler = nullptr;
  EventHotKeyRef plain = nullptr;     // Control+Option+Esc
  EventHotKeyRef shifted = nullptr;   // Shift+Control+Option+Esc
};

Hotkey& hotkey() {
  static Hotkey h;
  return h;
}

// Carbon calls this for the hot keys of this process. The work itself runs from Qt's event loop.
OSStatus onHotKey(EventHandlerCallRef /*next*/, EventRef event, void* /*data*/) {
  EventHotKeyID id{};
  if (GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, nullptr, sizeof id, nullptr, &id) != noErr ||
      id.signature != kSignature || (id.id != kPlainId && id.id != kShiftedId)) {
    return eventNotHandledErr;
  }
  if (hotkey().pressed && QCoreApplication::instance())
    QTimer::singleShot(0, QCoreApplication::instance(), [pressed = hotkey().pressed] { pressed(); });
  return noErr;
}

}  // namespace

Os os() { return Os::MacOS; }

QString panicShortcut() { return QStringLiteral("Control+Option+Esc"); }

bool installPanicHotkey(std::function<void()> pressed, QString* why) {
  Hotkey& h = hotkey();
  h.pressed = std::move(pressed);
  if (!h.handler) {
    const EventTypeSpec type{kEventClassKeyboard, kEventHotKeyPressed};
    if (InstallApplicationEventHandler(&onHotKey, 1, &type, nullptr, &h.handler) != noErr) {
      h.handler = nullptr;
      if (why) *why = QStringLiteral("macOS didn't let Vyrn listen for the panic key (Control+Option+Esc).");
      return false;
    }
  }
  const UInt32 keys = controlKey | optionKey;
  if (!h.plain &&
      RegisterEventHotKey(kVK_Escape, keys, EventHotKeyID{kSignature, kPlainId}, GetApplicationEventTarget(), 0,
                          &h.plain) != noErr) {
    h.plain = nullptr;
  }
  if (!h.shifted &&
      RegisterEventHotKey(kVK_Escape, keys | shiftKey, EventHotKeyID{kSignature, kShiftedId},
                          GetApplicationEventTarget(), 0, &h.shifted) != noErr) {
    h.shifted = nullptr;
  }
  if (h.plain && h.shifted) return true;
  if (why) {
    *why = h.plain ? QStringLiteral("Another program already uses Shift+Control+Option+Esc, so the panic key doesn't "
                                    "work while the AI holds Shift.")
                   : QStringLiteral("Another program already uses Control+Option+Esc, so Vyrn's panic key doesn't "
                                    "work. Stop the AI with Stop instead, or quit that program and start Vyrn again.");
  }
  return false;
}

bool isOwnChild(qint64 pid) {
  if (pid <= 0 || pid > INT_MAX) return false;
  struct proc_bsdinfo info {};
  const int size = static_cast<int>(sizeof info);
  if (::proc_pidinfo(static_cast<int>(pid), PROC_PIDTBSDINFO, 0, &info, size) != size) return false;
  return info.pbi_ppid == static_cast<uint32_t>(::getpid());
}

void useOwnProcessGroup(QProcess* process) {
  process->setChildProcessModifier([] { ::setpgid(0, 0); });
}

bool accessibilityAllowed() { return AXIsProcessTrusted() != 0; }

bool screenRecordingAllowed() { return CGPreflightScreenCaptureAccess(); }

void requestScreenRecording() { (void)CGRequestScreenCaptureAccess(); }

QString updateCommand() {
  return QStringLiteral("curl -fsSL https://raw.githubusercontent.com/LizardRush/vyrn/main/get-mac.sh | bash");
}

bool openUpdateTerminal(const QString& oneLiner) {
  QTemporaryFile file(QDir::temp().filePath(QStringLiteral("vyrn-update-XXXXXX.command")));
  file.setAutoRemove(false);   // Terminal reads it after this returns
  if (!file.open()) return false;
  file.write(("#!/bin/bash\n" + oneLiner + "\necho\nread -n1 -p 'Press any key to close.'\n").toUtf8());
  file.close();
  if (!QFile::setPermissions(file.fileName(), QFileDevice::Permissions(0755))) return false;
  return QProcess::startDetached(QStringLiteral("open"), {file.fileName()});
}

}  // namespace app_platform
