#include "LauncherWindow.h"

#include <QAction>
#include <QCheckBox>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QRadialGradient>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

#include "AgentsDialog.h"
#include "NewProjectDialog.h"
#include "OpenPackageDialog.h"
#include "Paths.h"
#include "ProjectStore.h"
#include "ProjectWindow.h"
#include "SetupDialog.h"
#include "ShareDialog.h"
#include "Sharing.h"
#include "Theme.h"
#include "Updater.h"

namespace launcher_detail {

constexpr int kCardTargetWidth = 300;   // columns are fitted to this width
constexpr int kCardMinWidth = 230;
constexpr int kGridSpacing = 16;
constexpr int kPollMs = 3000;           // locks, scores and new folders while the launcher is visible

QString updatedText(qint64 when) {
  if (when <= 0) return {};
  const qint64 ago = qMax<qint64>(0, QDateTime::currentSecsSinceEpoch() - when);
  if (ago < 60) return QStringLiteral("Updated just now");
  if (ago < 3600) return QStringLiteral("Updated %1 min ago").arg(ago / 60);
  if (ago < 86400) {
    const qint64 hours = ago / 3600;
    return hours == 1 ? QStringLiteral("Updated 1 hour ago") : QStringLiteral("Updated %1 hours ago").arg(hours);
  }
  const qint64 days = ago / 86400;
  if (days == 1) return QStringLiteral("Updated yesterday");
  if (days < 7) return QStringLiteral("Updated %1 days ago").arg(days);
  return QStringLiteral("Updated %1")
      .arg(QLocale().toString(QDateTime::fromSecsSinceEpoch(when).date(), QLocale::ShortFormat));
}

QString trainingText(const QString& by) {
  if (by == QLatin1String("agent")) return QStringLiteral("● Training now (AI agent)");
  if (by == QLatin1String("cli")) return QStringLiteral("● Running in vyrn-cli");
  if (by == QLatin1String("app")) return QStringLiteral("● Running in Vyrn");
  return {};
}

// The empty state's picture: what the AI sees in Catch (pictures), on a soft teal glow.
QPixmap heroPicture(qreal dpr) {
  const QSize size(260, 236);
  QPixmap pm(size * dpr);
  pm.setDevicePixelRatio(dpr);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  const QPointF centre(size.width() / 2.0, size.height() / 2.0);
  QRadialGradient glow(centre, size.height() / 2.0 - 1);   // fades out before the edges: no visible box
  QColor teal = theme::accent();
  teal.setAlpha(38);
  glow.setColorAt(0.0, teal);
  teal.setAlpha(12);
  glow.setColorAt(0.55, teal);
  teal.setAlpha(0);
  glow.setColorAt(1.0, teal);
  p.fillRect(QRectF(QPointF(0, 0), QSizeF(size)), glow);
  const int side = 136;
  p.drawPixmap(QPointF(centre.x() - side / 2.0, centre.y() - side / 2.0), theme::catchPicture(side, dpr));
  return pm;
}

QLabel* makeBadge(QWidget* parent) {
  auto* badge = new QLabel(parent);
  badge->setProperty("role", "badge");
  badge->setFont(theme::scaledFont(0.84, QFont::Bold));
  badge->setFixedHeight(QFontMetrics(badge->font()).height() + 6);   // same pill height wherever it sits
  badge->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
  return badge;
}

void setTone(QLabel* badge, const QString& tone) {
  if (badge->property("tone").toString() == tone) return;
  badge->setProperty("tone", tone);
  theme::repolish(badge);
}

// One line of text that ends in "…" when it doesn't fit.
class ElidedLabel : public QWidget {
 public:
  explicit ElidedLabel(QWidget* parent = nullptr) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  }
  void setText(const QString& text) {
    if (text == text_) return;
    text_ = text;
    updateTip();
    updateGeometry();
    update();
  }
  QString text() const { return text_; }
  QSize sizeHint() const override { return {fontMetrics().horizontalAdvance(text_), fontMetrics().height()}; }
  QSize minimumSizeHint() const override {
    return {fontMetrics().horizontalAdvance(QStringLiteral("Abc…")), fontMetrics().height()};
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setPen(palette().color(QPalette::WindowText));
    p.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText(text_, Qt::ElideRight, width()));
  }
  void resizeEvent(QResizeEvent* e) override {
    QWidget::resizeEvent(e);
    updateTip();
  }

 private:
  void updateTip() { setToolTip(fontMetrics().horizontalAdvance(text_) > width() ? text_ : QString()); }   // the full name

  QString text_;
};

// A project as a rounded card. Click opens it; right-click or "…" shows the menu.
class ProjectCardWidget : public QWidget {
  Q_OBJECT

 public:
  ProjectCardWidget(const ProjectCard& card, QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_Hover);
    setFocusPolicy(Qt::TabFocus);
    setCursor(Qt::PointingHandCursor);
    setMinimumWidth(kCardMinWidth);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 14, 10, 14);
    outer->setSpacing(14);

    auto* top = new QHBoxLayout;
    top->setSpacing(12);
    icon_ = new QLabel(this);
    icon_->setFixedSize(44, 44);
    top->addWidget(icon_, 0, Qt::AlignTop);

    auto* heading = new QVBoxLayout;
    heading->setSpacing(6);
    name_ = new ElidedLabel(this);
    name_->setFont(theme::scaledFont(1.12, QFont::Bold));
    heading->addWidget(name_);
    auto* badges = new QHBoxLayout;
    badges->setSpacing(6);
    modeBadge_ = makeBadge(this);
    trustBadge_ = makeBadge(this);
    trustBadge_->setText(QStringLiteral("Not trusted yet"));
    setTone(trustBadge_, QStringLiteral("warn"));
    trustBadge_->setToolTip(QStringLiteral("This project came from a shared file. Open it and read its scripts, then "
                                           "trust it to let Vyrn run them."));
    badges->addWidget(modeBadge_);
    badges->addWidget(trustBadge_);
    badges->addStretch(1);
    heading->addLayout(badges);
    top->addLayout(heading, 1);

    more_ = new QToolButton(this);
    more_->setIcon(theme::lineIcon(QStringLiteral("dots"), theme::muted()));
    more_->setIconSize(QSize(16, 16));
    more_->setAutoRaise(true);
    more_->setCursor(Qt::ArrowCursor);
    more_->setToolTip(QStringLiteral("More: rename, duplicate, share, show folder, move to trash"));
    more_->setAccessibleName(QStringLiteral("More actions"));
    top->addWidget(more_, 0, Qt::AlignTop);
    connect(more_, &QToolButton::clicked, this, [this] {
      Q_EMIT menuRequested(card_.path, more_->mapToGlobal(QPoint(0, more_->height())));
    });
    outer->addLayout(top);

    // Middle: best score and time trained (or "not trained yet").
    auto* bottom = new QHBoxLayout;
    bottom->setSpacing(26);
    auto stat = [this](QLabel*& caption, QLabel*& value, const QString& title) {
      auto* box = new QVBoxLayout;
      box->setSpacing(1);
      caption = new QLabel(title, this);
      QFont small = theme::scaledFont(0.78, QFont::Bold);
      small.setCapitalization(QFont::AllUppercase);
      small.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
      caption->setFont(small);
      caption->setProperty("role", "faint");
      value = new QLabel(this);
      value->setFont(theme::scaledFont(1.06, QFont::Bold));
      box->addWidget(caption);
      box->addWidget(value);
      return box;
    };
    bottom->addLayout(stat(bestCaption_, bestValue_, QStringLiteral("Best score")));
    bottom->addLayout(stat(timeCaption_, timeValue_, QStringLiteral("Trained for")));
    bestValue_->setToolTip(QStringLiteral("The best average points over 20 rounds"));
    untrained_ = new QLabel(QStringLiteral("Not trained yet"), this);
    untrained_->setProperty("role", "muted");
    untrained_->setMinimumHeight(bestCaption_->sizeHint().height() + bestValue_->sizeHint().height() + 1);
    untrained_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    bottom->addWidget(untrained_);
    bottom->addStretch(1);
    outer->addLayout(bottom);

    // Footer: "Training now (AI agent)" while someone trains it, else when it last changed.
    auto* footer = new QHBoxLayout;
    footer->setSpacing(6);
    liveBadge_ = makeBadge(this);
    setTone(liveBadge_, QStringLiteral("live"));
    updated_ = new QLabel(this);
    updated_->setProperty("role", "faint");
    updated_->setFont(theme::scaledFont(0.9));
    updated_->setMinimumHeight(liveBadge_->sizeHint().height());
    footer->addWidget(liveBadge_);
    footer->addWidget(updated_);
    footer->addStretch(1);
    outer->addLayout(footer);

    setCard(card);
  }

  // Updates the text in place (keeps hover and focus).
  void setCard(const ProjectCard& card) {
    const bool newIcon = card.templateId != card_.templateId || card.mode != card_.mode || icon_->pixmap().isNull();
    card_ = card;
    const QString name = card.name.isEmpty() ? QFileInfo(card.path).fileName() : card.name;
    name_->setText(name);
    if (newIcon) icon_->setPixmap(theme::templateIcon(card.templateId, card.mode, 44, devicePixelRatioF()));

    const bool realtime = card.mode == QLatin1String("realtime");
    modeBadge_->setText(realtime ? QStringLiteral("Real game") : QStringLiteral("Simulation"));
    setTone(modeBadge_, realtime ? QStringLiteral("real") : QStringLiteral("sim"));
    modeBadge_->setToolTip(realtime ? QStringLiteral("Plays a real program on your screen by pressing keys")
                                    : QStringLiteral("A game Vyrn simulates very fast (it has a world.py)"));
    trustBadge_->setVisible(!card.trusted);
    const QString live = trainingText(card.trainingBy);
    liveBadge_->setText(live);
    liveBadge_->setVisible(!live.isEmpty());
    updated_->setVisible(live.isEmpty());

    const bool trained = card.hasBest || card.timeTrained > 0;
    bestValue_->setText(card.hasBest ? store::formatPoints(card.best) : QStringLiteral("–"));
    timeValue_->setText(card.timeTrained > 0 ? store::formatDuration(card.timeTrained) : QStringLiteral("–"));
    for (QWidget* w : {static_cast<QWidget*>(bestCaption_), static_cast<QWidget*>(bestValue_),
                       static_cast<QWidget*>(timeCaption_), static_cast<QWidget*>(timeValue_)})
      w->setVisible(trained);
    untrained_->setVisible(!trained);
    updated_->setText(updatedText(card.modified));

    QString description = QStringLiteral("%1, %2").arg(name, modeBadge_->text());
    if (card.hasBest) description += QStringLiteral(", best score %1").arg(bestValue_->text());
    if (card.timeTrained > 0) description += QStringLiteral(", trained for %1").arg(timeValue_->text());
    if (!live.isEmpty()) description += QStringLiteral(", ") + live.mid(2);
    if (!card.trusted) description += QStringLiteral(", not trusted yet");
    setAccessibleName(description);
  }

  const ProjectCard& card() const { return card_; }

 Q_SIGNALS:
  void openRequested(const QString& path);
  void menuRequested(const QString& path, const QPoint& globalPos);
  void actionRequested(const QString& path, const QString& action);   // "rename" | "delete"

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QColor fill = hovered_ ? theme::panelHover() : theme::panel();
    if (pressed_) fill = fill.darker(112);
    QColor edge = theme::border();
    if (hovered_) edge = QColor(0x3f, 0x52, 0x6e);
    if (hasFocus()) edge = theme::accent();
    p.setPen(QPen(edge, 1.0));
    p.setBrush(fill);
    p.drawRoundedRect(r, 12, 12);
    if (!card_.trainingBy.isEmpty()) {
      // A thin accent line along the top: this project is busy right now.
      QColor line = theme::accent();
      line.setAlpha(160);
      p.setPen(QPen(line, 2.0, Qt::SolidLine, Qt::RoundCap));
      p.drawLine(QPointF(r.left() + 14, r.top() + 1.5), QPointF(r.right() - 14, r.top() + 1.5));
    }
  }

  bool event(QEvent* e) override {
    if (e->type() == QEvent::HoverEnter || e->type() == QEvent::HoverLeave) {
      hovered_ = e->type() == QEvent::HoverEnter;
      if (!hovered_) pressed_ = false;
      update();
    }
    return QWidget::event(e);
  }

  void mousePressEvent(QMouseEvent* e) override {
    if (e->button() == Qt::LeftButton) {
      pressed_ = true;
      update();
    }
    QWidget::mousePressEvent(e);
  }

  void mouseReleaseEvent(QMouseEvent* e) override {
    const bool click = pressed_ && e->button() == Qt::LeftButton && rect().contains(e->position().toPoint());
    pressed_ = false;
    update();
    if (click) Q_EMIT openRequested(card_.path);
    QWidget::mouseReleaseEvent(e);
  }

  void contextMenuEvent(QContextMenuEvent* e) override { Q_EMIT menuRequested(card_.path, e->globalPos()); }

  void keyPressEvent(QKeyEvent* e) override {
    switch (e->key()) {
      case Qt::Key_Return:
      case Qt::Key_Enter:
      case Qt::Key_Space:
        Q_EMIT openRequested(card_.path);
        return;
      case Qt::Key_F2:
        Q_EMIT actionRequested(card_.path, QStringLiteral("rename"));
        return;
      case Qt::Key_Delete:
        Q_EMIT actionRequested(card_.path, QStringLiteral("delete"));
        return;
      case Qt::Key_Menu:
        Q_EMIT menuRequested(card_.path, mapToGlobal(QPoint(width() / 2, height() / 2)));
        return;
      default:
        QWidget::keyPressEvent(e);
    }
  }

  void focusInEvent(QFocusEvent* e) override {
    update();
    QWidget::focusInEvent(e);
  }
  void focusOutEvent(QFocusEvent* e) override {
    update();
    QWidget::focusOutEvent(e);
  }

 private:
  ProjectCard card_;
  QLabel* icon_ = nullptr;
  ElidedLabel* name_ = nullptr;
  QLabel* modeBadge_ = nullptr;
  QLabel* trustBadge_ = nullptr;
  QLabel* liveBadge_ = nullptr;
  QToolButton* more_ = nullptr;
  QLabel* bestCaption_ = nullptr;
  QLabel* bestValue_ = nullptr;
  QLabel* timeCaption_ = nullptr;
  QLabel* timeValue_ = nullptr;
  QLabel* untrained_ = nullptr;
  QLabel* updated_ = nullptr;
  bool hovered_ = false;
  bool pressed_ = false;
};

// Cards in as many equal columns as fit (re-flowed when the window is resized). The column count follows the
// width the scroll area gives this widget, so it never asks for more width than it has.
class CardGrid : public QWidget {
 public:
  explicit CardGrid(QWidget* parent = nullptr) : QWidget(parent), grid_(new QGridLayout(this)) {
    grid_->setContentsMargins(0, 2, 0, 2);
    grid_->setHorizontalSpacing(kGridSpacing);
    grid_->setVerticalSpacing(kGridSpacing);
    grid_->setSizeConstraint(QLayout::SetNoConstraint);
  }

  void setCards(const QList<ProjectCardWidget*>& cards) {
    for (ProjectCardWidget* old : std::as_const(cards_)) {
      grid_->removeWidget(old);
      old->hide();
      old->deleteLater();   // never delete a card inside one of its own event handlers
    }
    cards_ = cards;
    columns_ = 0;
    relayout();
  }

  const QList<ProjectCardWidget*>& cards() const { return cards_; }

  QSize sizeHint() const override { return grid_->sizeHint(); }
  QSize minimumSizeHint() const override { return {kCardMinWidth, grid_->minimumSize().height()}; }

 protected:
  void resizeEvent(QResizeEvent* e) override {
    QWidget::resizeEvent(e);
    relayout();
  }

 private:
  void relayout() {
    const int columns = qMax(1, (width() + kGridSpacing) / (kCardTargetWidth + kGridSpacing));
    if (columns == columns_) return;
    columns_ = columns;
    for (ProjectCardWidget* card : std::as_const(cards_)) grid_->removeWidget(card);
    qDeleteAll(fillers_);
    fillers_.clear();
    for (int i = 0; i < cards_.size(); ++i) {
      grid_->addWidget(cards_[i], i / columns, i % columns);
      cards_[i]->show();
    }
    // With fewer cards than columns, empty fillers keep the cards at the same width as in a full row.
    for (int c = int(cards_.size()); c < columns; ++c) {
      auto* filler = new QWidget(this);
      filler->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
      filler->setFixedHeight(1);
      grid_->addWidget(filler, 0, c, Qt::AlignTop);
      fillers_ << filler;
    }
    for (int c = 0; c < qMax(columns, grid_->columnCount()); ++c) grid_->setColumnStretch(c, c < columns ? 1 : 0);
    updateGeometry();
  }

  QGridLayout* grid_ = nullptr;
  QList<ProjectCardWidget*> cards_;
  QList<QWidget*> fillers_;
  int columns_ = 0;
};

}  // namespace launcher_detail

using namespace launcher_detail;

struct LauncherWindow::Ui {
  QHBoxLayout* header = nullptr;
  QLabel* tagline = nullptr;
  QPushButton* agents = nullptr;
  QPushButton* open = nullptr;
  QStackedWidget* stack = nullptr;
  QScrollArea* scroll = nullptr;
  CardGrid* grid = nullptr;
  QLabel* count = nullptr;
  QWidget* cardsPage = nullptr;
  QWidget* emptyPage = nullptr;
  QFrame* dataWarning = nullptr;
  QPushButton* setup = nullptr;
  QFrame* updateBar = nullptr;
  Updater* updater = nullptr;
  bool manualCheck = false;

  QList<ProjectCard> projects;
  QStringList order;                                   // card paths, as shown
  QHash<QString, QPointer<ProjectWindow>> windows;     // open project windows by canonical path

  QFileSystemWatcher* watcher = nullptr;
  QTimer* poll = nullptr;
  QTimer* debounce = nullptr;
  SetupChecker* checker = nullptr;
  bool checkerStarted = false;
  bool checkerRealGame = false;

  int modal = 0;                 // a menu or dialog is open: refreshes wait until it closes
  bool refreshPending = false;
};

LauncherWindow::LauncherWindow(QWidget* parent) : QMainWindow(parent), ui_(new Ui) {
  setWindowTitle(QStringLiteral("Vyrn"));
  setMinimumSize(640, 480);
  resize(1080, 720);

  auto* central = new QWidget(this);
  auto* page = new QVBoxLayout(central);
  page->setContentsMargins(32, 26, 32, 18);
  page->setSpacing(20);

  // ---- header: logo, name, tagline, actions ----
  auto* header = new QHBoxLayout;
  ui_->header = header;
  header->setSpacing(10);
  auto* logo = new QLabel(central);
  const qreal dpr = devicePixelRatioF();
  QPixmap logoPixmap = QPixmap(QStringLiteral(":/vyrn.png"))
                           .scaled(QSize(52, 52) * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  logoPixmap.setDevicePixelRatio(dpr);
  logo->setPixmap(logoPixmap);
  header->addWidget(logo);
  header->addSpacing(4);
  auto* titles = new QVBoxLayout;
  titles->setSpacing(0);
  auto* name = new QLabel(QStringLiteral("Vyrn"), central);
  name->setFont(theme::scaledFont(1.9, QFont::Bold));
  auto* tagline = new QLabel(QStringLiteral("Teach an AI with three small Python files"), central);
  tagline->setProperty("role", "muted");
  ui_->tagline = tagline;
  titles->addStretch(1);
  titles->addWidget(name);
  titles->addWidget(tagline);
  titles->addStretch(1);
  header->addLayout(titles);
  header->addStretch(1);

  auto headerButton = [central](const QString& text, const QIcon& icon, const QString& tip) {
    auto* b = new QPushButton(icon, text, central);
    b->setToolTip(tip);
    b->setFocusPolicy(Qt::TabFocus);
    b->setCursor(Qt::PointingHandCursor);
    return b;
  };
  ui_->setup = headerButton(QStringLiteral("Setup"), theme::dotIcon(theme::faint()),
                            QStringLiteral("Check that this computer has what Vyrn needs"));
  auto* agents = headerButton(QStringLiteral("Connect an AI agent"),
                              theme::lineIcon(QStringLiteral("agent"), theme::muted()),
                              QStringLiteral("Let an AI coding agent (Claude Code, Codex, Gemini CLI) help you"));
  auto* open = headerButton(QStringLiteral("Open folder…"), theme::lineIcon(QStringLiteral("folder"), theme::muted()),
                            QStringLiteral("Open a project folder from anywhere (Ctrl+O)"));
  auto* create = headerButton(QStringLiteral("New project"), theme::lineIcon(QStringLiteral("plus"), QColor(0x04, 0x21, 0x1d)),
                              QStringLiteral("Start a new project from a template (Ctrl+N)"));
  create->setProperty("role", "primary");
  ui_->agents = agents;
  ui_->open = open;
  agents->setAccessibleName(QStringLiteral("Connect an AI agent"));
  open->setAccessibleName(QStringLiteral("Open folder"));
  for (QPushButton* b : {ui_->setup, agents, open, create}) header->addWidget(b, 0, Qt::AlignVCenter);
  page->addLayout(header);
  connect(ui_->setup, &QPushButton::clicked, this, &LauncherWindow::showSetup);
  connect(agents, &QPushButton::clicked, this, &LauncherWindow::showAgents);
  connect(open, &QPushButton::clicked, this, &LauncherWindow::openFolder);
  connect(create, &QPushButton::clicked, this, [this] { newProject(); });

  // ---- shown only when Vyrn's own files are missing ----
  ui_->dataWarning = new QFrame(central);
  ui_->dataWarning->setProperty("role", "callout");
  auto* warningLayout = new QVBoxLayout(ui_->dataWarning);
  warningLayout->setContentsMargins(16, 12, 16, 12);
  auto* warningText = new QLabel(
      QStringLiteral("<b>Vyrn can't find its own files</b> (the Python library and the templates), so projects "
                     "can't run. Reinstall it by running <code>%1</code> in the folder you downloaded Vyrn into.")
          .arg(paths::installCommand()),
      ui_->dataWarning);
  warningText->setWordWrap(true);
  warningLayout->addWidget(warningText);
  ui_->dataWarning->setVisible(paths::dataDir().isEmpty());
  page->addWidget(ui_->dataWarning);

  // ---- a quiet bar when a newer version exists ----
  ui_->updateBar = new QFrame(central);
  ui_->updateBar->setProperty("role", "callout");
  auto* updateRow = new QHBoxLayout(ui_->updateBar);
  updateRow->setContentsMargins(16, 10, 16, 10);
  updateRow->setSpacing(10);
  updateRow->addWidget(new QLabel(QStringLiteral("A newer version of Vyrn is available."), ui_->updateBar), 1);
  auto* never = new QCheckBox(QStringLiteral("Don't check automatically"), ui_->updateBar);
  auto* later = new QPushButton(QStringLiteral("Later"), ui_->updateBar);
  auto* now = new QPushButton(QStringLiteral("Update now"), ui_->updateBar);
  now->setProperty("role", "primary");
  for (QPushButton* b : {later, now}) b->setCursor(Qt::PointingHandCursor);
  updateRow->addWidget(never);
  updateRow->addWidget(later);
  updateRow->addWidget(now);
  ui_->updateBar->hide();
  page->addWidget(ui_->updateBar);
  connect(never, &QCheckBox::toggled, this, [](bool on) { Updater::setAutoEnabled(!on); });
  connect(later, &QPushButton::clicked, ui_->updateBar, &QWidget::hide);
  connect(now, &QPushButton::clicked, this, &LauncherWindow::startUpdate);

  ui_->updater = new Updater(this);
  connect(ui_->updater, &Updater::updateAvailable, this, [this] {
    ui_->updateBar->show();
    if (ui_->manualCheck) showUpdateMessage(QStringLiteral("A newer version of Vyrn is available. Update it from the bar at the top."));
    ui_->manualCheck = false;
  });
  connect(ui_->updater, &Updater::upToDate, this, [this] {
    ui_->manualCheck = false;
    showUpdateMessage(QStringLiteral("Vyrn is up to date."));
  });
  connect(ui_->updater, &Updater::failed, this, [this](const QString& why) {
    ui_->manualCheck = false;
    showUpdateMessage(QStringLiteral("Vyrn couldn't check for updates. %1").arg(why));
  });
  connect(ui_->updater, &Updater::unknown, this, [this] {
    ui_->manualCheck = false;
    showUpdateMessage(QStringLiteral("This copy of Vyrn wasn't installed with the installer, so it can't tell."));
  });
  QTimer::singleShot(3000, this, [this] { ui_->updater->check(false); });   // quiet unless there is an update

  ui_->stack = new QStackedWidget(central);
  page->addWidget(ui_->stack, 1);

  // ---- page 1: the cards ----
  ui_->cardsPage = new QWidget(ui_->stack);
  auto* cardsLayout = new QVBoxLayout(ui_->cardsPage);
  cardsLayout->setContentsMargins(0, 0, 0, 0);
  cardsLayout->setSpacing(12);
  auto* section = new QHBoxLayout;
  section->setSpacing(10);
  auto* sectionTitle = new QLabel(QStringLiteral("Your projects"), ui_->cardsPage);
  sectionTitle->setFont(theme::scaledFont(1.12, QFont::Bold));
  ui_->count = new QLabel(ui_->cardsPage);
  ui_->count->setProperty("role", "faint");
  auto* folderLink = new QPushButton(QStringLiteral("Show projects folder"), ui_->cardsPage);
  folderLink->setProperty("role", "link");
  folderLink->setCursor(Qt::PointingHandCursor);
  folderLink->setToolTip(paths::pretty(paths::projectsRoot()));
  auto* sharedLink = new QPushButton(QStringLiteral("Open shared file…"), ui_->cardsPage);
  sharedLink->setProperty("role", "link");
  sharedLink->setCursor(Qt::PointingHandCursor);
  sharedLink->setToolTip(QStringLiteral("Open a project (.vrlp) or a trained AI (.vrl) someone shared (Ctrl+Shift+O). "
                                        "You can also drop the file on this window."));
  section->addWidget(sectionTitle, 0, Qt::AlignBaseline);
  section->addWidget(ui_->count, 0, Qt::AlignBaseline);
  section->addStretch(1);
  auto* updateLink = new QPushButton(QStringLiteral("Check for updates"), ui_->cardsPage);
  updateLink->setProperty("role", "link");
  updateLink->setCursor(Qt::PointingHandCursor);
  connect(updateLink, &QPushButton::clicked, this, &LauncherWindow::checkForUpdates);
  section->addWidget(updateLink);
  section->addWidget(sharedLink);
  section->addWidget(folderLink);
  connect(sharedLink, &QPushButton::clicked, this, &LauncherWindow::chooseSharedFile);
  cardsLayout->addLayout(section);
  connect(folderLink, &QPushButton::clicked, this, [this] { showFolder(paths::projectsRoot()); });

  auto* gridHolder = new QWidget;
  auto* gridHolderLayout = new QVBoxLayout(gridHolder);
  gridHolderLayout->setContentsMargins(0, 0, 4, 8);
  ui_->grid = new CardGrid(gridHolder);
  gridHolderLayout->addWidget(ui_->grid);
  gridHolderLayout->addStretch(1);
  ui_->scroll = new QScrollArea(ui_->cardsPage);
  ui_->scroll->setWidget(gridHolder);
  ui_->scroll->setWidgetResizable(true);
  ui_->scroll->setFrameShape(QFrame::NoFrame);
  ui_->scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  ui_->scroll->viewport()->setAutoFillBackground(false);
  gridHolder->setAutoFillBackground(false);
  cardsLayout->addWidget(ui_->scroll, 1);
  ui_->stack->addWidget(ui_->cardsPage);

  // ---- page 2: nothing yet -> invite them to try Catch (pictures) ----
  ui_->emptyPage = new QWidget(ui_->stack);
  auto* empty = new QVBoxLayout(ui_->emptyPage);
  empty->setSpacing(14);
  empty->addStretch(2);
  auto* picture = new QLabel(ui_->emptyPage);
  picture->setPixmap(heroPicture(dpr));
  picture->setAlignment(Qt::AlignCenter);
  picture->setToolTip(QStringLiteral("What the AI sees in Catch (pictures): a falling fruit and the basket"));
  empty->addWidget(picture);
  empty->addSpacing(6);
  auto* emptyTitle = new QLabel(QStringLiteral("Start with Catch (pictures)"), ui_->emptyPage);
  emptyTitle->setFont(theme::scaledFont(1.6, QFont::Bold));
  emptyTitle->setAlignment(Qt::AlignCenter);
  empty->addWidget(emptyTitle);
  auto* emptyText = new QLabel(
      QStringLiteral("Your first AI learns to catch falling fruit in a few minutes. It's a simulation, so it never "
                     "touches your keyboard or screen. Press Train and watch the points climb."),
      ui_->emptyPage);
  emptyText->setProperty("role", "muted");
  emptyText->setWordWrap(true);
  emptyText->setAlignment(Qt::AlignCenter);
  emptyText->setFixedWidth(520);
  emptyText->setMinimumHeight(emptyText->heightForWidth(520));
  empty->addWidget(emptyText, 0, Qt::AlignHCenter);
  empty->addSpacing(8);
  auto* emptyButtons = new QHBoxLayout;
  emptyButtons->setSpacing(10);
  auto* tryCatch = new QPushButton(QStringLiteral("Create Catch (pictures)"), ui_->emptyPage);
  tryCatch->setProperty("role", "primary");
  tryCatch->setCursor(Qt::PointingHandCursor);
  auto* other = new QPushButton(QStringLiteral("Choose another template…"), ui_->emptyPage);
  other->setCursor(Qt::PointingHandCursor);
  auto* shared = new QPushButton(QStringLiteral("Open shared file…"), ui_->emptyPage);
  shared->setCursor(Qt::PointingHandCursor);
  shared->setToolTip(QStringLiteral("Open a project (.vrlp) or a trained AI (.vrl) someone shared"));
  emptyButtons->addStretch(1);
  emptyButtons->addWidget(tryCatch);
  emptyButtons->addWidget(other);
  emptyButtons->addWidget(shared);
  emptyButtons->addStretch(1);
  empty->addLayout(emptyButtons);
  empty->addStretch(3);
  ui_->stack->addWidget(ui_->emptyPage);
  connect(tryCatch, &QPushButton::clicked, this, &LauncherWindow::createCatchPictures);
  connect(other, &QPushButton::clicked, this, [this] { newProject(); });
  connect(shared, &QPushButton::clicked, this, &LauncherWindow::chooseSharedFile);

  setCentralWidget(central);
  // Start without a focus ring on any button (Tab still moves through them).
  central->setFocusPolicy(Qt::ClickFocus);
  central->setFocus();

  // ---- shortcuts ----
  auto shortcut = [this](const QKeySequence& keys, auto slot) {
    auto* action = new QAction(this);
    action->setShortcut(keys);
    connect(action, &QAction::triggered, this, slot);
    addAction(action);
  };
  shortcut(QKeySequence::New, [this] { newProject(); });
  shortcut(QKeySequence::Open, [this] { openFolder(); });
  shortcut(QKeySequence::Refresh, [this] { refresh(); });
  shortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O), [this] { chooseSharedFile(); });
  setAcceptDrops(true);   // .vrl / .vrlp files dropped anywhere on the launcher

  // ---- keeping the cards fresh: folder changes right away, locks and scores every few seconds ----
  ui_->watcher = new QFileSystemWatcher(this);
  ui_->watcher->addPath(paths::projectsRoot());
  ui_->debounce = new QTimer(this);
  ui_->debounce->setSingleShot(true);
  ui_->debounce->setInterval(250);
  connect(ui_->watcher, &QFileSystemWatcher::directoryChanged, ui_->debounce, qOverload<>(&QTimer::start));
  connect(ui_->debounce, &QTimer::timeout, this, &LauncherWindow::refresh);
  ui_->poll = new QTimer(this);
  ui_->poll->setInterval(kPollMs);
  connect(ui_->poll, &QTimer::timeout, this, [this] {
    if (isVisible() && !isMinimized()) refresh();
  });

  ui_->checker = new SetupChecker(this);
  connect(ui_->checker, &SetupChecker::finished, this, &LauncherWindow::updateSetupButton);

  refresh();
}

LauncherWindow::~LauncherWindow() {
  // Project windows have no parent (each is its own top-level window); close the ones still open.
  const QList<QPointer<ProjectWindow>> windows = ui_->windows.values();
  ui_->windows.clear();
  for (const QPointer<ProjectWindow>& window : windows) {
    if (!window) continue;
    disconnect(window, nullptr, this, nullptr);
    delete window.data();
  }
  delete ui_;
}

void LauncherWindow::refresh() {
  if (ui_->modal > 0) {
    ui_->refreshPending = true;
    return;
  }
  ui_->refreshPending = false;
  if (!ui_->watcher->directories().contains(paths::projectsRoot())) ui_->watcher->addPath(paths::projectsRoot());

  const QList<ProjectCard> projects = store::projects();
  QStringList order;
  order.reserve(projects.size());
  for (const ProjectCard& p : projects) order << p.path;
  ui_->projects = projects;
  if (!order.isEmpty() && order == ui_->order)
    updateCardsInPlace();
  else
    rebuildCards();
  ui_->order = order;

  // The quick setup check includes the real-game checks once a real-game project exists.
  const bool realGame = std::any_of(projects.begin(), projects.end(),
                                    [](const ProjectCard& p) { return p.mode == QLatin1String("realtime"); });
  if (!ui_->checkerStarted || realGame != ui_->checkerRealGame) runQuickSetupCheck();
}

void LauncherWindow::rebuildCards() {
  QList<ProjectCardWidget*> cards;
  for (const ProjectCard& project : std::as_const(ui_->projects)) {
    auto* card = new ProjectCardWidget(project, ui_->grid);
    // Queued: the card may be deleted by what these do, so never run them inside the card's own event handler.
    connect(card, &ProjectCardWidget::openRequested, this, &LauncherWindow::openProject, Qt::QueuedConnection);
    connect(card, &ProjectCardWidget::menuRequested, this,
            [this](const QString& path, const QPoint& pos) { showProjectMenu(path, pos); }, Qt::QueuedConnection);
    connect(card, &ProjectCardWidget::actionRequested, this,
            [this](const QString& path, const QString& action) {
              if (action == QLatin1String("rename")) renameProject(path);
              if (action == QLatin1String("delete")) deleteProject(path);
            },
            Qt::QueuedConnection);
    cards << card;
  }
  ui_->grid->setCards(cards);
  const int n = int(ui_->projects.size());
  ui_->count->setText(n == 1 ? QStringLiteral("1 project") : QStringLiteral("%1 projects").arg(n));
  ui_->stack->setCurrentWidget(n == 0 ? ui_->emptyPage : ui_->cardsPage);
}

void LauncherWindow::updateCardsInPlace() {
  const QList<ProjectCardWidget*>& cards = ui_->grid->cards();
  for (int i = 0; i < cards.size() && i < ui_->projects.size(); ++i) cards[i]->setCard(ui_->projects[i]);
}

ProjectWindow* LauncherWindow::openProject(const QString& path) {
  const QFileInfo info(path);
  const QString dir = QFileInfo(info.isFile() ? info.absolutePath() : path).canonicalFilePath();
  if (dir.isEmpty() || store::project(dir).name.isEmpty()) {
    auto modal = qScopeGuard([this] { --ui_->modal; });
    ++ui_->modal;
    QMessageBox::warning(this, QStringLiteral("Not a Vyrn project"),
                         QStringLiteral("%1 isn't a Vyrn project. A project folder has a vyrn.json or an inputs.py.")
                             .arg(paths::pretty(path)));
    return nullptr;
  }
  if (QPointer<ProjectWindow> existing = ui_->windows.value(dir); existing) {
    existing->setWindowState((existing->windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    existing->show();
    existing->raise();
    existing->activateWindow();
    return existing.data();
  }
  auto* window = new ProjectWindow(dir);
  window->setAttribute(Qt::WA_DeleteOnClose);
  ui_->windows.insert(dir, window);
  connect(window, &QObject::destroyed, this, [this, dir] {
    ui_->windows.remove(dir);
    ui_->debounce->start();   // its scores and time trained changed
  });
  // A .vrlp dropped on that window: open the new project here, like any other.
  connect(window, &ProjectWindow::openProjectRequested, this, [this](const QString& created, const QStringList& notes) {
    refresh();
    if (ProjectWindow* opened = openProject(created))
      opened->showNotes(QStringLiteral("Imported %1").arg(store::project(created).name), notes);
  });
  window->show();
  return window;
}

void LauncherWindow::newProject(const QString& preselectTemplate) {
  QString created;
  {
    auto modal = qScopeGuard([this] { --ui_->modal; });
    ++ui_->modal;
    NewProjectDialog dialog(this, preselectTemplate);
    if (dialog.exec() != QDialog::Accepted) return;
    created = dialog.createdPath();
  }
  refresh();
  if (!created.isEmpty()) openProject(created);
}

void LauncherWindow::createCatchPictures() {
  const QList<TemplateCard> templates = store::templates();
  const auto it = std::find_if(templates.begin(), templates.end(),
                               [](const TemplateCard& t) { return t.id == QLatin1String("catch-pictures"); });
  if (it == templates.end()) {
    newProject();
    return;
  }
  QString error;
  const QString path = store::createProject(store::uniqueName(it->name), *it, &error);
  if (path.isEmpty()) {
    auto modal = qScopeGuard([this] { --ui_->modal; });
    ++ui_->modal;
    QMessageBox::warning(this, QStringLiteral("Couldn't create the project"), error);
    return;
  }
  refresh();
  openProject(path);
}

void LauncherWindow::openFolder() {
  QString dir;
  {
    auto modal = qScopeGuard([this] { --ui_->modal; });
    ++ui_->modal;
    dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Open a project folder"), paths::projectsRoot());
  }
  if (!dir.isEmpty()) openProject(dir);
}

void LauncherWindow::checkForUpdates() {
  ui_->manualCheck = true;
  ui_->updater->check(true);
}

void LauncherWindow::showUpdateMessage(const QString& text) {
  QMessageBox::information(this, QStringLiteral("Updates"), text);
}

void LauncherWindow::startUpdate() {
  const auto answer = QMessageBox::question(
      this, QStringLiteral("Update Vyrn"),
      QStringLiteral("Vyrn will close, rebuild and reopen. This takes 15-30 minutes on a first install, and less after "
                     "that. Update now?"),
      QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
  if (answer != QMessageBox::Yes) return;
  if (!Updater::startUpdate()) {
    QMessageBox::warning(this, QStringLiteral("Couldn't start the update"),
                         QStringLiteral("Vyrn couldn't open a terminal window. You can update by hand: run %1")
                             .arg(Updater::oneLiner()));
    return;
  }
  QApplication::quit();
}

void LauncherWindow::showSetup() {
  {
    auto modal = qScopeGuard([this] { --ui_->modal; });
    ++ui_->modal;
    SetupDialog dialog(this, ui_->checkerRealGame);
    dialog.exec();
  }
  runQuickSetupCheck();   // something may have been fixed meanwhile
  if (ui_->refreshPending) refresh();
}

void LauncherWindow::showAgents() {
  {
    auto modal = qScopeGuard([this] { --ui_->modal; });
    ++ui_->modal;
    AgentsDialog dialog(this);
    dialog.exec();
  }
  if (ui_->refreshPending) refresh();
}

void LauncherWindow::showProjectMenu(const QString& path, const QPoint& globalPos) {
  const auto it = std::find_if(ui_->projects.cbegin(), ui_->projects.cend(),
                               [&path](const ProjectCard& p) { return p.path == path; });
  if (it == ui_->projects.cend()) return;

  QMenu menu(this);
  QAction* open = menu.addAction(theme::lineIcon(QStringLiteral("open"), theme::text()), QStringLiteral("Open"));
  QFont bold = open->font();
  bold.setBold(true);
  open->setFont(bold);
  QAction* rename = menu.addAction(theme::lineIcon(QStringLiteral("rename"), theme::muted()), QStringLiteral("Rename…"));
  QAction* duplicate = menu.addAction(theme::lineIcon(QStringLiteral("copy"), theme::muted()), QStringLiteral("Duplicate"));
  QAction* share = menu.addAction(theme::lineIcon(QStringLiteral("share"), theme::muted()),
                                  QStringLiteral("Share project (.vrlp)…"));
  QAction* folder = menu.addAction(theme::lineIcon(QStringLiteral("folder"), theme::muted()), QStringLiteral("Show folder"));
  menu.addSeparator();
  QAction* trash = menu.addAction(theme::lineIcon(QStringLiteral("trash"), theme::danger()),
                                  QStringLiteral("Move to trash…"));
  QAction* chosen = nullptr;
  {
    auto modal = qScopeGuard([this] { --ui_->modal; });
    ++ui_->modal;
    chosen = menu.exec(globalPos);
  }
  if (chosen == open)
    openProject(path);
  else if (chosen == rename)
    renameProject(path);
  else if (chosen == duplicate)
    duplicateProject(path);
  else if (chosen == share)
    shareProject(path);
  else if (chosen == folder)
    showFolder(path);
  else if (chosen == trash)
    deleteProject(path);
  else if (ui_->refreshPending)
    refresh();
}

void LauncherWindow::renameProject(const QString& path) {
  const ProjectCard card = store::project(path);
  if (card.name.isEmpty()) return;
  bool ok = false;
  QString name;
  {
    auto modal = qScopeGuard([this] { --ui_->modal; });
    ++ui_->modal;
    name = QInputDialog::getText(this, QStringLiteral("Rename project"), QStringLiteral("New name:"),
                                 QLineEdit::Normal, card.name, &ok);
  }
  if (ok && !name.simplified().isEmpty() && name.simplified() != card.name) {
    QString error;
    if (!store::renameProject(path, name, &error)) {
      auto modal = qScopeGuard([this] { --ui_->modal; });
      ++ui_->modal;
      QMessageBox::warning(this, QStringLiteral("Couldn't rename it"), error);
    }
  }
  refresh();
}

void LauncherWindow::duplicateProject(const QString& path) {
  QString error;
  if (store::duplicateProject(path, &error).isEmpty()) {
    auto modal = qScopeGuard([this] { --ui_->modal; });
    ++ui_->modal;
    QMessageBox::warning(this, QStringLiteral("Couldn't duplicate it"), error);
  }
  refresh();
}

void LauncherWindow::deleteProject(const QString& path) {
  const ProjectCard card = store::project(path);
  if (card.name.isEmpty()) {
    refresh();
    return;
  }
  auto modal = qScopeGuard([this] { --ui_->modal; });
  ++ui_->modal;
  const QString canonical = QFileInfo(path).canonicalFilePath();
  if (QPointer<ProjectWindow> open = ui_->windows.value(canonical); open) {
    QMessageBox::information(this, QStringLiteral("Close it first"),
                             QStringLiteral("“%1” is open in its own window. Close that window first, then try "
                                            "again.")
                                 .arg(card.name));
    return;
  }

  QMessageBox box(this);
  box.setWindowTitle(QStringLiteral("Move to trash?"));
  box.setTextFormat(Qt::RichText);
  box.setText(QStringLiteral("<b>Move “%1” to the trash?</b>").arg(card.name.toHtmlEscaped()));
  box.setInformativeText(QStringLiteral("Its scripts and trained brains go with it. You can get it back from the "
                                        "trash later."));
  QPushButton* remove = box.addButton(QStringLiteral("Move to trash"), QMessageBox::DestructiveRole);
  remove->setProperty("role", "danger");
  theme::repolish(remove);
  QPushButton* cancel = box.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);   // (no theme icon)
  box.setDefaultButton(cancel);
  box.setEscapeButton(cancel);
  box.exec();
  if (box.clickedButton() != remove) return;

  QString error;
  if (!store::deleteProject(path, &error))
    QMessageBox::warning(this, QStringLiteral("Couldn't move it to the trash"), error);
  modal.dismiss();
  --ui_->modal;
  refresh();
}

void LauncherWindow::showFolder(const QString& path) { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); }

void LauncherWindow::chooseSharedFile() {
  QString file;
  {
    auto modal = qScopeGuard([this] { --ui_->modal; });
    ++ui_->modal;
    file = QFileDialog::getOpenFileName(this, QStringLiteral("Open a shared file"), QDir::homePath(),
                                        QStringLiteral("Vyrn files (*.vrlp *.vrl);;Vyrn projects (*.vrlp);;"
                                                       "Trained AIs (*.vrl)"));
  }
  if (!file.isEmpty()) openSharedFile(file);
  else if (ui_->refreshPending) refresh();
}

void LauncherWindow::openSharedFile(const QString& file) {
  const sharing::FileKind kind = sharing::kindOf(file);
  if (kind == sharing::FileKind::None || !QFileInfo(file).isFile()) {
    auto modal = qScopeGuard([this] { --ui_->modal; });
    ++ui_->modal;
    QMessageBox::warning(this, QStringLiteral("Can't open that"),
                         kind == sharing::FileKind::None
                             ? QStringLiteral("Vyrn opens shared projects (.vrlp) and trained AIs (.vrl). %1 is "
                                              "neither.")
                                   .arg(QFileInfo(file).fileName())
                             : QStringLiteral("Vyrn can't find %1.").arg(paths::pretty(file)));
    return;
  }
  if (isMinimized()) showNormal();
  raise();
  activateWindow();

  if (kind == sharing::FileKind::Project) {
    QString created;
    QStringList notes;
    {
      auto modal = qScopeGuard([this] { --ui_->modal; });
      ++ui_->modal;
      OpenPackageDialog dialog(file, OpenPackageDialog::Mode::Project, this);
      if (dialog.exec() != QDialog::Accepted) {
        modal.dismiss();
        --ui_->modal;
        if (ui_->refreshPending) refresh();
        return;
      }
      created = dialog.importedProject();
      notes = dialog.notes();
    }
    refresh();
    if (ProjectWindow* window = openProject(created))
      window->showNotes(QStringLiteral("Imported %1").arg(QFileInfo(file).fileName()), notes);
    return;
  }

  // A trained AI: which project gets it?
  QString target;
  TemplateCard tmpl;
  bool fromTemplate = false;
  QString name;
  {
    auto modal = qScopeGuard([this] { --ui_->modal; });
    ++ui_->modal;
    OpenPackageDialog dialog(file, OpenPackageDialog::Mode::BrainChooseProject, this);
    if (dialog.exec() != QDialog::Accepted) {
      modal.dismiss();
      --ui_->modal;
      if (ui_->refreshPending) refresh();
      return;
    }
    target = dialog.chosenProject();
    fromTemplate = target.isEmpty() && dialog.chosenTemplate(&tmpl);
    name = dialog.packageName();
  }
  if (fromTemplate) {
    QString error;
    target = store::createProject(store::uniqueName(name.isEmpty() ? tmpl.name : name), tmpl, &error);
    if (target.isEmpty()) {
      auto modal = qScopeGuard([this] { --ui_->modal; });
      ++ui_->modal;
      QMessageBox::warning(this, QStringLiteral("Couldn't create the project"), error);
      return;
    }
  }
  if (target.isEmpty()) return;
  refresh();
  if (ProjectWindow* window = openProject(target)) window->importBrain(file, false);
}

void LauncherWindow::shareProject(const QString& path) {
  const QString dir = QFileInfo(path).canonicalFilePath();
  if (dir.isEmpty()) return;
  QImage seen;
  if (QPointer<ProjectWindow> open = ui_->windows.value(dir); open) {
    if (!open->saveAll()) return;   // share what its editors show
    seen = open->seesPicture();
  }
  {
    auto modal = qScopeGuard([this] { --ui_->modal; });
    ++ui_->modal;
    ShareDialog dialog(ShareDialog::Kind::Project, dir, seen, this);
    dialog.exec();
  }
  if (ui_->refreshPending) refresh();
}

void LauncherWindow::dragEnterEvent(QDragEnterEvent* event) {
  if (!sharing::packageIn(event->mimeData()).isEmpty())
    event->acceptProposedAction();
  else
    QMainWindow::dragEnterEvent(event);
}

void LauncherWindow::dragMoveEvent(QDragMoveEvent* event) {
  if (!sharing::packageIn(event->mimeData()).isEmpty())
    event->acceptProposedAction();
  else
    QMainWindow::dragMoveEvent(event);
}

void LauncherWindow::dropEvent(QDropEvent* event) {
  const QString file = sharing::packageIn(event->mimeData());
  if (file.isEmpty()) {
    QMainWindow::dropEvent(event);
    return;
  }
  event->acceptProposedAction();
  // Not inside the drop: the program the file came from waits until the drop is handled.
  QTimer::singleShot(0, this, [this, file] { openSharedFile(file); });
}

void LauncherWindow::runQuickSetupCheck() {
  ui_->checkerRealGame = std::any_of(ui_->projects.begin(), ui_->projects.end(),
                                     [](const ProjectCard& p) { return p.mode == QLatin1String("realtime"); });
  ui_->checkerStarted = true;
  ui_->checker->start(ui_->checkerRealGame, /*grabScreen=*/false);
  updateSetupButton();
}

void LauncherWindow::updateSetupButton() {
  const SetupChecker* c = ui_->checker;
  if (c->running()) {
    ui_->setup->setText(QStringLiteral("Setup"));
    ui_->setup->setIcon(theme::dotIcon(theme::faint()));
    ui_->setup->setToolTip(QStringLiteral("Vyrn is checking what it needs on this computer…"));
    return;
  }
  const int failed = c->total() - c->passed();
  if (failed == 0) {
    ui_->setup->setText(QStringLiteral("Setup"));
    ui_->setup->setIcon(theme::dotIcon(theme::success()));
    ui_->setup->setToolTip(QStringLiteral("Everything Vyrn needs is ready. Click to see the details."));
  } else {
    ui_->setup->setText(QStringLiteral("Setup %1/%2").arg(c->passed()).arg(c->total()));
    ui_->setup->setIcon(theme::dotIcon(theme::warning()));
    ui_->setup->setToolTip(failed == 1 ? QStringLiteral("1 thing needs fixing. Click to see how.")
                                       : QStringLiteral("%1 things need fixing. Click to see how.").arg(failed));
  }
  fitHeader();   // "Setup 3/5" is wider than "Setup"
}

void LauncherWindow::changeEvent(QEvent* event) {
  QMainWindow::changeEvent(event);
  if (event->type() == QEvent::ActivationChange && isActiveWindow()) ui_->debounce->start();
}

void LauncherWindow::showEvent(QShowEvent* event) {
  QMainWindow::showEvent(event);
  ui_->poll->start();
}

void LauncherWindow::hideEvent(QHideEvent* event) {
  QMainWindow::hideEvent(event);
  ui_->poll->stop();
}

void LauncherWindow::resizeEvent(QResizeEvent* event) {
  QMainWindow::resizeEvent(event);
  fitHeader();
}

void LauncherWindow::fitHeader() {
  // Full labels when they fit; else icon-only "Connect an AI agent" and "Open folder…" (their tooltips say what
  // they do); on a very narrow window also without the tagline.
  const QMargins margins = centralWidget()->layout()->contentsMargins();
  const int available = centralWidget()->width() - margins.left() - margins.right();
  for (int level = 0; level <= 2; ++level) {
    ui_->agents->setText(level >= 1 ? QString() : QStringLiteral("Connect an AI agent"));
    ui_->open->setText(level >= 1 ? QString() : QStringLiteral("Open folder…"));
    ui_->tagline->setVisible(level < 2);
    ui_->header->invalidate();
    if (ui_->header->sizeHint().width() <= available) break;
  }
}

#include "LauncherWindow.moc"
