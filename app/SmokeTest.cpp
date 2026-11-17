// vyrn --smoke-test: opens a copy of a project in a ProjectWindow and drives it the way a person would (Check,
// Train for a while, a deliberate NameError, a screenshot), then prints one JSON line
// (docs/dev/app-internals.md section 9). Works with QT_QPA_PLATFORM=offscreen; never waits for a person: the
// import and trust dialogs of --open-package / --trust are shown and their buttons pressed in code. A project with
// a Start program gets it started like in the app (the tests give it a harmless command, never a real game).
// --compile <folder>: the toolbar's Compile (CompileDialog) into <folder>; the copy of the project keeps its brains/.
#include "SmokeTest.h"

#include <QAction>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <functional>

#include "CompileDialog.h"
#include "ConsoleView.h"
#include "Editor.h"
#include "OpenPackageDialog.h"
#include "Paths.h"
#include "ProjectWindow.h"
#include "ProjectStore.h"
#include "SessionBridge.h"
#include "SettingsDialog.h"
#include "ShareDialog.h"
#include "StartProgram.h"
#include "TrustDialog.h"
#include "vyrn/package.h"

namespace {

using Action = vyrn::RunConfig::Action;

constexpr int kCheckTimeoutMs = 180'000;   // Python starts, imports numpy, runs a few random steps
constexpr int kStartTimeoutMs = 300'000;   // Train: the automatic Check, then the brain is built
constexpr int kStopTimeoutMs = 180'000;    // Stop: the brain is saved first
constexpr int kIdleGiveUpMs = 5'000;       // Train didn't follow its Check within this time: it won't
constexpr int kPackageTimeoutMs = 120'000; // reading (every weight is checked) or importing a shared file
constexpr int kDialogTimeoutMs = 5'000;    // a dialog the window opens at once
constexpr int kGameTimeoutMs = 10'000;     // Start program: the window's short wait for it, or its end after Stop

void printJson(const QJsonObject& object) {
  const QByteArray text = QJsonDocument(object).toJson(QJsonDocument::Compact);
  std::fwrite(text.constData(), 1, size_t(text.size()), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

int usage(const QString& why) {
  printJson(QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), why}});
  std::fputs("Usage: vyrn --smoke-test <project> [--check] [--train-seconds N [--panic]] [--inject-error]\n"
             "                        [--screenshot out.png]\n"
             "       vyrn --smoke-test --open-package <file.vrlp> [--trust] [--check] [...]\n"
             "       ... [--export-brain out.vrl] [--import-brain file.vrl] [--compile <new folder>]\n",
             stderr);
  return 2;
}

// Runs the event loop until done() is true (looked at every 50 ms) or the time is up.
bool waitUntil(const std::function<bool()>& done, int timeoutMs) {
  if (done()) return true;
  QEventLoop loop;
  QTimer poll;
  QElapsedTimer clock;
  clock.start();
  bool ok = false;
  QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
    if (done()) {
      ok = true;
      loop.quit();
    } else if (clock.elapsed() >= timeoutMs) {
      loop.quit();
    }
  });
  poll.start(50);
  loop.exec();
  return ok;
}

void pause(int ms) { waitUntil([] { return false; }, ms); }

// $VYRN_SMOKE_DIALOG_SHOTS=<folder>: also save a picture of each dialog the run shows (for looking at the UI).
void shootDialog(QWidget* dialog, const QString& name) {
  const QString folder = qEnvironmentVariable("VYRN_SMOKE_DIALOG_SHOTS");
  if (folder.isEmpty() || !dialog) return;
  QDir().mkpath(folder);
  dialog->grab().save(QDir(folder).filePath(name + QStringLiteral(".png")), "PNG");
}

// A copy of the project as a fresh start: without brains/ (unless keepBrains), runs/, __pycache__ and the lock.
bool copyProject(const QString& from, const QString& to, bool keepBrains, QString* error) {
  const QDir source(from);
  if (!QDir().mkpath(to)) {
    *error = QStringLiteral("can't create %1").arg(to);
    return false;
  }
  QDirIterator it(from, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    const QString path = it.next();
    const QString relative = source.relativeFilePath(path);
    const QString top = relative.section(QLatin1Char('/'), 0, 0);
    if ((top == QLatin1String("brains") && !keepBrains) || top == QLatin1String("runs") ||
        top == QLatin1String(".vyrn-lock") ||
        relative.contains(QLatin1String("__pycache__")))
      continue;
    const QFileInfo info = it.fileInfo();
    const QString target = QDir(to).filePath(relative);
    if (info.isSymLink()) continue;
    if (info.isDir()) {
      QDir().mkpath(target);
      continue;
    }
    QDir().mkpath(QFileInfo(target).absolutePath());
    if (!QFile::copy(path, target)) {
      *error = QStringLiteral("can't copy %1").arg(relative);
      return false;
    }
    QFile(target).setPermissions(QFile::permissions(target) | QFile::ReadOwner | QFile::WriteOwner);
  }
  return true;
}

}  // namespace

int runSmokeTest(const QStringList& args) {
  QString project, screenshot, package, exportBrainTo, importBrainFrom, compileTo;
  bool doCheck = false, injectError = false, doTrust = false, doPanic = false;
  int trainSeconds = 0;
  for (qsizetype i = 0; i < args.size(); ++i) {
    const QString& arg = args.at(i);
    if (arg == QLatin1String("--check")) {
      doCheck = true;
    } else if (arg == QLatin1String("--inject-error")) {
      injectError = true;
    } else if (arg == QLatin1String("--panic")) {
      doPanic = true;
    } else if (arg == QLatin1String("--train-seconds")) {
      bool ok = false;
      trainSeconds = ++i < args.size() ? args.at(i).toInt(&ok) : 0;
      if (!ok || trainSeconds < 0) return usage(QStringLiteral("--train-seconds needs a number of seconds"));
    } else if (arg == QLatin1String("--screenshot")) {
      if (++i >= args.size()) return usage(QStringLiteral("--screenshot needs a file name"));
      screenshot = QFileInfo(args.at(i)).absoluteFilePath();
    } else if (arg == QLatin1String("--open-package")) {
      if (++i >= args.size()) return usage(QStringLiteral("--open-package needs a .vrlp file"));
      package = QFileInfo(args.at(i)).absoluteFilePath();
    } else if (arg == QLatin1String("--trust")) {
      doTrust = true;
    } else if (arg == QLatin1String("--export-brain")) {
      if (++i >= args.size()) return usage(QStringLiteral("--export-brain needs a .vrl file name"));
      exportBrainTo = QFileInfo(args.at(i)).absoluteFilePath();
    } else if (arg == QLatin1String("--compile")) {
      if (++i >= args.size()) return usage(QStringLiteral("--compile needs a folder name"));
      compileTo = QFileInfo(args.at(i)).absoluteFilePath();
    } else if (arg == QLatin1String("--import-brain")) {
      if (++i >= args.size()) return usage(QStringLiteral("--import-brain needs a .vrl file"));
      importBrainFrom = QFileInfo(args.at(i)).absoluteFilePath();
    } else if (arg.startsWith(QLatin1Char('-'))) {
      return usage(QStringLiteral("unknown option %1").arg(arg));
    } else if (project.isEmpty()) {
      project = arg;
    } else {
      return usage(QStringLiteral("give one project folder"));
    }
  }
  if (!package.isEmpty() && !project.isEmpty())
    return usage(QStringLiteral("give a project folder or --open-package, not both"));
  if (package.isEmpty() && project.isEmpty())
    return usage(QStringLiteral("which project? vyrn --smoke-test <project folder>"));
  if (doTrust && package.isEmpty()) return usage(QStringLiteral("--trust goes with --open-package"));
  if (doPanic && trainSeconds <= 0) return usage(QStringLiteral("--panic goes with --train-seconds"));

  QTemporaryDir temp(QDir(QDir::tempPath()).filePath(QStringLiteral("vyrn-smoke-XXXXXX")));
  if (!temp.isValid()) return usage(QStringLiteral("can't make a temporary folder: %1").arg(temp.errorString()));
  // App settings (e.g. the remembered author of an export) go to the temporary folder, not the person's config.
  QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, QDir(temp.path()).filePath(QStringLiteral("config")));
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QDir(temp.path()).filePath(QStringLiteral("config")));
  QString dir;
  if (package.isEmpty()) {
    const QFileInfo source(project);
    const QDir sourceDir(source.absoluteFilePath());
    if (!source.isDir() ||
        (!sourceDir.exists(QStringLiteral("vyrn.json")) && !sourceDir.exists(QStringLiteral("inputs.py"))))
      return usage(QStringLiteral("%1 is not a Vyrn project folder").arg(project));
    const QString folder = source.fileName().isEmpty() ? QStringLiteral("project") : source.fileName();
    dir = QDir(temp.path()).filePath(folder);
    QString error;
    // Compile needs the trained brain: with --compile, the copy keeps brains/.
    if (!copyProject(sourceDir.absolutePath(), dir, !compileTo.isEmpty(), &error))
      return usage(QStringLiteral("can't copy the project: %1").arg(error));
  } else {
    // Open shared file → Import, through the import dialog: into $VYRN_PROJECTS (the tests point it at a
    // temporary folder), else into a temporary folder. Never into ~/Vyrn.
    const QString root = qEnvironmentVariableIsEmpty("VYRN_PROJECTS") ? QDir(temp.path()).filePath(QStringLiteral("projects"))
                                                                       : paths::projectsRoot();
    QDir().mkpath(root);
    auto notImported = [](const QString& why) {
      printJson(QJsonObject{{QStringLiteral("ok"), false},
                            {QStringLiteral("imported"), false},
                            {QStringLiteral("failures"), QJsonArray{why}}});
      return 1;
    };
    OpenPackageDialog dialog(package, OpenPackageDialog::Mode::Project);
    dialog.setProjectsRoot(root);
    dialog.show();
    if (!waitUntil([&dialog] { return !dialog.loading(); }, kPackageTimeoutMs))
      return notImported(QStringLiteral("open-package: reading the file timed out"));
    if (!dialog.loadError().isEmpty()) return notImported(QStringLiteral("open-package: ") + dialog.loadError());
    pause(200);   // the dialog lays out what it read
    shootDialog(&dialog, QStringLiteral("import-dialog"));
    dialog.importNow();   // the Import button
    if (!waitUntil([&dialog] { return !dialog.busy(); }, kPackageTimeoutMs))
      return notImported(QStringLiteral("open-package: Import timed out"));
    dir = dialog.importedProject();
    if (dir.isEmpty()) return notImported(QStringLiteral("open-package: ") + dialog.importError());
  }

  QJsonArray failures;
  auto fail = [&failures](const QString& why) { failures.append(why); };

  auto* window = new ProjectWindow(QFileInfo(dir).canonicalFilePath());
  window->resize(1280, 820);
  window->show();
  pause(300);
  SessionBridge* bridge = window->bridge();
  const bool trustBanner = window->trustBannerShown();
  // Vyrn Link projects: every text the Link bar showed (idle, waiting, the connected game).
  QStringList linkBar;
  auto noteLinkBar = [&] {
    const QString text = window->linkBarText();
    if (!text.isEmpty() && !linkBar.contains(text)) linkBar.append(text);
  };
  noteLinkBar();
  QObject::connect(bridge, &SessionBridge::linkChanged, window, [&] { QTimer::singleShot(0, window, noteLinkBar); });
  // A Vyrn Link project: Settings shows the pairing code in its Vyrn Link card.
  QJsonValue settingsLinkCode = QJsonValue::Null;
  if (!linkBar.isEmpty()) {
    SettingsDialog dialog(window->projectDir(), window);
    dialog.show();
    pause(100);
    shootDialog(&dialog, QStringLiteral("settings-dialog"));
    auto* code = dialog.findChild<QLabel*>(QStringLiteral("linkCode"));
    settingsLinkCode = code ? code->text() : QString();
    dialog.reject();
  }

  // What the runs report, straight from the session.
  struct Seen {
    int checks = 0;
    bool lastCheckOk = false;
    int errors = 0;
    QString lastError;
  } seen;
  QObject::connect(bridge, &SessionBridge::checkFinished, window, [&seen](const QJsonObject& result) {
    ++seen.checks;
    seen.lastCheckOk = result.value(QLatin1String("ok")).toBool();
  });
  QObject::connect(bridge, &SessionBridge::errorReceived, window, [&seen](const QJsonObject& e) {
    ++seen.errors;
    const QString script = e.value(QLatin1String("script")).toString();
    const int line = e.value(QLatin1String("line")).toInt();
    seen.lastError = (script.isEmpty() ? QString() : QStringLiteral("%1:%2: ").arg(script).arg(line)) +
                     e.value(QLatin1String("message")).toString();
  });
  auto stopAndWait = [&](const QString& step) {
    if (!bridge->running() && !window->startingGame()) return;
    window->stop();
    if (!waitUntil([&] { return !bridge->running() && !window->startingGame(); }, kStopTimeoutMs))
      fail(step + QStringLiteral(": Stop timed out"));
  };
  // A Check through the window (as the toolbar does it); true when it passed.
  auto runCheck = [&](const QString& step) {
    const int checksBefore = seen.checks, errorsBefore = seen.errors;
    window->check();
    waitUntil([&] { return !window->startingGame(); }, kGameTimeoutMs);   // a Start program opens first
    if (!bridge->running()) {
      fail(step + QStringLiteral(": Check didn't start"));
      return false;
    }
    if (!waitUntil([&] { return !bridge->running(); }, kCheckTimeoutMs)) {
      fail(step + QStringLiteral(": Check timed out"));
      stopAndWait(step);
      return false;
    }
    return seen.checks > checksBefore && seen.lastCheckOk && seen.errors == errorsBefore;
  };

  QJsonValue checkOk = QJsonValue::Null;
  QJsonValue trustDialogShown = QJsonValue::Null, trusted = QJsonValue::Null;
  if (doTrust) {
    // Check on a project nobody trusted yet opens the trust dialog instead of running; "Trust and run" trusts it
    // and the Check goes on.
    const int checksBefore = seen.checks, errorsBefore = seen.errors;
    window->check();
    TrustDialog* dialog = nullptr;
    const bool asked = waitUntil(
        [&] {
          dialog = window->findChild<TrustDialog*>();
          return dialog && dialog->isVisible();
        },
        kDialogTimeoutMs);
    trustDialogShown = asked && !bridge->running();
    if (!asked) {
      fail(QStringLiteral("trust: Check didn't ask to trust the project first"));
    } else {
      pause(100);
      shootDialog(dialog, QStringLiteral("trust-dialog"));
      dialog->trustNow();
      if (!waitUntil([&] { return bridge->running(); }, kDialogTimeoutMs)) {
        fail(QStringLiteral("trust: the Check didn't start after Trust and run"));
      } else if (!waitUntil([&] { return !bridge->running(); }, kCheckTimeoutMs)) {
        fail(QStringLiteral("trust: Check timed out"));
        stopAndWait(QStringLiteral("trust"));
      }
      const bool ok = seen.checks > checksBefore && seen.lastCheckOk && seen.errors == errorsBefore;
      checkOk = ok;
      if (!ok) fail(QStringLiteral("trust: the Check after Trust and run didn't pass") +
                    (seen.lastError.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(seen.lastError)));
    }
    bool nowTrusted = false;
    try {
      nowTrusted = vyrn::is_trusted(window->projectDir().toStdString());
    } catch (const std::exception&) {
    }
    trusted = nowTrusted && !window->trustBannerShown();
    if (!trusted.toBool()) fail(QStringLiteral("trust: the project still looks untrusted"));
  }

  // Settings → Start program: what the window started, and whether it still ran after the Check / after Stop.
  const StartProgram* game = window->startProgram();
  const QString startCommand =
      store::settings(window->projectDir()).value(QLatin1String("start_program")).toString().trimmed();
  QJsonValue gameAfterCheck = QJsonValue::Null, gameAfterStop = QJsonValue::Null;
  int panicKilled = 0;
  QJsonValue panicStopped = QJsonValue::Null;

  if (doCheck && !doTrust) {   // with --trust, the Check after "Trust and run" was this step
    const bool ok = runCheck(QStringLiteral("check"));
    checkOk = ok;
    if (!ok) fail(QStringLiteral("check: the Check didn't pass") +
                  (seen.lastError.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(seen.lastError)));
    gameAfterCheck = game->running();
  }

  if (trainSeconds > 0) {
    const int checksBefore = seen.checks, errorsBefore = seen.errors;
    window->train();   // saves, checks, then trains when the Check passes
    bool gaveUp = false;
    QElapsedTimer idle;
    const bool decided = waitUntil(
        [&] {
          if (bridge->running() && bridge->action() == Action::Train) return true;
          if (seen.errors > errorsBefore || (seen.checks > checksBefore && !seen.lastCheckOk)) {
            gaveUp = true;
            return true;
          }
          if (bridge->running() || window->startingGame()) {
            idle.invalidate();
          } else if (!idle.isValid()) {
            idle.start();
          } else if (idle.elapsed() > kIdleGiveUpMs) {
            gaveUp = true;
            return true;
          }
          return false;
        },
        kStartTimeoutMs);
    if (!decided || gaveUp) {
      fail(QStringLiteral("train: training didn't start") +
           (seen.lastError.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(seen.lastError)));
      stopAndWait(QStringLiteral("train"));
    } else {
      // Train for N seconds (it stops early only if something went wrong), then Stop like the button does.
      waitUntil(
          [&] {
            noteLinkBar();
            return !bridge->running();
          },
          trainSeconds * 1000);
      if (!bridge->running()) fail(QStringLiteral("train: training ended by itself before %1 s").arg(trainSeconds));
      if (doPanic) {
        // The panic key (the app's hotkey on macOS and Windows) instead of Stop: the run ends at once.
        panicKilled = ProjectWindow::panicStopAll();
        panicStopped = waitUntil([&] { return !bridge->running(); }, kStopTimeoutMs);
        if (!panicStopped.toBool()) fail(QStringLiteral("panic: the run didn't stop"));
      } else {
        stopAndWait(QStringLiteral("train"));
      }
      if (window->roundsCharted() == 0) fail(QStringLiteral("train: no rounds were charted"));
    }
    if (!startCommand.isEmpty()) {   // Stop ends the Start program Vyrn started (terminate, kill after 3 s)
      if (doPanic)
        pause(1'000);   // the panic key leaves it running
      else
        waitUntil([&] { return !game->running(); }, kGameTimeoutMs);
      gameAfterStop = game->running();
    }
  }

  // Brain ▾ → Export this brain (.vrl)…: the dialog's Save, without the file dialog.
  QJsonValue exported = QJsonValue::Null;
  if (!exportBrainTo.isEmpty()) {
    ShareDialog dialog(ShareDialog::Kind::Brain, window->projectDir(), window->seesPicture(), window);
    dialog.show();
    pause(100);
    shootDialog(&dialog, QStringLiteral("export-brain-dialog"));
    dialog.saveTo(exportBrainTo, false);
    if (!waitUntil([&dialog] { return !dialog.saving(); }, kPackageTimeoutMs))
      fail(QStringLiteral("export-brain: saving timed out"));
    exported = !dialog.savedFile().isEmpty();
    if (!exported.toBool()) fail(QStringLiteral("export-brain: ") + dialog.errorText());
    pause(100);
    shootDialog(&dialog, QStringLiteral("export-brain-saved"));
    dialog.hide();
  }

  // Brain ▾ → Import a brain (.vrl)…: a trusted project checks first, then the brain must become the active one.
  QJsonValue brainImported = QJsonValue::Null;
  if (!importBrainFrom.isEmpty()) {
    auto activeBrain = [&] {
      for (const BrainCard& b : store::brains(window->projectDir()))
        if (b.active) return b.id;
      return QString();
    };
    {
      // What the launcher shows for a .vrl: it must read the file and offer at least one project to add it to.
      OpenPackageDialog choose(importBrainFrom, OpenPackageDialog::Mode::BrainChooseProject);
      choose.show();
      if (!waitUntil([&choose] { return !choose.loading(); }, kPackageTimeoutMs) || !choose.loadError().isEmpty())
        fail(QStringLiteral("import-brain: the launcher's dialog can't read the file: ") + choose.loadError());
      pause(100);
      shootDialog(&choose, QStringLiteral("brain-choose-project-dialog"));
      auto* targets = choose.findChild<QListWidget*>(QStringLiteral("brainTargets"));
      if (!targets || !targets->currentItem()) fail(QStringLiteral("import-brain: the launcher's dialog offers no project"));
      choose.hide();
    }
    const QString activeBefore = activeBrain();
    const qsizetype countBefore = store::brains(window->projectDir()).size();
    window->importBrain(importBrainFrom, false);
    if (!waitUntil([&] { return !window->addingBrain(); }, kCheckTimeoutMs + kPackageTimeoutMs))
      fail(QStringLiteral("import-brain: adding the brain timed out"));
    const QString activeAfter = activeBrain();
    const bool ok = store::brains(window->projectDir()).size() == countBefore + 1 && !activeAfter.isEmpty() &&
                    activeAfter != activeBefore;
    brainImported = ok;
    if (!ok) {
      QString why = seen.lastError;
      for (QMessageBox* box : window->findChildren<QMessageBox*>(QStringLiteral("brainImportError"))) {
        why = box->informativeText();
        box->close();
      }
      fail(QStringLiteral("import-brain: the brain wasn't added") +
           (why.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(why)));
    }
  }

  // Compile (the toolbar button): a project nobody trusted shows the trust dialog first (closed here without
  // trusting); then the dialog's Compile, without the question about an existing folder.
  QJsonObject compiled;
  if (!compileTo.isEmpty()) {
    auto* act = window->findChild<QAction*>(QStringLiteral("compileAction"));
    compiled.insert(QStringLiteral("button_enabled"), act && act->isEnabled());
    if (window->trustBannerShown()) {
      window->compileProject();   // returns at once: the trust dialog is window-modal
      TrustDialog* trust = nullptr;
      const bool asked = waitUntil(
          [&] {
            trust = window->findChild<TrustDialog*>();
            return trust && trust->isVisible();
          },
          kDialogTimeoutMs);
      compiled.insert(QStringLiteral("trust_dialog"), asked);
      if (!asked) fail(QStringLiteral("compile: an untrusted project didn't ask to trust it first"));
      if (trust) {
        pause(100);
        shootDialog(trust, QStringLiteral("compile-trust-dialog"));
        trust->reject();
        pause(100);
      }
    }
    CompileDialog dialog(window->projectDir(), window);
    dialog.show();
    pause(100);
    shootDialog(&dialog, QStringLiteral("compile-dialog"));
    compiled.insert(QStringLiteral("default_folder"), dialog.folder());
    dialog.compileTo(compileTo, false);
    if (!waitUntil([&dialog] { return !dialog.compiling(); }, kPackageTimeoutMs))
      fail(QStringLiteral("compile: compiling timed out"));
    compiled.insert(QStringLiteral("compiled"), !dialog.compiledFolder().isEmpty());
    compiled.insert(QStringLiteral("folder"), dialog.compiledFolder());
    compiled.insert(QStringLiteral("files"), QJsonArray::fromStringList(dialog.compiledFiles()));
    compiled.insert(QStringLiteral("message"), dialog.doneMessage());
    compiled.insert(QStringLiteral("error"), dialog.errorText());
    pause(100);
    shootDialog(&dialog, dialog.compiledFolder().isEmpty() ? QStringLiteral("compile-error")
                                                           : QStringLiteral("compile-done"));
    dialog.hide();
  }

  QJsonValue errorMarker = QJsonValue::Null;
  QJsonObject injected;
  if (injectError) {
    Editor* points = window->editorFor(QStringLiteral("points.py"));
    if (!points) {
      fail(QStringLiteral("inject-error: points.py isn't open"));
    } else {
      // Typed at the end of points.py like a person would; check() saves it first.
      QString text = points->text();
      if (!text.isEmpty() && !text.endsWith(QLatin1Char('\n'))) {
        points->append(QStringLiteral("\n"));
        text += QLatin1Char('\n');
      }
      const int line = int(text.count(QLatin1Char('\n'))) + 1;
      points->append(QStringLiteral("total = scor + 1   # smoke test: a NameError on purpose\n"));
      injected = QJsonObject{{QStringLiteral("file"), QStringLiteral("points.py")}, {QStringLiteral("line"), line}};
      if (runCheck(QStringLiteral("inject-error"))) fail(QStringLiteral("inject-error: the Check passed with a NameError"));
      int marked = 0;
      for (int l = 1; l <= points->lines() && !marked; ++l)
        if (window->hasProblemMarker(QStringLiteral("points.py"), l)) marked = l;
      if (marked > 0)
        errorMarker = QJsonObject{{QStringLiteral("file"), QStringLiteral("points.py")}, {QStringLiteral("line"), marked}};
      if (!window->hasProblemMarker(QStringLiteral("points.py"), line))
        fail(QStringLiteral("inject-error: no error mark on points.py line %1").arg(line) +
             (seen.lastError.isEmpty() ? QString() : QStringLiteral(" (the error was %1)").arg(seen.lastError)));
    }
  }

  if (!screenshot.isEmpty()) {
    if (window->roundsCharted() > 0) window->showBottomTab(QStringLiteral("points"));
    pause(400);   // let the chart's throttled repaint and the layout settle
    const QPixmap shot = window->grab();
    if (shot.isNull() || !shot.save(screenshot, "PNG"))
      fail(QStringLiteral("screenshot: can't save %1").arg(screenshot));
  }

  stopAndWait(QStringLiteral("close"));
  QJsonObject result;
  result.insert(QStringLiteral("ok"), failures.isEmpty());
  result.insert(QStringLiteral("tabs"), QJsonArray::fromStringList(window->tabFiles()));
  if (auto* bottom = window->findChild<QTabWidget*>(QStringLiteral("bottomTabs"))) {
    QJsonArray names;
    for (int i = 0; i < bottom->count(); ++i) names.append(bottom->tabText(i).section(QStringLiteral(" ("), 0, 0));
    result.insert(QStringLiteral("bottom_tabs"), names);
  }
  result.insert(QStringLiteral("rounds"), window->roundsCharted());
  result.insert(QStringLiteral("error_marker"), errorMarker);
  result.insert(QStringLiteral("check_ok"), checkOk);
  if (!package.isEmpty()) {
    result.insert(QStringLiteral("imported"), true);
    result.insert(QStringLiteral("project"), window->projectDir());
    result.insert(QStringLiteral("trust_banner"), trustBanner);
  }
  if (doTrust) {
    result.insert(QStringLiteral("trust_dialog"), trustDialogShown);
    result.insert(QStringLiteral("trusted"), trusted);
  }
  if (!exportBrainTo.isEmpty()) result.insert(QStringLiteral("exported"), exported);
  if (!importBrainFrom.isEmpty()) result.insert(QStringLiteral("brain_imported"), brainImported);
  if (!compileTo.isEmpty()) result.insert(QStringLiteral("compile"), compiled);
  if (injectError) result.insert(QStringLiteral("injected"), injected);
  if (doPanic) {
    bool said = false;   // the console's "Panic stop" line
    if (auto* console = window->findChild<ConsoleView*>())
      said = console->toPlainText().contains(QLatin1String("Panic stop"));
    result.insert(QStringLiteral("panic"), QJsonObject{{QStringLiteral("stopped"), panicStopped},
                                                       {QStringLiteral("killed"), panicKilled},
                                                       {QStringLiteral("said"), said}});
  }
  if (!screenshot.isEmpty()) result.insert(QStringLiteral("screenshot"), screenshot);
  noteLinkBar();
  if (!linkBar.isEmpty()) result.insert(QStringLiteral("link_bar"), QJsonArray::fromStringList(linkBar));
  if (!settingsLinkCode.isNull()) result.insert(QStringLiteral("settings_link_code"), settingsLinkCode);
  if (!startCommand.isEmpty()) {
    QJsonArray lines;   // what the console said about it
    if (auto* console = window->findChild<ConsoleView*>())
      for (const QString& line : console->toPlainText().split(QLatin1Char('\n')))
        if (line.contains(startCommand)) lines.append(line);
    result.insert(QStringLiteral("start_program"),
                  QJsonObject{{QStringLiteral("starts"), game->starts()},
                              {QStringLiteral("running_after_check"), gameAfterCheck},
                              {QStringLiteral("running_after_stop"), gameAfterStop},
                              {QStringLiteral("console"), lines}});
  }
  if (!failures.isEmpty()) result.insert(QStringLiteral("failures"), failures);

  delete window;   // a session still running would be stopped and saved first
  printJson(result);
  return failures.isEmpty() ? 0 : 1;
}
