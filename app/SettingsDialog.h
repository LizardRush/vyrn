// Project settings (vyrn.json): decisions per second, round length, start program, Python, AI-agent screen
// access. Unknown keys are kept.
#pragma once

#include <QDialog>
#include <QString>

class SettingsDialog : public QDialog {
  Q_OBJECT

 public:
  explicit SettingsDialog(const QString& projectDir, QWidget* parent = nullptr);
  ~SettingsDialog() override;

 public Q_SLOTS:
  void accept() override;   // writes only the values that changed; stays open (with the reason) if it can't

 private:
  void testPython();

  struct Ui;
  Ui* ui_ = nullptr;
  QString projectDir_;
};
