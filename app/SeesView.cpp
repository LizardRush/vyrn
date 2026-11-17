// SeesView: the network input exactly as the AI gets it. A picture is scaled up with nearest-neighbour (with a
// 1-px grid once a cell is 12 px or more); numbers are bars around zero, one per input.
#include "SeesView.h"

#include <QFontMetrics>
#include <QPainter>

#include <algorithm>
#include <cmath>

#include "Theme.h"

namespace {

constexpr int kPad = 8;
constexpr int kNumberRow = 20;
constexpr double kNumberRange = 10.0;   // bars show -10..10; larger values are drawn at the end of the bar

QString numbersCaption(qsizetype count) {
  return count == 1 ? QStringLiteral("1 number") : QStringLiteral("%1 numbers").arg(count);
}

QString numberText(float value) {
  if (!std::isfinite(value)) return QStringLiteral("?");
  const double a = std::abs(double(value));
  if (a >= 1000) return QString::number(value, 'g', 4);
  if (a >= 100) return QString::number(value, 'f', 0);
  if (a >= 10) return QString::number(value, 'f', 1);
  return QString::number(value, 'f', 2);
}

}  // namespace

SeesView::SeesView(QWidget* parent) : QWidget(parent) {
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
  setMinimumHeight(160);
}

void SeesView::setLive(const vyrn::LiveView& view) {
  if (view.kind == "picture" && view.input.w > 0 && view.input.h > 0) {
    setPicture(view.input, QStringLiteral("%1×%2 colour").arg(view.input.w).arg(view.input.h));
  } else if (view.kind == "numbers") {
    QStringList names;
    names.reserve(qsizetype(view.number_names.size()));
    for (const std::string& name : view.number_names) names.append(QString::fromStdString(name));
    setNumbers(QVector<float>(view.numbers.begin(), view.numbers.end()), names,
               numbersCaption(qsizetype(view.numbers.size())));
  }
}

void SeesView::setPicture(const vyrn::Picture& picture, const QString& caption) {
  numbers_.clear();
  names_.clear();
  caption_ = caption;
  const size_t needed = size_t(std::max(0, picture.w)) * size_t(std::max(0, picture.h)) * 3;
  if (picture.w <= 0 || picture.h <= 0 || picture.rgb.size() < needed) {
    image_ = QImage();
  } else {
    if (image_.width() != picture.w || image_.height() != picture.h || image_.format() != QImage::Format_RGB888)
      image_ = QImage(picture.w, picture.h, QImage::Format_RGB888);
    const size_t row = size_t(picture.w) * 3;
    for (int y = 0; y < picture.h; ++y)   // QImage rows are padded to 4 bytes: copy row by row
      std::copy_n(picture.rgb.data() + size_t(y) * row, row, image_.scanLine(y));
  }
  update();
}

void SeesView::setNumbers(const QVector<float>& values, const QStringList& names, const QString& caption) {
  image_ = QImage();
  numbers_ = values;
  names_ = names;
  caption_ = caption.isEmpty() ? numbersCaption(values.size()) : caption;
  update();
}

void SeesView::clear() {
  image_ = QImage();
  numbers_.clear();
  names_.clear();
  caption_.clear();
  update();
}

QSize SeesView::sizeHint() const { return {260, 280}; }

void SeesView::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event);
  QPainter p(this);
  const QFontMetrics fm(font());
  const int w = width();

  QFont titleFont = font();
  titleFont.setWeight(QFont::DemiBold);
  p.setFont(titleFont);
  p.setPen(theme::text());
  const int titleH = fm.height() + 12;
  p.drawText(QRect(kPad, 0, w - 2 * kPad, titleH), Qt::AlignLeft | Qt::AlignVCenter,
             QStringLiteral("What the AI sees"));
  QFont small = font();
  small.setPointSizeF(std::max(7.0, font().pointSizeF() * 0.9));
  p.setFont(small);
  const QFontMetrics smallFm(small);
  int top = titleH;
  if (!caption_.isEmpty()) {
    p.setPen(theme::faint());
    p.drawText(QRect(kPad, top - 4, w - 2 * kPad, smallFm.height()), Qt::AlignLeft | Qt::AlignVCenter,
               smallFm.elidedText(caption_, Qt::ElideRight, w - 2 * kPad));
    top += smallFm.height() + 2;
  }
  p.setFont(font());
  const QRect area(kPad, top + 2, std::max(1, w - 2 * kPad), std::max(1, height() - top - 2 - kPad));

  if (!image_.isNull()) {
    // Whole-pixel scaling keeps every cell the same size; a picture bigger than the panel is shrunk to fit.
    const double fit = std::min(double(area.width()) / image_.width(), double(area.height()) / image_.height());
    const double cell = fit >= 1 ? std::floor(fit) : fit;
    const QSizeF size(image_.width() * cell, image_.height() * cell);
    const QRectF target(area.left() + (area.width() - size.width()) / 2.0, area.top(), size.width(), size.height());
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.drawImage(target, image_);
    if (cell >= 12) {
      p.setPen(QColor(128, 128, 128, 70));   // shows on black cells and on white ones
      for (int x = 1; x < image_.width(); ++x)
        p.drawLine(QPointF(target.left() + x * cell, target.top()), QPointF(target.left() + x * cell, target.bottom()));
      for (int y = 1; y < image_.height(); ++y)
        p.drawLine(QPointF(target.left(), target.top() + y * cell), QPointF(target.right(), target.top() + y * cell));
    }
    p.setPen(theme::border());
    p.setBrush(Qt::NoBrush);
    p.drawRect(target.adjusted(-0.5, -0.5, 0.5, 0.5));
    return;
  }

  if (!numbers_.isEmpty()) {
    const int fitRows = std::max(1, area.height() / kNumberRow);
    const int total = int(numbers_.size());
    const int shown = total <= fitRows ? total : std::max(0, fitRows - 1);   // leave a row for "… and N more"
    int nameW = 0;
    for (int i = 0; i < shown && i < names_.size(); ++i) nameW = std::max(nameW, fm.horizontalAdvance(names_[i]));
    if (shown > names_.size()) nameW = std::max(nameW, fm.horizontalAdvance(QStringLiteral("input %1").arg(shown)));
    nameW = std::min(nameW + 6, std::max(50, int(area.width() * 0.4)));
    const int valueW = fm.horizontalAdvance(QStringLiteral("-000.00")) + 4;
    const int trackX = area.left() + nameW + 4;
    const int trackW = std::max(10, area.right() - valueW - trackX);
    const double mid = trackX + trackW / 2.0;
    QColor negative = theme::warning();
    negative.setAlpha(200);
    p.setRenderHint(QPainter::Antialiasing, true);
    for (int i = 0; i < shown; ++i) {
      const int y = area.top() + i * kNumberRow;
      const QString name = i < names_.size() && !names_[i].isEmpty() ? names_[i] : QStringLiteral("input %1").arg(i + 1);
      p.setPen(theme::muted());
      p.drawText(QRect(area.left(), y, nameW, kNumberRow), Qt::AlignLeft | Qt::AlignVCenter,
                 fm.elidedText(name, Qt::ElideRight, nameW));
      const QRectF track(trackX, y + kNumberRow / 2.0 - 4, trackW, 8);
      p.setPen(Qt::NoPen);
      p.setBrush(theme::base());
      p.drawRoundedRect(track, 4, 4);
      const float value = numbers_[i];
      if (std::isfinite(value) && value != 0) {
        const double frac = std::clamp(double(value) / kNumberRange, -1.0, 1.0);
        const double len = std::max(2.0, std::abs(frac) * trackW / 2.0);
        p.setBrush(value > 0 ? theme::accent() : negative);
        p.drawRoundedRect(QRectF(value > 0 ? mid : mid - len, track.top(), len, track.height()), 3, 3);
      }
      p.setPen(theme::border().lighter(150));
      p.drawLine(QPointF(mid, track.top() - 2), QPointF(mid, track.bottom() + 2));
      p.setPen(theme::text());
      p.drawText(QRect(area.right() - valueW, y, valueW, kNumberRow), Qt::AlignRight | Qt::AlignVCenter,
                 numberText(value));
    }
    if (shown < total) {
      p.setPen(theme::faint());
      p.drawText(QRect(area.left(), area.top() + shown * kNumberRow, area.width(), kNumberRow),
                 Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("… and %1 more").arg(total - shown));
    }
    return;
  }

  p.setPen(theme::faint());
  p.drawText(area.adjusted(4, 4, -4, -4), Qt::AlignCenter | Qt::TextWordWrap,
             QStringLiteral("Press Check to see the picture or the numbers your inputs.py gives the AI"));
}
