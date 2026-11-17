// "Share project (.vrlp)…" and "Export this brain (.vrl)…" (docs/dev/package.md section 1): author (optional,
// remembered, never taken from the system), description, which trained AI goes in, the files and the size
// (projects only), a preview picture; then a save dialog and "Saved Catch.vrlp (412 KB)…" with Show in folder.
#pragma once

#include <QDialog>
#include <QImage>
#include <QString>

class ShareDialog : public QDialog {
  Q_OBJECT

 public:
  enum class Kind { Project, Brain };   // .vrlp | .vrl

  // seen: the "what the AI sees" picture shown now (may be null); it is offered as the package's preview.
  ShareDialog(Kind kind, const QString& projectDir, const QImage& seen, QWidget* parent = nullptr);
  ~ShareDialog() override;

  // Save… without the file dialog (it calls this after asking where; the smoke test calls it directly).
  // overwrite: replace an existing file (only when the person already agreed to that).
  void saveTo(const QString& file, bool overwrite);
  bool saving() const;
  QString savedFile() const;    // after a successful save; "" otherwise
  QString errorText() const;    // why the last save failed; "" otherwise
  QString savedMessage() const; // "Saved Catch.vrlp (412 KB). Anyone with Vyrn can…"

 private:
  struct Ui;
  Ui* ui_ = nullptr;
};
