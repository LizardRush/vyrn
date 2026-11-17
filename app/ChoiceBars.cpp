// ChoiceBars: one row per choice with the chance the AI picks it; the current pick is highlighted.
#include "ChoiceBars.h"

#include <QFontMetrics>
#include <QPainter>

#include <algorithm>
#include <cmath>

#include "Theme.h"

namespace {

constexpr int kRowHeight = 24;
constexpr int kPad = 8;

int headerHeight(const QFontMetrics& fm) { return fm.height() + 12; }

}  // namespace

ChoiceBars::ChoiceBars(QWidget* parent) : QWidget(parent) {
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

void ChoiceBars::setChoices(const QStringList& labels) {
  if (labels == labels_ && probs_.isEmpty()) return;
  const bool resized = labels.size() != labels_.size();
  labels_ = labels;
  probs_.clear();
  chosen_ = -1;
  if (resized) updateGeometry();
  update();
}

void ChoiceBars::setLive(const vyrn::LiveView& view) {
  QStringList labels = labels_;
  if (!view.choices.empty()) {
    labels.clear();
    for (const std::string& choice : view.choices) labels.append(QString::fromStdString(choice));
  }
  const bool resized = labels.size() != labels_.size();
  labels_ = labels;
  probs_ = QVector<float>(view.probs.begin(), view.probs.end());
  chosen_ = view.chosen;
  explore_ = view.explore;
  if (resized) updateGeometry();
  update();
}

void ChoiceBars::clear() {
  const bool resized = !labels_.isEmpty();
  labels_.clear();
  probs_.clear();
  chosen_ = -1;
  explore_ = 0;
  if (resized) updateGeometry();
  update();
}

QSize ChoiceBars::sizeHint() const {
  const QFontMetrics fm(font());
  const int rows = std::max<int>(1, int(labels_.size()));
  return {240, headerHeight(fm) + rows * kRowHeight + kPad};
}

void ChoiceBars::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event);
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);
  const QFontMetrics fm(font());
  const int w = width();

  // Header: "Choices" and, while the AI still tries things at random, how often it does.
  QFont titleFont = font();
  titleFont.setWeight(QFont::DemiBold);
  p.setFont(titleFont);
  p.setPen(theme::text());
  const int headH = headerHeight(fm);
  p.drawText(QRect(kPad, 0, w - 2 * kPad, headH), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Choices"));
  p.setFont(font());
  if (explore_ > 0.005 && !probs_.isEmpty()) {
    p.setPen(theme::faint());
    p.drawText(QRect(kPad, 0, w - 2 * kPad, headH), Qt::AlignRight | Qt::AlignVCenter,
               QStringLiteral("Exploring %1%").arg(int(std::lround(explore_ * 100))));
  }

  if (labels_.isEmpty()) {
    p.setPen(theme::faint());
    p.drawText(QRect(kPad, headH, w - 2 * kPad, kRowHeight), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("The choices appear after Check or Train"));
    return;
  }

  int labelW = 0;
  for (const QString& label : std::as_const(labels_)) labelW = std::max(labelW, fm.horizontalAdvance(label));
  labelW = std::min(labelW + 6, std::max(60, int(w * 0.38)));
  const int percentW = fm.horizontalAdvance(QStringLiteral("100%")) + 6;
  const int trackX = kPad + labelW + 6;
  const int trackW = std::max(10, w - trackX - percentW - kPad);
  QFont boldFont = font();
  boldFont.setWeight(QFont::DemiBold);
  QColor otherFill = theme::muted();
  otherFill.setAlpha(120);

  for (int i = 0; i < labels_.size(); ++i) {
    const int y = headH + i * kRowHeight;
    const bool picked = i == chosen_;
    const bool known = i < probs_.size() && std::isfinite(probs_[i]);
    const double prob = known ? std::clamp(double(probs_[i]), 0.0, 1.0) : 0.0;

    if (picked) {
      QColor row = theme::accent();
      row.setAlpha(26);
      p.setPen(Qt::NoPen);
      p.setBrush(row);
      p.drawRoundedRect(QRectF(2, y + 1, w - 4, kRowHeight - 2), 5, 5);
    }
    p.setFont(picked ? boldFont : font());
    p.setPen(picked ? theme::text() : theme::muted());
    p.drawText(QRect(kPad, y, labelW, kRowHeight), Qt::AlignLeft | Qt::AlignVCenter,
               fm.elidedText(labels_[i], Qt::ElideRight, labelW));

    const QRectF track(trackX, y + kRowHeight / 2.0 - 5, trackW, 10);
    p.setPen(Qt::NoPen);
    p.setBrush(theme::base());
    p.drawRoundedRect(track, 5, 5);
    if (prob > 0) {
      QRectF fill = track;
      fill.setWidth(std::max(4.0, trackW * prob));
      p.setBrush(picked ? theme::accent() : otherFill);
      p.drawRoundedRect(fill, 5, 5);
    }
    p.setPen(picked ? theme::text() : theme::faint());
    p.drawText(QRect(trackX + trackW, y, percentW, kRowHeight), Qt::AlignRight | Qt::AlignVCenter,
               known ? QStringLiteral("%1%").arg(int(std::lround(prob * 100))) : QStringLiteral("–"));
  }
}
