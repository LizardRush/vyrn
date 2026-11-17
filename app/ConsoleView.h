// The console: what the scripts print, plus app messages. "name.py:12" in any line is clickable.
#pragma once

#include <QPlainTextEdit>
#include <QString>

class ConsoleView : public QPlainTextEdit {
  Q_OBJECT

 public:
  explicit ConsoleView(QWidget* parent = nullptr);   // read-only, monospace, keeps the last 5000 lines
  void appendLine(const QString& stream, const QString& text);   // stream: "stdout" | "stderr" | "app"
  void clearAll();

 Q_SIGNALS:
  void openLocation(const QString& file, int line);

 protected:
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void contextMenuEvent(QContextMenuEvent* event) override;   // the usual menu + "Clear the console"

 private:
  bool hasLines_ = false;
};
