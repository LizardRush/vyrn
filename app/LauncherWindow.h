// The home screen: project cards, New project, Open folder, Open shared file, Setup, Connect an AI coding agent.
#pragma once

#include <QMainWindow>
#include <QString>

class ProjectWindow;

class LauncherWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit LauncherWindow(QWidget* parent = nullptr);
  ~LauncherWindow() override;                // closes (and saves) project windows that are still open
  void refresh();                            // re-read the projects folder
  ProjectWindow* openProject(const QString& path);   // opens (or raises) its ProjectWindow; nullptr: not a project
  // A .vrlp (import dialog, then the new project opens) or a .vrl (pick a project or a new one from the brain's
  // template, then that project window adds it). From Open shared file…, a drop or `vyrn <file>`.
  void openSharedFile(const QString& file);

 protected:
  void changeEvent(QEvent* event) override;  // coming back to the launcher refreshes the cards
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void dragEnterEvent(QDragEnterEvent* event) override;   // .vrl / .vrlp files
  void dragMoveEvent(QDragMoveEvent* event) override;
  void dropEvent(QDropEvent* event) override;

 private:
  void fitHeader();                          // shorter header buttons when the window is narrow
  void rebuildCards();
  void updateCardsInPlace();
  void newProject(const QString& preselectTemplate = QString());
  void createCatchPictures();
  void openFolder();
  void chooseSharedFile();                   // "Open shared file…"
  void shareProject(const QString& path);    // card menu: Share project (.vrlp)…
  void showSetup();
  void checkForUpdates();                    // "Check for updates": always shows the answer
  void showUpdateMessage(const QString& text);
  void startUpdate();                        // asks, opens the terminal, quits
  void showAgents();
  void showProjectMenu(const QString& path, const QPoint& globalPos);
  void renameProject(const QString& path);
  void duplicateProject(const QString& path);
  void deleteProject(const QString& path);
  void showFolder(const QString& path);
  void runQuickSetupCheck();
  void updateSetupButton();

  struct Ui;
  Ui* ui_ = nullptr;
};
