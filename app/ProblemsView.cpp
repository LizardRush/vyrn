// ProblemsView: one row per error or warning: a coloured dot, "points.py · line 7", the message, and the hint
// in a quieter colour. Clicking a row (or Enter) jumps to the line.
#include "ProblemsView.h"

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QPainter>
#include <QPointer>
#include <QStyledItemDelegate>
#include <QTextDocument>

#include <cmath>

#include "Theme.h"

namespace {

enum Role { KindRole = Qt::UserRole + 1, FileRole, LineRole, ColRole, MessageRole, HintRole };

constexpr int kPadX = 10;
constexpr int kPadY = 7;
constexpr int kDot = 8;
constexpr int kDotGap = 10;

QString html(const QModelIndex& index) {
  const QString file = index.data(FileRole).toString();
  const int line = index.data(LineRole).toInt();
  QString where;
  if (!file.isEmpty()) {
    where = line > 0 ? QStringLiteral("%1 · line %2").arg(file.toHtmlEscaped()).arg(line) : file.toHtmlEscaped();
  }
  QString text;
  if (!where.isEmpty()) text += QStringLiteral("<b>%1</b>&nbsp;&nbsp; ").arg(where);
  text += index.data(MessageRole).toString().toHtmlEscaped();
  const QString hint = index.data(HintRole).toString();
  if (!hint.isEmpty())
    text += QStringLiteral("<br><span style=\"color:%1\">%2</span>").arg(theme::muted().name(), hint.toHtmlEscaped());
  return text;
}

class ProblemDelegate : public QStyledItemDelegate {
 public:
  explicit ProblemDelegate(QListWidget* view) : QStyledItemDelegate(view), view_(view) {}

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    opt.text.clear();
    opt.icon = QIcon();
    const QWidget* widget = opt.widget;
    QStyle* style = widget ? widget->style() : nullptr;
    if (style) style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, widget);

    const bool error = index.data(KindRole).toString() == QLatin1String("error");
    QTextDocument doc;
    prepare(doc, index, opt.rect.width());
    const QPointF textAt(opt.rect.left() + kPadX + kDot + kDotGap, opt.rect.top() + kPadY);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    painter->setBrush(error ? theme::danger() : theme::warning());
    const qreal firstLine = QFontMetricsF(opt.font).height();
    painter->drawEllipse(QRectF(opt.rect.left() + kPadX, textAt.y() + (firstLine - kDot) / 2.0, kDot, kDot));
    painter->translate(textAt);
    QAbstractTextDocumentLayout::PaintContext context;
    context.palette = opt.palette;
    context.palette.setColor(QPalette::Text, theme::text());
    doc.documentLayout()->draw(painter, context);
    painter->restore();
  }

  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
    const int width = view_ ? view_->viewport()->width() : option.rect.width();
    QTextDocument doc;
    prepare(doc, index, width);
    return QSize(width, int(std::ceil(doc.size().height())) + 2 * kPadY);
  }

 private:
  void prepare(QTextDocument& doc, const QModelIndex& index, int rowWidth) const {
    doc.setDefaultFont(view_ ? view_->font() : QFont());
    doc.setDocumentMargin(0);
    doc.setHtml(html(index));
    doc.setTextWidth(qMax(80, rowWidth - 2 * kPadX - kDot - kDotGap));
  }

  QPointer<QListWidget> view_;
};

}  // namespace

ProblemsView::ProblemsView(QWidget* parent) : QListWidget(parent) {
  setItemDelegate(new ProblemDelegate(this));
  setSelectionMode(QAbstractItemView::SingleSelection);
  setResizeMode(QListView::Adjust);   // rows are re-measured when the width changes (the text wraps)
  setUniformItemSizes(false);
  setWordWrap(true);
  setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setFrameShape(QFrame::NoFrame);
  setMouseTracking(true);

  auto open = [this](QListWidgetItem* item) {
    if (!item) return;
    const QString file = item->data(FileRole).toString();
    if (!file.isEmpty()) Q_EMIT openLocation(file, item->data(LineRole).toInt(), item->data(ColRole).toInt());
  };
  connect(this, &QListWidget::itemClicked, this, open);
  connect(this, &QListWidget::itemActivated, this, open);
}

void ProblemsView::addProblem(const QString& kind, const QString& file, int line, int col, const QString& message,
                              const QString& hint) {
  const QString k = kind == QLatin1String("error") ? QStringLiteral("error") : QStringLiteral("warning");
  for (int i = 0; i < count(); ++i) {
    const QListWidgetItem* it = item(i);
    if (it->data(KindRole).toString() == k && it->data(FileRole).toString() == file &&
        it->data(LineRole).toInt() == line && it->data(MessageRole).toString() == message) {
      return;
    }
  }
  auto* entry = new QListWidgetItem;
  entry->setData(KindRole, k);
  entry->setData(FileRole, file);
  entry->setData(LineRole, line);
  entry->setData(ColRole, col);
  entry->setData(MessageRole, message);
  entry->setData(HintRole, hint);
  QString tip = message;
  if (!hint.isEmpty()) tip += QStringLiteral("\n") + hint;
  if (!file.isEmpty()) tip += QStringLiteral("\n\nClick to go to %1%2.").arg(file, line > 0 ? QStringLiteral(" line %1").arg(line) : QString());
  entry->setToolTip(tip);
  if (k == QLatin1String("error")) {
    insertItem(errorCount(), entry);   // after the other errors, before the warnings
  } else {
    addItem(entry);
  }
  viewport()->update();
}

void ProblemsView::clearProblems() {
  clear();
  viewport()->update();
}

int ProblemsView::errorCount() const {
  int n = 0;
  for (int i = 0; i < count(); ++i) n += item(i)->data(KindRole).toString() == QLatin1String("error");
  return n;
}

int ProblemsView::warningCount() const { return count() - errorCount(); }

void ProblemsView::paintEvent(QPaintEvent* event) {
  QListWidget::paintEvent(event);
  if (count() > 0) return;
  QPainter painter(viewport());
  painter.setPen(theme::muted());
  painter.drawText(viewport()->rect().adjusted(16, 12, -16, -12), Qt::AlignCenter | Qt::TextWordWrap,
                   QStringLiteral("No problems found. Check looks for mistakes in your scripts; anything it finds "
                                  "shows up here."));
}
