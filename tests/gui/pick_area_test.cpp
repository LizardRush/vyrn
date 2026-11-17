// Unit test (ctest gui_pick_area, labels unit and gui): Pick area's coordinate conversion, app/PickTool.h. Pure
// functions, so it runs on every system without a screen. Built by app/CMakeLists.txt.
#include <QPoint>
#include <QRect>
#include <QSize>

#include <cstdio>

#include "PickTool.h"

namespace {

int failures = 0;

void expectArea(const char* what, const picktool::Area& got, int x, int y, int width, int height) {
  if (got.x == x && got.y == y && got.width == width && got.height == height) return;
  ++failures;
  std::printf("FAIL %s: got x=%d y=%d width=%d height=%d, want x=%d y=%d width=%d height=%d\n", what, got.x, got.y,
              got.width, got.height, x, y, width, height);
}

void expectRect(const char* what, const QRect& got, int x, int y, int width, int height) {
  if (got == QRect(x, y, width, height)) return;
  ++failures;
  std::printf("FAIL %s: got %d,%d %dx%d, want %d,%d %dx%d\n", what, got.x(), got.y(), got.width(), got.height(), x, y,
              width, height);
}

}  // namespace

int main() {
  using picktool::Units;
  using picktool::shotRect;
  using picktool::toArea;
  const QRect dragged(10, 20, 100, 50);

  // Linux and macOS: logical pixels (points on macOS), whatever the screen's ratio.
  expectArea("logical, main screen", toArea(dragged, QPoint(0, 0), 1.0, Units::Logical), 10, 20, 100, 50);
  expectArea("logical, Retina", toArea(dragged, QPoint(0, 0), 2.0, Units::Logical), 10, 20, 100, 50);
  expectArea("logical, screen left of the main one", toArea(dragged, QPoint(-1440, 0), 2.0, Units::Logical), -1430,
             20, 100, 50);
  expectArea("logical, screen above", toArea(dragged, QPoint(0, -1080), 1.0, Units::Logical), 10, -1060, 100, 50);

  // Windows: physical pixels. The screen's top-left is already in device pixels; only the dragged part scales.
  expectArea("physical, 100 %", toArea(dragged, QPoint(0, 0), 1.0, Units::Physical), 10, 20, 100, 50);
  expectArea("physical, 150 %", toArea(dragged, QPoint(0, 0), 1.5, Units::Physical), 15, 30, 150, 75);
  expectArea("physical, 200 %", toArea(dragged, QPoint(0, 0), 2.0, Units::Physical), 20, 40, 200, 100);
  // 125 %: each edge rounds on its own (12.5 -> 13, 137.5 -> 138; 25 -> 25, 87.5 -> 88).
  expectArea("physical, 125 %", toArea(dragged, QPoint(0, 0), 1.25, Units::Physical), 13, 25, 125, 63);
  expectArea("physical, second screen at 150 %", toArea(QRect(0, 0, 1280, 720), QPoint(2560, 0), 1.5, Units::Physical),
             2560, 0, 1920, 1080);
  expectArea("physical, screen left of the main one", toArea(dragged, QPoint(-1920, 0), 1.0, Units::Physical), -1910,
             20, 100, 50);
  // Two areas dragged side by side still meet after rounding.
  const picktool::Area left = toArea(QRect(0, 0, 3, 3), QPoint(0, 0), 1.25, Units::Physical);
  const picktool::Area right = toArea(QRect(3, 0, 3, 3), QPoint(0, 0), 1.25, Units::Physical);
  if (left.x + left.width != right.x) {
    ++failures;
    std::printf("FAIL neighbours: the left area ends at %d, the right one starts at %d\n", left.x + left.width, right.x);
  }

  // Snapshot: the pixels under the dragged rectangle in a picture of the whole screen.
  expectRect("shot, same size", shotRect(dragged, QSize(1920, 1080), QSize(1920, 1080)), 10, 20, 100, 50);
  expectRect("shot, Retina", shotRect(dragged, QSize(1440, 900), QSize(2880, 1800)), 20, 40, 200, 100);
  expectRect("shot, 125 %", shotRect(dragged, QSize(1536, 864), QSize(1920, 1080)), 13, 25, 125, 63);
  expectRect("shot, clipped at the edge", shotRect(QRect(1900, 1000, 100, 100), QSize(1920, 1080), QSize(1920, 1080)),
             1900, 1000, 20, 80);
  expectRect("shot, no screen", shotRect(dragged, QSize(), QSize(100, 100)), 0, 0, 0, 0);
  // On Windows the saved picture and the area have the same size.
  const picktool::Area area = toArea(dragged, QPoint(0, 0), 1.25, Units::Physical);
  const QRect pixels = shotRect(dragged, QSize(1536, 864), QSize(1920, 1080));
  if (pixels.width() != area.width || pixels.height() != area.height) {
    ++failures;
    std::printf("FAIL picture size %dx%d != area %dx%d\n", pixels.width(), pixels.height(), area.width, area.height);
  }

  if (failures == 0) std::printf("pick area: all coordinate checks passed\n");
  return failures == 0 ? 0 : 1;
}
