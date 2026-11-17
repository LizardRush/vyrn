#include "Paths.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>

#include <exception>

#include "AppPlatform.h"
#include "vyrn/platform.h"
#include "vyrn/project.h"
#include "vyrn/session.h"

namespace paths {

namespace {

struct State {
  bool initialized = false;
  QString dataDir;
};

State& state() {
  static State s;
  return s;
}

QString exeDir() {
  if (QCoreApplication::instance()) return QCoreApplication::applicationDirPath();
  const QString exe = QString::fromStdString(vyrn::platform::executable_path());
  return exe.isEmpty() ? QString() : QFileInfo(QFileInfo(exe).canonicalFilePath()).absolutePath();
}

// A program's file name here: "vyrn-cli", or "vyrn-cli.exe" on Windows.
QString programFile(const QString& name) {
  return app_platform::os() == app_platform::Os::Windows ? name + QStringLiteral(".exe") : name;
}

QString executableAt(const QString& path) {
  const QFileInfo info(path);
  return info.isFile() && info.isExecutable() ? QDir::cleanPath(info.absoluteFilePath()) : QString();
}

QString homeBin(const QString& name) { return QDir::home().filePath(QStringLiteral(".local/bin/") + name); }

// `name` next to this executable, then in the build tree (<build>/<buildSubdir>/<name>, for running from a build
// folder), then on PATH, then in ~/.local/bin (the Linux install prefix, in case PATH lacks it).
QString findProgram(const QString& name, const QString& buildSubdir) {
  const QDir here(exeDir());
  const QString file = programFile(name);
  QStringList candidates{here.filePath(file)};
  if (!buildSubdir.isEmpty()) {
    candidates << here.filePath(QStringLiteral("../%1/%2").arg(buildSubdir, file));
    // A macOS build folder: this program is <build>/app/Vyrn.app/Contents/MacOS/Vyrn.
    if (here.path().endsWith(QLatin1String(".app/Contents/MacOS")))
      candidates << here.filePath(QStringLiteral("../../../../%1/%2").arg(buildSubdir, file));
  }
  for (const QString& candidate : candidates) {
    if (QString found = executableAt(candidate); !found.isEmpty()) return found;
  }
  if (QString onPath = QStandardPaths::findExecutable(name); !onPath.isEmpty()) return onPath;
  return executableAt(homeBin(name));
}

}  // namespace

void init(const char* argv0) {
  State& s = state();
  s.dataDir = QString::fromStdString(vyrn::find_data_dir(argv0));
  s.initialized = true;
}

QString dataDir() {
  if (!state().initialized) init(nullptr);
  return state().dataDir;
}

QString projectsRoot() {
  try {
    return QString::fromStdString(vyrn::projects_root());
  } catch (const std::exception&) {
    return QDir::home().filePath(QStringLiteral("Vyrn"));   // could not be created; callers report it
  }
}

QString templatesDir() {
  const QString data = dataDir();
  return data.isEmpty() ? QString() : QDir(data).filePath(QStringLiteral("templates"));
}

QString focusHelper() { return findProgram(QStringLiteral("vyrn-focus"), QStringLiteral("focus")); }

QString cliPath() { return findProgram(QStringLiteral("vyrn-cli"), QStringLiteral("cli")); }

QString installedCli() {
  switch (app_platform::os()) {
    case app_platform::Os::MacOS:
      return QDir::home().filePath(QStringLiteral("Applications/Vyrn.app/Contents/MacOS/vyrn-cli"));
    case app_platform::Os::Windows: {
      // install-windows.ps1 installs into %LOCALAPPDATA%\Programs\Vyrn: the programs in bin\ (a flat folder works too).
      QString base = qEnvironmentVariable("LOCALAPPDATA");
      if (base.isEmpty()) base = QDir::home().filePath(QStringLiteral("AppData/Local"));
      const QDir vyrn(QDir(base).filePath(QStringLiteral("Programs/Vyrn")));
      const QString flat = vyrn.filePath(QStringLiteral("vyrn-cli.exe"));
      const QString inBin = vyrn.filePath(QStringLiteral("bin/vyrn-cli.exe"));
      return !QFileInfo::exists(inBin) && QFileInfo::exists(flat) ? flat : inBin;
    }
    case app_platform::Os::Linux:
      break;
  }
  return homeBin(QStringLiteral("vyrn-cli"));
}

QString panicScript() {
  if (QString installed = executableAt(homeBin(QStringLiteral("vyrn-panic"))); !installed.isEmpty()) return installed;
  return executableAt(QDir(exeDir()).filePath(QStringLiteral("vyrn-panic")));
}

QString cacheDir() {
  QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
  if (dir.isEmpty()) dir = QDir::temp().filePath(QStringLiteral("vyrn-cache"));
  QDir().mkpath(dir);
  return dir;
}

QString installScript() {
#ifdef VYRN_SOURCE_DIR
  const QString script = QDir(QStringLiteral(VYRN_SOURCE_DIR)).filePath(QStringLiteral("install.sh"));
  if (QFileInfo(script).isFile()) return script;
#endif
  return {};
}

QString installCommand() {
  switch (app_platform::os()) {
    case app_platform::Os::MacOS: return QStringLiteral("./install-mac.sh");
    case app_platform::Os::Windows: return QStringLiteral("install-windows.ps1");
    case app_platform::Os::Linux: break;
  }
  return QStringLiteral("./install.sh --app");
}

QString pretty(const QString& path) {
  const QString home = QDir::homePath();
  if (path == home) return QStringLiteral("~");
  if (path.startsWith(home + QLatin1Char('/'))) return QStringLiteral("~") + path.mid(home.size());
  return path;
}

}  // namespace paths
