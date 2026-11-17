// One bar per choice: the chance the AI picks it, with the current pick highlighted.
#pragma once

#include <QStringList>
#include <QVector>
#include <QWidget>

#include "vyrn/session.h"

class ChoiceBars : public QWidget {
  Q_OBJECT

 public:
  explicit ChoiceBars(QWidget* parent = nullptr);
  void setChoices(const QStringList& labels);   // before a run (e.g. from the spec)
  void setLive(const vyrn::LiveView& view);
  void clear();
  QSize sizeHint() const override;

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  QStringList labels_;
  QVector<float> probs_;
  int chosen_ = -1;
  double explore_ = 0;
};
