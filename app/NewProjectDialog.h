// New project: a name and a template (with a one-line description each).
#pragma once

#include <QDialog>
#include <QString>

class NewProjectDialog : public QDialog {
  Q_OBJECT

 public:
  explicit NewProjectDialog(QWidget* parent = nullptr, const QString& preselectTemplate = QString());
  ~NewProjectDialog() override;
  QString createdPath() const;   // set after Create succeeds (the dialog is then accepted)

 private:
  void templateChanged();
  void validate();
  void create();

  struct Ui;
  Ui* ui_ = nullptr;
  QString createdPath_;
};
