// "What the AI sees": the network input as a picture (scaled up, nearest-neighbour) or labelled number bars.
#pragma once

#include <QImage>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include "vyrn/session.h"

class SeesView : public QWidget {
  Q_OBJECT

 public:
  explicit SeesView(QWidget* parent = nullptr);
  void setLive(const vyrn::LiveView& view);                          // picture or numbers from a running session
  void setPicture(const vyrn::Picture& picture, const QString& caption);   // e.g. a Check preview
  void setNumbers(const QVector<float>& values, const QStringList& names, const QString& caption);
  void clear();
  QImage picture() const { return image_; }   // the picture shown now (RGB888); null when numbers or nothing
  QSize sizeHint() const override;

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  QImage image_;
  QVector<float> numbers_;
  QStringList names_;
  QString caption_;
};
