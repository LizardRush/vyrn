// Pick area / Snapshot. Wayland: slurp lets the user drag a rectangle on the screen (it draws above every window, so
// Vyrn never minimizes), grim takes the picture; both run as QProcess, so the UI never waits for them. Elsewhere
// (macOS, Windows, X11): AreaOverlay shows a picture of the screen under the mouse and the user drags on that.
#include "PickTool.h"

#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QWidget>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <functional>
#include <stdexcept>
#include <utility>

#include "AppPlatform.h"
#include "Editor.h"
#include "Theme.h"
#include "vyrn/project.h"

namespace picktool {

namespace {

constexpr const char* kBusyProperty = "vyrnPickBusy";   // one rectangle at a time per window

// What the user picked: the area in vyrn.screen() coordinates and, from the overlay, the picture under it at that
// size (null from slurp: grim takes the picture afterwards).
struct Picked {
  Area area;
  QImage picture;
};
using Done = std::function<void(const Picked&)>;

// Linux on Wayland keeps slurp and grim (even when Qt runs through XWayland, where grabWindow sees no Wayland
// windows).
bool useSlurp() {
  return app_platform::os() == app_platform::Os::Linux &&
         (QGuiApplication::platformName().startsWith(QLatin1String("wayland")) ||
          !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"));
}

// vyrn.screen() coordinates here (docs/dev/platforms.md).
Units areaUnits() { return app_platform::os() == app_platform::Os::Windows ? Units::Physical : Units::Logical; }

QString installHint(const QString& program) {
  return QStringLiteral("Vyrn uses the small program %1 for this, but it isn't installed.\n\nInstall it in a "
                        "terminal:\n    sudo apt install slurp grim")
      .arg(program);
}

QString firstLines(const QByteArray& text, int maxLines = 4) {
  QStringList lines = QString::fromUtf8(text).trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
  if (lines.size() > maxLines) lines = lines.mid(0, maxLines);
  return lines.join(QLatin1Char('\n'));
}

// Runs slurp and hands the dragged rectangle to `done`. Esc in slurp cancels quietly.
void selectWithSlurp(QWidget* window, Done done) {
  const QString slurp = QStandardPaths::findExecutable(QStringLiteral("slurp"));
  if (slurp.isEmpty()) {
    QMessageBox::information(window, QStringLiteral("Pick area needs slurp"), installHint(QStringLiteral("slurp")));
    return;
  }
  window->setProperty(kBusyProperty, true);
  auto* process = new QProcess(window);   // a window that closes takes slurp with it
  QObject::connect(process, &QProcess::errorOccurred, window, [window, process](QProcess::ProcessError error) {
    if (error != QProcess::FailedToStart) return;   // crashes also end in finished()
    window->setProperty(kBusyProperty, false);
    process->deleteLater();
    QMessageBox::warning(window, QStringLiteral("Couldn't pick an area"),
                         QStringLiteral("Vyrn couldn't start slurp: %1").arg(process->errorString()));
  });
  QObject::connect(process, &QProcess::finished, window,
                   [window, process, done = std::move(done)](int code, QProcess::ExitStatus status) {
                     window->setProperty(kBusyProperty, false);
                     process->deleteLater();
                     const QByteArray err = process->readAllStandardError();
                     if (status != QProcess::NormalExit || code != 0) {
                       if (QString::fromUtf8(err).contains(QLatin1String("cancel"), Qt::CaseInsensitive)) return;
                       QMessageBox::warning(
                           window, QStringLiteral("Couldn't pick an area"),
                           QStringLiteral("slurp stopped without an area.%1")
                               .arg(err.trimmed().isEmpty() ? QString() : QStringLiteral("\n\n") + firstLines(err)));
                       return;
                     }
                     // slurp printed "x y width height".
                     const QStringList parts =
                         QString::fromUtf8(process->readAllStandardOutput()).simplified().split(QLatin1Char(' '));
                     Area area;
                     bool ok = parts.size() == 4;
                     if (ok) {
                       bool a = false, b = false, c = false, d = false;
                       area = Area{parts[0].toInt(&a), parts[1].toInt(&b), parts[2].toInt(&c), parts[3].toInt(&d)};
                       ok = a && b && c && d && area.width > 0 && area.height > 0;
                     }
                     if (!ok) return;   // a click without dragging: nothing to do
                     done(Picked{area, QImage()});
                   });
  process->start(slurp, {QStringLiteral("-f"), QStringLiteral("%x %y %w %h")});
}

// A frameless window over one screen, showing a picture of it; the user drags a rectangle on it. Esc, a right click
// or a click on another screen cancels. `done` gets the rectangle (logical pixels from the screen's top-left), or an
// empty one when cancelled, once the overlay closes; it deletes itself.
class AreaOverlay : public QWidget {
 public:
  AreaOverlay(const QRect& screenGeometry, const QPixmap& shot, double ratio, const QString& hint,
              std::function<void(const QRect&)> done)
      : QWidget(nullptr, flagsHere()), screen_(screenGeometry), shot_(shot), ratio_(ratio), hint_(hint),
        done_(std::move(done)) {
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setCursor(Qt::CrossCursor);
    setGeometry(screenGeometry);
    // Drawn at the screen's logical size, whatever its pixel count.
    if (screenGeometry.width() > 0) shot_.setDevicePixelRatio(double(shot_.width()) / screenGeometry.width());
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.drawPixmap(0, 0, shot_);
    const QRect area = selection();
    QPainterPath dim;   // odd-even fill: everything but the selection gets darker
    dim.addRect(QRectF(rect()));
    if (!area.isEmpty()) dim.addRect(QRectF(area));
    p.fillPath(dim, QColor(0, 0, 0, 115));
    QFont font = p.font();
    if (font.pointSizeF() > 0) font.setPointSizeF(font.pointSizeF() * 1.15);
    p.setFont(font);
    const QFontMetrics metrics(font);
    if (!area.isEmpty()) {
      p.setPen(QPen(theme::accent(), 2));
      p.setBrush(Qt::NoBrush);
      p.drawRect(QRectF(area).adjusted(0.5, 0.5, -0.5, -0.5));
      const Area a = toArea(area, screen_.topLeft(), ratio_, areaUnits());   // the numbers vyrn.screen() gets
      const QString numbers =
          QStringLiteral("x=%1, y=%2, width=%3, height=%4").arg(a.x).arg(a.y).arg(a.width).arg(a.height);
      label(p, metrics, numbers, QPoint(area.left(), area.bottom() + 8));
    }
    const QRect hint = metrics.boundingRect(hint_).adjusted(-14, -8, 14, 8);
    label(p, metrics, hint_, QPoint((width() - hint.width()) / 2, 24));
  }

  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() == Qt::RightButton) {
      close();
      return;
    }
    if (event->button() != Qt::LeftButton) return;
    from_ = to_ = event->position().toPoint();
    dragging_ = true;
    update();
  }

  void mouseMoveEvent(QMouseEvent* event) override {
    if (!dragging_) return;
    to_ = event->position().toPoint();
    update();
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    if (!dragging_ || event->button() != Qt::LeftButton) return;
    dragging_ = false;
    to_ = event->position().toPoint();
    const QRect area = selection();
    if (area.width() < 2 || area.height() < 2) {   // a click without dragging: drag again
      from_ = to_;
      update();
      return;
    }
    picked_ = area;
    close();
  }

  void keyPressEvent(QKeyEvent* event) override {
    if (event->key() == Qt::Key_Escape) {
      close();
      return;
    }
    QWidget::keyPressEvent(event);
  }

  void closeEvent(QCloseEvent* event) override {
    if (done_) std::exchange(done_, nullptr)(picked_);   // empty: cancelled
    QWidget::closeEvent(event);
  }

 private:
  // A popup gets the mouse and the keys at once. macOS: a popup already floats above the menu bar and the Dock, and
  // "stays on top" would move it below them.
  static Qt::WindowFlags flagsHere() {
    Qt::WindowFlags flags = Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint;
    if (app_platform::os() != app_platform::Os::MacOS) flags |= Qt::WindowStaysOnTopHint;
    return flags;
  }

  QRect selection() const {
    const QPoint topLeft(std::min(from_.x(), to_.x()), std::min(from_.y(), to_.y()));
    return QRect(topLeft, QSize(std::abs(to_.x() - from_.x()), std::abs(to_.y() - from_.y()))) & rect();
  }

  // White text on a dark rounded box at `at` (its top-left), kept on the screen.
  void label(QPainter& p, const QFontMetrics& metrics, const QString& text, QPoint at) const {
    QRect box = metrics.boundingRect(text).adjusted(-14, -8, 14, 8);
    at.setX(std::clamp(at.x(), 8, std::max(8, width() - box.width() - 8)));
    at.setY(std::clamp(at.y(), 8, std::max(8, height() - box.height() - 8)));
    box.moveTopLeft(at);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(20, 24, 32, 225));
    p.drawRoundedRect(box, 8, 8);
    p.setPen(Qt::white);
    p.drawText(box, Qt::AlignCenter, text);
  }

  QRect screen_;
  QPixmap shot_;
  double ratio_ = 1;
  QString hint_;
  std::function<void(const QRect&)> done_;
  QPoint from_, to_;
  bool dragging_ = false;
  QRect picked_;
};

// Shows AreaOverlay on the screen under the mouse and hands the area and the picture under it to `done`.
void selectOnScreen(QWidget* window, const QString& hint, Done done) {
  // macOS shows Vyrn other apps' windows only with Screen Recording (the picture would hold just the desktop).
  if (!app_platform::screenRecordingAllowed()) {
    app_platform::requestScreenRecording();   // macOS asks, the first time
    QMessageBox box(QMessageBox::Information, QStringLiteral("Vyrn needs Screen Recording"),
                    QStringLiteral("To pick an area of your game, Vyrn needs to see the screen. Turn on Vyrn in System "
                                   "Settings → Privacy & Security → Screen Recording, then quit and reopen Vyrn."),
                    QMessageBox::Close, window);
    QPushButton* open = box.addButton(QStringLiteral("Open Screen Recording settings"), QMessageBox::AcceptRole);
    box.exec();
    if (box.clickedButton() == open) {
      QDesktopServices::openUrl(
          QUrl(QStringLiteral("x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture")));
    }
    return;
  }
  QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
  if (!screen) screen = window->screen();
  if (!screen) screen = QGuiApplication::primaryScreen();
  const QPixmap shot = screen ? screen->grabWindow(0) : QPixmap();
  if (shot.isNull()) {
    QMessageBox::warning(window, QStringLiteral("Couldn't pick an area"),
                         QStringLiteral("Vyrn couldn't take a picture of the screen."));
    return;
  }
  window->setProperty(kBusyProperty, true);
  const QRect geometry = screen->geometry();
  const double ratio = screen->devicePixelRatio();
  QPointer<QWidget> owner(window);
  auto* overlay = new AreaOverlay(
      geometry, shot, ratio, hint, [owner, done = std::move(done), shot, geometry, ratio](const QRect& dragged) {
        if (!owner) return;
        owner->setProperty(kBusyProperty, false);
        if (dragged.isEmpty()) return;   // cancelled
        Picked picked;
        picked.area = toArea(dragged, geometry.topLeft(), ratio, areaUnits());
        // The picture at the area's own size: points on macOS (as Pillow's ImageGrab returns them), physical
        // pixels on Windows.
        QImage image = shot.toImage().copy(shotRect(dragged, geometry.size(), shot.size()));
        image.setDevicePixelRatio(1);
        const QSize size(picked.area.width, picked.area.height);
        if (!image.isNull() && image.size() != size)
          image = image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        picked.picture = image;
        QTimer::singleShot(0, owner, [done, picked] { done(picked); });   // after the overlay is gone
      });
  overlay->show();
  overlay->raise();
  overlay->activateWindow();
}

// Hands the rectangle the user drags to `done`: slurp on Wayland, AreaOverlay elsewhere.
void selectArea(QWidget* window, const QString& hint, Done done) {
  if (!window || window->property(kBusyProperty).toBool()) return;
  if (useSlurp())
    selectWithSlurp(window, std::move(done));
  else
    selectOnScreen(window, hint, std::move(done));
}

// Where the first vyrn.screen( ... ) call is in Python source: [start, end) in QString positions, or {-1, -1}.
// Comments and strings are skipped; the closing parenthesis is found by counting.
std::pair<qsizetype, qsizetype> findScreenCall(const QString& code) {
  static const QRegularExpression call(QStringLiteral(R"(\bvyrn\s*\.\s*screen\s*\()"));
  enum class In { Code, Comment, Single, Double, TripleSingle, TripleDouble };
  In in = In::Code;
  qsizetype callStart = -1;
  int depth = 0;
  for (qsizetype i = 0; i < code.size(); ++i) {
    const QChar c = code.at(i);
    switch (in) {
      case In::Comment:
        if (c == QLatin1Char('\n')) in = In::Code;
        continue;
      case In::Single:
      case In::Double: {
        if (c == QLatin1Char('\\')) {
          ++i;
        } else if (c == (in == In::Single ? QLatin1Char('\'') : QLatin1Char('"')) || c == QLatin1Char('\n')) {
          in = In::Code;
        }
        continue;
      }
      case In::TripleSingle:
      case In::TripleDouble: {
        const QString close = in == In::TripleSingle ? QStringLiteral("'''") : QStringLiteral("\"\"\"");
        if (c == QLatin1Char('\\')) {
          ++i;
        } else if (code.mid(i, 3) == close) {
          i += 2;
          in = In::Code;
        }
        continue;
      }
      case In::Code:
        break;
    }
    if (c == QLatin1Char('#')) {
      in = In::Comment;
    } else if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
      const bool triple = code.mid(i, 3) == QString(3, c);
      in = c == QLatin1Char('\'') ? (triple ? In::TripleSingle : In::Single) : (triple ? In::TripleDouble : In::Double);
      if (triple) i += 2;
    } else if (callStart < 0) {
      const QRegularExpressionMatch m = call.match(code, i, QRegularExpression::NormalMatch,
                                                   QRegularExpression::AnchorAtOffsetMatchOption);
      if (m.hasMatch()) {
        callStart = i;
        depth = 1;
        i = m.capturedEnd() - 1;
      }
    } else if (c == QLatin1Char('(')) {
      ++depth;
    } else if (c == QLatin1Char(')')) {
      if (--depth == 0) return {callStart, i + 1};
    }
  }
  return {-1, -1};
}

// QString position -> QScintilla (line, index in characters).
void lineIndexAt(const QString& text, qsizetype pos, int* line, int* index) {
  const qsizetype lineStart = pos > 0 ? text.lastIndexOf(QLatin1Char('\n'), pos - 1) + 1 : 0;
  *line = int(text.left(lineStart).count(QLatin1Char('\n')));
  *index = int(QStringView(text).mid(lineStart, pos - lineStart).toUcs4().size());
}

// Inserts code at the cursor (replacing a selection) as one undoable step.
void insertAtCursor(Editor* editor, const QString& code) {
  editor->beginUndoAction();
  editor->replaceSelectedText(code);
  editor->endUndoAction();
  editor->setFocus();
}

QString screenCall(const Area& a) {
  return QStringLiteral("vyrn.screen(x=%1, y=%2, width=%3, height=%4)").arg(a.x).arg(a.y).arg(a.width).arg(a.height);
}

// The name the user typed as a safe file name in pictures/, with ".png". "" (and a message) when it can't be used.
QString pictureName(QWidget* window, const QString& typed, const QString& picturesDir) {
  QString name = typed.trimmed();
  if (name.isEmpty()) return {};
  if (!name.endsWith(QLatin1String(".png"), Qt::CaseInsensitive)) name += QStringLiteral(".png");
  try {
    (void)vyrn::safe_join(picturesDir.toStdString(), name.toStdString());
  } catch (const std::exception& e) {
    QMessageBox::warning(window, QStringLiteral("Choose another name"), QString::fromStdString(e.what()));
    return {};
  }
  return name;
}

// Saves a Snapshot picture with where it came from (the PNG text "vyrn-area", for on_screen() and people) and inserts
// vyrn.on_screen("<name>", x=…, y=…) at the cursor of `editor` (when it is still open).
void savePicture(QWidget* owner, Editor* editor, const QString& path, const QString& name, const Area& area,
                 QImage image) {
  image.setText(QStringLiteral("vyrn-area"),
                QStringLiteral("%1,%2,%3,%4").arg(area.x).arg(area.y).arg(area.width).arg(area.height));
  if (!image.save(path, "PNG")) {
    QMessageBox::warning(owner, QStringLiteral("Couldn't save the snapshot"),
                         QStringLiteral("Vyrn couldn't write %1.").arg(path));
    return;
  }
  if (editor)
    insertAtCursor(editor, QStringLiteral("vyrn.on_screen(\"%1\", x=%2, y=%3)").arg(name).arg(area.x).arg(area.y));
}

}  // namespace

void pickArea(QWidget* window, Editor* inputs) {
  if (!inputs) return;
  QPointer<Editor> target(inputs);
  const QString hint = QStringLiteral("Drag over the part of the screen the AI should see. Esc cancels.");
  selectArea(window, hint, [target](const Picked& picked) {
    if (!target) return;
    const QString code = target->text();
    const auto [start, end] = findScreenCall(code);
    if (start < 0) {
      insertAtCursor(target, screenCall(picked.area));
      return;
    }
    int lineFrom = 0, indexFrom = 0, lineTo = 0, indexTo = 0;
    lineIndexAt(code, start, &lineFrom, &indexFrom);
    lineIndexAt(code, end, &lineTo, &indexTo);
    target->setSelection(lineFrom, indexFrom, lineTo, indexTo);
    insertAtCursor(target, screenCall(picked.area));
  });
}

void snapshot(QWidget* window, const QString& projectDir, Editor* target) {
  if (!window) return;
  QString grim;   // Wayland: grim takes the picture once the area is named
  if (useSlurp()) {
    grim = QStandardPaths::findExecutable(QStringLiteral("grim"));
    if (grim.isEmpty()) {
      QMessageBox::information(window, QStringLiteral("Snapshot needs grim"), installHint(QStringLiteral("grim")));
      return;
    }
  }
  QPointer<Editor> editor(target);
  QPointer<QWidget> owner(window);
  const QString hint = QStringLiteral("Drag over the part of the screen to save as a picture. Esc cancels.");
  selectArea(window, hint, [owner, editor, projectDir, grim](const Picked& picked) {
    if (!owner) return;
    const Area area = picked.area;
    const QString pictures = QDir(projectDir).filePath(QStringLiteral("pictures"));
    bool ok = false;
    const QString typed = QInputDialog::getText(
        owner, QStringLiteral("Name this snapshot"),
        QStringLiteral("A name for the picture (it is saved in the project's pictures folder):"), QLineEdit::Normal,
        QStringLiteral("game_over.png"), &ok);
    if (!ok || !owner) return;
    const QString name = pictureName(owner, typed, pictures);
    if (name.isEmpty()) return;
    if (!QDir().mkpath(pictures)) {
      QMessageBox::warning(owner, QStringLiteral("Couldn't save the snapshot"),
                           QStringLiteral("Vyrn couldn't create the folder %1.").arg(pictures));
      return;
    }
    const QString path = QDir(pictures).filePath(name);
    if (QFileInfo::exists(path) &&
        QMessageBox::question(owner, QStringLiteral("Replace the picture?"),
                              QStringLiteral("There is already a picture called %1. Replace it?").arg(name),
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
      return;
    }
    if (!picked.picture.isNull()) {   // the overlay's picture of the screen
      savePicture(owner, editor, path, name, area, picked.picture);
      return;
    }

    auto* process = new QProcess(owner);
    QObject::connect(process, &QProcess::errorOccurred, owner, [owner, process](QProcess::ProcessError error) {
      if (error != QProcess::FailedToStart) return;
      process->deleteLater();
      QMessageBox::warning(owner, QStringLiteral("Couldn't take the snapshot"),
                           QStringLiteral("Vyrn couldn't start grim: %1").arg(process->errorString()));
    });
    QObject::connect(
        process, &QProcess::finished, owner,
        [owner, editor, process, path, name, area](int code, QProcess::ExitStatus status) {
          process->deleteLater();
          QImage image;
          if (status != QProcess::NormalExit || code != 0 || !image.load(path)) {
            const QByteArray err = process->readAllStandardError();
            QMessageBox::warning(owner, QStringLiteral("Couldn't take the snapshot"),
                                 QStringLiteral("grim couldn't take the picture.%1")
                                     .arg(err.trimmed().isEmpty() ? QString() : QStringLiteral("\n\n") + firstLines(err)));
            return;
          }
          savePicture(owner, editor, path, name, area, image);
        });
    process->start(grim, {QStringLiteral("-g"),
                          QStringLiteral("%1,%2 %3x%4").arg(area.x).arg(area.y).arg(area.width).arg(area.height),
                          QStringLiteral("-t"), QStringLiteral("png"), path});
  });
}

}  // namespace picktool
