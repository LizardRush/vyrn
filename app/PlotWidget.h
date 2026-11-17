// Points per round (dots) and the 20-round mean (thick accent line). Custom QPainter, fast for 100k rounds.
#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

class PlotWidget : public QWidget {
  Q_OBJECT

 public:
  explicit PlotWidget(QWidget* parent = nullptr);
  void clear();
  void addRound(qint64 round, double points, double mean20);
  void setEmptyText(const QString& text);
  // Only the 20-round mean (e.g. a shared brain's history, which has no per-round points): no dots legend.
  void setMeanOnly(bool on);
  int roundCount() const;
  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void leaveEvent(QEvent* event) override;

 private:
  struct Point {
    qint64 round;
    float points, mean;
  };
  QVector<Point> points_;
  QString emptyText_;
  int hoverX_ = -1;
  bool meanOnly_ = false;
  bool dirty_ = false;
};
