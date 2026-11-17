// "Trust this project?" (docs/dev/package.md section 7): shown for an imported project whose Python code hasn't run
// on this computer yet, from the trust banner or when Check, Train or Watch is pressed. Lists the scripts
// (clicking one opens it and closes this dialog), the author's Start program hint, and for real games that the AI
// presses keys. The main button calls vyrn::trust_project().
#pragma once

#include <QDialog>
#include <QString>

class QLabel;

class TrustDialog : public QDialog {
  Q_OBJECT

 public:
  // andRun: the button says "Trust and run" (the Check, Train or Watch that was pressed continues afterwards);
  // otherwise "Trust this project".
  TrustDialog(const QString& projectDir, bool andRun, QWidget* parent = nullptr);

 public Q_SLOTS:
  void trustNow();   // the main button: trusts the project and accepts; an error stays in the dialog

 Q_SIGNALS:
  void scriptClicked(const QString& file);   // relative to the project folder; the dialog closes (rejected)

 private:
  QString dir_;
  QLabel* error_ = nullptr;
};
