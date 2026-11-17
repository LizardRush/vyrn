// "Compile" (docs/dev/standalone.md): turns the trained AI into a folder with play.py that plays with plain Python and
// the vyrn package, without Vyrn. The form: which brain (Best/Latest), the output folder (default ~/<Name>-play; the
// button picks the folder it goes in) and "Include the vyrn package (.whl)". vyrn::export_standalone runs on a
// background thread; then "Compiled to … Run it with: python3 play.py" with Show in folder.
#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class CompileDialog : public QDialog {
  Q_OBJECT

 public:
  explicit CompileDialog(const QString& projectDir, QWidget* parent = nullptr);
  ~CompileDialog() override;

  QString folder() const;          // the output folder the form shows now
  // Compiles into `folder` without any question (the Compile button calls it after asking about an existing
  // folder; the smoke test calls it directly). overwrite: replace a folder an earlier Compile made.
  void compileTo(const QString& folder, bool overwrite);
  bool compiling() const;
  QString compiledFolder() const;  // after a successful compile; "" otherwise
  QStringList compiledFiles() const;   // what it wrote, relative to compiledFolder()
  QString errorText() const;       // why the last compile failed; "" otherwise
  QString doneMessage() const;     // "Compiled to ~/Catch-numbers-play. Run it with: python3 play.py …"

 private:
  struct Ui;
  Ui* ui_ = nullptr;
};
