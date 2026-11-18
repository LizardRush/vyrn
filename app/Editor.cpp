// Editor: QScintilla with the Python lexer, Vyrn's colours, vyrn.* autocompletion, error marks and a find bar.
#include "Editor.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPointer>
#include <QResizeEvent>
#include <QSaveFile>
#include <QStringList>
#include <QTimer>
#include <QToolButton>

#include "Paths.h"
#include "Sharing.h"
#include "Theme.h"

namespace {

// Margins, markers, indicators and styles used for problems. Indicators 8-31 and styles above the lexer's are
// free for applications; the annotation styles are set up with raw Scintilla calls (QsciStyle needs a header the
// app doesn't include).
constexpr int kNumberMargin = 0;
constexpr int kMarkerMargin = 1;
constexpr int kErrorMarker = 1;
constexpr int kWarningMarker = 2;
constexpr unsigned kErrorMask = 1u << kErrorMarker;
constexpr unsigned kProblemMask = (1u << kErrorMarker) | (1u << kWarningMarker);
constexpr int kErrorIndicator = 20;
constexpr int kWarningIndicator = 21;
constexpr int kErrorAnnotationStyle = 200;
constexpr int kWarningAnnotationStyle = 201;

// Python names worth completing besides the keywords and the vyrn API.
constexpr const char* kBuiltins[] = {"abs",   "all",    "any",   "bool",  "dict",  "enumerate", "float",
                                     "int",   "len",    "list",  "max",   "min",   "print",     "range",
                                     "round", "sorted", "str",   "sum",   "tuple", "zip",       "random",
                                     "world", "import", "return"};

bool isIdentChar(QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('_'); }

QString withoutEol(QString line) {
  while (line.endsWith(QLatin1Char('\n')) || line.endsWith(QLatin1Char('\r'))) line.chop(1);
  return line;
}

int firstNonSpace(const QString& line) {
  int i = 0;
  while (i < line.size() && line.at(i).isSpace()) ++i;
  return i;
}

int trimmedEnd(const QString& line) {
  int end = int(line.size());
  while (end > 0 && line.at(end - 1).isSpace()) --end;
  return end;
}

// Autocompletion and call tips for vyrn.* (":/vyrn.api", generated from the library) plus Python's keywords.
// Preparing the word list happens on a background thread; the result is cached by content, so only the very
// first editor ever prepares it.
void setUpCompletion(QsciLexerPython* lexer) {
  auto* apis = new QsciAPIs(lexer);
  QFile api(QStringLiteral(":/vyrn.api"));
  const QByteArray apiText = api.open(QIODevice::ReadOnly) ? api.readAll() : QByteArray();
  const QByteArray key = QCryptographicHash::hash(apiText + QSCINTILLA_VERSION_STR, QCryptographicHash::Md5).toHex();
  const QString cache = QDir(paths::cacheDir()).filePath(QStringLiteral("vyrn-api-%1.prepared").arg(QString::fromLatin1(key.left(16))));
  if (apis->isPrepared(cache) && apis->loadPrepared(cache)) return;

  for (const QString& line : QString::fromUtf8(apiText).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) apis->add(line);
  for (const QString& word : QString::fromLatin1(lexer->keywords(1)).split(QLatin1Char(' '), Qt::SkipEmptyParts))
    apis->add(word);
  for (const char* word : kBuiltins) apis->add(QString::fromLatin1(word));
  QObject::connect(apis, &QsciAPIs::apiPreparationFinished, apis, [apis, cache] {
    if (!apis->isPrepared(cache)) apis->savePrepared(cache);
  });
  apis->prepare();
}

}  // namespace

// ---- find bar -------------------------------------------------------------------------------------------------

// A small "find" box in the editor's top-right corner: Enter finds the next match, Shift+Enter the previous one,
// Esc closes it.
class Editor::FindBar : public QFrame {
 public:
  explicit FindBar(Editor* editor) : QFrame(editor), editor_(editor) {
    setObjectName(QStringLiteral("findBar"));
    setStyleSheet(QStringLiteral("QFrame#findBar { background-color: %1; border: 1px solid %2; border-radius: 8px; }")
                      .arg(theme::panelHover().name(), theme::border().name()));
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(6, 4, 4, 4);
    row->setSpacing(2);
    field_ = new QLineEdit(this);
    field_->setPlaceholderText(QStringLiteral("Find in this file"));
    field_->setClearButtonEnabled(true);
    field_->setMinimumWidth(190);
    field_->installEventFilter(this);
    row->addWidget(field_);
    row->addWidget(button(QStringLiteral(":/icons/chevron-up.png"), QStringLiteral("Previous match (Shift+Enter)"),
                          [this] { find(false); }));
    row->addWidget(button(QStringLiteral(":/icons/chevron-down.png"), QStringLiteral("Next match (Enter)"),
                          [this] { find(true); }));
    row->addWidget(button(QStringLiteral(":/icons/close.png"), QStringLiteral("Close (Esc)"), [this] { close(); }));
    // Typing searches as you go, from where the current match starts.
    QObject::connect(field_, &QLineEdit::textEdited, this, [this] { find(true, true); });
    hide();
  }

  void open() {
    const QString selected = editor_->selectedText();
    if (!selected.isEmpty() && !selected.contains(QLatin1Char('\n'))) field_->setText(selected);
    show();
    raise();
    place();
    field_->setFocus();
    field_->selectAll();
  }

  void close() {
    hide();
    editor_->setFocus();
  }

  void place() {
    adjustSize();
    QWidget* view = editor_->viewport();
    const QPoint topRight = view->geometry().topRight();
    move(qMax(view->geometry().left(), topRight.x() - width() - 12), view->geometry().top() + 8);
  }

  void find(bool forward, bool fromSelectionStart = false) {
    const QString text = field_->text();
    const bool found = text.isEmpty() || editor_->findText(text, forward, fromSelectionStart);
    if (field_->property("invalid").toBool() != !found) {
      field_->setProperty("invalid", !found);
      theme::repolish(field_);
    }
  }

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (watched == field_ && event->type() == QEvent::KeyPress) {
      auto* key = static_cast<QKeyEvent*>(event);
      if (key->key() == Qt::Key_Escape) {
        close();
        return true;
      }
      if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
        find(!(key->modifiers() & Qt::ShiftModifier));
        return true;
      }
    }
    return QFrame::eventFilter(watched, event);
  }

 private:
  template <typename Slot>
  QToolButton* button(const QString& icon, const QString& tip, Slot slot) {
    auto* b = new QToolButton(this);
    b->setIcon(QIcon(icon));
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::NoFocus);
    QObject::connect(b, &QToolButton::clicked, this, slot);
    return b;
  }

  Editor* editor_;
  QLineEdit* field_ = nullptr;
};

// ---- editor ---------------------------------------------------------------------------------------------------

Editor::Editor(QWidget* parent) : QsciScintilla(parent) {
  setUtf8(true);
  setEolMode(EolUnix);

  auto* lexer = new QsciLexerPython(this);
  theme::styleLexer(lexer);
  lexer->setIndentationWarning(QsciLexerPython::Inconsistent);   // tabs mixed with spaces get marked
  setLexer(lexer);
  setUpCompletion(lexer);

  // Indentation: 4 spaces, Python-aware auto-indent, Backspace removes one level.
  setIndentationsUseTabs(false);
  setTabWidth(4);
  setIndentationWidth(4);
  setAutoIndent(true);
  setBackspaceUnindents(true);
  setTabIndents(true);
  setIndentationGuides(true);
  setIndentationGuidesForegroundColor(theme::border());
  setIndentationGuidesBackgroundColor(theme::editorPaper());

  // Colours the lexer doesn't cover.
  const QFont mono = theme::monoFont();
  setFont(mono);
  setPaper(theme::editorPaper());
  setColor(theme::text());
  setCaretForegroundColor(theme::accent());
  setCaretWidth(2);
  setCaretLineVisible(true);
  setCaretLineBackgroundColor(theme::editorCurrentLine());
  setSelectionBackgroundColor(QColor(0x1b, 0x4f, 0x4c));
  setSelectionForegroundColor(theme::text());
  setBraceMatching(SloppyBraceMatch);
  setMatchedBraceBackgroundColor(QColor(0x1b, 0x4f, 0x4c));
  setMatchedBraceForegroundColor(theme::text());
  setUnmatchedBraceForegroundColor(theme::danger());
  setUnmatchedBraceBackgroundColor(theme::editorPaper());
  setFolding(NoFoldStyle);
  setWrapMode(WrapNone);
  setScrollWidthTracking(true);
  setScrollWidth(1);
  setFrameShape(QFrame::NoFrame);
  setExtraAscent(1);
  setExtraDescent(1);

  // Margins: line numbers, then the problem markers.
  setMarginsFont(theme::monoFont(10));
  setMarginsBackgroundColor(theme::editorMargin());
  setMarginsForegroundColor(theme::editorLineNumber());
  setMarginType(kNumberMargin, NumberMargin);
  setMarginLineNumbers(kNumberMargin, true);
  setMarginWidth(kNumberMargin, QStringLiteral("00000"));
  setMarginType(kMarkerMargin, SymbolMargin);
  setMarginWidth(kMarkerMargin, 16);
  setMarginMarkerMask(kNumberMargin, 0);
  setMarginMarkerMask(kMarkerMargin, int(kProblemMask));
  setMarginSensitivity(kMarkerMargin, false);
  defineProblemStyles();

  // Autocompletion: vyrn.* names and Python words after 2 letters; call tips while typing arguments.
  setAutoCompletionSource(AcsAll);
  setAutoCompletionThreshold(2);
  setAutoCompletionCaseSensitivity(true);
  setAutoCompletionReplaceWord(false);
  setAutoCompletionUseSingle(AcusNever);
  setCallTipsStyle(CallTipsContext);
  setCallTipsVisible(0);
  setCallTipsPosition(CallTipsBelowText);
  setCallTipsBackgroundColor(theme::panelHover());
  setCallTipsForegroundColor(theme::text());
  setCallTipsHighlightColor(theme::accent());

  // A mark goes away as soon as the user edits its line. Scintilla allows no changes while it reports one, so
  // the clean-up runs right after.
  connect(this, &QsciScintillaBase::SCN_MODIFIED, this,
          [this](int position, int type, const char*, int, int linesAdded, int, int, int, int, int) {
            if (!(type & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))) return;
            int line = 0, index = 0;
            lineIndexFromPosition(position, &line, &index);
            const int last = line + qMax(0, linesAdded);
            QPointer<Editor> self(this);
            QTimer::singleShot(0, this, [self, line, last] {
              if (!self) return;
              for (int l = line; l <= last; ++l) self->clearProblemLine(l);
            });
          });
}

void Editor::defineProblemStyles() {
  markerDefine(Circle, kErrorMarker);
  setMarkerBackgroundColor(theme::danger(), kErrorMarker);
  setMarkerForegroundColor(theme::danger(), kErrorMarker);
  markerDefine(Circle, kWarningMarker);
  setMarkerBackgroundColor(theme::warning(), kWarningMarker);
  setMarkerForegroundColor(theme::warning(), kWarningMarker);

  indicatorDefine(SquigglePixmapIndicator, kErrorIndicator);
  setIndicatorForegroundColor(theme::danger(), kErrorIndicator);
  indicatorDefine(SquigglePixmapIndicator, kWarningIndicator);
  setIndicatorForegroundColor(theme::warning(), kWarningIndicator);

  // Inline messages under the line, in the UI font (plain English reads better in it than in monospace).
  const QFont ui = QApplication::font();
  const QByteArray family = ui.family().toUtf8();
  const int size = qMax(8, ui.pointSize() > 0 ? ui.pointSize() : 10);
  struct Look {
    int style;
    QColor fore, back;
  };
  for (const Look& look : {Look{kErrorAnnotationStyle, QColor(0xff, 0xd9, 0xd9), QColor(0x3a, 0x17, 0x1c)},
                           Look{kWarningAnnotationStyle, QColor(0xfd, 0xe9, 0xb4), QColor(0x33, 0x2a, 0x12)}}) {
    SendScintilla(SCI_STYLESETFORE, look.style, look.fore);
    SendScintilla(SCI_STYLESETBACK, look.style, look.back);
    SendScintilla(SCI_STYLESETFONT, look.style, family.constData());
    SendScintilla(SCI_STYLESETSIZE, look.style, size);
    SendScintilla(SCI_STYLESETEOLFILLED, look.style, 1);
  }
  setAnnotationDisplay(AnnotationBoxed);
}

bool Editor::load(const QString& path, QString* error) {
  path_ = path;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    if (error) {
      *error = file.exists() ? QStringLiteral("Vyrn couldn't open %1: %2").arg(fileName(), file.errorString())
                             : QStringLiteral("%1 doesn't exist yet. It's created when you save.").arg(fileName());
    }
    setText(QString());
    SendScintilla(SCI_EMPTYUNDOBUFFER);
    setModified(false);
    return false;
  }
  setText(QString::fromUtf8(file.readAll()));
  SendScintilla(SCI_EMPTYUNDOBUFFER);
  setModified(false);
  clearProblems();
  return true;
}

bool Editor::save(QString* error) {
  if (path_.isEmpty()) {
    if (error) *error = QStringLiteral("This tab has no file.");
    return false;
  }
  // QSaveFile writes a temporary file and renames it over the old one: a crash never leaves half a script.
  QSaveFile file(path_);
  if (!file.open(QIODevice::WriteOnly) || file.write(text().toUtf8()) < 0 || !file.commit()) {
    if (error) *error = QStringLiteral("Vyrn couldn't save %1: %2").arg(fileName(), file.errorString());
    return false;
  }
  setModified(false);
  Q_EMIT saved();
  return true;
}

bool Editor::reloadIfUnmodified() {
  if (isModified()) return false;
  QFile file(path_);
  if (!file.open(QIODevice::ReadOnly)) return true;   // gone or unreadable: keep what is shown
  const QString onDisk = QString::fromUtf8(file.readAll());
  if (onDisk == text()) return true;
  // Keep the view where it was.
  int line = 0, index = 0;
  getCursorPosition(&line, &index);
  const int first = firstVisibleLine();
  setText(onDisk);
  SendScintilla(SCI_EMPTYUNDOBUFFER);
  setModified(false);
  clearProblems();
  setCursorPosition(qMin(line, qMax(0, lines() - 1)), index);
  setFirstVisibleLine(first);
  return true;
}

QString Editor::path() const { return path_; }

QString Editor::fileName() const { return QFileInfo(path_).fileName(); }

void Editor::markProblem(int line, int col, const QString& message, bool isError) {
  if (lines() <= 0) return;
  // A line past the end (e.g. "unexpected end of file") marks the last line.
  const int l = qBound(0, line - 1, lines() - 1);
  markerAdd(l, isError ? kErrorMarker : kWarningMarker);

  // The squiggle: from `col` to the end of that word, or the whole line when there's no column.
  const QString text = withoutEol(this->text(l));
  int start = col > 0 ? qMin(col - 1, int(text.size())) : firstNonSpace(text);
  int end = trimmedEnd(text);
  if (col > 0 && start < text.size() && isIdentChar(text.at(start))) {
    end = start;
    while (end < text.size() && isIdentChar(text.at(end))) ++end;
  }
  if (end <= start) {
    start = firstNonSpace(text);
    end = trimmedEnd(text);
  }
  if (end > start) fillIndicatorRange(l, start, l, end, isError ? kErrorIndicator : kWarningIndicator);

  // The message under the line (several problems on one line are listed together; an error colours the box).
  const QString existing = annotation(l);
  if (existing.split(QLatin1Char('\n')).contains(message)) return;
  const QString combined = existing.isEmpty() ? message : existing + QLatin1Char('\n') + message;
  const bool error = isError || (markersAtLine(l) & kErrorMask);
  annotate(l, combined, error ? kErrorAnnotationStyle : kWarningAnnotationStyle);
}

void Editor::clearProblems() {
  markerDeleteAll(kErrorMarker);
  markerDeleteAll(kWarningMarker);
  clearAnnotations();
  const int last = qMax(0, lines() - 1);
  const int lastLength = qMax(0, lineLength(last));
  clearIndicatorRange(0, 0, last, lastLength, kErrorIndicator);
  clearIndicatorRange(0, 0, last, lastLength, kWarningIndicator);
}

void Editor::clearProblemLine(int line0) {
  if (line0 < 0 || line0 >= lines() || !(markersAtLine(line0) & kProblemMask)) return;
  markerDelete(line0, kErrorMarker);
  markerDelete(line0, kWarningMarker);
  clearAnnotations(line0);
  const int length = qMax(0, lineLength(line0));
  clearIndicatorRange(line0, 0, line0, length, kErrorIndicator);
  clearIndicatorRange(line0, 0, line0, length, kWarningIndicator);
}

bool Editor::hasProblemAt(int line) const {
  const int l = line - 1;
  return l >= 0 && l < lines() && (markersAtLine(l) & kProblemMask) != 0;
}

void Editor::goToLine(int line, int col) {
  if (lines() <= 0) return;
  const int l = qBound(0, line - 1, lines() - 1);
  const int index = qBound(0, col - 1, qMax(0, int(withoutEol(text(l)).size())));
  setCursorPosition(l, index);
  // Show the line in the middle when it is off screen.
  const int visible = int(SendScintilla(SCI_LINESONSCREEN));
  const int first = firstVisibleLine();
  if (l < first || l >= first + visible) setFirstVisibleLine(qMax(0, l - visible / 2));
  ensureLineVisible(l);
  setFocus();
}

void Editor::showFindBar() {
  if (!findBar_) findBar_ = new FindBar(this);
  findBar_->open();
}

bool Editor::findText(const QString& text, bool forward, bool fromSelectionStart) {
  if (text.isEmpty()) return false;
  int lineFrom = -1, indexFrom = -1, lineTo = -1, indexTo = -1;
  getSelection(&lineFrom, &indexFrom, &lineTo, &indexTo);
  int line = 0, index = 0;
  if (lineFrom < 0) {
    getCursorPosition(&line, &index);
  } else if (forward && !fromSelectionStart) {
    line = lineTo;
    index = indexTo;
  } else {
    line = lineFrom;
    index = indexFrom;
  }
  return findFirst(text, /*re=*/false, /*cs=*/false, /*wo=*/false, /*wrap=*/true, forward, line, index, /*show=*/true);
}

QString Editor::vyrnNameAtCursor() const {
  int line = 0, index = 0;
  getCursorPosition(&line, &index);
  const QString text = withoutEol(this->text(line));
  index = qBound(0, index, int(text.size()));
  int start = index, end = index;
  while (start > 0 && isIdentChar(text.at(start - 1))) --start;
  while (end < text.size() && isIdentChar(text.at(end))) ++end;
  const QString word = text.mid(start, end - start);
  if (word.isEmpty()) return {};
  if (start >= 5 && text.mid(start - 5, 5) == QLatin1String("vyrn.")) return word;   // vyrn.scr|een
  if (word == QLatin1String("vyrn") && end < text.size() && text.at(end) == QLatin1Char('.')) {   // vy|rn.screen
    int nameEnd = end + 1;
    while (nameEnd < text.size() && isIdentChar(text.at(nameEnd))) ++nameEnd;
    return text.mid(end + 1, nameEnd - end - 1);
  }
  // picture.dot(...) / picture.box(...) on a canvas.
  if (start > 0 && text.at(start - 1) == QLatin1Char('.') &&
      (word == QLatin1String("dot") || word == QLatin1String("box"))) {
    return word;
  }
  return {};
}

bool Editor::canInsertFromMimeData(const QMimeData* source) const {
  if (!sharing::packageIn(source).isEmpty()) return false;
  return QsciScintilla::canInsertFromMimeData(source);
}

void Editor::keyPressEvent(QKeyEvent* event) {
  if (event->matches(QKeySequence::Save)) {
    if (isModified() || !QFileInfo::exists(path_)) save();
    event->accept();
    return;
  }
  if (event->matches(QKeySequence::Find)) {
    showFindBar();
    event->accept();
    return;
  }
  if (event->matches(QKeySequence::FindNext) || event->matches(QKeySequence::FindPrevious)) {
    if (findBar_ && findBar_->isVisible()) findBar_->find(event->matches(QKeySequence::FindNext));
    else showFindBar();
    event->accept();
    return;
  }
  if (event->key() == Qt::Key_F1 && event->modifiers() == Qt::NoModifier) {
    Q_EMIT helpRequested(vyrnNameAtCursor());
    event->accept();
    return;
  }
  if (event->key() == Qt::Key_Escape && !isListActive() && !isCallTipActive() && findBar_ && findBar_->isVisible()) {
    findBar_->close();
    event->accept();
    return;
  }
  QsciScintilla::keyPressEvent(event);
}

void Editor::resizeEvent(QResizeEvent* event) {
  QsciScintilla::resizeEvent(event);
  if (findBar_ && findBar_->isVisible()) findBar_->place();
}
