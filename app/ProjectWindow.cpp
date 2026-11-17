// ProjectWindow: the IDE and the dashboard for one project (docs/dev/app-internals.md section 4).
// The window talks to the core only through SessionBridge, so nothing here blocks the UI thread. All widgets and
// run state live in ProjectWindow::Ui.
#include "ProjectWindow.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFontMetrics>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMetaMethod>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSlider>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "AppPlatform.h"
#include "Background.h"
#include "ChoiceBars.h"
#include "CompileDialog.h"
#include "ConsoleView.h"
#include "Editor.h"
#include "OpenPackageDialog.h"
#include "Paths.h"
#include "PickTool.h"
#include "PlotWidget.h"
#include "ProblemsView.h"
#include "ProjectStore.h"
#include "SeesView.h"
#include "SessionBridge.h"
#include "SettingsDialog.h"
#include "ShareDialog.h"
#include "Sharing.h"
#include "StartProgram.h"
#include "Theme.h"
#include "TrustDialog.h"
#include "vyrn/package.h"
#include "vyrn/platform.h"

namespace {

using Action = vyrn::RunConfig::Action;

// The panic shortcut people can rely on: "" when the system refused the app's own hotkey (macOS, Windows).
QString workingPanicShortcut() {
  if (qApp && !qApp->property(ProjectWindow::kPanicKeyProblem).toString().isEmpty()) return {};
  return app_platform::panicShortcut();
}

// The panic key: kills the Python workers this app started, with everything they started. Every worker has a pid
// file <runtime dir>/<pid>.pid (docs/dev/core-internals.md); only those whose process is a child of this app count.
// The panic marker goes first, so the core reports their end as a panic stop. Returns how many it killed.
int killOwnWorkers() {
  const QDir runtime(QString::fromStdString(vyrn::platform::runtime_dir()));
  static const QRegularExpression pidFile(QStringLiteral("^([0-9]+)\\.pid$"));
  QList<qint64> own;
  for (const QString& name : runtime.entryList({QStringLiteral("*.pid")}, QDir::Files)) {
    const QRegularExpressionMatch m = pidFile.match(name);
    bool ok = false;
    const qint64 pid = m.hasMatch() ? m.captured(1).toLongLong(&ok) : 0;
    if (ok && pid > 0 && app_platform::isOwnChild(pid)) own << pid;
  }
  if (own.isEmpty()) return 0;
  const std::string marker = runtime.filePath(QStringLiteral("panic")).toStdString();
  vyrn::platform::write_file_atomic(marker, marker + ".tmp", std::string(), 0600);
  int killed = 0;
  for (const qint64 pid : own) killed += vyrn::platform::stop_process_tree(pid, true) ? 1 : 0;
  return killed;
}

// Every project has the first three; simulations also have world.py. They are always open, in this order.
const std::array<QLatin1StringView, 4> kMainScripts = {QLatin1StringView("inputs.py"), QLatin1StringView("responses.py"),
                                                       QLatin1StringView("points.py"), QLatin1StringView("world.py")};

int mainRank(const QString& file) {
  for (size_t i = 0; i < kMainScripts.size(); ++i)
    if (file == kMainScripts[i]) return int(i);
  return int(kMainScripts.size());
}

bool isMainScript(const QString& file) { return mainRank(file) < int(kMainScripts.size()); }

// AGENTS.md, CLAUDE.md, CLAUDE.local.md, GEMINI.md (any case, any folder): Vyrn's notes for AI coding agents.
bool isAgentFile(const QString& file) {
  const QString name = QFileInfo(file).fileName().toLower();
  return name == QLatin1String("agents.md") || name == QLatin1String("claude.md") ||
         name == QLatin1String("claude.local.md") || name == QLatin1String("gemini.md");
}

QString purposeOf(const QString& file) {
  switch (mainRank(file)) {
    case 0: return QStringLiteral("what the AI sees");
    case 1: return QStringLiteral("what its choices do");
    case 2: return QStringLiteral("how it scores");
    case 3: return QStringLiteral("the simulated game");
    default: return {};
  }
}

// Watching a simulation: multiples of its decisions per second; 0 = as fast as possible.
constexpr std::array<double, 7> kSpeeds = {0.5, 1, 2, 5, 10, 50, 0};
constexpr int kDefaultSpeed = 1;

// Settings → Start program: the moment the game gets to open before the Check (or the countdown) begins.
constexpr int kGameOpenMs = 2'000;

QString speedText(double speed) {
  return speed <= 0 ? QStringLiteral("Max") : QStringLiteral("%1×").arg(QString::number(speed));
}

QString rgba(const QColor& c, int alpha) {
  return QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(alpha);
}

QString rate(double perSecond) {
  if (perSecond < 10) return QString::number(perSecond, 'f', 1);
  return QLocale().toString(qint64(std::llround(perSecond)));
}

QString plural(qint64 n, const QString& one, const QString& many) {
  return QStringLiteral("%1 %2").arg(n).arg(n == 1 ? one : many);
}

QString holdReasonText(const QString& reason) {
  if (reason == QLatin1String("unfocused")) return QStringLiteral("the game window isn't the active window");
  if (reason == QLatin1String("user")) return QStringLiteral("you clicked Vyrn");
  if (reason == QLatin1String("paused")) return QStringLiteral("the run is paused");
  if (reason == QLatin1String("late")) return QStringLiteral("the game answered too slowly");
  if (reason == QLatin1String("game")) return QStringLiteral("Vyrn waits for the game to connect");
  return reason.isEmpty() ? QStringLiteral("on hold") : reason;
}

// "a 10×10 picture" / "3 numbers" from the spec's observation (shape [H, W, 3] or [D]).
QString describeObservation(const QJsonObject& spec) {
  const QJsonObject obs = spec.value(QLatin1String("obs")).toObject();
  const QJsonArray shape = obs.value(QLatin1String("shape")).toArray();
  const QString kind = obs.value(QLatin1String("kind")).toString();
  if (kind == QLatin1String("picture") && shape.size() >= 2)
    return QStringLiteral("a %1×%2 picture").arg(shape.at(1).toInt()).arg(shape.at(0).toInt());
  if (kind == QLatin1String("numbers") && !shape.isEmpty())
    return plural(shape.at(0).toInt(), QStringLiteral("number"), QStringLiteral("numbers"));
  return {};
}

QStringList choiceLabels(const QJsonObject& spec) {
  QJsonArray list = spec.value(QLatin1String("labels")).toArray();
  if (list.isEmpty()) list = spec.value(QLatin1String("choices")).toArray();
  QStringList labels;
  for (const QJsonValue& v : std::as_const(list)) labels.append(v.toString());
  return labels;
}

// Toolbar icons drawn in code: crisp at any scale, in the theme's colours.
QIcon toolIcon(const QString& name, const QColor& color) {
  constexpr int kSize = 18;
  constexpr qreal kDpr = 2.0;
  QPixmap pixmap(QSize(kSize, kSize) * kDpr);
  pixmap.setDevicePixelRatio(kDpr);
  pixmap.fill(Qt::transparent);
  QPainter p(&pixmap);
  p.setRenderHint(QPainter::Antialiasing, true);
  QPen pen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
  if (name == QLatin1String("check")) {
    p.drawPolyline(QPolygonF({QPointF(3.5, 9.5), QPointF(7.5, 13.5), QPointF(14.5, 4.5)}));
  } else if (name == QLatin1String("train")) {
    QPainterPath path;
    path.moveTo(5, 3.5);
    path.lineTo(15, 9);
    path.lineTo(5, 14.5);
    path.closeSubpath();
    p.setBrush(color);
    p.drawPath(path);
  } else if (name == QLatin1String("watch")) {
    QPainterPath eye;
    eye.moveTo(1.8, 9);
    eye.quadTo(9, 1.8, 16.2, 9);
    eye.quadTo(9, 16.2, 1.8, 9);
    p.drawPath(eye);
    p.setBrush(color);
    p.drawEllipse(QPointF(9, 9), 2.4, 2.4);
  } else if (name == QLatin1String("stop")) {
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawRoundedRect(QRectF(4.5, 4.5, 9, 9), 1.8, 1.8);
  } else if (name == QLatin1String("area")) {
    pen.setDashPattern({2.0, 1.6});
    p.setPen(pen);
    p.drawRect(QRectF(2.5, 3.5, 13, 11));
  } else if (name == QLatin1String("snapshot")) {
    p.drawRoundedRect(QRectF(2, 5.5, 14, 9.5), 2, 2);
    p.drawPolyline(QPolygonF({QPointF(6, 5.5), QPointF(7.3, 3.3), QPointF(10.7, 3.3), QPointF(12, 5.5)}));
    p.drawEllipse(QPointF(9, 10.2), 2.6, 2.6);
  } else if (name == QLatin1String("brain")) {
    const QPointF a(4, 4.5), b(4, 13.5), c(14, 9);
    p.drawLine(a, c);
    p.drawLine(b, c);
    p.drawLine(a, b);
    p.setBrush(color);
    for (const QPointF& at : {a, b, c}) p.drawEllipse(at, 2.2, 2.2);
  } else if (name == QLatin1String("settings")) {
    p.setPen(QPen(color, 2.6, Qt::SolidLine, Qt::FlatCap));
    for (int i = 0; i < 8; ++i) {
      const double angle = i * std::numbers::pi / 4;
      p.drawLine(QPointF(9 + 5.0 * std::cos(angle), 9 + 5.0 * std::sin(angle)),
                 QPointF(9 + 7.6 * std::cos(angle), 9 + 7.6 * std::sin(angle)));
    }
    p.setPen(pen);
    p.drawEllipse(QPointF(9, 9), 5.0, 5.0);
    p.drawEllipse(QPointF(9, 9), 1.9, 1.9);
  } else if (name == QLatin1String("compile")) {
    // A page with a folded corner and a terminal prompt: a program (play.py) of its own.
    QPainterPath page;
    page.moveTo(4, 2.5);
    page.lineTo(10.5, 2.5);
    page.lineTo(14, 6);
    page.lineTo(14, 15.5);
    page.lineTo(4, 15.5);
    page.closeSubpath();
    p.drawPath(page);
    p.drawPolyline(QPolygonF({QPointF(10.5, 2.5), QPointF(10.5, 6), QPointF(14, 6)}));
    p.drawPolyline(QPolygonF({QPointF(6.3, 8.6), QPointF(8.4, 10.4), QPointF(6.3, 12.2)}));
    p.drawLine(QPointF(9.6, 12.4), QPointF(11.8, 12.4));
  }
  p.end();
  return QIcon(pixmap);
}

// A slim coloured bar above the editor: a message on the left, buttons on the right.
struct Bar {
  QFrame* frame = nullptr;
  QLabel* text = nullptr;
  QHBoxLayout* layout = nullptr;
};

void setBarTone(const Bar& bar, const QColor& tone) {
  bar.frame->setStyleSheet(QStringLiteral("QFrame#%1 { background-color: %2; border: 1px solid %3; border-radius: 6px; }")
                               .arg(bar.frame->objectName(), rgba(tone, 38), rgba(tone, 120)));
}

Bar makeBar(QWidget* parent, const QString& name, const QColor& tone) {
  Bar bar;
  bar.frame = new QFrame(parent);
  bar.frame->setObjectName(name);
  setBarTone(bar, tone);
  bar.layout = new QHBoxLayout(bar.frame);
  bar.layout->setContentsMargins(12, 5, 6, 5);
  bar.layout->setSpacing(8);
  bar.text = new QLabel(bar.frame);
  bar.text->setWordWrap(true);
  bar.text->setTextFormat(Qt::PlainText);
  bar.layout->addWidget(bar.text, 1);
  bar.frame->hide();
  return bar;
}

QToolButton* hideButton(QWidget* parent) {
  auto* button = new QToolButton(parent);
  button->setIcon(QIcon(QStringLiteral(":/icons/close.png")));
  button->setAutoRaise(true);
  button->setToolTip(QStringLiteral("Hide"));
  return button;
}

QPixmap thumbnail(const vyrn::Picture& picture, int maxSide) {
  if (picture.w <= 0 || picture.h <= 0 || picture.rgb.size() < size_t(picture.w) * size_t(picture.h) * 3) return {};
  QImage image(picture.w, picture.h, QImage::Format_RGB888);
  const size_t row = size_t(picture.w) * 3;
  for (int y = 0; y < picture.h; ++y) std::copy_n(picture.rgb.data() + size_t(y) * row, row, image.scanLine(y));
  const int longest = std::max(picture.w, picture.h);
  const double scale = longest >= maxSide ? double(maxSide) / longest : std::floor(double(maxSide) / longest);
  return QPixmap::fromImage(image.scaled(std::max(1, int(picture.w * scale)), std::max(1, int(picture.h * scale)),
                                         Qt::IgnoreAspectRatio, Qt::FastTransformation));
}

}  // namespace

// ---- all of the window's parts and state ----------------------------------------------------------------------

struct ProjectWindow::Ui {
  enum class Then { Nothing, Train, Watch };   // what starts once the running Check passes
  enum class Tone { Good, Info, Bad };

  ProjectWindow* w = nullptr;
  QString dir, name;
  bool sim = false;
  SessionBridge* bridge = nullptr;

  // Toolbar
  QAction *checkAct = nullptr, *trainAct = nullptr, *watchAct = nullptr, *stopAct = nullptr;
  QAction *pickAct = nullptr, *snapshotAct = nullptr, *settingsAct = nullptr, *speedAct = nullptr;
  QToolButton* brainButton = nullptr;
  QMenu* brainMenu = nullptr;
  QToolButton* shareButton = nullptr;
  QMenu* shareMenu = nullptr;
  QAction* compileAct = nullptr;   // Compile: the trained AI as a Python program (CompileDialog)
  QSlider* speed = nullptr;
  QLabel* speedLabel = nullptr;

  // Above the editor
  QPushButton* keysBanner = nullptr;
  Bar trustBar, lockBar, runBar, linkBar, toast;
  QPushButton* linkCopy = nullptr;
  QPushButton* trustButton = nullptr;
  QPushButton* lockStop = nullptr;
  QPushButton* runApply = nullptr;
  QTimer* toastTimer = nullptr;

  // Editors
  QTabWidget* tabs = nullptr;
  QMenu* moreMenu = nullptr;
  QFileSystemWatcher* watcher = nullptr;

  // Right dock
  QDockWidget* rightDock = nullptr;
  SeesView* sees = nullptr;
  ChoiceBars* choices = nullptr;
  QLabel* valuesHint = nullptr;
  QTreeWidget* values = nullptr;
  QListWidget* pictures = nullptr;

  // Bottom dock
  QDockWidget* bottomDock = nullptr;
  QTabWidget* bottom = nullptr;
  ConsoleView* console = nullptr;
  PlotWidget* plot = nullptr;
  ProblemsView* problems = nullptr;
  QWidget* docsPage = nullptr;
  QComboBox* docsPick = nullptr;
  QTextBrowser* docs = nullptr;

  // Status bar
  QLabel* status = nullptr;
  QLabel* modeBadge = nullptr;

  // The run
  QJsonObject spec;
  int frames = 0;                // pictures the brain sees at once (the spec's "frames"), 0 = unknown
  bool specThisRun = false;      // the running (or last) run sent its spec
  Then then = Then::Nothing;
  bool thenNewBrain = false;
  bool checkPassed = false;
  int runErrors = 0;
  bool allowInput = false;       // this run may press keys (real games, after the safety flow)
  bool stopping = false;
  bool closeAfterStop = false;
  bool closeConfirmed = false;   // unsaved scripts were already dealt with
  bool restartWatch = false;
  bool watchBest = true;
  QString state, stateDetail;
  int countdownLeft = -1;
  bool holding = false;
  QString holdReason;
  qint64 round = 0;
  double best = std::numeric_limits<double>::quiet_NaN();
  double explore = 0, decisionsPerSecond = 0;
  QJsonObject lastTiming, lastUpdate;
  QString lastRunSummary;
  std::vector<std::pair<std::string, std::string>> shownText;
  std::vector<std::string> shownPictureNames;

  // Vyrn Link (docs/dev/link.md): projects whose vyrn.json has "link"
  LinkSettings link;
  QJsonObject linkInfo;          // the run's latest "link" event ({} when no run listens)

  // Sharing and trust (docs/dev/package.md)
  bool trusted = true;           // vyrn::is_trusted(): false for an imported project nobody trusted yet
  QPointer<TrustDialog> trustDialog;
  QString pendingBrain;          // a .vrl waiting for the running Check's spec (Import a brain)
  bool importing = false;        // a brain is being added (on a background thread)

  // Settings → Start program (vyrn.json "start_program"): the program Vyrn starts when Check, Train or Watch
  // begins, then the short wait for it to open ("Starting the game…") before the run itself begins
  StartProgram* game = nullptr;
  QTimer* gameWait = nullptr;
  std::function<void()> afterGameWait;

  // Another program training this project (its lock), and following its rounds.csv
  QTimer* lockTimer = nullptr;
  QTimer* followTimer = nullptr;
  QString lockBy;
  int lockPid = 0;
  QString followFile;
  qint64 followOffset = 0;
  QByteArray followPartial;
  int followRoundCol = -1, followPointsCol = -1, followMeanCol = -1, followBestCol = -1;
  qint64 followRound = 0;
  double followBest = std::numeric_limits<double>::quiet_NaN();

  void build();
  void buildToolbar();
  void buildCentral();
  void buildRightDock();
  void buildBottomDock();
  void connectBridge();

  Editor* editorAt(int index) const { return qobject_cast<Editor*>(tabs->widget(index)); }
  Editor* currentEditor() const { return qobject_cast<Editor*>(tabs->currentWidget()); }
  Editor* openScript(const QString& file, bool select);
  void updateTabTitle(Editor* editor);
  void closeTab(int index);
  void rebuildMoreMenu();
  void newScript();
  void openLocation(const QString& file, int line, int col);
  void fileChanged(const QString& path);
  void reload(Editor* editor);
  void folderChanged();
  void setModeUi();

  bool canBegin();                                       // nothing runs or waits; else a toast says why
  void startGameThen(std::function<void()> next);         // Settings → Start program, then `next`
  bool waitingForGame() const { return gameWait->isActive(); }
  bool cancelGameWait();                                  // true when a run was waiting for the game
  void stopPressed();                                     // Stop (the button, the red banner)
  void closeGame();                                       // ends the Start program when Vyrn started it
  void runCheck(Then next, bool newBrain);
  bool startRun(Action action, bool newBrain, bool keys);
  void startAfterCheck(Then next);
  bool confirmKeys(Then next);
  void stopRun();
  bool panicStop();                                       // the panic key: stop now; false when nothing ran
  void applyChanges();
  void startOver();

  void onState(const QString& newState, const QString& detail);
  void onSpec(const QJsonObject& s);
  void onCheck(const QJsonObject& result);
  void onError(const QJsonObject& e);
  void onWarning(const QJsonObject& e);
  void onLive(const vyrn::LiveView& view);
  void onCheckpoint(const QJsonObject& info);
  void onFinished();
  void onSaved(Editor* editor);

  void clearProblems();
  void addProblem(const QString& kind, const QString& file, int line, int col, const QString& message,
                  const QString& hint);
  void updateProblemsTab();
  void clearValues();
  void updateValues(const vyrn::LiveView& view);

  void updateActions();
  void refreshLink();             // reads vyrn.json "link" again
  void updateLinkBar();
  void updateBanner();
  void updateStatus();
  void updateStatusTip();
  void showToast(const QString& text, Tone tone);
  void showRunBar(const QString& text, bool apply);
  void rebuildBrainMenu();
  void rebuildShareMenu();
  void showGuide(const QString& file);                  // a user guide (docs/user/<file>) in the Docs tab
  void loadDoc(int index, const QString& anchor = QString());

  void refreshTrust();                                  // reads the marker; shows or hides the trust banner
  bool ensureTrusted(const std::function<void()>& next);   // false: the trust dialog opens instead (next on Trust)
  void importBrainNow(const QString& file, const std::string& specJson);
  void openSharedFile(const QString& file);             // dropped on the window

  void pollLock();
  bool otherTrainer() const { return !lockBy.isEmpty(); }
  void stopOtherTrainer();
  void followTick();
  void stopFollowing();
  double currentSpeed() const { return kSpeeds[size_t(std::clamp(speed->value(), 0, int(kSpeeds.size()) - 1))]; }
};

// ---- building the window ---------------------------------------------------------------------------------------

void ProjectWindow::Ui::build() {
  sim = QFileInfo::exists(QDir(dir).filePath(QStringLiteral("world.py")));
  const ProjectCard card = store::project(dir);
  name = card.name.isEmpty() ? QFileInfo(dir).fileName() : card.name;
  w->setWindowTitle(QStringLiteral("%1 — Vyrn").arg(name));
  w->setObjectName(QStringLiteral("projectWindow"));
  w->resize(1280, 820);
  w->setAcceptDrops(true);   // .vrl / .vrlp files (the editors pass them on, see Editor::canInsertFromMimeData)

  bridge = new SessionBridge(w);
  watcher = new QFileSystemWatcher(w);
  game = new StartProgram(w);
  gameWait = new QTimer(w);
  gameWait->setSingleShot(true);
  gameWait->setInterval(kGameOpenMs);
  buildToolbar();
  buildCentral();
  buildRightDock();
  buildBottomDock();

  status = new QLabel(w->statusBar());
  status->setTextFormat(Qt::PlainText);
  status->setContentsMargins(8, 0, 8, 0);
  w->statusBar()->addWidget(status, 1);
  modeBadge = new QLabel(w->statusBar());
  modeBadge->setProperty("role", QStringLiteral("badge"));
  w->statusBar()->addPermanentWidget(modeBadge);
  w->statusBar()->setSizeGripEnabled(false);
  connectBridge();

  for (const QLatin1StringView script : kMainScripts) {
    const QString file(script);
    if (file == QLatin1String("world.py") && !sim) continue;
    openScript(file, false);
  }
  tabs->setCurrentIndex(0);
  watcher->addPath(dir);
  QObject::connect(watcher, &QFileSystemWatcher::fileChanged, w, [this](const QString& path) { fileChanged(path); });
  QObject::connect(watcher, &QFileSystemWatcher::directoryChanged, w, [this] { folderChanged(); });

  QObject::connect(gameWait, &QTimer::timeout, w, [this] {
    const std::function<void()> next = std::exchange(afterGameWait, nullptr);
    updateActions();
    updateStatus();
    if (next) next();
  });
  QObject::connect(game, &StartProgram::ended, w,
                   [this](const QString& message) { console->appendLine(QStringLiteral("app"), message); });

  lockTimer = new QTimer(w);
  lockTimer->setInterval(2000);
  QObject::connect(lockTimer, &QTimer::timeout, w, [this] { pollLock(); });
  lockTimer->start();
  followTimer = new QTimer(w);
  followTimer->setInterval(1000);
  QObject::connect(followTimer, &QTimer::timeout, w, [this] { followTick(); });

  refreshLink();
  setModeUi();
  pollLock();   // also shows the trust banner when the project needs it
  updateActions();
  updateStatus();
  console->appendLine(QStringLiteral("app"),
                      QStringLiteral("Opened %1 (%2).").arg(paths::pretty(dir), sim ? QStringLiteral("a simulation")
                                                                                    : QStringLiteral("a real game")));
}

void ProjectWindow::Ui::buildToolbar() {
  QToolBar* bar = w->addToolBar(QStringLiteral("Run"));
  bar->setObjectName(QStringLiteral("mainToolbar"));
  bar->setMovable(false);
  bar->setFloatable(false);
  bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  bar->setIconSize(QSize(18, 18));
  bar->toggleViewAction()->setEnabled(false);
  const QColor ink = theme::text();

  checkAct = bar->addAction(toolIcon(QStringLiteral("check"), ink), QStringLiteral("Check"));
  checkAct->setToolTip(QStringLiteral("Save and test your scripts: the AI makes a few random choices and Vyrn tells "
                                      "you about any problem"));
  trainAct = bar->addAction(toolIcon(QStringLiteral("train"), theme::accent()), QStringLiteral("Train"));
  trainAct->setToolTip(QStringLiteral("Save, check, then let the AI learn by playing (it continues where it left off)"));
  watchAct = bar->addAction(toolIcon(QStringLiteral("watch"), ink), QStringLiteral("Watch"));
  watchAct->setToolTip(QStringLiteral("Let the trained AI play without learning (the Brain menu picks which brain)"));
  stopAct = bar->addAction(toolIcon(QStringLiteral("stop"), theme::danger()), QStringLiteral("Stop"));
  stopAct->setToolTip(QStringLiteral("Stop. Training saves the brain first."));
  bar->addSeparator();
  pickAct = bar->addAction(toolIcon(QStringLiteral("area"), ink), QStringLiteral("Pick area"));
  snapshotAct = bar->addAction(toolIcon(QStringLiteral("snapshot"), ink), QStringLiteral("Snapshot"));
  bar->addSeparator();

  brainButton = new QToolButton(bar);
  brainButton->setText(QStringLiteral("Brain"));
  brainButton->setIcon(toolIcon(QStringLiteral("brain"), ink));
  brainButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  brainButton->setPopupMode(QToolButton::InstantPopup);
  brainButton->setToolTip(QStringLiteral("Which brain Watch uses, older brains, and starting over"));
  brainMenu = new QMenu(brainButton);
  brainButton->setMenu(brainMenu);
  bar->addWidget(brainButton);

  auto* speedBox = new QWidget(bar);
  auto* speedLayout = new QHBoxLayout(speedBox);
  speedLayout->setContentsMargins(10, 0, 6, 0);
  speedLayout->setSpacing(6);
  auto* speedTitle = new QLabel(QStringLiteral("Watch speed"), speedBox);
  speedTitle->setProperty("role", QStringLiteral("muted"));
  speed = new QSlider(Qt::Horizontal, speedBox);
  speed->setRange(0, int(kSpeeds.size()) - 1);
  speed->setValue(kDefaultSpeed);
  speed->setPageStep(1);
  speed->setFixedWidth(96);
  speed->setToolTip(QStringLiteral("How fast Watch plays the simulation: 0.5× to as fast as the Pi can"));
  speedLabel = new QLabel(speedText(currentSpeed()), speedBox);
  speedLabel->setMinimumWidth(QFontMetrics(speedLabel->font()).horizontalAdvance(QStringLiteral("0.5×")) + 4);
  speedLayout->addWidget(speedTitle);
  speedLayout->addWidget(speed);
  speedLayout->addWidget(speedLabel);
  speedAct = bar->addWidget(speedBox);

  auto* spacer = new QWidget(bar);
  spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  bar->addWidget(spacer);
  compileAct = bar->addAction(toolIcon(QStringLiteral("compile"), ink), QStringLiteral("Compile"));
  compileAct->setObjectName(QStringLiteral("compileAction"));
  shareButton = new QToolButton(bar);
  shareButton->setObjectName(QStringLiteral("shareButton"));
  shareButton->setText(QStringLiteral("Share"));
  shareButton->setIcon(theme::lineIcon(QStringLiteral("share"), ink, 18));
  shareButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  shareButton->setPopupMode(QToolButton::InstantPopup);
  shareButton->setToolTip(QStringLiteral("Save this project or its trained AI as a file to give to others"));
  shareMenu = new QMenu(shareButton);
  shareButton->setMenu(shareMenu);
  bar->addWidget(shareButton);
  settingsAct = bar->addAction(toolIcon(QStringLiteral("settings"), ink), QStringLiteral("Settings"));
  settingsAct->setToolTip(QStringLiteral("Decisions per second, round length, start program, Python"));

  QObject::connect(checkAct, &QAction::triggered, w, [this] { w->check(); });
  QObject::connect(trainAct, &QAction::triggered, w, [this] { w->train(); });
  QObject::connect(watchAct, &QAction::triggered, w, [this] { w->watch(); });
  QObject::connect(stopAct, &QAction::triggered, w, [this] { w->stop(); });
  QObject::connect(compileAct, &QAction::triggered, w, [this] { w->compileProject(); });
  QObject::connect(pickAct, &QAction::triggered, w, [this] {
    if (Editor* inputs = openScript(QStringLiteral("inputs.py"), true)) picktool::pickArea(w, inputs);
  });
  QObject::connect(snapshotAct, &QAction::triggered, w, [this] {
    Editor* target = currentEditor();
    if (!target) target = openScript(QStringLiteral("points.py"), true);
    picktool::snapshot(w, dir, target);
  });
  QObject::connect(settingsAct, &QAction::triggered, w, [this] {
    SettingsDialog dialog(dir, w);
    dialog.exec();
    refreshLink();
    updateStatus();
  });
  QObject::connect(brainMenu, &QMenu::aboutToShow, w, [this] { rebuildBrainMenu(); });
  QObject::connect(shareMenu, &QMenu::aboutToShow, w, [this] { rebuildShareMenu(); });

  // A new speed applies to the next Watch; a Watch that is running restarts with it (after a short pause, so
  // dragging the slider doesn't restart it at every step).
  auto* speedDebounce = new QTimer(w);
  speedDebounce->setSingleShot(true);
  speedDebounce->setInterval(700);
  QObject::connect(speed, &QSlider::valueChanged, w, [this, speedDebounce](int) {
    speedLabel->setText(speedText(currentSpeed()));
    speedDebounce->start();
  });
  QObject::connect(speedDebounce, &QTimer::timeout, w, [this] {
    if (bridge->running() && bridge->action() == Action::Watch && !stopping) {
      restartWatch = true;
      stopRun();
    }
  });
}

void ProjectWindow::Ui::buildCentral() {
  auto* central = new QWidget(w);
  auto* layout = new QVBoxLayout(central);
  layout->setContentsMargins(6, 6, 6, 0);
  layout->setSpacing(6);

  // The red banner while the AI presses keys: clicking it stops the run.
  keysBanner = new QPushButton(central);
  keysBanner->setObjectName(QStringLiteral("keysBanner"));
  keysBanner->setCursor(Qt::PointingHandCursor);
  keysBanner->setFocusPolicy(Qt::NoFocus);
  keysBanner->hide();
  layout->addWidget(keysBanner);
  QObject::connect(keysBanner, &QPushButton::clicked, w, [this] { stopPressed(); });

  // An imported project nobody trusted yet: its Python code doesn't run until the person trusts it.
  trustBar = makeBar(central, QStringLiteral("trustBar"), theme::warning());
  trustButton = new QPushButton(QStringLiteral("Trust this project"), trustBar.frame);
  trustButton->setObjectName(QStringLiteral("trustProjectButton"));
  trustButton->setToolTip(QStringLiteral("Lists its scripts once more, then lets Vyrn run them"));
  trustBar.layout->addWidget(trustButton);
  layout->addWidget(trustBar.frame);
  QObject::connect(trustButton, &QPushButton::clicked, w, [this] { ensureTrusted(nullptr); });

  lockBar = makeBar(central, QStringLiteral("lockBar"), theme::warning());
  lockStop = new QPushButton(QStringLiteral("Stop"), lockBar.frame);
  lockStop->setProperty("role", QStringLiteral("danger"));
  lockBar.layout->addWidget(lockStop);
  layout->addWidget(lockBar.frame);
  QObject::connect(lockStop, &QPushButton::clicked, w, [this] { stopOtherTrainer(); });

  runBar = makeBar(central, QStringLiteral("runBar"), theme::accent());
  runApply = new QPushButton(QStringLiteral("Apply changes"), runBar.frame);
  runApply->setProperty("role", QStringLiteral("primary"));
  runApply->setToolTip(QStringLiteral("Restart Python with your saved scripts. The brain keeps what it learned."));
  runBar.layout->addWidget(runApply);
  QToolButton* runHide = hideButton(runBar.frame);
  runBar.layout->addWidget(runHide);
  layout->addWidget(runBar.frame);
  QObject::connect(runApply, &QPushButton::clicked, w, [this] { applyChanges(); });
  QObject::connect(runHide, &QToolButton::clicked, w, [this] { runBar.frame->hide(); });

  // Vyrn Link: where the game connects, the pairing code, and whether it is connected.
  linkBar = makeBar(central, QStringLiteral("linkBar"), theme::accent());
  linkBar.text->setTextInteractionFlags(Qt::TextSelectableByMouse);
  linkCopy = new QPushButton(QStringLiteral("Copy address"), linkBar.frame);
  linkCopy->setObjectName(QStringLiteral("linkCopyButton"));
  linkCopy->setToolTip(QStringLiteral("Copy the address to type into your game's mod"));
  linkBar.layout->addWidget(linkCopy);
  auto* linkGuide = new QPushButton(QStringLiteral("Guide"), linkBar.frame);
  linkGuide->setToolTip(QStringLiteral("How to connect a game with Vyrn Link (Geometry Dash and others)"));
  linkBar.layout->addWidget(linkGuide);
  layout->addWidget(linkBar.frame);
  QObject::connect(linkGuide, &QPushButton::clicked, w, [this] { showGuide(QStringLiteral("07-geometry-dash.md")); });
  QObject::connect(linkCopy, &QPushButton::clicked, w, [this] {
    const QString url = linkInfo.value(QLatin1String("url")).toString(store::linkUrl(link.port));
    QGuiApplication::clipboard()->setText(url);
    showToast(QStringLiteral("Copied %1").arg(url), Tone::Good);
  });

  toast = makeBar(central, QStringLiteral("toastBar"), theme::success());
  QToolButton* toastHide = hideButton(toast.frame);
  toast.layout->addWidget(toastHide);
  layout->addWidget(toast.frame);
  toastTimer = new QTimer(w);
  toastTimer->setSingleShot(true);
  QObject::connect(toastTimer, &QTimer::timeout, w, [this] { toast.frame->hide(); });
  QObject::connect(toastHide, &QToolButton::clicked, w, [this] { toast.frame->hide(); });

  tabs = new QTabWidget(central);
  tabs->setObjectName(QStringLiteral("editorTabs"));
  tabs->setDocumentMode(true);
  tabs->setTabsClosable(true);
  tabs->setElideMode(Qt::ElideNone);
  tabs->setUsesScrollButtons(true);
  auto* more = new QToolButton(tabs);
  more->setIcon(theme::lineIcon(QStringLiteral("plus"), theme::muted(), 16));
  more->setAutoRaise(true);
  more->setPopupMode(QToolButton::InstantPopup);
  more->setToolTip(QStringLiteral("Open another script of this project, or make a new one"));
  moreMenu = new QMenu(more);
  more->setMenu(moreMenu);
  tabs->setCornerWidget(more, Qt::TopRightCorner);
  layout->addWidget(tabs, 1);
  QObject::connect(moreMenu, &QMenu::aboutToShow, w, [this] { rebuildMoreMenu(); });
  QObject::connect(tabs, &QTabWidget::tabCloseRequested, w, [this](int index) { closeTab(index); });

  w->setCentralWidget(central);
}

void ProjectWindow::Ui::buildRightDock() {
  rightDock = new QDockWidget(QStringLiteral("Dashboard"), w);
  rightDock->setObjectName(QStringLiteral("dashboardDock"));
  rightDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
  rightDock->setTitleBarWidget(new QWidget(rightDock));   // the panels have their own titles
  auto* panel = new QWidget(rightDock);
  auto* layout = new QVBoxLayout(panel);
  layout->setContentsMargins(4, 6, 8, 8);
  layout->setSpacing(10);

  sees = new SeesView(panel);
  layout->addWidget(sees, 3);
  choices = new ChoiceBars(panel);
  layout->addWidget(choices);

  auto* valuesTitle = new QLabel(QStringLiteral("Your values"), panel);
  QFont titleFont = valuesTitle->font();
  titleFont.setWeight(QFont::DemiBold);
  valuesTitle->setFont(titleFont);
  valuesTitle->setContentsMargins(8, 0, 0, 0);
  layout->addWidget(valuesTitle);
  valuesHint = new QLabel(QStringLiteral("vyrn.show(\"name\", value) in a script puts numbers, text and pictures here "
                                         "while the AI plays."),
                          panel);
  valuesHint->setProperty("role", QStringLiteral("faint"));
  valuesHint->setWordWrap(true);
  valuesHint->setContentsMargins(8, 0, 0, 0);
  layout->addWidget(valuesHint);
  values = new QTreeWidget(panel);
  values->setColumnCount(2);
  values->setHeaderHidden(true);
  values->setRootIsDecorated(false);
  values->setUniformRowHeights(true);
  values->setSelectionMode(QAbstractItemView::NoSelection);
  values->setFocusPolicy(Qt::NoFocus);
  values->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  values->header()->setStretchLastSection(true);
  values->hide();
  layout->addWidget(values);
  pictures = new QListWidget(panel);
  pictures->setViewMode(QListView::IconMode);
  pictures->setIconSize(QSize(56, 56));
  pictures->setResizeMode(QListView::Adjust);
  pictures->setMovement(QListView::Static);
  pictures->setSelectionMode(QAbstractItemView::NoSelection);
  pictures->setFocusPolicy(Qt::NoFocus);
  pictures->setFixedHeight(56 + 2 * QFontMetrics(pictures->font()).height() + 10);
  pictures->hide();
  layout->addWidget(pictures);
  layout->addStretch(0);

  panel->setMinimumWidth(250);
  rightDock->setWidget(panel);
  w->addDockWidget(Qt::RightDockWidgetArea, rightDock);
}

void ProjectWindow::Ui::buildBottomDock() {
  bottomDock = new QDockWidget(QStringLiteral("Output"), w);
  bottomDock->setObjectName(QStringLiteral("outputDock"));
  bottomDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
  bottomDock->setTitleBarWidget(new QWidget(bottomDock));
  bottom = new QTabWidget(bottomDock);
  bottom->setObjectName(QStringLiteral("bottomTabs"));
  bottom->setDocumentMode(true);

  console = new ConsoleView(bottom);
  bottom->addTab(console, QStringLiteral("Console"));
  plot = new PlotWidget(bottom);
  bottom->addTab(plot, QStringLiteral("Points"));
  problems = new ProblemsView(bottom);
  bottom->addTab(problems, QStringLiteral("Problems"));

  docsPage = new QWidget(bottom);
  auto* docsLayout = new QVBoxLayout(docsPage);
  docsLayout->setContentsMargins(0, 4, 0, 0);
  docsLayout->setSpacing(4);
  docsPick = new QComboBox(docsPage);
  docsPick->addItem(QStringLiteral("The vyrn library (F1 on a vyrn. name opens its entry)"),
                    QStringLiteral("qrc:/docs/api.html"));
  const QStringList guides = QDir(QStringLiteral(":/guide")).entryList({QStringLiteral("*.md")}, QDir::Files, QDir::Name);
  for (const QString& guide : guides) {
    QString title = QFileInfo(guide).completeBaseName();
    QFile file(QStringLiteral(":/guide/") + guide);
    if (file.open(QIODevice::ReadOnly)) {
      while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.startsWith(QLatin1String("# "))) {
          title = line.mid(2).trimmed();
          break;
        }
      }
    }
    docsPick->addItem(QStringLiteral("Guide: %1").arg(title), QStringLiteral("qrc:/guide/") + guide);
  }
  auto* pickRow = new QHBoxLayout;
  pickRow->setContentsMargins(4, 0, 4, 0);
  pickRow->addWidget(docsPick);
  pickRow->addStretch(1);
  docsLayout->addLayout(pickRow);
  docs = new QTextBrowser(docsPage);
  docs->setOpenExternalLinks(true);
  docsLayout->addWidget(docs, 1);
  bottom->addTab(docsPage, QStringLiteral("Docs"));

  bottomDock->setWidget(bottom);
  w->addDockWidget(Qt::BottomDockWidgetArea, bottomDock);
  w->resizeDocks({rightDock}, {290}, Qt::Horizontal);
  w->resizeDocks({bottomDock}, {250}, Qt::Vertical);

  QObject::connect(console, &ConsoleView::openLocation, w,
                   [this](const QString& file, int line) { openLocation(file, line, 1); });
  QObject::connect(problems, &ProblemsView::openLocation, w,
                   [this](const QString& file, int line, int col) { openLocation(file, line, col); });
  QObject::connect(docsPick, &QComboBox::activated, w, [this](int index) { loadDoc(index); });
  QObject::connect(bottom, &QTabWidget::currentChanged, w, [this](int) {
    if (bottom->currentWidget() == docsPage && docs->source().isEmpty()) loadDoc(docsPick->currentIndex());
  });
}

void ProjectWindow::Ui::connectBridge() {
  QObject::connect(bridge, &SessionBridge::stateChanged, w,
                   [this](const QString& s, const QString& detail) { onState(s, detail); });
  QObject::connect(bridge, &SessionBridge::logLine, w,
                   [this](const QString& stream, const QString& text) { console->appendLine(stream, text); });
  QObject::connect(bridge, &SessionBridge::specReceived, w, [this](const QJsonObject& s) { onSpec(s); });
  QObject::connect(bridge, &SessionBridge::errorReceived, w, [this](const QJsonObject& e) { onError(e); });
  QObject::connect(bridge, &SessionBridge::warningReceived, w, [this](const QJsonObject& e) { onWarning(e); });
  QObject::connect(bridge, &SessionBridge::roundFinished, w, [this](qint64 r, double points, double mean, double b) {
    plot->addRound(r, points, mean);
    round = std::max(round, r);
    if (std::isfinite(b)) best = b;
  });
  QObject::connect(bridge, &SessionBridge::updateReceived, w, [this](const QJsonObject& u) { lastUpdate = u; });
  QObject::connect(bridge, &SessionBridge::timingReceived, w, [this](const QJsonObject& t) {
    lastTiming = t;
    updateStatusTip();
  });
  QObject::connect(bridge, &SessionBridge::countdown, w, [this](int secondsLeft) {
    countdownLeft = secondsLeft;
    updateBanner();
    updateStatus();
  });
  QObject::connect(bridge, &SessionBridge::holdChanged, w, [this](bool on, const QString& reason) {
    holding = on;
    holdReason = reason;
    updateBanner();
    updateStatus();
  });
  QObject::connect(bridge, &SessionBridge::checkpointSaved, w, [this](const QJsonObject& c) { onCheckpoint(c); });
  QObject::connect(bridge, &SessionBridge::checkFinished, w, [this](const QJsonObject& r) { onCheck(r); });
  QObject::connect(bridge, &SessionBridge::linkChanged, w, [this](const QJsonObject& e) {
    if (linkInfo.isEmpty()) {   // the run starts listening
      console->appendLine(QStringLiteral("app"), QStringLiteral("Vyrn Link: connect your game to %1 with the pairing "
                                                                "code %2.")
                                                     .arg(e.value(QLatin1String("url")).toString(),
                                                          e.value(QLatin1String("code")).toString()));
    }
    linkInfo = e;
    updateLinkBar();
  });
  QObject::connect(bridge, &SessionBridge::liveUpdated, w, [this](const vyrn::LiveView& v) { onLive(v); });
  QObject::connect(bridge, &SessionBridge::finished, w, [this] { onFinished(); });
}

// ---- editor tabs -----------------------------------------------------------------------------------------------

Editor* ProjectWindow::Ui::openScript(const QString& file, bool select) {
  if (isAgentFile(file)) return nullptr;   // instructions for AI coding agents, not one of the project's scripts
  if (Editor* open = w->editorFor(file)) {
    if (select) tabs->setCurrentWidget(open);
    return open;
  }
  const QString path = QDir::cleanPath(QDir(dir).filePath(file));
  for (int i = 0; i < tabs->count(); ++i) {   // e.g. "lib/helpers.py", open under its file name
    Editor* open = editorAt(i);
    if (open && QDir::cleanPath(open->path()) == path) {
      if (select) tabs->setCurrentWidget(open);
      return open;
    }
  }
  auto* editor = new Editor(tabs);
  QString error;
  if (!editor->load(path, &error) && QFileInfo::exists(path)) console->appendLine(QStringLiteral("app"), error);

  // The main scripts keep their fixed order in front of any other tab.
  int index = tabs->count();
  if (isMainScript(file)) {
    index = 0;
    while (index < tabs->count() && editorAt(index) && mainRank(editorAt(index)->fileName()) < mainRank(file)) ++index;
  }
  index = tabs->insertTab(index, editor, QString());
  if (isMainScript(file)) {   // the main scripts can't be closed
    tabs->tabBar()->setTabButton(index, QTabBar::RightSide, nullptr);
    tabs->tabBar()->setTabButton(index, QTabBar::LeftSide, nullptr);
  }
  updateTabTitle(editor);
  QObject::connect(editor, &QsciScintilla::modificationChanged, w, [this, editor](bool) { updateTabTitle(editor); });
  QObject::connect(editor, &Editor::saved, w, [this, editor] { onSaved(editor); });
  QObject::connect(editor, &Editor::helpRequested, w, [this](const QString& vyrnName) { w->showHelp(vyrnName); });
  if (QFileInfo::exists(path)) watcher->addPath(path);
  if (select) tabs->setCurrentWidget(editor);
  return editor;
}

void ProjectWindow::Ui::updateTabTitle(Editor* editor) {
  const int index = tabs->indexOf(editor);
  if (index < 0) return;
  const QString file = editor->fileName();
  const QString purpose = purposeOf(file);
  QString text = purpose.isEmpty() ? file : QStringLiteral("%1 — %2").arg(file, purpose);
  if (editor->isModified()) text += QStringLiteral(" •");
  tabs->setTabText(index, text);
  tabs->setTabToolTip(index, paths::pretty(editor->path()) +
                                 (editor->isModified() ? QStringLiteral("\nNot saved yet: Ctrl+S saves") : QString()));
}

void ProjectWindow::Ui::closeTab(int index) {
  Editor* editor = editorAt(index);
  if (!editor || isMainScript(editor->fileName())) return;
  if (editor->isModified()) {
    const auto answer = QMessageBox::question(
        w, QStringLiteral("Save %1?").arg(editor->fileName()),
        QStringLiteral("%1 has changes that aren't saved yet.").arg(editor->fileName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Cancel) return;
    QString error;
    if (answer == QMessageBox::Save && !editor->save(&error)) {
      showToast(error, Tone::Bad);
      return;
    }
  }
  watcher->removePath(editor->path());
  tabs->removeTab(tabs->indexOf(editor));
  editor->deleteLater();
}

void ProjectWindow::Ui::rebuildMoreMenu() {
  moreMenu->clear();
  const QStringList open = w->tabFiles();
  const QStringList files =
      QDir(dir).entryList({QStringLiteral("*.py")}, QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase);
  bool any = false;
  for (const QString& file : files) {
    if (open.contains(file)) continue;
    any = true;
    moreMenu->addAction(file, w, [this, file] { openScript(file, true); });
  }
  if (!any) moreMenu->addAction(QStringLiteral("No other scripts in this project"))->setEnabled(false);
  moreMenu->addSeparator();
  moreMenu->addAction(QStringLiteral("New script…"), w, [this] { newScript(); });
}

void ProjectWindow::Ui::newScript() {
  bool ok = false;
  QString file = QInputDialog::getText(w, QStringLiteral("New script"),
                                       QStringLiteral("Name of the new Python file (your scripts can import it):"),
                                       QLineEdit::Normal, QStringLiteral("helpers.py"), &ok)
                     .trimmed();
  if (!ok || file.isEmpty()) return;
  if (!file.endsWith(QLatin1String(".py"))) file += QStringLiteral(".py");
  static const QRegularExpression valid(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*\\.py$"));
  if (!valid.match(file).hasMatch()) {
    QMessageBox::warning(w, QStringLiteral("New script"),
                         QStringLiteral("Use letters, digits and _ only, and start with a letter: for example "
                                        "helpers.py, which your scripts load with  import helpers"));
    return;
  }
  const QString path = QDir(dir).filePath(file);
  if (!QFileInfo::exists(path)) {
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(QStringLiteral("# %1\n").arg(file).toUtf8()) < 0) {
      QMessageBox::warning(w, QStringLiteral("New script"),
                           QStringLiteral("Vyrn couldn't create %1: %2").arg(file, out.errorString()));
      return;
    }
  }
  openScript(file, true);
}

void ProjectWindow::Ui::openLocation(const QString& file, int line, int col) {
  if (file.isEmpty()) return;
  QString script = file;
  const QFileInfo info(file);
  if (info.isAbsolute()) {   // only scripts directly in this project
    if (QFileInfo(info.absolutePath()).canonicalFilePath() != dir) return;
    script = info.fileName();
  }
  if (script.contains(QLatin1Char('/')) || !script.endsWith(QLatin1String(".py"))) return;
  if (!w->editorFor(script) && !QFileInfo::exists(QDir(dir).filePath(script))) return;
  if (Editor* editor = openScript(script, true)) editor->goToLine(std::max(1, line), std::max(1, col));
}

void ProjectWindow::Ui::fileChanged(const QString& path) {
  for (int i = 0; i < tabs->count(); ++i) {
    Editor* editor = editorAt(i);
    if (editor && editor->path() == path) reload(editor);
  }
  // Saving with a rename (as editors and Vyrn do) ends the watch: watch the new file.
  if (QFileInfo::exists(path) && !watcher->files().contains(path)) watcher->addPath(path);
}

// After a change on disk (another editor, an AI agent): show it, unless the tab has unsaved edits. During a run
// that is like saving: offer Apply changes.
void ProjectWindow::Ui::reload(Editor* editor) {
  const QString before = editor->text();
  if (!editor->reloadIfUnmodified()) {
    showToast(QStringLiteral("%1 changed on disk. Your unsaved changes are kept: saving writes them over it.")
                  .arg(editor->fileName()),
              Tone::Info);
  } else if (editor->text() != before) {
    onSaved(editor);
  }
}

void ProjectWindow::Ui::folderChanged() {
  const QStringList watched = watcher->files();
  for (int i = 0; i < tabs->count(); ++i) {
    Editor* editor = editorAt(i);
    if (!editor || !QFileInfo::exists(editor->path()) || watched.contains(editor->path())) continue;
    watcher->addPath(editor->path());
    reload(editor);
  }
  refreshLink();   // vyrn.json may have changed (it is saved with a rename)
  const bool hasWorld = QFileInfo::exists(QDir(dir).filePath(QStringLiteral("world.py")));
  if (hasWorld != sim) {
    sim = hasWorld;
    if (sim) openScript(QStringLiteral("world.py"), false);
    setModeUi();
    updateActions();
  }
}

void ProjectWindow::Ui::setModeUi() {
  modeBadge->setText(sim ? QStringLiteral("Simulation") : QStringLiteral("Real game"));
  modeBadge->setProperty("tone", sim ? QStringLiteral("sim") : QStringLiteral("real"));
  modeBadge->setToolTip(sim        ? QStringLiteral("world.py runs the game, so Vyrn can play it very fast")
                        : link.on ? QStringLiteral("The game connects to Vyrn with Vyrn Link: it sends its numbers, "
                                                   "and the AI holds its buttons")
                                  : QStringLiteral("The AI plays a real game window: it looks at the screen and presses keys"));
  theme::repolish(modeBadge);
  speedAct->setVisible(sim);
  const QString realOnly = QStringLiteral(" (for real games: projects without world.py)");
  pickAct->setToolTip(QStringLiteral("Drag a rectangle on the screen: inputs.py's vyrn.screen(...) looks there") +
                      (sim ? realOnly : QString()));
  snapshotAct->setToolTip(QStringLiteral("Save a part of the screen as a picture to look for, e.g. \"Game over\"") +
                          (sim ? realOnly : QString()));
}

// ---- runs ------------------------------------------------------------------------------------------------------

bool ProjectWindow::Ui::canBegin() {
  if (bridge->running() || waitingForGame()) {
    showToast(QStringLiteral("Vyrn is already running this project. Stop it first."), Tone::Info);
    return false;
  }
  if (importing) {
    showToast(QStringLiteral("Vyrn is adding a brain to this project. Try again in a moment."), Tone::Info);
    return false;
  }
  return true;
}

// Settings → Start program: when Check, Train or Watch begins, Vyrn starts the project's program (usually the game)
// unless the copy it started earlier is still running, and gives it a moment to open before `next` begins the run.
// Only here, for a project the person trusts (the trust dialog comes first; imports never set start_program):
// vyrn-cli and the MCP server never start it, so AI agents can never launch a program this way.
void ProjectWindow::Ui::startGameThen(std::function<void()> next) {
  const QString command = store::settings(dir).value(QLatin1String("start_program")).toString().trimmed();
  if (command.isEmpty() || !trusted || (game->running() && game->command() == command)) {
    next();
    return;
  }
  QString error;
  if (!game->start(command, dir, &error)) {
    console->appendLine(QStringLiteral("app"), error);
    showToast(error, Tone::Bad);
    next();   // the run goes on: the person can start the program themselves
    return;
  }
  console->appendLine(QStringLiteral("app"),
                      QStringLiteral("Started %1. Vyrn closes it again when you press Stop or close this window.")
                          .arg(command));
  afterGameWait = std::move(next);
  gameWait->start();
  updateActions();
  updateStatus();
}

bool ProjectWindow::Ui::cancelGameWait() {
  if (!waitingForGame()) return false;
  gameWait->stop();
  afterGameWait = nullptr;
  return true;
}

void ProjectWindow::Ui::stopPressed() {
  if (cancelGameWait()) console->appendLine(QStringLiteral("app"), QStringLiteral("Stopped before the run began."));
  stopRun();
  closeGame();
  updateActions();
  updateStatus();
}

void ProjectWindow::Ui::closeGame() {
  if (!game->running()) return;   // never started by Vyrn, or already closed: nothing to do
  console->appendLine(QStringLiteral("app"), QStringLiteral("Closing %1, which Vyrn started.").arg(game->command()));
  game->stop();
}

void ProjectWindow::Ui::runCheck(Then next, bool newBrain) {
  if (!canBegin()) return;
  if (!w->saveAll()) return;
  clearProblems();
  runBar.frame->hide();
  then = next;
  thenNewBrain = newBrain;
  console->appendLine(QStringLiteral("app"), next == Then::Nothing ? QStringLiteral("Checking your scripts…")
                                                                   : QStringLiteral("Checking your scripts first…"));
  if (!startRun(Action::Check, false, false)) then = Then::Nothing;
}

bool ProjectWindow::Ui::startRun(Action action, bool newBrain, bool keys) {
  vyrn::RunConfig config;
  config.action = action;
  config.project_dir = dir.toStdString();
  config.data_dir = paths::dataDir().toStdString();
  config.focus_helper = paths::focusHelper().toStdString();
  config.watch_brain = watchBest ? "best" : "latest";
  config.watch_speed = currentSpeed();
  config.allow_input = keys;
  config.new_brain = newBrain;
  QString error;
  if (!bridge->start(config, &error)) {
    console->appendLine(QStringLiteral("app"), error);
    showToast(error, Tone::Bad);
    updateActions();
    return false;
  }
  allowInput = keys;
  stopping = false;
  specThisRun = false;
  state = QStringLiteral("starting");
  stateDetail.clear();
  countdownLeft = -1;
  holding = false;
  runErrors = 0;
  checkPassed = false;
  if (action != Action::Check) {
    stopFollowing();
    plot->clear();
    round = 0;
    best = std::numeric_limits<double>::quiet_NaN();
    explore = 0;
    decisionsPerSecond = 0;
    lastTiming = QJsonObject();
    lastUpdate = QJsonObject();
    lastRunSummary.clear();
    status->setToolTip(QString());
    runBar.frame->hide();
    if (action == Action::Train)
      console->appendLine(QStringLiteral("app"), newBrain ? QStringLiteral("Training a brand-new brain…")
                                                          : QStringLiteral("Training…"));
    else
      console->appendLine(QStringLiteral("app"), QStringLiteral("Watching the %1 brain play…")
                                                     .arg(watchBest ? QStringLiteral("best") : QStringLiteral("latest")));
  }
  clearValues();
  updateActions();
  updateBanner();
  updateStatus();
  return true;
}

void ProjectWindow::Ui::startAfterCheck(Then next) {
  if (bridge->running() || next == Then::Nothing) return;
  // Real games whose choices press keys: the safety flow (confirmation, then the core's countdown and focus lock).
  const bool keys = spec.value(QLatin1String("mode")).toString() == QLatin1String("realtime") &&
                    spec.value(QLatin1String("uses")).toObject().value(QLatin1String("keys")).toBool();
  if (keys && !confirmKeys(next)) return;
  startRun(next == Then::Train ? Action::Train : Action::Watch, thenNewBrain, keys);
}

bool ProjectWindow::Ui::confirmKeys(Then next) {
  QStringList effects;
  for (const QJsonValue& v : spec.value(QLatin1String("effects")).toArray()) {
    const QString effect = v.toString();
    if (!effect.isEmpty() && effect != QLatin1String("do nothing") && !effects.contains(effect)) effects.append(effect);
  }
  QMessageBox box(w);
  box.setIcon(QMessageBox::Warning);
  box.setWindowTitle(QStringLiteral("The AI will press keys"));
  box.setTextFormat(Qt::RichText);
  box.setText(QStringLiteral("<b>The AI is about to press keys</b>"));
  const QString keys = workingPanicShortcut();
  const QString howToStop =
      keys.isEmpty() ? QStringLiteral("To stop it: click Vyrn or press Stop. ") +
                           qApp->property(ProjectWindow::kPanicKeyProblem).toString()
                     : QStringLiteral("To stop it: click Vyrn, press Stop, or press %1.").arg(keys);
  box.setInformativeText(
      QStringLiteral("After a short countdown it presses keys in the window that is active then%1. Click the game "
                     "window during the countdown.\n\n%2")
          .arg(effects.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(effects.join(QStringLiteral(", "))),
               howToStop));
  QPushButton* go = box.addButton(next == Then::Train ? QStringLiteral("Start training") : QStringLiteral("Start watching"),
                                  QMessageBox::AcceptRole);
  box.addButton(QMessageBox::Cancel);
  box.setDefaultButton(go);
  box.exec();
  return box.clickedButton() == go;
}

void ProjectWindow::Ui::stopRun() {
  if (!bridge->running()) return;
  const Action action = bridge->action();
  if (action == Action::Check) then = Then::Nothing;   // stopping the Check also cancels what would follow it
  if (!stopping) {
    stopping = true;
    bridge->stop();
    console->appendLine(QStringLiteral("app"), action == Action::Train ? QStringLiteral("Stopping: saving the brain…")
                                                                       : QStringLiteral("Stopping…"));
  }
  updateActions();
  updateBanner();
  updateStatus();
}

// Like Stop, at once and without asking, but a game Vyrn started keeps running (as with vyrn-panic on Linux).
bool ProjectWindow::Ui::panicStop() {
  if (!bridge->running() && !waitingForGame()) return false;
  then = Then::Nothing;
  restartWatch = false;
  cancelGameWait();
  stopRun();
  console->appendLine(QStringLiteral("app"), QStringLiteral("Panic stop: Vyrn stopped the AI and let go of every key."));
  showToast(QStringLiteral("Panic stop: the AI stopped pressing keys."), Tone::Bad);
  updateActions();
  updateBanner();
  updateStatus();
  return true;
}

void ProjectWindow::Ui::applyChanges() {
  if (!bridge->running() || bridge->action() == Action::Check || stopping) {
    runBar.frame->hide();
    return;
  }
  if (!w->saveAll()) return;
  runBar.frame->hide();   // after saving: a save during a run offers this bar again
  clearProblems();
  bridge->applyChanges();
  console->appendLine(QStringLiteral("app"),
                      QStringLiteral("Applying your changes: Python restarts with the new scripts, the brain stays."));
}

void ProjectWindow::Ui::startOver() {
  if (bridge->running() || waitingForGame()) return;
  if (!ensureTrusted([this] { startOver(); })) return;
  QMessageBox box(w);
  box.setIcon(QMessageBox::Question);
  box.setWindowTitle(QStringLiteral("Start over?"));
  box.setTextFormat(Qt::RichText);
  box.setText(QStringLiteral("<b>Train a brand-new brain?</b>"));
  box.setInformativeText(QStringLiteral("The new brain starts knowing nothing. The current one is kept: you can pick "
                                        "it again from the Brain menu."));
  QPushButton* go = box.addButton(QStringLiteral("Start over"), QMessageBox::AcceptRole);
  box.addButton(QMessageBox::Cancel);
  box.setDefaultButton(go);
  box.exec();
  if (box.clickedButton() == go && canBegin()) startGameThen([this] { runCheck(Then::Train, true); });
}

// ---- events from the session -----------------------------------------------------------------------------------

void ProjectWindow::Ui::onState(const QString& newState, const QString& detail) {
  state = newState;
  stateDetail = detail;
  if (newState == QLatin1String("stopping")) stopping = true;
  if (newState != QLatin1String("countdown")) countdownLeft = -1;
  if (newState == QLatin1String("running")) holding = false;
  updateActions();
  updateBanner();
  updateStatus();
}

void ProjectWindow::Ui::onSpec(const QJsonObject& s) {
  spec = s;
  choices->setChoices(choiceLabels(s));
  specThisRun = true;
  if (const QJsonValue count = s.value(QLatin1String("frames")); count.isDouble()) {
    frames = std::max(0, count.toInt());
  } else {
    // Older cores: "small CNN, 12x10x10 -> 3 choices" means 12 channels = the last 4 colour frames.
    static const QRegularExpression cnn(QStringLiteral("CNN, (\\d+)x\\d+x\\d+"));
    const QRegularExpressionMatch net = cnn.match(s.value(QLatin1String("net")).toString());
    const int channels = net.hasMatch() ? net.captured(1).toInt() : 0;
    frames = channels > 0 && channels % 3 == 0 ? channels / 3 : 0;
  }
  for (const QJsonValue& v : s.value(QLatin1String("warnings")).toArray()) onWarning(v.toObject());
  if (bridge->action() == Action::Check) return;
  const QString brain = s.value(QLatin1String("brain")).toString();
  const QString why = s.value(QLatin1String("why")).toString();
  if (brain == QLatin1String("new")) {
    console->appendLine(QStringLiteral("app"),
                        QStringLiteral("A new brain: it starts knowing nothing and learns by trying."));
  } else if (brain == QLatin1String("resumed")) {
    console->appendLine(QStringLiteral("app"), QStringLiteral("The brain continues where it left off."));
  } else if (brain == QLatin1String("incompatible")) {
    console->appendLine(QStringLiteral("app"),
                        QStringLiteral("The saved brain doesn't fit these scripts any more%1.")
                            .arg(why.isEmpty() ? QString() : QStringLiteral(": ") + why));
  }
}

void ProjectWindow::Ui::onCheck(const QJsonObject& result) {
  for (const QJsonValue& v : result.value(QLatin1String("warnings")).toArray()) onWarning(v.toObject());
  checkPassed = result.value(QLatin1String("ok")).toBool() && runErrors == 0;
  const int warnings = problems->warningCount();
  QString text;
  if (checkPassed) {
    QStringList parts;
    const qsizetype count = choiceLabels(spec).size();
    if (count > 0) parts.append(plural(count, QStringLiteral("choice"), QStringLiteral("choices")));
    const QString seen = describeObservation(spec);
    if (!seen.isEmpty()) parts.append(QStringLiteral("the AI sees ") + seen);
    text = parts.isEmpty() ? QStringLiteral("Looks good")
                           : QStringLiteral("Looks good: ") + parts.join(QStringLiteral(", "));
    if (warnings > 0)
      text += QStringLiteral(" (%1: see Problems)")
                  .arg(plural(warnings, QStringLiteral("warning"), QStringLiteral("warnings")));
    showToast(text, warnings > 0 ? Tone::Info : Tone::Good);
    if (warnings > 0 && then == Then::Nothing) w->showBottomTab(QStringLiteral("problems"));
  } else {
    text = QStringLiteral("Check found a problem: see Problems.");
    showToast(text, Tone::Bad);
    w->showBottomTab(QStringLiteral("problems"));
  }
  console->appendLine(QStringLiteral("app"), text);
}

void ProjectWindow::Ui::onError(const QJsonObject& e) {
  ++runErrors;
  checkPassed = false;
  if (e.value(QLatin1String("kind")).toString() == QLatin1String("not_trusted") ||
      e.value(QLatin1String("reason")).toString() == QLatin1String("not_trusted")) {
    // The core refuses to run an imported project nobody trusted (docs/dev/package.md section 7).
    then = Then::Nothing;
    QString message = e.value(QLatin1String("message")).toString();
    if (message.isEmpty())
      message = QStringLiteral("Vyrn won't run this project's Python code until you trust it: read its scripts, "
                               "then press Trust this project.");
    console->appendLine(QStringLiteral("app"), message);
    showToast(message, Tone::Bad);
    QTimer::singleShot(0, w, [this] { ensureTrusted(nullptr); });   // the banner, and the dialog to trust it
    return;
  }
  const QString script = e.value(QLatin1String("script")).toString();
  const int line = e.value(QLatin1String("line")).toInt();
  const int col = e.value(QLatin1String("col")).toInt();
  QString message = e.value(QLatin1String("message")).toString();
  const QString exc = e.value(QLatin1String("exc_type")).toString();
  if (!exc.isEmpty() && !message.startsWith(exc)) message = QStringLiteral("%1: %2").arg(exc, message);
  const QString hint = e.value(QLatin1String("hint")).toString();
  const bool fatal = e.value(QLatin1String("fatal")).toBool();
  addProblem(QStringLiteral("error"), script, line, col, message, hint);

  QString where;
  if (!script.isEmpty()) where = line > 0 ? QStringLiteral("%1:%2").arg(script).arg(line) : script;
  console->appendLine(QStringLiteral("stderr"), where.isEmpty() ? message : where + QStringLiteral(": ") + message);
  if (!hint.isEmpty()) console->appendLine(QStringLiteral("app"), QStringLiteral("  ") + hint);
  const QJsonArray trace = e.value(QLatin1String("trace")).toArray();
  if (trace.size() > 1)
    for (const QJsonValue& frame : trace)
      console->appendLine(QStringLiteral("stderr"), QStringLiteral("    ") + frame.toString());

  w->showBottomTab(QStringLiteral("problems"));
  if (runErrors == 1 && line > 0) {   // show the first problem in its script
    if (Editor* editor = w->editorFor(script)) {
      tabs->setCurrentWidget(editor);
      editor->goToLine(line, std::max(1, col));
    }
  }
  const Action action = bridge->action();
  if (action == Action::Check) return;
  const QString what = action == Action::Train ? QStringLiteral("Training") : QStringLiteral("Watching");
  if (fatal) {
    showToast(QStringLiteral("%1 stopped: fix the problem, then press %2 again.")
                  .arg(what, action == Action::Train ? QStringLiteral("Train") : QStringLiteral("Watch")),
              Tone::Bad);
  } else {
    showRunBar(QStringLiteral("%1 is paused: %2 has a problem. Fix it and save, then press Apply changes (the brain "
                              "is kept).")
                   .arg(what, where.isEmpty() ? QStringLiteral("a script") : where),
               true);
  }
}

void ProjectWindow::Ui::onWarning(const QJsonObject& e) {
  const QString message = e.value(QLatin1String("message")).toString();
  if (message.isEmpty()) return;
  addProblem(QStringLiteral("warning"), e.value(QLatin1String("script")).toString(),
             e.value(QLatin1String("line")).toInt(), 0, message, e.value(QLatin1String("hint")).toString());
}

void ProjectWindow::Ui::onLive(const vyrn::LiveView& view) {
  if (view.kind == "picture" && frames > 1 && bridge->action() != Action::Check)
    sees->setPicture(view.input, QStringLiteral("%1×%2 colour, last of %3 frames")
                                     .arg(view.input.w)
                                     .arg(view.input.h)
                                     .arg(frames));
  else
    sees->setLive(view);
  choices->setLive(view);
  updateValues(view);
  if (bridge->action() != Action::Check) {
    round = std::max(round, qint64(view.round));
    if (view.has_best) best = view.best_mean;
    explore = view.explore;
    decisionsPerSecond = view.ticks_per_s;
  }
  updateStatus();
}

void ProjectWindow::Ui::onCheckpoint(const QJsonObject& info) {
  if (info.value(QLatin1String("which")).toString() != QLatin1String("best")) return;
  const QJsonValue value = info.value(QLatin1String("best_mean20"));
  if (value.isDouble())
    console->appendLine(QStringLiteral("app"), QStringLiteral("New best brain saved (mean of the last 20 rounds: %1).")
                                                   .arg(store::formatPoints(value.toDouble())));
}

void ProjectWindow::Ui::onFinished() {
  const Action action = bridge->action();
  stopping = false;
  allowInput = false;
  countdownLeft = -1;
  holding = false;
  linkInfo = QJsonObject();   // nothing listens any more
  updateLinkBar();
  state = QStringLiteral("finished");
  if (action == Action::Check && !pendingBrain.isEmpty()) {   // Import a brain: the Check gave the spec
    const QString file = std::exchange(pendingBrain, QString());
    if (checkPassed && specThisRun && !closeAfterStop) {
      const std::string specJson = QJsonDocument(spec).toJson(QJsonDocument::Compact).toStdString();
      importing = true;   // from now on: no new run until the brain is in
      QTimer::singleShot(0, w, [this, file, specJson] { importBrainNow(file, specJson); });
    } else if (!closeAfterStop) {
      showToast(QStringLiteral("The brain wasn't added: Vyrn needs a Check that passes to see whether it fits your "
                               "scripts. Fix any problem (see Problems), then import it again."),
                Tone::Bad);
    }
  }
  if (action == Action::Check) {
    const Then next = then;
    then = Then::Nothing;
    if (next != Then::Nothing && !closeAfterStop) {
      if (checkPassed) {
        QTimer::singleShot(0, w, [this, next] { startAfterCheck(next); });
      } else {
        showToast(QStringLiteral("%1 didn't start: fix the problems first (see Problems).")
                      .arg(next == Then::Train ? QStringLiteral("Training") : QStringLiteral("Watching")),
                  Tone::Bad);
      }
    }
  } else {
    QStringList summary{QStringLiteral("Stopped after round %1").arg(round)};
    if (std::isfinite(best)) summary.append(QStringLiteral("Best %1").arg(store::formatPoints(best)));
    lastRunSummary = summary.join(QStringLiteral(" · "));
    if (action == Action::Train && runErrors == 0) {
      console->appendLine(QStringLiteral("app"), QStringLiteral("Training stopped. The brain is saved."));
      if (!closeAfterStop && !restartWatch)
        showToast(QStringLiteral("Training stopped and the brain is saved. Train continues from here; Watch shows "
                                 "what it learned."),
                  Tone::Good);
    } else {
      console->appendLine(QStringLiteral("app"), action == Action::Train ? QStringLiteral("Training stopped.")
                                                                         : QStringLiteral("Watching stopped."));
    }
    runBar.frame->hide();
  }
  updateActions();
  updateBanner();
  updateStatus();
  if (closeAfterStop) {
    QTimer::singleShot(0, w, [this] { w->close(); });
    return;
  }
  if (restartWatch) {
    restartWatch = false;
    QTimer::singleShot(0, w, [this] { w->watch(); });
  }
}

void ProjectWindow::Ui::onSaved(Editor* editor) {
  if (!bridge->running() || bridge->action() == Action::Check || stopping) return;
  if (state == QLatin1String("error")) {
    showRunBar(QStringLiteral("You saved %1. Apply changes restarts Python with it and continues (the brain is kept).")
                   .arg(editor->fileName()),
               true);
  } else {
    showRunBar(QStringLiteral("You changed %1 — Apply changes (keeps the brain)").arg(editor->fileName()), true);
  }
}

// ---- problems and values ---------------------------------------------------------------------------------------

void ProjectWindow::Ui::clearProblems() {
  problems->clearProblems();
  for (int i = 0; i < tabs->count(); ++i)
    if (Editor* editor = editorAt(i)) editor->clearProblems();
  updateProblemsTab();
}

void ProjectWindow::Ui::addProblem(const QString& kind, const QString& file, int line, int col, const QString& message,
                                   const QString& hint) {
  problems->addProblem(kind, file, line, col, message, hint);
  if (line > 0 && file.endsWith(QLatin1String(".py")) && !file.contains(QLatin1Char('/'))) {
    Editor* editor = w->editorFor(file);
    if (!editor && QFileInfo::exists(QDir(dir).filePath(file))) editor = openScript(file, false);
    if (editor)
      editor->markProblem(line, col, hint.isEmpty() ? message : QStringLiteral("%1 — %2").arg(message, hint),
                          kind == QLatin1String("error"));
  }
  updateProblemsTab();
}

void ProjectWindow::Ui::updateProblemsTab() {
  const int count = problems->errorCount() + problems->warningCount();
  bottom->setTabText(bottom->indexOf(problems),
                     count > 0 ? QStringLiteral("Problems (%1)").arg(count) : QStringLiteral("Problems"));
}

void ProjectWindow::Ui::clearValues() {
  shownText.clear();
  shownPictureNames.clear();
  values->clear();
  pictures->clear();
  values->hide();
  pictures->hide();
  valuesHint->show();
}

void ProjectWindow::Ui::updateValues(const vyrn::LiveView& view) {
  if (view.show_text != shownText) {
    const bool sameNames = view.show_text.size() == shownText.size() &&
                           std::equal(view.show_text.begin(), view.show_text.end(), shownText.begin(),
                                      [](const auto& a, const auto& b) { return a.first == b.first; });
    if (sameNames) {   // update in place: no flicker, the scroll position stays
      for (size_t i = 0; i < view.show_text.size(); ++i)
        if (QTreeWidgetItem* item = values->topLevelItem(int(i)))
          item->setText(1, QString::fromStdString(view.show_text[i].second));
    } else {
      values->clear();
      for (const auto& [key, value] : view.show_text)
        new QTreeWidgetItem(values, {QString::fromStdString(key), QString::fromStdString(value)});
      // As tall as its rows (up to 8; it scrolls beyond), so "What the AI sees" keeps the room.
      const int rows = std::min(values->topLevelItemCount(), 8);
      const int rowHeight = std::max(values->sizeHintForRow(0), QFontMetrics(values->font()).height() + 2);
      values->setFixedHeight(std::max(1, rows) * rowHeight + 2 * values->frameWidth() + 6);
    }
    shownText = view.show_text;
  }
  std::vector<std::string> names;
  names.reserve(view.show_pictures.size());
  for (const auto& entry : view.show_pictures) names.push_back(entry.first);
  if (names != shownPictureNames) {
    pictures->clear();
    for (const auto& entry : view.show_pictures)
      new QListWidgetItem(QIcon(thumbnail(entry.second, 56)), QString::fromStdString(entry.first), pictures);
    shownPictureNames = names;
  } else {
    for (size_t i = 0; i < view.show_pictures.size(); ++i)
      if (QListWidgetItem* item = pictures->item(int(i)))
        item->setIcon(QIcon(thumbnail(view.show_pictures[i].second, 56)));
  }
  values->setVisible(!shownText.empty());
  pictures->setVisible(!shownPictureNames.empty());
  valuesHint->setVisible(shownText.empty() && shownPictureNames.empty());
}

// ---- toolbar state, banner, status -----------------------------------------------------------------------------

void ProjectWindow::Ui::updateActions() {
  const bool running = bridge->running() || waitingForGame();
  const bool busy = otherTrainer();
  checkAct->setEnabled(!running && !importing);
  trainAct->setEnabled(!running && !busy && !importing);
  watchAct->setEnabled(!running && !busy && !importing);
  stopAct->setEnabled(running && !stopping);
  stopAct->setText(stopping ? QStringLiteral("Stopping…") : QStringLiteral("Stop"));
  pickAct->setEnabled(!sim);
  snapshotAct->setEnabled(!sim);
  // Compile needs a trained brain (the active one with a save); while a brain is being added it waits.
  const bool trainedBrain = store::hasTrainedBrain(dir);
  compileAct->setEnabled(trainedBrain && !importing);
  compileAct->setToolTip(trainedBrain ? QStringLiteral("Turn the trained AI into a Python program that runs without "
                                                       "Vyrn (play.py)")
                                      : QStringLiteral("Turn the trained AI into a Python program that runs without "
                                                       "Vyrn. Train it first: there is no trained brain yet."));
  const QString agent = QStringLiteral("Another program is training or watching this project right now (see the bar above "
                                       "the editor)");
  trainAct->setToolTip(busy ? agent
                            : QStringLiteral("Save, check, then let the AI learn by playing (it continues where it "
                                             "left off)"));
  watchAct->setToolTip(busy ? agent
                            : QStringLiteral("Let the trained AI play without learning (the Brain menu picks which "
                                             "brain)"));
}

void ProjectWindow::Ui::refreshLink() {
  const bool wasOn = link.on;
  link = sim ? LinkSettings() : store::linkSettings(store::settings(dir));
  if (link.on != wasOn) setModeUi();
  updateLinkBar();
}

void ProjectWindow::Ui::updateLinkBar() {
  const bool listening = !linkInfo.isEmpty() && bridge->running();
  if (!link.on && !listening) {
    linkBar.frame->hide();
    return;
  }
  QString text;
  if (!listening) {
    text = QStringLiteral("Vyrn Link: your game connects to %1 with the pairing code %2. Vyrn listens while Check, "
                          "Train or Watch runs.")
               .arg(store::linkUrl(link.port), store::linkCode(dir));
  } else if (linkInfo.value(QLatin1String("connected")).toBool()) {
    const QString game = linkInfo.value(QLatin1String("game")).toString();
    const QString client = linkInfo.value(QLatin1String("client")).toString();
    text = QStringLiteral("Vyrn Link: %1%2 is connected from %3")
               .arg(game.isEmpty() ? QStringLiteral("a game") : game,
                    client.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(client),
                    linkInfo.value(QLatin1String("from")).toString());
    if (linkInfo.value(QLatin1String("lockstep")).toBool()) {
      const int frames = linkInfo.value(QLatin1String("frames_per_step")).toInt(1);
      text += frames == 1 ? QStringLiteral(" · lockstep") : QStringLiteral(" · lockstep, %1 frames per step").arg(frames);
    }
    text += QLatin1Char('.');
  } else {
    text = QStringLiteral("Vyrn Link: waiting for your game. Connect it to %1 with the pairing code %2.")
               .arg(linkInfo.value(QLatin1String("url")).toString(), linkInfo.value(QLatin1String("code")).toString());
  }
  linkBar.text->setText(text);
  linkBar.frame->show();
}

void ProjectWindow::Ui::updateBanner() {
  const bool live = bridge->running() && allowInput && !stopping;
  if (!live) {
    keysBanner->hide();
    return;
  }
  QString text;
  QColor tone = theme::danger();
  int pointSize = 0;
  if (state == QLatin1String("countdown") || state == QLatin1String("starting")) {
    text = countdownLeft > 0
               ? QStringLiteral("Click the game window now: the AI starts pressing keys in %1 s. Click here to cancel.")
                     .arg(countdownLeft)
               : QStringLiteral("Getting ready to press keys… Click here to cancel.");
    tone = theme::warning();
    pointSize = 2;
  } else if (holding || state == QLatin1String("holding") || state == QLatin1String("paused") ||
             state == QLatin1String("error")) {
    text = QStringLiteral("Keys released: %1. Click the game to let the AI continue, or click here to stop.")
               .arg(holding ? holdReasonText(holdReason) : holdReasonText(QStringLiteral("paused")));
    tone = theme::warning();
  } else {
    const QString keys = workingPanicShortcut();
    text = keys.isEmpty() ? QStringLiteral("AI is pressing keys — click here to stop")
                          : QStringLiteral("AI is pressing keys — click here or press %1 to stop").arg(keys);
  }
  const QColor ink = tone == theme::danger() ? QColor(Qt::white) : QColor(20, 16, 4);
  QColor fill = tone == theme::danger() ? QColor(185, 28, 28) : tone;
  keysBanner->setStyleSheet(
      QStringLiteral("QPushButton#keysBanner { background-color: %1; color: %2; border: none; border-radius: 6px; "
                     "padding: 9px 12px; font-weight: 600; %3 } QPushButton#keysBanner:hover { background-color: %4; }")
          .arg(fill.name(), ink.name(),
               pointSize > 0 ? QStringLiteral("font-size: %1pt;").arg(keysBanner->font().pointSize() + pointSize)
                             : QString(),
               fill.lighter(115).name()));
  keysBanner->setText(text);
  keysBanner->show();
}

void ProjectWindow::Ui::updateStatus() {
  QStringList parts;
  const bool running = bridge->running();
  const Action action = bridge->action();
  if (waitingForGame()) {
    parts.append(QStringLiteral("Starting the game…"));
  } else if (running && action == Action::Check) {
    parts.append(stopping ? QStringLiteral("Stopping…")
                 : then == Then::Nothing ? QStringLiteral("Checking your scripts…")
                                         : QStringLiteral("Checking your scripts before starting…"));
  } else if (running) {
    if (stopping) {
      parts.append(action == Action::Train ? QStringLiteral("Saving the brain…") : QStringLiteral("Stopping…"));
    } else if (state == QLatin1String("starting") || state == QLatin1String("restarting")) {
      parts.append(stateDetail.isEmpty() ? QStringLiteral("Starting…") : stateDetail);
    } else if (state == QLatin1String("countdown")) {
      parts.append(countdownLeft > 0 ? QStringLiteral("Starting in %1…").arg(countdownLeft)
                                     : QStringLiteral("Starting…"));
    } else if (state == QLatin1String("paused")) {
      parts.append(QStringLiteral("Paused"));
    } else if (state == QLatin1String("error")) {
      parts.append(QStringLiteral("Paused by a problem"));
    } else if (holding && holdReason == QLatin1String("game")) {
      parts.append(QStringLiteral("Waiting for the game"));
    } else if (holding || state == QLatin1String("holding")) {
      parts.append(QStringLiteral("On hold"));
    } else if (action == Action::Watch) {
      parts.append(QStringLiteral("Watching"));
    }
    parts.append(QStringLiteral("Round %1").arg(round));
    if (std::isfinite(best)) parts.append(QStringLiteral("Best %1").arg(store::formatPoints(best)));
    if (action == Action::Train && explore >= 0.005)
      parts.append(QStringLiteral("Exploring %1%").arg(std::lround(explore * 100)));
    if (decisionsPerSecond > 0) parts.append(QStringLiteral("%1 decisions/s").arg(rate(decisionsPerSecond)));
  } else if (followTimer && followTimer->isActive()) {
    parts.append(lockBy == QLatin1String("agent") ? QStringLiteral("An AI agent is training")
                                                  : QStringLiteral("Training elsewhere"));
    if (followRound > 0) parts.append(QStringLiteral("Round %1").arg(followRound));
    if (std::isfinite(followBest)) parts.append(QStringLiteral("Best %1").arg(store::formatPoints(followBest)));
  } else if (!lastRunSummary.isEmpty()) {
    parts.append(lastRunSummary);
  } else {
    parts.append(QStringLiteral("Ready. Check tests your scripts; Train lets the AI learn."));
  }
  status->setText(parts.join(QStringLiteral(" · ")));
}

void ProjectWindow::Ui::updateStatusTip() {
  auto ms = [this](const char* key) {
    return QStringLiteral("%1 ms").arg(QString::number(lastTiming.value(QLatin1String(key)).toDouble(), 'f', 2));
  };
  QStringList lines;
  if (!lastTiming.isEmpty()) {
    lines.append(QStringLiteral("Time per decision: see %1 · points %2 · respond %3 · brain %4 · messages %5")
                     .arg(ms("see_ms"), ms("points_ms"), ms("respond_ms"), ms("brain_ms"), ms("ipc_ms")));
    lines.append(QStringLiteral("%1 decisions/s · late (99th percentile) %2 · missed deadlines %3")
                     .arg(rate(lastTiming.value(QLatin1String("ticks_per_s")).toDouble()), ms("late_p99_ms"))
                     .arg(lastTiming.value(QLatin1String("overruns")).toInteger()));
  }
  if (!lastUpdate.isEmpty()) {
    lines.append(QStringLiteral("Learning: %1 updates (%2/s) · loss %3 · learning rate %4")
                     .arg(QLocale().toString(lastUpdate.value(QLatin1String("updates")).toInteger()),
                          rate(lastUpdate.value(QLatin1String("updates_per_s")).toDouble()),
                          QString::number(lastUpdate.value(QLatin1String("loss")).toDouble(), 'g', 3),
                          QString::number(lastUpdate.value(QLatin1String("lr")).toDouble(), 'g', 3)));
  }
  status->setToolTip(lines.join(QLatin1Char('\n')));
}

void ProjectWindow::Ui::showToast(const QString& text, Tone tone) {
  setBarTone(toast, tone == Tone::Good ? theme::success() : tone == Tone::Bad ? theme::danger() : theme::accent());
  toast.text->setText(text);
  toast.frame->show();
  toastTimer->start(tone == Tone::Bad ? 15000 : 8000);
}

void ProjectWindow::Ui::showRunBar(const QString& text, bool apply) {
  runBar.text->setText(text);
  runApply->setVisible(apply);
  runBar.frame->show();
}

void ProjectWindow::Ui::rebuildBrainMenu() {
  brainMenu->clear();
  const bool running = bridge->running();
  brainMenu->addSection(QStringLiteral("Watch uses"));
  QAction* bestAct = brainMenu->addAction(QStringLiteral("Best: the brain at its best 20-round mean"));
  QAction* latestAct = brainMenu->addAction(QStringLiteral("Latest: the brain as it was last saved"));
  bestAct->setCheckable(true);
  latestAct->setCheckable(true);
  bestAct->setChecked(watchBest);
  latestAct->setChecked(!watchBest);
  QObject::connect(bestAct, &QAction::triggered, w, [this] { watchBest = true; });
  QObject::connect(latestAct, &QAction::triggered, w, [this] { watchBest = false; });

  brainMenu->addSection(QStringLiteral("Brains"));
  const QList<BrainCard> brains = store::brains(dir);
  if (brains.isEmpty()) brainMenu->addAction(QStringLiteral("No brain yet: press Train to make one"))->setEnabled(false);
  for (const BrainCard& brain : brains) {
    QStringList parts{QStringLiteral("Trained %1").arg(store::formatDuration(brain.timeTrained))};
    if (brain.hasBest) parts.append(QStringLiteral("best %1").arg(store::formatPoints(brain.best)));
    if (!brain.updated.isEmpty()) parts.append(QStringLiteral("saved %1").arg(brain.updated));
    QAction* act = brainMenu->addAction(parts.join(QStringLiteral(" · ")));
    act->setCheckable(true);
    act->setChecked(brain.active);
    act->setEnabled(!running || brain.active);
    act->setToolTip(brain.active ? QStringLiteral("The brain Train continues and Watch uses")
                                 : QStringLiteral("Use this older brain from now on"));
    const QString id = brain.id;
    const bool active = brain.active;
    QObject::connect(act, &QAction::triggered, w, [this, id, active] {
      if (active || bridge->running()) return;
      QString error;
      if (!store::mergeSettings(dir, QJsonObject{{QStringLiteral("brain"), id}}, &error)) {
        showToast(error, Tone::Bad);
        return;
      }
      showToast(QStringLiteral("Using that older brain now: Train continues it and Watch shows it."), Tone::Good);
    });
  }
  brainMenu->addSeparator();
  QAction* over = brainMenu->addAction(QStringLiteral("Start over (new brain)…"));
  over->setEnabled(!running && !otherTrainer() && !importing);
  QObject::connect(over, &QAction::triggered, w, [this] { startOver(); });

  brainMenu->addSection(QStringLiteral("Share"));
  QAction* exportAct = brainMenu->addAction(theme::lineIcon(QStringLiteral("share"), theme::muted()),
                                            QStringLiteral("Export this brain (.vrl)…"));
  exportAct->setEnabled(!brains.isEmpty());
  exportAct->setToolTip(QStringLiteral("Save the trained AI as a file others can add to a project with the same scripts"));
  QObject::connect(exportAct, &QAction::triggered, w, [this] { w->exportBrain(); });
  QAction* importAct = brainMenu->addAction(theme::lineIcon(QStringLiteral("import"), theme::muted()),
                                            QStringLiteral("Import a brain (.vrl)…"));
  importAct->setEnabled(!running && !otherTrainer() && !importing);
  QObject::connect(importAct, &QAction::triggered, w, [this] {
    const QString file = QFileDialog::getOpenFileName(w, QStringLiteral("Import a brain"), QDir::homePath(),
                                                      QStringLiteral("Vyrn brains (*.vrl)"));
    if (!file.isEmpty()) w->importBrain(file, true);
  });
}

void ProjectWindow::Ui::rebuildShareMenu() {
  shareMenu->clear();
  QAction* project = shareMenu->addAction(theme::lineIcon(QStringLiteral("share"), theme::text()),
                                          QStringLiteral("Share project (.vrlp)…"));
  project->setToolTip(QStringLiteral("Scripts, pictures, settings and the trained AI in one file"));
  QObject::connect(project, &QAction::triggered, w, [this] { w->shareProject(); });
  QAction* brain = shareMenu->addAction(QStringLiteral("Export this brain (.vrl)…"));
  brain->setEnabled(!store::brains(dir).isEmpty());
  QObject::connect(brain, &QAction::triggered, w, [this] { w->exportBrain(); });
  shareMenu->addSeparator();
  QAction* guide = shareMenu->addAction(QStringLiteral("How sharing works"));
  QObject::connect(guide, &QAction::triggered, w, [this] { showGuide(QStringLiteral("06-sharing.md")); });
}

void ProjectWindow::Ui::showGuide(const QString& file) {
  const int index = docsPick->findData(QStringLiteral("qrc:/guide/") + file);
  if (index < 0) return;
  docsPick->setCurrentIndex(index);
  loadDoc(index);
  w->showBottomTab(QStringLiteral("docs"));
}

void ProjectWindow::Ui::loadDoc(int index, const QString& anchor) {
  const QString target = docsPick->itemData(index).toString();
  if (target.isEmpty()) return;
  if (!QFile::exists(QLatin1Char(':') + QUrl(target).path())) {
    docs->setHtml(QStringLiteral("<p>This build of Vyrn has no copy of that page.</p>"));
    return;
  }
  QUrl url(target);
  if (!anchor.isEmpty()) url.setFragment(anchor);
  docs->setSource(url, target.endsWith(QLatin1String(".md")) ? QTextDocument::MarkdownResource
                                                              : QTextDocument::HtmlResource);
  if (!anchor.isEmpty()) docs->scrollToAnchor(anchor);
}

// ---- sharing and trust (docs/dev/package.md) ------------------------------------------------------------------

void ProjectWindow::Ui::refreshTrust() {
  const std::string project = dir.toStdString();
  std::optional<vyrn::ImportOrigin> origin;
  bool now = true;
  try {
    now = vyrn::not_trusted_message(project).empty();   // the same test Session::start makes
    if (!now) origin = vyrn::import_origin(project);
  } catch (const std::exception&) {
    now = false;
  }
  trusted = now;
  if (now) {
    trustBar.frame->hide();
    return;
  }
  const QString file = origin && !origin->file.empty() ? sharing::qs(origin->file) : QStringLiteral("a shared file");
  const QString by = origin && !origin->author.empty() ? QStringLiteral(" (by %1)").arg(sharing::qs(origin->author))
                                                       : QString();
  trustBar.text->setText(QStringLiteral("This project came from %1%2. Its Python code hasn't run on this computer yet. "
                                        "Read the scripts, then trust it.")
                             .arg(file, by));
  trustBar.frame->show();
}

bool ProjectWindow::Ui::ensureTrusted(const std::function<void()>& next) {
  refreshTrust();
  if (trusted) return true;
  if (trustDialog) {   // already asking
    trustDialog->raise();
    trustDialog->activateWindow();
    return false;
  }
  auto* dialog = new TrustDialog(dir, bool(next), w);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  trustDialog = dialog;
  QObject::connect(dialog, &TrustDialog::scriptClicked, w, [this](const QString& file) { openScript(file, true); });
  QObject::connect(dialog, &QDialog::accepted, w, [this, next] {
    refreshTrust();
    console->appendLine(QStringLiteral("app"), QStringLiteral("You trusted this project: Vyrn runs its scripts from now on."));
    if (next) QTimer::singleShot(0, w, next);
  });
  dialog->open();   // window-modal, returns at once: the smoke test presses its buttons
  return false;
}

void ProjectWindow::Ui::importBrainNow(const QString& file, const std::string& specJson) {
  importing = true;
  updateActions();
  const QString fileName = QFileInfo(file).fileName();
  console->appendLine(QStringLiteral("app"), QStringLiteral("Adding the brain from %1…").arg(fileName));
  const std::string path = file.toStdString(), project = dir.toStdString();
  background::run<vyrn::ImportResult>(
      w, [path, project, specJson] { return vyrn::import_brain(path, project, specJson, true); },
      [this, fileName](background::Outcome<vyrn::ImportResult> outcome) {
        importing = false;
        updateActions();
        updateStatus();
        if (!outcome.value) {
          console->appendLine(QStringLiteral("stderr"), QStringLiteral("%1: %2").arg(fileName, outcome.error));
          auto* box = new QMessageBox(QMessageBox::Warning, QStringLiteral("Couldn't add the brain"),
                                      QStringLiteral("Vyrn couldn't add the brain from %1.").arg(fileName),
                                      QMessageBox::Ok, w);
          box->setObjectName(QStringLiteral("brainImportError"));
          box->setInformativeText(outcome.error);   // the core's own words (PackageError)
          box->setAttribute(Qt::WA_DeleteOnClose);
          box->open();
          return;
        }
        QStringList notes;
        for (const std::string& note : outcome.value->notes) notes.append(sharing::qs(note));
        console->appendLine(QStringLiteral("app"), QStringLiteral("Added the brain from %1. Watch uses it now; Train "
                                                                  "continues from it.")
                                                       .arg(fileName));
        showToast(QStringLiteral("Added the brain from %1. Watch shows what it learned; Train continues from it.")
                      .arg(fileName),
                  Tone::Good);
        if (!notes.isEmpty()) w->showNotes(QStringLiteral("Added the brain from %1").arg(fileName), notes);
      });
}

void ProjectWindow::Ui::openSharedFile(const QString& file) {
  if (sharing::kindOf(file) == sharing::FileKind::Brain) {
    w->importBrain(file, true);
    return;
  }
  QString created;
  QStringList notes;
  {
    OpenPackageDialog dialog(file, OpenPackageDialog::Mode::Project, w);
    if (dialog.exec() != QDialog::Accepted) return;
    created = dialog.importedProject();
    notes = dialog.notes();
  }
  if (created.isEmpty()) return;
  if (w->isSignalConnected(QMetaMethod::fromSignal(&ProjectWindow::openProjectRequested))) {
    Q_EMIT w->openProjectRequested(created, notes);   // the launcher opens it (and keeps track of it)
    return;
  }
  auto* window = new ProjectWindow(created);
  window->setAttribute(Qt::WA_DeleteOnClose);
  window->show();
  window->showNotes(QStringLiteral("Imported %1").arg(QFileInfo(file).fileName()), notes);
}

// ---- someone else training this project ------------------------------------------------------------------------

void ProjectWindow::Ui::pollLock() {
  refreshTrust();   // e.g. trusted meanwhile with vyrn-cli trust
  int pid = 0;
  QString by = store::lockHolder(dir, &pid);
  if (by == QLatin1String("app") && pid == int(QCoreApplication::applicationPid())) by.clear();   // our own run
  if (by == lockBy && pid == lockPid) return;
  lockBy = by;
  lockPid = pid;
  if (by.isEmpty()) {
    lockBar.frame->hide();
    if (followTimer->isActive()) {
      followTick();   // the last rounds
      stopFollowing();
      console->appendLine(QStringLiteral("app"), QStringLiteral("The other training run ended."));
    }
  } else {
    if (by == QLatin1String("agent"))
      lockBar.text->setText(QStringLiteral("An AI agent is training this project. Train and Watch are off until it "
                                           "stops; the Points chart follows its rounds."));
    else if (by == QLatin1String("cli"))
      lockBar.text->setText(QStringLiteral("vyrn-cli is training or watching this project in a terminal. Train and Watch are "
                                           "off until it stops."));
    else
      lockBar.text->setText(QStringLiteral("This project is being trained in another Vyrn window."));
    lockStop->setVisible(by == QLatin1String("agent") || by == QLatin1String("cli"));
    lockStop->setEnabled(true);
    lockStop->setText(QStringLiteral("Stop"));
    lockBar.frame->show();
    if (!bridge->running()) {
      followFile.clear();
      followTimer->start();
      followTick();
    }
  }
  updateActions();
  updateStatus();
}

void ProjectWindow::Ui::stopOtherTrainer() {
  if (lockBy != QLatin1String("agent") && lockBy != QLatin1String("cli")) return;
  const QString who = lockBy == QLatin1String("agent") ? QStringLiteral("the AI agent's training")
                                                       : QStringLiteral("the vyrn-cli run in the terminal");
  const auto answer = QMessageBox::question(w, QStringLiteral("Stop training?"),
                                            QStringLiteral("Stop %1? It saves its brain first.").arg(who));
  if (answer != QMessageBox::Yes) return;
  // A stop request: an empty <project>/.vyrn-stop (vyrn::kStopMarkerFile). The other program's Session notices it,
  // deletes it and stops the way its own Stop does (Train saves the brain first). No signal is sent.
  QFile request(QDir(dir).filePath(QString::fromLatin1(vyrn::kStopMarkerFile)));
  if (!request.open(QIODevice::WriteOnly)) {
    showToast(QStringLiteral("Vyrn couldn't ask it to stop: it can't write a small file (.vyrn-stop) in the project "
                             "folder. Stop it where it runs."),
              Tone::Bad);
    return;
  }
  request.close();
  lockStop->setEnabled(false);
  lockStop->setText(QStringLiteral("Asked it to stop…"));
  console->appendLine(QStringLiteral("app"), QStringLiteral("Asked %1 to stop. It saves its brain first, then ends.").arg(who));
}

void ProjectWindow::Ui::followTick() {
  // The newest run folder's rounds.csv: round,points,ticks,truncated,game_over,mean20,best_mean20,...
  const QFileInfoList runs = QDir(QDir(dir).filePath(QStringLiteral("runs")))
                                 .entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
  QString newest;
  for (const QFileInfo& run : runs) {
    const QString csv = QDir(run.filePath()).filePath(QStringLiteral("rounds.csv"));
    if (QFileInfo::exists(csv)) {
      newest = csv;
      break;
    }
  }
  if (newest.isEmpty()) return;
  QFile file(newest);
  if (!file.open(QIODevice::ReadOnly)) return;
  if (newest != followFile || file.size() < followOffset) {
    followFile = newest;
    followOffset = 0;
    followPartial.clear();
    followRoundCol = followPointsCol = followMeanCol = followBestCol = -1;
    followRound = 0;
    followBest = std::numeric_limits<double>::quiet_NaN();
    plot->clear();
  }
  if (!file.seek(followOffset)) return;
  const QByteArray chunk = file.read(8 << 20);
  followOffset += chunk.size();
  followPartial += chunk;
  qsizetype newline;
  while ((newline = followPartial.indexOf('\n')) >= 0) {
    const QByteArray line = followPartial.left(newline).trimmed();
    followPartial.remove(0, newline + 1);
    if (line.isEmpty()) continue;
    const QList<QByteArray> cells = line.split(',');
    if (followRoundCol < 0) {   // the header
      followRoundCol = int(cells.indexOf("round"));
      followPointsCol = int(cells.indexOf("points"));
      followMeanCol = int(cells.indexOf("mean20"));
      followBestCol = int(cells.indexOf("best_mean20"));
      if (followRoundCol < 0 || followPointsCol < 0) {   // not the file we know: give up on it
        followRoundCol = followPointsCol = int(cells.size()) + 1;
      }
      continue;
    }
    bool okRound = false, okPoints = false;
    const qint64 r = cells.value(followRoundCol).toLongLong(&okRound);
    const double points = cells.value(followPointsCol).toDouble(&okPoints);
    if (!okRound || !okPoints) continue;
    bool okMean = false;
    const double mean = followMeanCol >= 0 ? cells.value(followMeanCol).toDouble(&okMean) : 0.0;
    plot->addRound(r, points, okMean ? mean : std::numeric_limits<double>::quiet_NaN());
    followRound = r;
    bool okBest = false;
    const double b = followBestCol >= 0 ? cells.value(followBestCol).toDouble(&okBest) : 0.0;
    if (okBest && std::isfinite(b)) followBest = b;
  }
  updateStatus();
}

void ProjectWindow::Ui::stopFollowing() {
  followTimer->stop();
  followFile.clear();
  followPartial.clear();
}

// ---- ProjectWindow ---------------------------------------------------------------------------------------------

ProjectWindow::ProjectWindow(const QString& projectDir, QWidget* parent) : QMainWindow(parent) {
  ui_ = new Ui;
  ui_->w = this;
  const QString canonical = QFileInfo(projectDir).canonicalFilePath();
  ui_->dir = canonical.isEmpty() ? QDir::cleanPath(QFileInfo(projectDir).absoluteFilePath()) : canonical;
  ui_->build();
}

ProjectWindow::~ProjectWindow() {
  // First the session: SessionBridge's destructor stops a running one and waits until the brain is saved.
  delete ui_->bridge;
  ui_->bridge = nullptr;
  // The child widgets are deleted after this body: none of their signals may reach the Ui any more.
  const QList<QObject*> children = findChildren<QObject*>();
  for (QObject* child : children) QObject::disconnect(child, nullptr, this, nullptr);
  delete ui_;
  ui_ = nullptr;
}

QString ProjectWindow::projectDir() const { return ui_->dir; }

void ProjectWindow::check() {
  if (!ui_->ensureTrusted([this] { check(); })) return;   // an imported project: the trust dialog first
  if (!ui_->canBegin()) return;
  ui_->startGameThen([this] { ui_->runCheck(Ui::Then::Nothing, false); });
}

void ProjectWindow::train() {
  if (!ui_->ensureTrusted([this] { train(); })) return;
  if (!ui_->canBegin()) return;
  ui_->startGameThen([this] { ui_->runCheck(Ui::Then::Train, false); });
}

void ProjectWindow::watch() {
  if (!ui_->canBegin()) return;
  if (store::brains(ui_->dir).isEmpty()) {
    ui_->showToast(QStringLiteral("There's no trained brain to watch yet: press Train first."), Ui::Tone::Info);
    return;
  }
  if (!ui_->ensureTrusted([this] { watch(); })) return;
  ui_->startGameThen([this] {
    if (!ui_->sim) {   // real games always check first (the key-safety flow starts with a Check)
      ui_->runCheck(Ui::Then::Watch, false);
      return;
    }
    if (!ui_->canBegin() || !saveAll()) return;
    ui_->clearProblems();
    ui_->startRun(Action::Watch, false, false);
  });
}

void ProjectWindow::stop() { ui_->stopPressed(); }

int ProjectWindow::panicStopAll() {
  // 1. Every run stops, so nothing starts Python again.
  for (QWidget* widget : QApplication::topLevelWidgets()) {
    if (auto* window = qobject_cast<ProjectWindow*>(widget)) window->ui_->panicStop();
  }
  // 2. The Python workers this app started end at once, with everything they started.
  const int killed = killOwnWorkers();
  // 3. Every key goes up (macOS, Windows; on Linux the kernel does that), and once more a moment later, for a key a
  //    dying worker pressed in between.
  vyrn::platform::release_all_keys();
  QTimer::singleShot(300, qApp, [] { vyrn::platform::release_all_keys(); });
  return killed;
}

bool ProjectWindow::saveAll() {
  for (int i = 0; i < ui_->tabs->count(); ++i) {
    Editor* editor = ui_->editorAt(i);
    if (!editor || !editor->isModified()) continue;
    QString error;
    if (!editor->save(&error)) {
      ui_->console->appendLine(QStringLiteral("app"), error);
      ui_->showToast(error, Ui::Tone::Bad);
      return false;
    }
  }
  return true;
}

Editor* ProjectWindow::editorFor(const QString& fileName) const {
  for (int i = 0; i < ui_->tabs->count(); ++i) {
    Editor* editor = ui_->editorAt(i);
    if (editor && editor->fileName() == fileName) return editor;
  }
  return nullptr;
}

QStringList ProjectWindow::tabFiles() const {
  QStringList files;
  for (int i = 0; i < ui_->tabs->count(); ++i)
    if (Editor* editor = ui_->editorAt(i)) files.append(editor->fileName());
  return files;
}

int ProjectWindow::roundsCharted() const { return ui_->plot->roundCount(); }

QString ProjectWindow::linkBarText() const {
  return ui_->linkBar.frame->isHidden() ? QString() : ui_->linkBar.text->text();
}

bool ProjectWindow::hasProblemMarker(const QString& fileName, int line) const {
  const Editor* editor = editorFor(fileName);
  return editor && editor->hasProblemAt(line);
}

SessionBridge* ProjectWindow::bridge() const { return ui_->bridge; }

void ProjectWindow::showBottomTab(const QString& name) {
  QWidget* page = nullptr;
  if (name == QLatin1String("console")) page = ui_->console;
  else if (name == QLatin1String("points")) page = ui_->plot;
  else if (name == QLatin1String("problems")) page = ui_->problems;
  else if (name == QLatin1String("docs")) page = ui_->docsPage;
  if (!page) return;
  ui_->bottomDock->show();
  ui_->bottom->setCurrentWidget(page);
}

void ProjectWindow::showHelp(const QString& vyrnName) {
  ui_->docsPick->setCurrentIndex(0);
  ui_->loadDoc(0, vyrnName);
  showBottomTab(QStringLiteral("docs"));
}

void ProjectWindow::shareProject() {
  if (!saveAll()) return;   // share what the editors show
  ShareDialog dialog(ShareDialog::Kind::Project, ui_->dir, seesPicture(), this);
  dialog.exec();
}

void ProjectWindow::compileProject() {
  if (!ui_->ensureTrusted([this] { compileProject(); })) return;   // an imported project: the trust dialog first
  if (!saveAll()) return;                                           // compile what the editors show
  CompileDialog dialog(ui_->dir, this);
  dialog.exec();
}

void ProjectWindow::exportBrain() {
  if (store::brains(ui_->dir).isEmpty()) {
    ui_->showToast(QStringLiteral("There's no trained brain to export yet: press Train first."), Ui::Tone::Info);
    return;
  }
  ShareDialog dialog(ShareDialog::Kind::Brain, ui_->dir, seesPicture(), this);
  dialog.exec();
}

void ProjectWindow::importBrain(const QString& file, bool confirm) {
  Ui& u = *ui_;
  auto busy = [&u] {
    if (u.bridge->running() || u.importing || u.waitingForGame()) {
      u.showToast(QStringLiteral("Vyrn is running this project. Stop it first, then add the brain."), Ui::Tone::Info);
      return true;
    }
    if (u.otherTrainer()) {
      u.showToast(QStringLiteral("Someone else is training this project right now. Add the brain when they're done."),
                  Ui::Tone::Info);
      return true;
    }
    return false;
  };
  if (busy()) return;
  if (confirm) {
    OpenPackageDialog dialog(file, OpenPackageDialog::Mode::BrainIntoProject, this, u.name);
    if (dialog.exec() != QDialog::Accepted || busy()) return;
  }
  u.refreshTrust();
  if (!u.trusted) {
    // Its scripts may not run yet, so there's no Check: the core compares the brain with the active one instead.
    u.importBrainNow(file, std::string());
    return;
  }
  // A Check first: the brain must fit what these scripts see and choose (docs/dev/package.md section 6).
  if (!saveAll()) return;
  u.pendingBrain = file;
  u.console->appendLine(QStringLiteral("app"), QStringLiteral("Adding the brain from %1: checking your scripts first, "
                                                              "to see whether it fits them.")
                                                   .arg(QFileInfo(file).fileName()));
  u.runCheck(Ui::Then::Nothing, false);
  if (!u.bridge->running()) u.pendingBrain.clear();   // the Check didn't start (the toast says why)
}

QImage ProjectWindow::seesPicture() const { return ui_->sees->picture(); }

bool ProjectWindow::addingBrain() const { return !ui_->pendingBrain.isEmpty() || ui_->importing; }

bool ProjectWindow::trustBannerShown() const { return ui_->trustBar.frame->isVisibleTo(this); }

bool ProjectWindow::startingGame() const { return ui_->waitingForGame(); }

const StartProgram* ProjectWindow::startProgram() const { return ui_->game; }

void ProjectWindow::showNotes(const QString& title, const QStringList& notes) {
  if (notes.isEmpty()) return;
  for (const QString& note : notes) ui_->console->appendLine(QStringLiteral("app"), QStringLiteral("• ") + note);
  QTimer::singleShot(250, this, [this, title, notes] {   // once the window is on screen
    auto* box = new QMessageBox(QMessageBox::Information, title, QStringLiteral("Good to know:"), QMessageBox::Ok, this);
    box->setObjectName(QStringLiteral("importNotes"));
    box->setInformativeText(QStringLiteral("• ") + notes.join(QStringLiteral("\n• ")));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->open();
  });
}

void ProjectWindow::dragEnterEvent(QDragEnterEvent* event) {
  if (!sharing::packageIn(event->mimeData()).isEmpty())
    event->acceptProposedAction();
  else
    QMainWindow::dragEnterEvent(event);
}

void ProjectWindow::dragMoveEvent(QDragMoveEvent* event) {
  if (!sharing::packageIn(event->mimeData()).isEmpty())
    event->acceptProposedAction();
  else
    QMainWindow::dragMoveEvent(event);
}

void ProjectWindow::dropEvent(QDropEvent* event) {
  const QString file = sharing::packageIn(event->mimeData());
  if (file.isEmpty()) {
    QMainWindow::dropEvent(event);
    return;
  }
  event->acceptProposedAction();
  // Not inside the drop: the program the file came from waits until the drop is handled.
  QTimer::singleShot(0, this, [this, file] { ui_->openSharedFile(file); });
}

void ProjectWindow::closeEvent(QCloseEvent* event) {
  if (!ui_->closeConfirmed) {
    QStringList unsaved;
    for (int i = 0; i < ui_->tabs->count(); ++i)
      if (Editor* editor = ui_->editorAt(i); editor && editor->isModified()) unsaved.append(editor->fileName());
    if (!unsaved.isEmpty()) {
      const auto answer = QMessageBox::question(
          this, QStringLiteral("Save your changes?"),
          QStringLiteral("These scripts have changes that aren't saved yet: %1.").arg(unsaved.join(QStringLiteral(", "))),
          QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
      if (answer == QMessageBox::Cancel || (answer == QMessageBox::Save && !saveAll())) {
        event->ignore();
        return;
      }
    }
    ui_->closeConfirmed = true;
  }
  ui_->cancelGameWait();
  if (ui_->bridge->running()) {   // stop and save first; onFinished() closes the window again
    ui_->closeAfterStop = true;
    ui_->then = Ui::Then::Nothing;
    ui_->stopRun();
    ui_->showToast(QStringLiteral("Saving the brain… the window closes when it's done."), Ui::Tone::Info);
    event->ignore();
    return;
  }
  ui_->game->stop();   // the Start program, when Vyrn started it (the window's destructor waits for it to end)
  QMainWindow::closeEvent(event);
}

void ProjectWindow::changeEvent(QEvent* event) {
  QMainWindow::changeEvent(event);
  // Real games: while Vyrn is the active window the AI holds (its keys are released). Clicking the game again
  // lets it continue (the focus guard in the worker checks the game window is the active one).
  if (event->type() == QEvent::ActivationChange && ui_ && ui_->bridge && ui_->bridge->running() && ui_->allowInput)
    ui_->bridge->setHold(isActiveWindow());
}
