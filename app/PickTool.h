// Pick area / Snapshot (real-game projects). Wayland: slurp lets the user drag a rectangle and grim takes the
// picture, both run asynchronously. Elsewhere (macOS, Windows, X11): a frameless overlay over one screen shows a
// picture of it (QScreen::grabWindow) and the user drags the rectangle on that.
#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>

#include <cmath>

class QWidget;
class Editor;

namespace picktool {

// Lets the user drag a rectangle, then rewrites the first vyrn.screen(...) call in `inputs`
// (or inserts one at the cursor) as one undoable edit.
void pickArea(QWidget* window, Editor* inputs);

// Lets the user drag a rectangle, asks for a name (default "game_over.png"), saves it to
// <projectDir>/pictures/<name> with the PNG text "vyrn-area" = "x,y,w,h", and inserts
// vyrn.on_screen("<name>", x=…, y=…) at the cursor of `target`. The picture has the area's size in the units below.
void snapshot(QWidget* window, const QString& projectDir, Editor* target);

// ---- coordinates: pure functions (tests/gui/pick_area_test.cpp) -----------------------------------------------

// A rectangle in the coordinates vyrn.screen() takes.
struct Area {
  int x = 0, y = 0, width = 0, height = 0;
};

// What vyrn.screen() coordinates count (docs/dev/platforms.md): logical pixels on Linux and macOS (points there),
// physical pixels on Windows.
enum class Units { Logical, Physical };

// The overlay covers one screen. `dragged` is the rectangle dragged on it, in logical pixels from the screen's
// top-left; `screenTopLeft` is QScreen::geometry().topLeft() (Qt keeps a screen's top-left in device pixels and
// scales only its size); `ratio` is QScreen::devicePixelRatio(). Physical areas round each edge on its own, so areas
// dragged side by side still meet.
inline Area toArea(const QRect& dragged, const QPoint& screenTopLeft, double ratio, Units units) {
  if (units == Units::Logical)
    return {screenTopLeft.x() + dragged.x(), screenTopLeft.y() + dragged.y(), dragged.width(), dragged.height()};
  const auto edge = [ratio](int logical) { return static_cast<int>(std::lround(logical * ratio)); };
  const int left = edge(dragged.x());
  const int top = edge(dragged.y());
  return {screenTopLeft.x() + left, screenTopLeft.y() + top, edge(dragged.x() + dragged.width()) - left,
          edge(dragged.y() + dragged.height()) - top};
}

// The pixels under `dragged` in a picture of the whole screen: `shot` pixels across a screen of `screen` logical
// pixels (Retina and scaled screens have more pixels than logical pixels). Clipped to the picture.
inline QRect shotRect(const QRect& dragged, const QSize& screen, const QSize& shot) {
  if (screen.isEmpty() || shot.isEmpty()) return {};
  const double sx = double(shot.width()) / screen.width();
  const double sy = double(shot.height()) / screen.height();
  const int left = static_cast<int>(std::lround(dragged.x() * sx));
  const int top = static_cast<int>(std::lround(dragged.y() * sy));
  const int right = static_cast<int>(std::lround((dragged.x() + dragged.width()) * sx));
  const int bottom = static_cast<int>(std::lround((dragged.y() + dragged.height()) * sy));
  return QRect(left, top, right - left, bottom - top) & QRect(QPoint(0, 0), shot);
}

}  // namespace picktool
