// Errors and warnings from Check/Train; double-click jumps to the line.
#pragma once

#include <QListWidget>
#include <QString>

class ProblemsView : public QListWidget {
  Q_OBJECT

 public:
  explicit ProblemsView(QWidget* parent = nullptr);
  // kind: "error" | "warning". file is relative to the project ("" when unknown); line/col are 1-based (0 = none).
  // The same problem twice is listed once; errors are listed before warnings.
  void addProblem(const QString& kind, const QString& file, int line, int col, const QString& message,
                  const QString& hint);
  void clearProblems();
  int errorCount() const;
  int warningCount() const;

 Q_SIGNALS:
  void openLocation(const QString& file, int line, int col);

 protected:
  void paintEvent(QPaintEvent* event) override;   // a friendly line when the list is empty
};
