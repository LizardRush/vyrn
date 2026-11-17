// AppPlatform.h on Linux (labwc/Wayland, the reference system). The panic key is a labwc keybind that runs
// vyrn-panic, so the app registers no hotkey; keys need no permission (uinput, set up by install.sh --keyboard).
#include "AppPlatform.h"
#include <QStandardPaths>
#include <QStringList>

#include <QByteArray>
#include <QFile>
#include <QList>
#include <QProcess>

#include <unistd.h>

namespace app_platform {

Os os() { return Os::Linux; }

QString panicShortcut() { return QStringLiteral("Ctrl+Alt+Esc"); }

bool installPanicHotkey(std::function<void()> /*pressed*/, QString* why) {
  if (why) why->clear();
  return false;
}

bool isOwnChild(qint64 pid) {
  if (pid <= 0) return false;
  // /proc/<pid>/stat is "pid (comm) state ppid ...": comm may hold spaces and parentheses, so read after the last ')'.
  QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
  if (!stat.open(QIODevice::ReadOnly)) return false;
  const QByteArray text = stat.readAll();
  const qsizetype end = text.lastIndexOf(')');
  if (end < 0) return false;
  const QList<QByteArray> fields = text.mid(end + 1).simplified().split(' ');
  bool ok = false;
  const qint64 parent = fields.size() > 1 ? fields.at(1).toLongLong(&ok) : 0;
  return ok && parent == qint64(::getpid());
}

void useOwnProcessGroup(QProcess* process) {
  process->setChildProcessModifier([] { ::setpgid(0, 0); });
}

bool accessibilityAllowed() { return true; }
bool screenRecordingAllowed() { return true; }
void requestScreenRecording() {}

QString updateCommand() {
  return QStringLiteral("curl -fsSL https://raw.githubusercontent.com/LizardRush/vyrn/main/get-linux.sh | bash");
}

bool openUpdateTerminal(const QString& oneLiner) {
  const QString script = oneLiner + QStringLiteral("; echo; read -n1 -p 'Press any key to close.'");
  for (const char* name : {"x-terminal-emulator", "lxterminal", "gnome-terminal", "konsole", "xfce4-terminal", "xterm"}) {
    const QString exe = QStandardPaths::findExecutable(QString::fromLatin1(name));
    if (exe.isEmpty()) continue;
    // gnome-terminal wants "--" and xfce4-terminal -x before the command; the others take -e.
    const QLatin1String which(name);
    const QString flag = which == QLatin1String("gnome-terminal")  ? QStringLiteral("--")
                         : which == QLatin1String("xfce4-terminal") ? QStringLiteral("-x")
                                                                    : QStringLiteral("-e");
    QStringList args{flag, QStringLiteral("bash"), QStringLiteral("-c"), script};
    if (QLatin1String(name) == QLatin1String("konsole")) args.prepend(QStringLiteral("--separate"));
    if (QProcess::startDetached(exe, args)) return true;
  }
  return false;
}

}  // namespace app_platform
