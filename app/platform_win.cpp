// AppPlatform.h on Windows. The panic hotkey is RegisterHotKey on a message-only window of the UI thread: a native
// event filter takes WM_HOTKEY out of Qt's event loop, and the window's own procedure takes it while Windows runs a
// modal loop of its own (a window being dragged, a native file dialog), where Qt's filters see nothing. Keys and the
// screen need no permission here.
#include "AppPlatform.h"
#include <QProcess>

#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QCoreApplication>
#include <QTimer>

#include <utility>

#include <windows.h>
#include <tlhelp32.h>

namespace app_platform {

namespace {

constexpr int kPlainId = 0x5659;     // RegisterHotKey ids for an application: 0x0000-0xBFFF
constexpr int kShiftedId = 0x565A;
constexpr wchar_t kWindowClass[] = L"VyrnPanicHotkey";

std::function<void()>& pressedCallback() {
  static std::function<void()> pressed;
  return pressed;
}

bool isPanicHotkey(UINT message, WPARAM id) {
  return message == WM_HOTKEY && (id == WPARAM(kPlainId) || id == WPARAM(kShiftedId));
}

// The work runs from Qt's event loop, never inside the message dispatch.
void schedulePanic() {
  if (pressedCallback() && QCoreApplication::instance())
    QTimer::singleShot(0, QCoreApplication::instance(), [pressed = pressedCallback()] { pressed(); });
}

LRESULT CALLBACK hotkeyWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (isPanicHotkey(message, wparam)) {
    schedulePanic();
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

class HotkeyFilter final : public QAbstractNativeEventFilter {
 public:
  // Hot keys come as "windows_dispatcher_MSG" (system-wide messages), messages of Qt's own windows as
  // "windows_generic_MSG"; both are a MSG.
  bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override {
    if ((eventType != "windows_dispatcher_MSG" && eventType != "windows_generic_MSG") || !message) return false;
    const MSG* msg = static_cast<const MSG*>(message);
    if (!isPanicHotkey(msg->message, msg->wParam)) return false;
    schedulePanic();
    if (result) *result = 0;
    return true;   // handled here: not dispatched to the window procedure as well
  }
};

HWND hotkeyWindow() {
  static const HWND window = [] {
    WNDCLASSW type{};
    type.lpfnWndProc = hotkeyWindowProc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = kWindowClass;
    RegisterClassW(&type);
    return CreateWindowExW(0, kWindowClass, L"Vyrn panic key", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, type.hInstance,
                           nullptr);
  }();
  return window;
}

// When a process started (100 ns units since 1601); 0 when unknown.
ULONGLONG startTime(HANDLE process) {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
  return (ULONGLONG(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

}  // namespace

Os os() { return Os::Windows; }

QString panicShortcut() { return QStringLiteral("Ctrl+Alt+Esc"); }

bool installPanicHotkey(std::function<void()> pressed, QString* why) {
  pressedCallback() = std::move(pressed);
  static HotkeyFilter filter;
  static bool filterInstalled = false;
  if (!filterInstalled && QCoreApplication::instance()) {
    QCoreApplication::instance()->installNativeEventFilter(&filter);
    filterInstalled = true;
  }
  static bool plain = false;
  static bool shifted = false;
  const HWND window = hotkeyWindow();
  if (!window) {
    if (why) *why = QStringLiteral("Windows didn't let Vyrn listen for the panic key (Ctrl+Alt+Esc).");
    return false;
  }
  if (!plain) plain = RegisterHotKey(window, kPlainId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_ESCAPE) != 0;
  if (!shifted)
    shifted = RegisterHotKey(window, kShiftedId, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_ESCAPE) != 0;
  if (plain && shifted) return true;
  if (why) {
    *why = plain ? QStringLiteral("Another program already uses Shift+Ctrl+Alt+Esc, so the panic key doesn't work "
                                  "while the AI holds Shift.")
                 : QStringLiteral("Another program already uses Ctrl+Alt+Esc, so Vyrn's panic key doesn't work. Stop "
                                  "the AI with Stop instead, or close that program and start Vyrn again.");
  }
  return false;
}

bool isOwnChild(qint64 pid) {
  if (pid <= 0 || pid > 0xFFFFFFFFLL) return false;
  const DWORD child = static_cast<DWORD>(pid);
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return false;
  bool parentIsSelf = false;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof entry;
  for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
    if (entry.th32ProcessID == child) {
      parentIsSelf = entry.th32ParentProcessID == GetCurrentProcessId();
      break;
    }
  }
  CloseHandle(snapshot);
  if (!parentIsSelf) return false;
  // A parent id stays when the parent ends, and ids are reused: a real child started after this process did.
  const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, child);
  if (!process) return false;
  const ULONGLONG childStart = startTime(process);
  CloseHandle(process);
  const ULONGLONG selfStart = startTime(GetCurrentProcess());
  return childStart != 0 && selfStart != 0 && childStart >= selfStart;
}

void useOwnProcessGroup(QProcess* /*process*/) {}

bool accessibilityAllowed() { return true; }
bool screenRecordingAllowed() { return true; }
void requestScreenRecording() {}

QString updateCommand() {
  return QStringLiteral("irm https://raw.githubusercontent.com/LizardRush/vyrn/main/get-windows.ps1 | iex");
}

bool openUpdateTerminal(const QString& oneLiner) {
  return QProcess::startDetached(QStringLiteral("cmd"),
                                 {QStringLiteral("/c"), QStringLiteral("start"), QStringLiteral("powershell"),
                                  QStringLiteral("-NoExit"), QStringLiteral("-Command"), oneLiner});
}

}  // namespace app_platform
