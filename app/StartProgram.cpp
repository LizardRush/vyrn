// StartProgram: Settings → Start program for one project window (see StartProgram.h).
#include "StartProgram.h"

#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>

#include <utility>

#include "AppPlatform.h"
#include "vyrn/platform.h"

namespace {

constexpr int kStartedMs = 5'000;     // exec() normally answers at once; this only guards a stuck system
constexpr int kTerminateMs = 3'000;   // after terminate: how long the program may take to close before kill

// Ends the program together with what it started (a launcher script and the game it runs end together):
// vyrn::platform::stop_process_tree() sends SIGTERM (force: SIGKILL) to its process group on Linux and macOS, and on
// Windows (force only) ends the whole tree. Otherwise QProcess ends the process alone: terminate() (Windows: closes
// its windows) or kill().
void endProgram(QProcess* p, bool force) {
  const qint64 pid = p->processId();
  if (pid > 0 && vyrn::platform::stop_process_tree(pid, force)) return;
  if (force)
    p->kill();
  else
    p->terminate();
}

QString endedMessage(const QString& command, int exitCode, QProcess::ExitStatus status) {
  if (status == QProcess::CrashExit) return QStringLiteral("“%1” stopped unexpectedly.").arg(command);
  if (exitCode == 0) return QStringLiteral("“%1” has closed.").arg(command);
  return QStringLiteral("“%1” ended with an error (exit code %2). Try the same command in a terminal in the project "
                        "folder to see why.")
      .arg(command)
      .arg(exitCode);
}

}  // namespace

StartProgram::StartProgram(QObject* parent) : QObject(parent) {}

StartProgram::~StartProgram() {
  // The window is going away: end every copy still running (the current one, and one Stop is still ending).
  const QList<QProcess*> processes = findChildren<QProcess*>(Qt::FindDirectChildrenOnly);
  for (QProcess* p : processes) {
    p->disconnect(this);   // nothing may reach this object while it is being destroyed
    if (p->state() != QProcess::NotRunning) endProgram(p, false);
  }
  for (QProcess* p : processes) {
    if (p->state() == QProcess::NotRunning || p->waitForFinished(kTerminateMs)) continue;
    endProgram(p, true);
    p->waitForFinished(1'000);
  }
}

bool StartProgram::start(const QString& command, const QString& folder, QString* error) {
  QStringList args = QProcess::splitCommand(command);
  const QString program = args.isEmpty() ? QString() : args.takeFirst();
  if (program.isEmpty()) {
    *error = QStringLiteral("Vyrn couldn't start “%1”: it doesn't name a program. Check Settings → Start program.")
                 .arg(command);
    return false;
  }
  stop();   // a copy of an earlier command (Settings changed since) ends first

  auto* p = new QProcess(this);
  p->setWorkingDirectory(folder);
  p->setProgram(program);
  p->setArguments(args);
  p->setStandardInputFile(QProcess::nullDevice());
  p->setProcessChannelMode(QProcess::ForwardedChannels);   // its output goes where Vyrn's goes, never piles up here
  app_platform::useOwnProcessGroup(p);                      // Linux, macOS: its own process group (see endProgram)
  p->start();
  if (!p->waitForStarted(kStartedMs)) {
    const bool missing = !program.contains(QLatin1Char('/')) && !program.contains(QDir::separator()) &&
                         QStandardPaths::findExecutable(program).isEmpty();
    const QString why = missing ? QStringLiteral("there's no program called “%1” on this computer").arg(program)
                        : p->error() == QProcess::FailedToStart
                            ? QStringLiteral("“%1” can't be run (%2)").arg(program, p->errorString())
                            : p->errorString();
    *error = QStringLiteral("Vyrn couldn't start “%1”: %2. Check Settings → Start program.").arg(command, why);
    p->kill();
    delete p;
    return false;
  }
  process_ = p;
  command_ = command;
  ++starts_;
  connect(p, &QProcess::finished, this, [this, p](int exitCode, QProcess::ExitStatus status) {
    if (process_ != p) return;
    const QString message = endedMessage(std::exchange(command_, QString()), exitCode, status);
    process_ = nullptr;
    p->deleteLater();
    Q_EMIT ended(message);
  });
  return true;
}

bool StartProgram::running() const { return process_ && process_->state() != QProcess::NotRunning; }

QString StartProgram::command() const { return running() ? command_ : QString(); }

qint64 StartProgram::pid() const { return running() ? process_->processId() : 0; }

void StartProgram::stop() {
  QProcess* p = std::exchange(process_, nullptr);
  command_.clear();
  if (!p) return;
  p->disconnect(this);   // it ends because Vyrn asked: no "has closed" message
  if (p->state() == QProcess::NotRunning) {
    p->deleteLater();
    return;
  }
  connect(p, &QProcess::finished, p, &QObject::deleteLater);
  endProgram(p, false);
  QTimer::singleShot(kTerminateMs, p, [p] {
    if (p->state() != QProcess::NotRunning) endProgram(p, true);
  });
}
