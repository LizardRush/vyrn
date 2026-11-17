// PlotWidget: points per round as small dots and the 20-round mean as a thick accent line. Rounds are bucketed
// into pixel columns (at most a min and a max dot per column), so 100k rounds draw as fast as 100.
#include "PlotWidget.h"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "ProjectStore.h"
#include "Theme.h"

namespace {

constexpr int kRepaintMs = 250;   // at most 4 repaints a second while rounds pour in

// A "nice" tick step (1, 2 or 5 times a power of ten) giving about `target` ticks over `range`.
double niceStep(double range, int target) {
  if (!(range > 0) || !std::isfinite(range)) return 1;
  const double rough = range / std::max(1, target);
  const double power = std::pow(10.0, std::floor(std::log10(rough)));
  const double f = rough / power;
  const double nice = f < 1.5 ? 1 : f < 3 ? 2 : f < 7 ? 5 : 10;
  return nice * power;
}

// A y label with as many decimals as the step needs ("0.5", "10", "-2.25").
QString yLabel(double value, double step) {
  int decimals = 0;
  if (step < 1) decimals = std::min(6, int(std::ceil(-std::log10(step) - 1e-9)));
  if (std::abs(value) < step * 1e-6) value = 0;   // never "-0"
  return QString::number(value, 'f', decimals);
}

// A round number on the x axis: "250", "1500", "25k" (whole thousands only, for big steps).
QString xLabel(double value, double step) {
  const qint64 v = qint64(std::llround(value));
  if (step >= 1000 && v % 1000 == 0) return QStringLiteral("%1k").arg(v / 1000);
  return QString::number(v);
}

QString pointsWord(double points) { return std::abs(points) == 1.0 ? QStringLiteral("point") : QStringLiteral("points"); }

}  // namespace

PlotWidget::PlotWidget(QWidget* parent) : QWidget(parent) {
  emptyText_ = QStringLiteral("Points per round appear here once training starts");
  setMouseTracking(true);
  setAttribute(Qt::WA_OpaquePaintEvent);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void PlotWidget::clear() {
  points_.clear();
  points_.squeeze();
  hoverX_ = -1;
  update();
}

void PlotWidget::addRound(qint64 round, double points, double mean20) {
  points_.append(Point{round, float(points), float(mean20)});
  if (dirty_) return;
  dirty_ = true;
  QTimer::singleShot(kRepaintMs, this, [this] {
    dirty_ = false;
    update();
  });
}

void PlotWidget::setMeanOnly(bool on) {
  meanOnly_ = on;
  update();
}

void PlotWidget::setEmptyText(const QString& text) {
  emptyText_ = text;
  if (points_.isEmpty()) update();
}

int PlotWidget::roundCount() const { return int(points_.size()); }

QSize PlotWidget::sizeHint() const { return {520, 220}; }

QSize PlotWidget::minimumSizeHint() const { return {220, 120}; }

void PlotWidget::mouseMoveEvent(QMouseEvent* event) {
  const int x = int(event->position().x());
  if (x != hoverX_) {
    hoverX_ = x;
    if (!points_.isEmpty()) update();
  }
  QWidget::mouseMoveEvent(event);
}

void PlotWidget::leaveEvent(QEvent* event) {
  hoverX_ = -1;
  if (!points_.isEmpty()) update();
  QWidget::leaveEvent(event);
}

void PlotWidget::paintEvent(QPaintEvent* event) {
  Q_UNUSED(event);
  QPainter p(this);
  p.fillRect(rect(), theme::base());
  const QFontMetrics fm(font());

  if (points_.isEmpty()) {
    p.setPen(theme::faint());
    p.drawText(rect().adjusted(16, 16, -16, -16), Qt::AlignCenter | Qt::TextWordWrap, emptyText_);
    return;
  }

  // ---- ranges -----------------------------------------------------------------------------------------------
  qint64 r0 = std::numeric_limits<qint64>::max(), r1 = std::numeric_limits<qint64>::min();
  double y0 = std::numeric_limits<double>::infinity(), y1 = -y0;
  for (const Point& pt : std::as_const(points_)) {
    r0 = std::min(r0, pt.round);
    r1 = std::max(r1, pt.round);
    if (std::isfinite(pt.points)) {
      y0 = std::min(y0, double(pt.points));
      y1 = std::max(y1, double(pt.points));
    }
    if (std::isfinite(pt.mean)) {
      y0 = std::min(y0, double(pt.mean));
      y1 = std::max(y1, double(pt.mean));
    }
  }
  if (!std::isfinite(y0)) {
    y0 = 0;
    y1 = 1;
  }
  if (y1 - y0 < 1e-9) {
    const double pad = std::max(1.0, std::abs(y0) * 0.1);
    y0 -= pad;
    y1 += pad;
  }
  const double margin = (y1 - y0) * 0.05;
  y0 -= margin;
  y1 += margin;
  double xr0 = double(r0), xr1 = double(r1);
  if (r1 == r0) {
    xr0 -= 1;
    xr1 += 1;
  }

  // ---- layout -----------------------------------------------------------------------------------------------
  const int legendH = fm.height() + 10;
  const QRect area = rect().adjusted(0, legendH, 0, 0);
  const int plotHeight = std::max(10, area.height() - fm.height() - 14);
  const double yStep = niceStep(y1 - y0, std::max(2, plotHeight / 40));
  int labelW = 0;
  for (double v = std::ceil(y0 / yStep) * yStep; v <= y1 + yStep * 1e-9; v += yStep)
    labelW = std::max(labelW, fm.horizontalAdvance(yLabel(v, yStep)));
  const QRect plot(area.left() + labelW + 14, area.top() + 4, std::max(10, area.width() - labelW - 14 - 14),
                   plotHeight);
  const double pxW = std::max(1, plot.width() - 1);
  auto xOf = [&](double round) { return plot.left() + (round - xr0) / (xr1 - xr0) * pxW; };
  auto yOf = [&](double v) { return plot.bottom() - (v - y0) / (y1 - y0) * (plot.height() - 1); };

  // ---- grid and axes ----------------------------------------------------------------------------------------
  p.setPen(theme::border());
  p.drawRect(plot.adjusted(0, 0, -1, -1));
  QColor gridColor = theme::border();
  gridColor.setAlpha(150);
  for (double v = std::ceil(y0 / yStep) * yStep; v <= y1 + yStep * 1e-9; v += yStep) {
    const int y = int(std::lround(yOf(v)));
    if (y < plot.top() || y > plot.bottom()) continue;
    p.setPen(gridColor);
    p.drawLine(plot.left() + 1, y, plot.right() - 1, y);
    p.setPen(theme::faint());
    p.drawText(QRect(area.left(), y - fm.height() / 2, labelW + 6, fm.height()), Qt::AlignRight | Qt::AlignVCenter,
               yLabel(v, yStep));
  }
  if (y0 < 0 && y1 > 0) {   // the zero line a bit stronger
    const int y = int(std::lround(yOf(0)));
    p.setPen(theme::border().lighter(140));
    p.drawLine(plot.left() + 1, y, plot.right() - 1, y);
  }
  const double xStep = std::max(1.0, niceStep(xr1 - xr0, std::max(2, plot.width() / 90)));
  p.setPen(theme::faint());
  for (double v = std::ceil(xr0 / xStep) * xStep; v <= xr1 + 1e-9; v += xStep) {
    const int x = int(std::lround(xOf(v)));
    const QString text = xLabel(v, xStep);
    const int w = fm.horizontalAdvance(text);
    p.drawLine(x, plot.bottom(), x, plot.bottom() + 3);
    p.drawText(QRect(std::clamp(x - w / 2, 0, std::max(0, width() - w)), plot.bottom() + 5, w, fm.height()),
               Qt::AlignCenter, text);
  }

  // ---- bucket the rounds into pixel columns -----------------------------------------------------------------
  const int columns = plot.width();
  struct Column {
    float lo = std::numeric_limits<float>::infinity(), hi = -std::numeric_limits<float>::infinity();
    float mean = std::numeric_limits<float>::quiet_NaN();
    int count = 0;
  };
  std::vector<Column> cols(size_t(std::max(1, columns)));
  for (const Point& pt : std::as_const(points_)) {
    const int c = std::clamp(int((double(pt.round) - xr0) / (xr1 - xr0) * pxW), 0, int(cols.size()) - 1);
    Column& col = cols[size_t(c)];
    if (std::isfinite(pt.points)) {
      col.lo = std::min(col.lo, pt.points);
      col.hi = std::max(col.hi, pt.points);
      ++col.count;
    }
    if (std::isfinite(pt.mean)) col.mean = pt.mean;   // the newest mean in this column
  }

  p.save();
  p.setClipRect(plot.adjusted(1, 1, -1, -1));
  QColor dot = theme::muted();
  dot.setAlpha(190);
  const bool sparse = points_.size() * 4 <= qsizetype(columns);
  if (sparse) {
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(dot);
    for (const Point& pt : std::as_const(points_)) {
      if (!std::isfinite(pt.points)) continue;
      p.drawEllipse(QPointF(xOf(double(pt.round)), yOf(pt.points)), 2.2, 2.2);
    }
  } else {
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QPen(dot, 2));
    for (int c = 0; c < int(cols.size()); ++c) {
      const Column& col = cols[size_t(c)];
      if (col.count == 0) continue;
      const int x = plot.left() + c;
      const int ylo = int(std::lround(yOf(col.lo))), yhi = int(std::lround(yOf(col.hi)));
      p.drawPoint(x, ylo);
      if (yhi != ylo) p.drawPoint(x, yhi);
    }
  }

  // The 20-round mean: one vertex per column.
  QPolygonF line;
  line.reserve(qsizetype(cols.size()));
  if (sparse) {
    for (const Point& pt : std::as_const(points_))
      if (std::isfinite(pt.mean)) line.append(QPointF(xOf(double(pt.round)), yOf(pt.mean)));
  } else {
    for (int c = 0; c < int(cols.size()); ++c)
      if (std::isfinite(cols[size_t(c)].mean)) line.append(QPointF(plot.left() + c + 0.5, yOf(cols[size_t(c)].mean)));
  }
  p.setRenderHint(QPainter::Antialiasing, true);
  if (line.size() == 1) {
    p.setPen(Qt::NoPen);
    p.setBrush(theme::accent());
    p.drawEllipse(line.first(), 3.0, 3.0);
  } else if (line.size() > 1) {
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(theme::accent(), 2.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPolyline(line);
  }
  p.restore();

  // ---- legend -----------------------------------------------------------------------------------------------
  p.setRenderHint(QPainter::Antialiasing, true);
  int lx = plot.left();
  const int ly = legendH / 2 + 1;
  if (!meanOnly_) {
    p.setPen(Qt::NoPen);
    p.setBrush(dot);
    p.drawEllipse(QPointF(lx + 4, ly), 2.6, 2.6);
    lx += 12;
    const QString pointsText = QStringLiteral("Points per round");
    p.setPen(theme::muted());
    p.drawText(QRect(lx, 0, fm.horizontalAdvance(pointsText) + 2, legendH), Qt::AlignVCenter, pointsText);
    lx += fm.horizontalAdvance(pointsText) + 18;
  }
  p.setPen(QPen(theme::accent(), 2.6, Qt::SolidLine, Qt::RoundCap));
  p.drawLine(QPointF(lx, ly), QPointF(lx + 16, ly));
  lx += 22;
  const QString meanText = QStringLiteral("Mean of the last 20 rounds");
  p.setPen(theme::muted());
  p.drawText(QRect(lx, 0, fm.horizontalAdvance(meanText) + 2, legendH), Qt::AlignVCenter, meanText);
  if (!meanOnly_) {   // a shared brain's history is a sample of its rounds: no count
    const QString countText = QStringLiteral("%1 rounds").arg(points_.size());
    p.setPen(theme::faint());
    p.drawText(QRect(plot.left(), 0, plot.width(), legendH), Qt::AlignRight | Qt::AlignVCenter, countText);
  }

  // ---- hover: the round under the mouse ---------------------------------------------------------------------
  if (hoverX_ < plot.left() || hoverX_ > plot.right()) return;
  const double wanted = xr0 + (hoverX_ - plot.left()) / pxW * (xr1 - xr0);
  // Rounds arrive in order: binary search, then take the nearer neighbour.
  const qsizetype n = points_.size();
  const qsizetype at = std::lower_bound(points_.cbegin(), points_.cend(), wanted,
                                        [](const Point& a, double r) { return double(a.round) < r; }) -
                       points_.cbegin();
  qsizetype pick = std::min(at, n - 1);
  if (at > 0 && (at >= n || std::abs(double(points_[at - 1].round) - wanted) <=
                                std::abs(double(points_[at].round) - wanted)))
    pick = at - 1;
  const Point* best = &points_[pick];
  const double hx = xOf(double(best->round));
  p.setPen(QPen(theme::faint(), 1, Qt::DashLine));
  p.drawLine(QPointF(hx, plot.top() + 1), QPointF(hx, plot.bottom() - 1));
  if (std::isfinite(best->points)) {
    p.setPen(QPen(theme::text(), 1.5));
    p.setBrush(theme::base());
    p.drawEllipse(QPointF(hx, yOf(best->points)), 3.5, 3.5);
  }
  QString text;
  if (std::isfinite(best->points)) {
    text = QStringLiteral("Round %1: %2 %3").arg(best->round).arg(store::formatPoints(best->points),
                                                                  pointsWord(best->points));
    if (std::isfinite(best->mean)) text += QStringLiteral(" (mean %1)").arg(store::formatPoints(best->mean));
  } else {
    text = QStringLiteral("Round %1: mean %2").arg(best->round).arg(store::formatPoints(best->mean));
  }
  const int tw = fm.horizontalAdvance(text) + 16, th = fm.height() + 8;
  int tx = int(hx) + 10;
  if (tx + tw > plot.right()) tx = int(hx) - 10 - tw;
  tx = std::max(plot.left() + 2, tx);
  const QRect box(tx, plot.top() + 6, tw, th);
  p.setPen(theme::border().lighter(130));
  QColor fill = theme::panel();
  fill.setAlpha(235);
  p.setBrush(fill);
  p.drawRoundedRect(QRectF(box).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
  p.setPen(theme::text());
  p.drawText(box, Qt::AlignCenter, text);
}
