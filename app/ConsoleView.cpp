// ConsoleView: a read-only log with coloured streams and clickable script locations.
#include "ConsoleView.h"

#include <QContextMenuEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPalette>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>

#include "Theme.h"

namespace {

constexpr int kMaxLines = 5000;

// "points.py:12", "/home/me/Vyrn/catch/points.py:12" and Python's traceback lines 'File ".../points.py", line 12'.
const QRegularExpression& locationPattern() {
  static const QRegularExpression pattern(QStringLiteral(
      R"re(File "(?<tfile>[^"\n]+?\.py)", line (?<tline>\d+)|(?<file>(?:[\w.\-]*/)*[\w.\-]+\.py):(?<line>\d+))re"));
  return pattern;
}

QColor streamColor(const QString& stream) {
  if (stream == QLatin1String("stderr")) return QColor(0xf5, 0xa9, 0x7f);   // a warm orange: Python's complaints
  if (stream == QLatin1String("app")) return theme::muted();                // Vyrn's own messages, dimmed
  return theme::text();
}

// A link's target is stored in the text as "<line>|<file>".
bool parseHref(const QString& href, QString* file, int* line) {
  const qsizetype bar = href.indexOf(QLatin1Char('|'));
  if (bar <= 0) return false;
  bool ok = false;
  *line = href.left(bar).toInt(&ok);
  *file = href.mid(bar + 1);
  return ok && *line > 0 && !file->isEmpty();
}

}  // namespace

ConsoleView::ConsoleView(QWidget* parent) : QPlainTextEdit(parent) {
  setReadOnly(true);
  setUndoRedoEnabled(false);
  setMaximumBlockCount(kMaxLines);
  setFont(theme::monoFont(10));
  setLineWrapMode(QPlainTextEdit::WidgetWidth);
  setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
  setFrameShape(QFrame::NoFrame);
  setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
  setPlaceholderText(QStringLiteral("What your scripts print shows up here. Click a place like points.py:7 to jump "
                                    "to that line."));
  document()->setDocumentMargin(8);
  viewport()->setMouseTracking(true);
  QPalette p = palette();
  p.setColor(QPalette::Base, theme::base());
  p.setColor(QPalette::Text, theme::text());
  setPalette(p);
}

void ConsoleView::appendLine(const QString& stream, const QString& text) {
  QScrollBar* bar = verticalScrollBar();
  const bool follow = bar->value() >= bar->maximum() - 2;   // only scroll along when the user is at the bottom

  QTextCharFormat plain;
  plain.setForeground(streamColor(stream));
  QTextCharFormat link = plain;
  link.setForeground(theme::accent());
  link.setFontUnderline(true);
  link.setAnchor(true);

  QTextCursor cursor(document());
  cursor.movePosition(QTextCursor::End);
  cursor.beginEditBlock();
  if (hasLines_) cursor.insertBlock();
  hasLines_ = true;
  qsizetype at = 0;
  QRegularExpressionMatchIterator it = locationPattern().globalMatch(text);
  while (it.hasNext()) {
    const QRegularExpressionMatch m = it.next();
    const bool traceback = m.capturedStart(QStringLiteral("tfile")) >= 0;
    const QString file = m.captured(traceback ? QStringLiteral("tfile") : QStringLiteral("file"));
    const QString line = m.captured(traceback ? QStringLiteral("tline") : QStringLiteral("line"));
    if (m.capturedStart() > at) cursor.insertText(text.mid(at, m.capturedStart() - at), plain);
    link.setAnchorHref(line + QLatin1Char('|') + file);
    cursor.insertText(m.captured(0), link);
    at = m.capturedEnd();
  }
  if (at < text.size()) cursor.insertText(text.mid(at), plain);
  cursor.endEditBlock();

  if (follow) bar->setValue(bar->maximum());
}

void ConsoleView::clearAll() {
  clear();
  hasLines_ = false;
}

void ConsoleView::mouseMoveEvent(QMouseEvent* event) {
  QPlainTextEdit::mouseMoveEvent(event);
  const bool onLink = !anchorAt(event->position().toPoint()).isEmpty();
  viewport()->setCursor(onLink ? Qt::PointingHandCursor : Qt::IBeamCursor);
}

void ConsoleView::mouseReleaseEvent(QMouseEvent* event) {
  const QString href = event->button() == Qt::LeftButton ? anchorAt(event->position().toPoint()) : QString();
  const bool selecting = textCursor().hasSelection();   // dragging over a link selects text instead
  QPlainTextEdit::mouseReleaseEvent(event);
  QString file;
  int line = 0;
  if (!href.isEmpty() && !selecting && parseHref(href, &file, &line)) Q_EMIT openLocation(file, line);
}

void ConsoleView::contextMenuEvent(QContextMenuEvent* event) {
  QMenu* menu = createStandardContextMenu(event->pos());
  menu->addSeparator();
  QAction* clearAction = menu->addAction(QStringLiteral("Clear the console"));
  clearAction->setEnabled(hasLines_);
  connect(clearAction, &QAction::triggered, this, &ConsoleView::clearAll);
  menu->exec(event->globalPos());
  delete menu;
}
