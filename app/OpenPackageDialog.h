// Opening a shared file (docs/dev/package.md sections 1, 6, 7): shows what a .vrlp or .vrl holds, as read by
// vyrn::inspect_package() (name, author, description, preview, mode, the trained AI's stats with a mini points
// chart, files, the Start program hint, and that a project's Python code runs once it is trusted).
//  - Project: Import creates a new, untrusted project (vyrn::import_project).
//  - BrainChooseProject (.vrl from the launcher): pick one of your projects, or a new project from the brain's
//    template; the project window then adds it (ProjectWindow::importBrain).
//  - BrainIntoProject (.vrl in a project window): confirms adding it to that project.
#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

#include "ProjectStore.h"

class OpenPackageDialog : public QDialog {
  Q_OBJECT

 public:
  enum class Mode { Project, BrainChooseProject, BrainIntoProject };

  // intoProject: BrainIntoProject only, the project's name (for the text). Reading the file starts at once.
  OpenPackageDialog(const QString& file, Mode mode, QWidget* parent = nullptr, const QString& intoProject = QString());
  ~OpenPackageDialog() override;

  bool loading() const;                   // still reading the file
  QString loadError() const;              // why it can't be opened; "" when it was read fine (or still loading)
  QString packageName() const;            // the project name it carries
  bool busy() const;                      // Import is running
  QString importError() const;            // why the last Import failed ("" = it didn't)
  void setProjectsRoot(const QString& root);   // where Import creates projects (default: the projects folder)

  // After accept():
  QString importedProject() const;        // Project: the new project's folder
  QStringList notes() const;              // Project: plain-English things to know about the import
  QString chosenProject() const;          // BrainChooseProject: the project to add the brain to; "" = new one
  bool chosenTemplate(TemplateCard* out) const;   // BrainChooseProject: true when "New <template> project" was picked

 public Q_SLOTS:
  void importNow();                       // the Import / Add brain button
  void reject() override;                 // ignored while Import is writing the project

 Q_SIGNALS:
  void loaded();                          // reading the file finished (see loadError())

 private:
  struct Ui;
  Ui* ui_ = nullptr;
};
