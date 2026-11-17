// The look: Fusion style, a dark palette, one accent colour, and the editor's colours.
#pragma once

#include <QColor>
#include <QFont>
#include <QIcon>
#include <QPixmap>
#include <QString>

class QApplication;
class QsciLexerPython;
class QWidget;

namespace theme {

void apply(QApplication& app);          // Fusion + dark QPalette + stylesheet (":/style.qss" when present)
QColor accent();                        // teal #2dd4bf
QColor danger();                        // errors, the "AI is pressing keys" banner
QColor warning();                       // warnings
QColor muted();                         // secondary text
QColor panel();                         // card / panel background
QFont monoFont(int pointSize = 11);     // first available of DejaVu Sans Mono, Noto Sans Mono, Monospace
void styleLexer(QsciLexerPython* lexer);  // dark colours for every Python style (set on the lexer, not the editor)

// ---- more of the palette (the stylesheet uses the same values) ----
QColor success();                       // ticks, "ready"
QColor text();                          // main text
QColor faint();                         // hints, disabled text
QColor window();                        // window background
QColor base();                          // text fields, lists
QColor border();                        // hairlines around panels and fields
QColor panelHover();                    // a card under the mouse
QColor editorPaper();                   // editor background (also used by styleLexer)
QColor editorCurrentLine();             // the caret line in the editor
QColor editorMargin();                  // line-number margin background
QColor editorLineNumber();              // line-number text

// The application font scaled by `factor` with the given weight (titles, small captions).
QFont scaledFont(double factor, QFont::Weight weight = QFont::Normal);
// Re-applies the stylesheet after a widget's "role"/"tone" property changed while it was visible.
void repolish(QWidget* widget);

// ---- small pictures drawn in code (crisp at any scale) ----
// A rounded tile for a project or template: a tiny scene for the known templates, else one per mode
// ("sim" | "realtime"). `size` is in device-independent pixels.
QPixmap templateIcon(const QString& templateId, const QString& mode, int size, qreal devicePixelRatio = 1.0);
// "What the AI sees" in Catch (pictures): a 10x10 picture with a falling fruit and the basket.
QPixmap catchPicture(int size, qreal devicePixelRatio = 1.0);
// Simple line icons in one colour: "plus", "folder", "dots", "agent", "setup", "copy", "check", "trash",
// "rename", "duplicate", "open".
QIcon lineIcon(const QString& name, const QColor& color, int size = 16);
// A filled dot (status lights on buttons).
QIcon dotIcon(const QColor& color, int size = 10);

// A command shown in a monospace box (selectable, wraps when long) with a "Copy" button next to it. The button
// copies the exact text to the clipboard and says "Copied" for a moment.
QWidget* copyLine(const QString& command, QWidget* parent = nullptr);

// Sizes a dialog whose middle part is a QScrollArea so that everything shows without scrolling, as far as the
// screen allows. growOnly: never make it smaller (e.g. after more content appeared).
void fitDialogHeight(QWidget* dialog, int width, bool growOnly = false);

}  // namespace theme
