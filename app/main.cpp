// Vyrn: the launcher and IDE. Startup order matters (docs/dev/app-internals.md section 2).
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPointer>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <memory>

#include "AppPlatform.h"
#include "Background.h"
#include "LauncherWindow.h"
#include "Paths.h"
#include "ProjectStore.h"
#include "ProjectWindow.h"
#include "Sharing.h"
#include "SmokeTest.h"
#include "Theme.h"
#include "Updater.h"
#include "vyrn/platform.h"

#ifndef VYRN_VERSION
#define VYRN_VERSION "dev"
#endif

namespace {

void printUsage(FILE* to) {
  std::fputs(
      "Usage: vyrn [project folder | shared file (.vrlp, .vrl)]\n"
      "       vyrn --smoke-test <project> [--check] [--train-seconds N] [--inject-error] [--screenshot out.png]\n"
      "       vyrn --smoke-test --open-package <file.vrlp> [--trust] [--check] [...]\n"
      "       ... [--export-brain out.vrl] [--import-brain file.vrl] [--compile <new folder>]\n"
      "\n"
      "Without a folder, Vyrn shows your projects (in ~/Vyrn, or $VYRN_PROJECTS). A .vrlp file (a shared project)\n"
      "opens the import dialog; a .vrl file (a trained AI) asks which project gets it.\n",
      to);
}

// "vyrn path/to/project/points.py" (e.g. from the file manager) opens the project the file is in; "vyrn ." the
// current folder. The result is a clean absolute path.
QString projectFolderFor(const QString& argument) {
  const QFileInfo info(argument);
  const QFileInfo folder(info.isFile() ? info.absolutePath() : info.absoluteFilePath());
  const QString canonical = folder.canonicalFilePath();
  return canonical.isEmpty() ? QDir::cleanPath(folder.absoluteFilePath()) : canonical;
}

// vyrn --update-test <stamp folder> (tests/gui/test_updater.py): one manual update check against $VYRN_UPDATE_URL,
// then the Update now step with a recording terminal hook. Prints one JSON line. Settings go to a temporary folder;
// no terminal opens.
int runUpdateTest(const QStringList& args) {
  QTemporaryDir temp;
  QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, temp.filePath(QStringLiteral("config")));
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.filePath(QStringLiteral("config")));
  Updater::setStampDir(args.value(0));
  QJsonObject out{{QStringLiteral("result"), QStringLiteral("none")}};
  QString command;
  Updater::setTerminalHook([&command](const QString& line) {
    command = line;
    return true;
  });
  Updater updater;
  QEventLoop loop;
  auto done = [&](const QString& result, const QString& detail) {
    out.insert(QStringLiteral("result"), result);
    out.insert(QStringLiteral("detail"), detail);
    loop.quit();
  };
  QObject::connect(&updater, &Updater::updateAvailable, [&](const QString& sha) { done(QStringLiteral("available"), sha); });
  QObject::connect(&updater, &Updater::upToDate, [&] { done(QStringLiteral("current"), QString()); });
  QObject::connect(&updater, &Updater::failed, [&](const QString& why) { done(QStringLiteral("failed"), why); });
  QObject::connect(&updater, &Updater::unknown, [&] { done(QStringLiteral("unknown"), QString()); });
  QTimer::singleShot(20000, &loop, &QEventLoop::quit);
  updater.check(true);
  loop.exec();
  out.insert(QStringLiteral("started"), Updater::startUpdate());
  out.insert(QStringLiteral("command"), command);
  std::printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // libgomp reads OMP_WAIT_POLICY once, when it is loaded, and busy-waiting OpenMP threads would eat the Pi's cores:
  // Linux and macOS set it to PASSIVE and start this program again; Windows sets it for child processes only.
  vyrn::platform::ensure_passive_openmp(argv);

  QApplication::setApplicationName(QStringLiteral("Vyrn"));
  QApplication::setOrganizationName(QStringLiteral("Vyrn"));
  QApplication::setApplicationVersion(QStringLiteral(VYRN_VERSION));
  QGuiApplication::setDesktopFileName(QStringLiteral("vyrn"));   // the Wayland app_id: the focus guard relies on it
  QApplication app(argc, argv);
  app.setWindowIcon(QIcon(QStringLiteral(":/vyrn.png")));

  paths::init(argv[0]);
  theme::apply(app);

  QStringList args = app.arguments().mid(1);
  if (const qsizetype smoke = args.indexOf(QStringLiteral("--smoke-test")); smoke >= 0)
    return runSmokeTest(args.mid(smoke + 1));
  if (const qsizetype test = args.indexOf(QStringLiteral("--update-test")); test >= 0)
    return runUpdateTest(args.mid(test + 1));
  if (args.contains(QStringLiteral("--help")) || args.contains(QStringLiteral("-h"))) {
    printUsage(stdout);
    return 0;
  }
  if (args.contains(QStringLiteral("--version"))) {
    std::printf("vyrn %s\n", VYRN_VERSION);
    return 0;
  }
  // macOS and Windows: the panic key is this app's own global hotkey (Linux: labwc's keybinds run vyrn-panic). When
  // the system refuses it, the red banner and the countdown stop promising it (ProjectWindow reads the property).
  QString hotkeyProblem;
  if (!app_platform::installPanicHotkey([] { ProjectWindow::panicStopAll(); }, &hotkeyProblem) &&
      !hotkeyProblem.isEmpty()) {
    std::fprintf(stderr, "vyrn: %s\n", qPrintable(hotkeyProblem));
    app.setProperty(ProjectWindow::kPanicKeyProblem, hotkeyProblem);
  }

  QString projectArg;
  for (const QString& arg : std::as_const(args)) {
    if (arg.startsWith(QLatin1Char('-'))) {
      std::fprintf(stderr, "vyrn: unknown option %s\n\n", qPrintable(arg));
      printUsage(stderr);
      return 2;
    }
    if (projectArg.isEmpty()) projectArg = arg;   // a desktop entry may pass several files: use the first
  }

  // "vyrn Catch.vrlp" (a double-click in the file manager): the launcher opens the import flow.
  QString sharedFile;
  if (!projectArg.isEmpty() && sharing::kindOf(projectArg) != sharing::FileKind::None) {
    sharedFile = QFileInfo(projectArg).absoluteFilePath();
    projectArg.clear();
  }

  if (!projectArg.isEmpty()) {
    const QString dir = projectFolderFor(projectArg);
    if (!store::project(dir).name.isEmpty()) {
      QPointer<ProjectWindow> window = new ProjectWindow(dir);
      window->setAttribute(Qt::WA_DeleteOnClose);
      window->show();
      const int code = app.exec();
      delete window.data();   // normally already gone (closed and deleted)
      background::waitForJobs(10'000);   // e.g. a file still being written
      return code;
    }
    QMessageBox::warning(nullptr, QStringLiteral("Not a Vyrn project"),
                         QStringLiteral("%1 isn't a Vyrn project. A project folder has a vyrn.json or an inputs.py. "
                                        "Vyrn shows your projects instead.")
                             .arg(paths::pretty(dir)));
  }

  auto launcher = std::make_unique<LauncherWindow>();
  launcher->show();
  if (!sharedFile.isEmpty())
    QTimer::singleShot(0, launcher.get(), [l = launcher.get(), sharedFile] { l->openSharedFile(sharedFile); });
  const int code = app.exec();
  launcher.reset();
  background::waitForJobs(10'000);   // e.g. a shared file still being written
  return code;
}
