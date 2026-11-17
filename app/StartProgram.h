// Settings → Start program (vyrn.json "start_program", docs/dev/app-internals.md section 4): the program, usually
// the game, that a project window starts when Check, Train or Watch begins, unless the copy it started earlier is
// still running. Stop and closing the window end that copy. A program the person started themselves is never
// touched: this only ever ends what it started.
//
// Only the app does this, and only for a project the person trusts. vyrn-cli and the MCP server never start
// programs from vyrn.json: AI agents must never be able to launch a program this way.
#pragma once

#include <QObject>
#include <QString>

class QProcess;

class StartProgram : public QObject {
  Q_OBJECT

 public:
  explicit StartProgram(QObject* parent = nullptr);
  ~StartProgram() override;   // ends the copy it started: terminate, wait up to 3 s, then kill

  // Starts `command` in `folder`: split with QProcess::splitCommand, run without a shell (Linux, macOS: in a process
  // group of its own). A copy of another command it started earlier is ended first. False with a plain message in
  // *error when it can't start (no program, not found, not allowed to run).
  bool start(const QString& command, const QString& folder, QString* error);
  bool running() const;        // the copy it started is still running
  QString command() const;     // what that copy runs ("" when none runs)
  qint64 pid() const;          // its process id (0 when none runs)
  int starts() const { return starts_; }   // programs started so far (the smoke test counts them)
  // Ends the copy it started, if it still runs: terminate now, kill after about 3 s. Returns at once.
  void stop();

 Q_SIGNALS:
  void ended(const QString& message);   // the copy it started ended by itself (closed, failed): plain words

 private:
  QProcess* process_ = nullptr;
  QString command_;
  int starts_ = 0;
};
