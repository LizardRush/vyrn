// Environment checks with copyable fixes: Python + numpy + Pillow; and for real games, per system
// (docs/dev/app-internals.md section 7): Linux screen capture (grim), keyboard access (/dev/uinput + evdev), the focus
// helper and the panic keys; macOS the Screen Recording and Accessibility permissions; Windows nothing more.
#pragma once

#include <QByteArray>
#include <QDialog>
#include <QImage>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <array>
#include <functional>

class QShowEvent;

// Runs the checks in the background (QProcess, never blocking the UI) and reports each result as it arrives.
// SetupDialog shows them; the launcher uses a quick run (no test screenshot) for its "Setup n/m" button.
class SetupChecker : public QObject {
  Q_OBJECT

 public:
  // macOS uses Screen for Screen Recording and Keyboard for Accessibility; checks that don't exist on this system
  // (appliesHere() false) stay NotNeeded, and SetupDialog shows no row for them.
  enum Check { Python = 0, Screen, Keyboard, FocusHelper, PanicKeys, CheckCount };
  enum class Status { NotNeeded, Waiting, Running, Ok, Failed };
  struct Button {
    QString label;
    std::function<void()> action;
  };
  struct Result {
    Status status = Status::NotNeeded;
    QString summary;   // one plain sentence: what Vyrn found
    QString fix;       // a command to copy ("" when there is nothing to run)
    QString fixNote;   // where/how to run it
    QImage picture;    // Screen: the test grab
    QList<Button> buttons;   // what the person can click to fix it (macOS: open System Settings)
  };

  explicit SetupChecker(QObject* parent = nullptr);
  ~SetupChecker() override;

  // Starts (or restarts) every check. realGame: also this system's real-game checks. grabScreen (Linux): the screen
  // check takes a real 64x64 test picture (else it only looks for grim).
  void start(bool realGame, bool grabScreen);
  void cancel();
  bool running() const;
  bool realGameChecked() const { return realGame_; }
  const Result& result(Check check) const;
  int passed() const;   // needed checks that passed
  int total() const;    // checks that are needed

  static QString title(Check check);   // "Python with numpy and Pillow", ...
  static bool appliesHere(Check check);   // the check exists on this system

 Q_SIGNALS:
  void changed(int check);              // a check started or finished
  void finished(int passed, int total);

 private:
  struct ProcessResult {
    bool started = false;    // false: the program could not be run (missing, not executable)
    bool timedOut = false;   // it was stopped because it took too long
    int exitCode = -1;       // -1 when it crashed or was stopped
    QByteArray out, err;
  };
  using Done = std::function<void(const ProcessResult&)>;
  void run(const QString& program, const QStringList& args, int timeoutMs, Done done);
  void set(Check check, Status status, const QString& summary, const QString& fix = QString(),
           const QString& fixNote = QString(), const QImage& picture = QImage(),
           const QList<Button>& buttons = QList<Button>());
  void checkPython();
  void checkScreen(bool grab);   // Linux
  void checkKeyboard();          // Linux
  void checkEvdev();             // Linux
  void checkFocusHelper();       // Linux
  void checkPanicKeys();         // Linux
  void checkScreenRecording();   // macOS
  void checkAccessibility();     // macOS
  void maybeFinished();

  std::array<Result, CheckCount> results_;
  int generation_ = 0;   // results from an older start() are ignored
  bool realGame_ = false;
  bool finishedSent_ = true;
};

class SetupDialog : public QDialog {
  Q_OBJECT

 public:
  explicit SetupDialog(QWidget* parent = nullptr, bool includeRealGameChecks = true);
  ~SetupDialog() override;
  void runChecks();   // asynchronous; rows update as each check finishes
  bool allPassed() const;   // after the checks finished: every needed check passed

 Q_SIGNALS:
  void checksFinished(int passed, int total);

 protected:
  void showEvent(QShowEvent* event) override;   // starts the checks the first time, if runChecks() wasn't called

 private:
  void updateRow(int check);
  void updateSummary();

  struct Ui;
  Ui* ui_ = nullptr;
};
