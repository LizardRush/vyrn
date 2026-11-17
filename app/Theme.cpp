// The look: Fusion + a dark palette + ":/style.qss" (its {{tokens}} are filled in from the colours below),
// the editor's colours, and a few small pictures drawn with QPainter.
#include "Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QStyleFactory>
#include <QTimer>
#include <QToolTip>
#include <QWidget>

#include <array>
#include <cmath>
#include <utility>

#include "Qsci.h"

namespace theme {

namespace {

// ---- the palette (one place; style.qss refers to these by name) --------------------------------------------
constexpr QRgb kWindow = 0xff0a0a0a;
constexpr QRgb kPanel = 0xff121212;
constexpr QRgb kPanelHover = 0xff1a1a1a;
constexpr QRgb kPanel2 = 0xff161616;        // menus, popups, tooltips
constexpr QRgb kBase = 0xff050505;          // text fields, lists
constexpr QRgb kButton = 0xff1a1a1a;
constexpr QRgb kButtonHover = 0xff262626;
constexpr QRgb kButtonPressed = 0xff101010;
constexpr QRgb kBorder = 0xff2a2a2a;
constexpr QRgb kBorderStrong = 0xff444444;
constexpr QRgb kText = 0xfff2f2f2;
constexpr QRgb kMuted = 0xff9a9a9a;
constexpr QRgb kFaint = 0xff666666;
constexpr QRgb kAccent = 0xffffffff;
constexpr QRgb kAccentHover = 0xffe0e0e0;
constexpr QRgb kAccentPressed = 0xffbdbdbd;
constexpr QRgb kOnAccent = 0xff000000;      // text on a teal button
constexpr QRgb kSelection = 0xff3a3a3a;     // selected text / items: a dark teal
constexpr QRgb kDanger = 0xfff87171;
constexpr QRgb kWarning = 0xfffbbf24;
constexpr QRgb kSuccess = 0xffcfcfcf;
constexpr QRgb kInfo = 0xffa0a0a0;          // "Simulation" badge
constexpr QRgb kScrollHandle = 0xff333333;
constexpr QRgb kScrollHandleHover = 0xff4d4d4d;

// The editor.
constexpr QRgb kEditorPaper = 0xff060606;
constexpr QRgb kEditorCurrentLine = 0xff111111;
constexpr QRgb kEditorMargin = 0xff0a0a0a;
constexpr QRgb kEditorLineNumber = 0xff5a5a5a;

// Colours of the app icon's chip, reused by the drawn tiles.
const QColor kChipTop(42, 42, 42);
const QColor kChipBottom(17, 17, 17);
const QColor kChipEdge(85, 85, 85);
const QColor kPin(153, 153, 153);
const QColor kGlow(255, 255, 255);

QColor rgb(QRgb value) { return QColor::fromRgba(value); }

QColor withAlpha(QRgb value, int alpha) {
  QColor c = rgb(value);
  c.setAlpha(alpha);
  return c;
}

QString cssColor(const QColor& c) { return c.alpha() == 255 ? c.name(QColor::HexRgb) : c.name(QColor::HexArgb); }

// {{name}} -> colour, for style.qss.
QList<std::pair<QString, QColor>> styleTokens() {
  return {
      {"window", rgb(kWindow)},
      {"panel", rgb(kPanel)},
      {"panelHover", rgb(kPanelHover)},
      {"panel2", rgb(kPanel2)},
      {"base", rgb(kBase)},
      {"button", rgb(kButton)},
      {"buttonHover", rgb(kButtonHover)},
      {"buttonPressed", rgb(kButtonPressed)},
      {"border", rgb(kBorder)},
      {"borderStrong", rgb(kBorderStrong)},
      {"text", rgb(kText)},
      {"muted", rgb(kMuted)},
      {"faint", rgb(kFaint)},
      {"accent", rgb(kAccent)},
      {"accentHover", rgb(kAccentHover)},
      {"accentPressed", rgb(kAccentPressed)},
      {"accentSoft", withAlpha(kAccent, 34)},
      {"accentLine", withAlpha(kAccent, 110)},
      {"onAccent", rgb(kOnAccent)},
      {"selection", rgb(kSelection)},
      {"danger", rgb(kDanger)},
      {"dangerSoft", withAlpha(kDanger, 34)},
      {"dangerLine", withAlpha(kDanger, 110)},
      {"warning", rgb(kWarning)},
      {"warningSoft", withAlpha(kWarning, 32)},
      {"warningLine", withAlpha(kWarning, 100)},
      {"success", rgb(kSuccess)},
      {"successSoft", withAlpha(kSuccess, 30)},
      {"info", rgb(kInfo)},
      {"infoSoft", withAlpha(kInfo, 32)},
      {"scrollHandle", rgb(kScrollHandle)},
      {"scrollHandleHover", rgb(kScrollHandleHover)},
  };
}

QPalette darkPalette() {
  QPalette p;
  auto set = [&p](QPalette::ColorRole role, QColor normal, QColor disabled = QColor()) {
    p.setColor(QPalette::All, role, normal);
    if (disabled.isValid()) p.setColor(QPalette::Disabled, role, disabled);
  };
  set(QPalette::Window, rgb(kWindow));
  set(QPalette::WindowText, rgb(kText), rgb(kFaint));
  set(QPalette::Base, rgb(kBase), rgb(kPanel));
  set(QPalette::AlternateBase, rgb(kPanel));
  set(QPalette::ToolTipBase, rgb(kPanel2));
  set(QPalette::ToolTipText, rgb(kText));
  set(QPalette::PlaceholderText, rgb(kFaint));
  set(QPalette::Text, rgb(kText), rgb(kFaint));
  set(QPalette::Button, rgb(kButton), rgb(kPanel));
  set(QPalette::ButtonText, rgb(kText), rgb(kFaint));
  set(QPalette::BrightText, QColor(Qt::white));
  set(QPalette::Light, rgb(kBorderStrong));
  set(QPalette::Midlight, rgb(kBorder));
  set(QPalette::Mid, rgb(kPanel));
  set(QPalette::Dark, rgb(kBase));
  set(QPalette::Shadow, QColor(0, 0, 0));
  set(QPalette::Highlight, rgb(kSelection), rgb(kBorder));
  set(QPalette::HighlightedText, rgb(kText));
  set(QPalette::Link, rgb(kAccentHover));
  set(QPalette::LinkVisited, rgb(kAccent));
  set(QPalette::Accent, rgb(kAccent));
  return p;
}

QString loadStyleSheet() {
  QFile file(QStringLiteral(":/style.qss"));
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
  QString css = QString::fromUtf8(file.readAll());
  for (const auto& [name, color] : styleTokens())
    css.replace(QStringLiteral("{{%1}}").arg(name), cssColor(color));
  return css;
}

// Paints at 1x and 2x so icons stay sharp on any screen.
template <typename Draw>
QIcon iconFrom(int size, Draw draw) {
  QIcon icon;
  for (qreal dpr : {1.0, 2.0}) {
    QPixmap pm(QSize(size, size) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    draw(p, qreal(size));
    p.end();
    icon.addPixmap(pm);
  }
  return icon;
}

QPixmap blankPixmap(int size, qreal dpr) {
  QPixmap pm(QSize(size, size) * dpr);
  pm.setDevicePixelRatio(dpr);
  pm.fill(Qt::transparent);
  return pm;
}

// A monitor outline in the unit square [0,10]x[0,10] (scaled by `u`), returning the screen rectangle.
QRectF drawMonitor(QPainter& p, qreal u) {
  const QRectF screen(2.0 * u, 2.4 * u, 6.0 * u, 4.2 * u);
  p.setPen(QPen(kPin, 0.55 * u, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  p.setBrush(QColor(8, 8, 8));
  p.drawRoundedRect(screen, 0.7 * u, 0.7 * u);
  p.drawLine(QPointF(5.0 * u, 6.6 * u), QPointF(5.0 * u, 7.5 * u));
  p.drawLine(QPointF(3.7 * u, 7.6 * u), QPointF(6.3 * u, 7.6 * u));
  return screen;
}

}  // namespace

// ---- palette accessors ---------------------------------------------------------------------------------------
QColor accent() { return rgb(kAccent); }
QColor danger() { return rgb(kDanger); }
QColor warning() { return rgb(kWarning); }
QColor muted() { return rgb(kMuted); }
QColor panel() { return rgb(kPanel); }
QColor success() { return rgb(kSuccess); }
QColor text() { return rgb(kText); }
QColor faint() { return rgb(kFaint); }
QColor window() { return rgb(kWindow); }
QColor base() { return rgb(kBase); }
QColor border() { return rgb(kBorder); }
QColor panelHover() { return rgb(kPanelHover); }
QColor editorPaper() { return rgb(kEditorPaper); }
QColor editorCurrentLine() { return rgb(kEditorCurrentLine); }
QColor editorMargin() { return rgb(kEditorMargin); }
QColor editorLineNumber() { return rgb(kEditorLineNumber); }

// ---- setup ------------------------------------------------------------------------------------------------------
void apply(QApplication& app) {
  if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) QApplication::setStyle(fusion);

  // Thin or tiny fonts are hard to read on a dark background: keep the desktop's font family and size, but use
  // at least the normal weight and 10 pt.
  QFont font = QApplication::font();
  bool fontChanged = false;
  if (font.weight() < QFont::Normal) {
    font.setWeight(QFont::Normal);
    fontChanged = true;
  }
  if (font.pointSizeF() > 0 && font.pointSizeF() < 10.0) {
    font.setPointSizeF(10.0);
    fontChanged = true;
  }
  if (fontChanged) QApplication::setFont(font);

  const QPalette palette = darkPalette();
  QApplication::setPalette(palette);
  QToolTip::setPalette(palette);
  app.setStyleSheet(loadStyleSheet());
}

QFont monoFont(int pointSize) {
  static const QString family = [] {
    const QStringList available = QFontDatabase::families();
    for (const char* name : {"DejaVu Sans Mono", "Noto Sans Mono", "Monospace"}) {
      if (available.contains(QLatin1String(name), Qt::CaseInsensitive)) return QString::fromLatin1(name);
    }
    return QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
  }();
  QFont font(family, pointSize);
  font.setStyleHint(QFont::Monospace);
  font.setFixedPitch(true);
  return font;
}

QFont scaledFont(double factor, QFont::Weight weight) {
  QFont font = QApplication::font();
  if (font.pointSizeF() > 0)
    font.setPointSizeF(font.pointSizeF() * factor);
  else if (font.pixelSize() > 0)
    font.setPixelSize(qRound(font.pixelSize() * factor));
  font.setWeight(weight);
  return font;
}

void repolish(QWidget* widget) {
  if (!widget) return;
  widget->style()->unpolish(widget);
  widget->style()->polish(widget);
  widget->update();
}

void styleLexer(QsciLexerPython* lexer) {
  if (!lexer) return;
  const QFont mono = monoFont();
  const QColor paper = editorPaper();
  const QColor code(0xe6, 0xe6, 0xe6);

  // Defaults for every style first (-1), then the individual styles.
  lexer->setDefaultPaper(paper);
  lexer->setDefaultColor(code);
  lexer->setDefaultFont(mono);
  lexer->setPaper(paper, -1);
  lexer->setColor(code, -1);
  lexer->setFont(mono, -1);

  struct StyleColour {
    int style;
    QRgb colour;
    bool italic;
  };
  const QRgb strings = 0xffbdbdbd;
  const std::array<StyleColour, 20> styles{{
      {QsciLexerPython::Default, 0xffe6e6e6, false},
      {QsciLexerPython::Comment, 0xff707070, true},
      {QsciLexerPython::CommentBlock, 0xff707070, true},
      {QsciLexerPython::Number, 0xffd0d0d0, false},
      {QsciLexerPython::DoubleQuotedString, strings, false},
      {QsciLexerPython::SingleQuotedString, strings, false},
      {QsciLexerPython::TripleSingleQuotedString, strings, false},
      {QsciLexerPython::TripleDoubleQuotedString, strings, false},
      {QsciLexerPython::DoubleQuotedFString, strings, false},
      {QsciLexerPython::SingleQuotedFString, strings, false},
      {QsciLexerPython::TripleSingleQuotedFString, strings, false},
      {QsciLexerPython::TripleDoubleQuotedFString, strings, false},
      {QsciLexerPython::Keyword, 0xff2dd4bf, false},
      {QsciLexerPython::ClassName, 0xff5eead4, false},
      {QsciLexerPython::FunctionMethodName, 0xff5eead4, false},
      {QsciLexerPython::Operator, 0xff9a9a9a, false},
      {QsciLexerPython::Identifier, 0xffe6e6e6, false},
      {QsciLexerPython::HighlightedIdentifier, 0xff5eead4, false},
      {QsciLexerPython::Decorator, 0xff14b8a6, false},
      {QsciLexerPython::UnclosedString, kDanger, false},
  }};
  QFont italic = mono;
  italic.setItalic(true);
  for (const StyleColour& s : styles) {
    lexer->setColor(rgb(s.colour), s.style);
    lexer->setPaper(paper, s.style);
    lexer->setFont(s.italic ? italic : mono, s.style);
  }
  lexer->setPaper(QColor(0x2a, 0x10, 0x10), QsciLexerPython::UnclosedString);
}

// ---- drawn pictures ------------------------------------------------------------------------------------------
QPixmap templateIcon(const QString& templateId, const QString& mode, int size, qreal devicePixelRatio) {
  QPixmap pm = blankPixmap(size, devicePixelRatio);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);

  // The tile: the app icon's chip body (soft vertical gradient, thin lighter edge).
  const QRectF tile(0.5, 0.5, size - 1.0, size - 1.0);
  QLinearGradient body(0, 0, 0, size);
  body.setColorAt(0, kChipTop);
  body.setColorAt(1, kChipBottom);
  p.setPen(QPen(kChipEdge, 1.0));
  p.setBrush(body);
  p.drawRoundedRect(tile, size * 0.24, size * 0.24);

  const qreal u = size / 10.0;   // a 10x10 design grid
  const QColor teal = accent();
  const bool realtime = mode == QLatin1String("realtime");
  p.setPen(Qt::NoPen);

  if (templateId == QLatin1String("catch-pictures") || (!realtime && templateId != QLatin1String("catch-numbers"))) {
    // A falling fruit with a fading trail above a basket.
    const qreal cell = 1.25 * u;
    QColor trail = teal;
    trail.setAlpha(55);
    p.setBrush(trail);
    p.drawRoundedRect(QRectF(5.4 * u, 1.9 * u, cell, cell), 0.3 * u, 0.3 * u);
    trail.setAlpha(120);
    p.setBrush(trail);
    p.drawRoundedRect(QRectF(5.4 * u, 3.3 * u, cell, cell), 0.3 * u, 0.3 * u);
    p.setBrush(kGlow);
    p.drawRoundedRect(QRectF(5.4 * u, 4.7 * u, cell, cell), 0.3 * u, 0.3 * u);
    p.setBrush(kPin);
    p.drawRoundedRect(QRectF(3.0 * u, 7.1 * u, 4.0 * u, 1.15 * u), 0.4 * u, 0.4 * u);
  } else if (templateId == QLatin1String("catch-numbers")) {
    // Three labelled number bars.
    const std::array<qreal, 3> lengths{{3.6, 2.2, 4.6}};
    for (int i = 0; i < 3; ++i) {
      const qreal y = (2.6 + i * 1.75) * u;
      p.setBrush(kPin);
      p.drawEllipse(QRectF(2.0 * u, y, 0.9 * u, 0.9 * u));
      QColor bar = teal;
      bar.setAlpha(i == 1 ? 150 : 235);
      p.setBrush(bar);
      p.drawRoundedRect(QRectF(3.4 * u, y, lengths[i] * u, 0.9 * u), 0.45 * u, 0.45 * u);
    }
  } else if (templateId == QLatin1String("demo-game")) {
    // A tiny Dodge: the player at the bottom, a red block falling towards it.
    const QRectF screen = drawMonitor(p, u);
    p.setPen(Qt::NoPen);
    p.setBrush(danger());
    p.drawRoundedRect(QRectF(screen.left() + 1.0 * u, screen.top() + 0.7 * u, 1.1 * u, 1.1 * u), 0.25 * u, 0.25 * u);
    p.setBrush(teal);
    p.drawRoundedRect(QRectF(screen.center().x() + 0.2 * u, screen.bottom() - 1.6 * u, 1.3 * u, 1.0 * u), 0.25 * u,
                      0.25 * u);
  } else {
    // Any real game: a monitor with a play button.
    const QRectF screen = drawMonitor(p, u);
    p.setPen(Qt::NoPen);
    p.setBrush(teal);
    const QPointF c = screen.center();
    QPainterPath play;
    play.moveTo(c.x() - 0.75 * u, c.y() - 1.0 * u);
    play.lineTo(c.x() + 1.05 * u, c.y());
    play.lineTo(c.x() - 0.75 * u, c.y() + 1.0 * u);
    play.closeSubpath();
    p.drawPath(play);
  }
  return pm;
}

QPixmap catchPicture(int size, qreal devicePixelRatio) {
  QPixmap pm = blankPixmap(size, devicePixelRatio);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);

  const QRectF frame(0.5, 0.5, size - 1.0, size - 1.0);
  p.setPen(QPen(border(), 1.0));
  p.setBrush(QColor(5, 5, 5));
  p.drawRoundedRect(frame, size * 0.07, size * 0.07);

  const qreal margin = size * 0.08;
  const qreal cell = (size - 2 * margin) / 10.0;
  auto cellRect = [&](int x, int y) {
    return QRectF(margin + x * cell + 0.5, margin + y * cell + 0.5, cell - 1.0, cell - 1.0);
  };

  // A faint grid shows that the AI sees 10x10 pixels.
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(255, 255, 255, 9));
  for (int y = 0; y < 10; ++y)
    for (int x = 0; x < 10; ++x) p.drawRect(cellRect(x, y));

  // The fruit falls in column 6 (fading trail above it); the basket waits at the bottom.
  QColor trail = accent();
  trail.setAlpha(45);
  p.setBrush(trail);
  p.drawRect(cellRect(6, 2));
  trail.setAlpha(100);
  p.setBrush(trail);
  p.drawRect(cellRect(6, 3));
  p.setBrush(QColor(Qt::white));
  p.drawRect(cellRect(6, 4));
  p.setBrush(QColor(128, 128, 128));
  for (int x = 4; x <= 6; ++x) p.drawRect(cellRect(x, 9));
  return pm;
}

QIcon lineIcon(const QString& name, const QColor& color, int size) {
  return iconFrom(size, [&](QPainter& p, qreal s) {
    const qreal u = s / 16.0;   // drawn on a 16x16 grid
    QPen pen(color, 1.5 * u, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    auto pt = [u](qreal x, qreal y) { return QPointF(x * u, y * u); };
    auto rect = [u](qreal x, qreal y, qreal w, qreal h) { return QRectF(x * u, y * u, w * u, h * u); };

    if (name == QLatin1String("plus")) {
      p.drawLine(pt(8, 3), pt(8, 13));
      p.drawLine(pt(3, 8), pt(13, 8));
    } else if (name == QLatin1String("folder")) {
      QPainterPath path;
      path.moveTo(pt(2, 4.5));
      path.lineTo(pt(6.2, 4.5));
      path.lineTo(pt(7.6, 6));
      path.lineTo(pt(14, 6));
      path.lineTo(pt(14, 12.5));
      path.lineTo(pt(2, 12.5));
      path.closeSubpath();
      p.drawPath(path);
    } else if (name == QLatin1String("dots")) {
      p.setPen(Qt::NoPen);
      p.setBrush(color);
      for (qreal x : {3.5, 8.0, 12.5}) p.drawEllipse(pt(x, 8), 1.4 * u, 1.4 * u);
    } else if (name == QLatin1String("agent")) {
      p.drawRoundedRect(rect(2.5, 5, 11, 8), 2.2 * u, 2.2 * u);
      p.drawLine(pt(8, 5), pt(8, 2.8));
      p.setPen(Qt::NoPen);
      p.setBrush(color);
      p.drawEllipse(pt(8, 2.2), 1.1 * u, 1.1 * u);
      p.drawEllipse(pt(5.8, 9), 1.15 * u, 1.15 * u);
      p.drawEllipse(pt(10.2, 9), 1.15 * u, 1.15 * u);
    } else if (name == QLatin1String("setup")) {
      p.drawRoundedRect(rect(2.5, 2.5, 11, 11), 2.5 * u, 2.5 * u);
      p.drawPolyline(QList<QPointF>{pt(5.2, 8.2), pt(7.2, 10.2), pt(10.8, 6.2)});
    } else if (name == QLatin1String("copy")) {
      p.drawRoundedRect(rect(5.5, 5.5, 8, 8), 1.6 * u, 1.6 * u);
      p.drawPolyline(QList<QPointF>{pt(10.5, 3.5), pt(10.5, 2.5), pt(2.5, 2.5), pt(2.5, 10.5), pt(3.5, 10.5)});
    } else if (name == QLatin1String("check")) {
      QPen bold = pen;
      bold.setWidthF(2.0 * u);
      p.setPen(bold);
      p.drawPolyline(QList<QPointF>{pt(3.5, 8.5), pt(6.5, 11.5), pt(12.5, 4.5)});
    } else if (name == QLatin1String("trash")) {
      p.drawLine(pt(2.5, 4.5), pt(13.5, 4.5));
      p.drawPolyline(QList<QPointF>{pt(6, 4.5), pt(6, 2.5), pt(10, 2.5), pt(10, 4.5)});
      QPainterPath can;
      can.moveTo(pt(4, 4.5));
      can.lineTo(pt(4.8, 13.5));
      can.lineTo(pt(11.2, 13.5));
      can.lineTo(pt(12, 4.5));
      p.drawPath(can);
      p.drawLine(pt(6.8, 7), pt(6.8, 11));
      p.drawLine(pt(9.2, 7), pt(9.2, 11));
    } else if (name == QLatin1String("rename")) {
      QPainterPath pencil;
      pencil.moveTo(pt(3, 13));
      pencil.lineTo(pt(3.5, 10.2));
      pencil.lineTo(pt(10.8, 2.9));
      pencil.lineTo(pt(13.1, 5.2));
      pencil.lineTo(pt(5.8, 12.5));
      pencil.closeSubpath();
      p.drawPath(pencil);
      p.drawLine(pt(9.2, 4.5), pt(11.5, 6.8));
    } else if (name == QLatin1String("open")) {
      p.drawPolyline(QList<QPointF>{pt(9, 2.5), pt(13.5, 2.5), pt(13.5, 7)});
      p.drawLine(pt(13.2, 2.8), pt(7.5, 8.5));
      p.drawPolyline(QList<QPointF>{pt(11.5, 9.5), pt(11.5, 13.5), pt(2.5, 13.5), pt(2.5, 4.5), pt(6.5, 4.5)});
    } else if (name == QLatin1String("share")) {   // an arrow up out of a tray
      p.drawLine(pt(8, 2.5), pt(8, 10));
      p.drawPolyline(QList<QPointF>{pt(5, 5.3), pt(8, 2.3), pt(11, 5.3)});
      p.drawPolyline(QList<QPointF>{pt(5, 8), pt(2.5, 8), pt(2.5, 13.5), pt(13.5, 13.5), pt(13.5, 8), pt(11, 8)});
    } else if (name == QLatin1String("import")) {   // an arrow down into a tray
      p.drawLine(pt(8, 2.5), pt(8, 10));
      p.drawPolyline(QList<QPointF>{pt(5, 7.2), pt(8, 10.2), pt(11, 7.2)});
      p.drawPolyline(QList<QPointF>{pt(2.5, 9.5), pt(2.5, 13.5), pt(13.5, 13.5), pt(13.5, 9.5)});
    }
  });
}

QIcon dotIcon(const QColor& color, int size) {
  return iconFrom(size, [&](QPainter& p, qreal s) {
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(QRectF(1, 1, s - 2, s - 2));
  });
}

// ---- shared pieces --------------------------------------------------------------------------------------------
QWidget* copyLine(const QString& command, QWidget* parent) {
  auto* box = new QWidget(parent);
  auto* row = new QHBoxLayout(box);
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(8);

  auto* code = new QLabel(command, box);
  code->setProperty("role", "code");
  code->setTextFormat(Qt::PlainText);
  code->setWordWrap(true);
  code->setTextInteractionFlags(Qt::TextSelectableByMouse);
  code->setCursor(Qt::IBeamCursor);
  // Monospace looks bigger than the UI font at the same size: a little smaller keeps commands on one line.
  const double appPoints = QApplication::font().pointSizeF() > 0 ? QApplication::font().pointSizeF() : 10.0;
  code->setFont(monoFont(qMax(9, qRound(appPoints * 0.84))));
  code->setMinimumWidth(160);
  code->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

  auto* copy = new QPushButton(QStringLiteral("Copy"), box);
  copy->setIcon(lineIcon(QStringLiteral("copy"), muted()));
  copy->setToolTip(QStringLiteral("Copy this line, then paste it into a terminal"));
  copy->setAccessibleName(QStringLiteral("Copy: %1").arg(command));
  copy->setMinimumWidth(copy->sizeHint().width() + copy->fontMetrics().horizontalAdvance(QStringLiteral("ied")));
  QObject::connect(copy, &QPushButton::clicked, copy, [copy, command] {
    QGuiApplication::clipboard()->setText(command);
    copy->setText(QStringLiteral("Copied"));
    copy->setIcon(lineIcon(QStringLiteral("check"), success()));
    QTimer::singleShot(1600, copy, [copy] {
      copy->setText(QStringLiteral("Copy"));
      copy->setIcon(lineIcon(QStringLiteral("copy"), muted()));
    });
  });

  row->addWidget(code, 1);
  row->addWidget(copy, 0, Qt::AlignVCenter);
  return box;
}

void fitDialogHeight(QWidget* dialog, int width, bool growOnly) {
  QLayout* outer = dialog ? dialog->layout() : nullptr;
  if (!outer) return;
  dialog->ensurePolished();   // the stylesheet's paddings count
  const QMargins margins = outer->contentsMargins();
  const int inner = width - margins.left() - margins.right();
  auto heightAt = [inner](QLayoutItem* item) {
    return item->hasHeightForWidth() ? item->heightForWidth(inner) : item->sizeHint().height();
  };
  int wanted = margins.top() + margins.bottom();
  int shown = 0;
  for (int i = 0; i < outer->count(); ++i) {
    QLayoutItem* item = outer->itemAt(i);
    if (item->isEmpty()) continue;
    if (auto* scroll = qobject_cast<QScrollArea*>(item->widget()); scroll && scroll->widget()) {
      // Measured as if the scroll bar were showing (text wraps a little sooner then), so that it isn't needed.
      QWidget* content = scroll->widget();
      const int width = inner - scroll->verticalScrollBar()->sizeHint().width();
      const int h = content->hasHeightForWidth() ? content->heightForWidth(width) : -1;
      wanted += h > 0 ? h : content->sizeHint().height();
    } else {
      wanted += heightAt(item);
    }
    ++shown;
  }
  wanted += qMax(0, shown - 1) * qMax(0, outer->spacing()) + 4;
  const int screenHeight = dialog->screen() ? dialog->screen()->availableGeometry().height() : 900;
  wanted = qMin(wanted, screenHeight - 60);
  if (!growOnly || wanted > dialog->height()) dialog->resize(width, growOnly ? qMax(wanted, dialog->height()) : wanted);
}

}  // namespace theme
