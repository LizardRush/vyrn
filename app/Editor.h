// A Python editor for one script: dark theme, line numbers, vyrn autocompletion and call tips, error marks.
#pragma once

#include <QString>

#include "Qsci.h"

class QMimeData;
class QsciAPIs;
class QResizeEvent;

class Editor : public QsciScintilla {
  Q_OBJECT

 public:
  explicit Editor(QWidget* parent = nullptr);
  bool load(const QString& path, QString* error = nullptr);   // the path is kept even when the file is missing
  bool save(QString* error = nullptr);          // writes the file; clears the modified flag
  bool reloadIfUnmodified();                    // after an outside change; false when the user has edits
  QString path() const;
  QString fileName() const;                     // e.g. "points.py"
  // line and col are 1-based (as in Python tracebacks); col 0 = whole line.
  void markProblem(int line, int col, const QString& message, bool isError);
  void clearProblems();
  bool hasProblemAt(int line) const;
  void goToLine(int line, int col = 1);
  void showFindBar();

 Q_SIGNALS:
  void helpRequested(const QString& vyrnName);  // F1 on "vyrn.<name>": the name without "vyrn." ("" elsewhere)
  void saved();

 protected:
  void keyPressEvent(QKeyEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;   // keeps the find bar in the top-right corner
  // A dropped .vrl/.vrlp file isn't text to insert: the project window imports it instead.
  bool canInsertFromMimeData(const QMimeData* source) const override;

 private:
  class FindBar;

  QString vyrnNameAtCursor() const;
  void clearProblemLine(int line0);             // 0-based
  bool findText(const QString& text, bool forward, bool fromSelectionStart);
  void defineProblemStyles();

  QString path_;
  FindBar* findBar_ = nullptr;
};
