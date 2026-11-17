// The IDE and dashboard for one project: editor tabs, toolbar (Check/Train/Watch/Stop/Pick/Snapshot/Brain/Compile/
// Share/Settings), trust banner, "What the AI sees", choice bars, console, points chart, problems, docs, status bar.
#pragma once

#include <QImage>
#include <QMainWindow>
#include <QString>
#include <QStringList>

class SessionBridge;
class Editor;
class StartProgram;

class ProjectWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit ProjectWindow(const QString& projectDir, QWidget* parent = nullptr);
  ~ProjectWindow() override;
  QString projectDir() const;

  // Used by the toolbar, menus and the smoke test.
  void check();
  void train();
  void watch();
  void stop();
  bool saveAll();
  Editor* editorFor(const QString& fileName) const;   // "points.py" etc.; nullptr when not open
  QStringList tabFiles() const;                        // file names of the open tabs, in order
  int roundsCharted() const;
  QString linkBarText() const;                         // the Vyrn Link bar's text ("" when it is hidden)
  bool hasProblemMarker(const QString& fileName, int line) const;
  SessionBridge* bridge() const;
  // Shows a bottom tab: "console" | "points" | "problems" | "docs".
  void showBottomTab(const QString& name);
  // The Docs tab at the vyrn library entry `vyrnName` (e.g. "screen"); "" opens the docs at the top.
  void showHelp(const QString& vyrnName);

  // Sharing (docs/dev/package.md): Share ▾ and Brain ▾ menus, and .vrl/.vrlp files dropped on the window.
  void shareProject();                                  // the .vrlp export dialog
  void exportBrain();                                   // the .vrl export dialog
  // Compile (docs/dev/standalone.md): the trained AI as a Python program that runs without Vyrn. An untrusted
  // project shows the trust dialog first; then the editors are saved and CompileDialog opens.
  void compileProject();
  // Adds a .vrl brain. A trusted project runs a Check first, so the brain is compared with what the scripts
  // see and choose. confirm: first show what the file holds (Import a brain…, a drop).
  void importBrain(const QString& file, bool confirm);
  bool addingBrain() const;                             // importBrain is still checking or writing
  QImage seesPicture() const;                           // "what the AI sees" now; null when it isn't a picture
  bool trustBannerShown() const;                        // the "This project came from …" banner
  // Settings → Start program: Check, Train or Watch started it and waits a moment for it to open; what Vyrn started.
  bool startingGame() const;
  const StartProgram* startProgram() const;
  void showNotes(const QString& title, const QStringList& notes);   // console lines + a message when any

  // The panic key on macOS and Windows (the app's global hotkey; on Linux labwc runs vyrn-panic): every project
  // window stops its run at once, the Python workers this app started are killed with what they started, every key
  // goes up, and the windows that were running say "Panic stop". A game Vyrn started keeps running. Returns how many
  // workers it killed.
  static int panicStopAll();
  // The qApp property main() sets to a plain sentence when the system refused the panic hotkey.
  static constexpr const char* kPanicKeyProblem = "vyrnPanicKeyProblem";

 Q_SIGNALS:
  // A .vrlp dropped on this window was imported. Nothing connected: this window opens the new project itself.
  void openProjectRequested(const QString& dir, const QStringList& notes);

 protected:
  void closeEvent(QCloseEvent* event) override;        // stops (and saves) a running session first
  void changeEvent(QEvent* event) override;            // real games: clicking Vyrn holds the AI (keys released)
  void dragEnterEvent(QDragEnterEvent* event) override;   // .vrl / .vrlp files
  void dragMoveEvent(QDragMoveEvent* event) override;
  void dropEvent(QDropEvent* event) override;

 private:
  struct Ui;
  Ui* ui_ = nullptr;
};
