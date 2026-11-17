// Self-update check: asks GitHub (through `curl`, no QtNetwork) for the newest commit of Vyrn and compares it with the
// commit this copy was built from (<data dir>/commit, written by get-linux.sh / get-mac.sh / get-windows.ps1).
// Updating means running the same one-liner as the README in a terminal window (app_platform::openUpdateTerminal).
// Tests: VYRN_UPDATE_URL replaces the API address (curl takes file:// too), VYRN_UPDATE_NO_AUTO=1 never checks on
// its own, and setTerminalHook() replaces the terminal so no window ever opens.
#pragma once

#include <QObject>
#include <QProcess>
#include <QString>

#include <functional>

class QTimer;

class Updater : public QObject {
  Q_OBJECT

 public:
  explicit Updater(QObject* parent = nullptr);

  // Starts a check (ignored while one runs). manual: always answers with one of the signals below; automatic: only
  // when "updates/auto" is on, the last check is over 24 hours old and VYRN_UPDATE_NO_AUTO isn't set, and then only
  // updateAvailable() is ever emitted.
  void check(bool manual);

  static QString installedCommit();        // 40 hex characters, "" when this copy has no stamp (a dev build)
  static bool autoEnabled();               // QSettings "updates/auto", default true
  static void setAutoEnabled(bool on);
  static bool validCommit(const QString& text);
  static void setStampDir(const QString& dir);   // tests: read the stamp from here instead of paths::dataDir()
  static QString updateUrl();              // VYRN_UPDATE_URL or the GitHub API address
  static QString oneLiner();               // the install command of this system (platform file)

  // Runs the update one-liner in a terminal window. The hook (tests) replaces app_platform::openUpdateTerminal.
  static bool startUpdate();
  static void setTerminalHook(std::function<bool(const QString&)> hook);

 Q_SIGNALS:
  void updateAvailable(const QString& latest);
  void upToDate();
  void failed(const QString& reason);      // a plain sentence
  void unknown();                          // this copy has no stamp: it can't tell

 private:
  void finish(bool ok, const QString& why);

  QProcess* process_ = nullptr;
  QTimer* timeout_ = nullptr;
  bool manual_ = false;
  QString installed_;
};
