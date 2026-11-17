#include "NewProjectDialog.h"

#include <QApplication>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include "Paths.h"
#include "ProjectStore.h"
#include "Theme.h"

namespace {

enum Role { IdRole = Qt::UserRole, DescriptionRole, ModeRole, IndexRole };

// Each template as a rounded card: its picture, name, a mode badge and the description (up to two lines).
class TemplateDelegate : public QStyledItemDelegate {
 public:
  explicit TemplateDelegate(QListWidget* view) : QStyledItemDelegate(view), view_(view) {}
  void setExpectedWidth(int width) { expectedWidth_ = width; }   // before the list has its real size

  QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
    const int viewport = view_->viewport()->width();
    const int width = view_->isVisible() && viewport > 100 ? viewport : expectedWidth_;
    const QFontMetrics nameMetrics(nameFont(option.font));
    const int text = nameMetrics.height() + kLineGap + descriptionHeight(option.font, index, textWidth(width));
    return {width, qMax(kIcon, text) + 2 * kPad + 2 * kGap};
  }

  void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;
    const QRectF card = QRectF(option.rect).adjusted(1.5, kGap + 0.5, -1.5, -kGap - 0.5);

    QColor edge = selected ? theme::accent() : hovered ? QColor(0x3f, 0x52, 0x6e) : theme::border();
    p->setPen(QPen(edge, selected ? 1.6 : 1.0));
    p->setBrush(selected || hovered ? theme::panelHover() : theme::panel());
    p->drawRoundedRect(card, 10, 10);

    const QString id = index.data(IdRole).toString();
    const QString mode = index.data(ModeRole).toString();
    const qreal dpr = p->device() ? p->device()->devicePixelRatioF() : 1.0;
    const QRect iconRect(int(card.left()) + kPad, int(card.center().y()) - kIcon / 2, kIcon, kIcon);
    p->drawPixmap(iconRect, theme::templateIcon(id, mode, kIcon, dpr));

    // Name + mode badge on the first line, the description below; the block is centred vertically.
    const int left = iconRect.right() + kIconGap;
    const int right = int(card.right()) - kPad;
    const QFont name = nameFont(option.font);
    const QFontMetrics nameMetrics(name);
    const int descHeight = descriptionHeight(option.font, index, right - left);
    const int block = nameMetrics.height() + kLineGap + descHeight;
    const int top = int(card.center().y()) - block / 2;

    QFont badgeFont = option.font;
    badgeFont.setPointSizeF(option.font.pointSizeF() * 0.84);
    badgeFont.setWeight(QFont::Bold);
    const QFontMetrics badgeMetrics(badgeFont);
    const bool realtime = mode == QLatin1String("realtime");
    const QString badge = realtime ? QStringLiteral("Real game") : QStringLiteral("Simulation");
    const int badgeWidth = badgeMetrics.horizontalAdvance(badge) + 16;

    const QString title =
        nameMetrics.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, right - left - badgeWidth - 10);
    p->setFont(name);
    p->setPen(theme::text());
    const QRect nameRect(left, top, nameMetrics.horizontalAdvance(title), nameMetrics.height());
    p->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, title);

    const QColor tone = realtime ? theme::warning() : QColor(0x7c, 0xb7, 0xff);
    QColor soft = tone;
    soft.setAlpha(34);
    const qreal pillHeight = badgeMetrics.height() + 4;
    const QRectF pill(nameRect.right() + 10, nameRect.center().y() - pillHeight / 2.0 + 1, badgeWidth, pillHeight);
    p->setPen(Qt::NoPen);
    p->setBrush(soft);
    p->drawRoundedRect(pill, 6, 6);
    p->setFont(badgeFont);
    p->setPen(tone);
    p->drawText(pill, Qt::AlignCenter, badge);

    p->setFont(option.font);
    p->setPen(theme::muted());
    const QRect descRect(left, nameRect.bottom() + 1 + kLineGap, right - left, descHeight);
    p->drawText(descRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, description(option.font, index, descRect.width()));
    p->restore();
  }

 private:
  static constexpr int kIcon = 44;
  static constexpr int kIconGap = 14;
  static constexpr int kPad = 14;
  static constexpr int kGap = 4;
  static constexpr int kLineGap = 4;

  static QFont nameFont(const QFont& base) {
    QFont f = base;
    f.setWeight(QFont::Bold);
    f.setPointSizeF(base.pointSizeF() * 1.06);
    return f;
  }
  static int textWidth(int rowWidth) { return rowWidth - 3 - 2 * kPad - kIcon - kIconGap; }

  // The description, shortened with "…" if it would need more than two lines.
  static QString description(const QFont& font, const QModelIndex& index, int width) {
    const QFontMetrics fm(font);
    const QString text = index.data(DescriptionRole).toString();
    if (fm.boundingRect(QRect(0, 0, width, 10000), Qt::TextWordWrap, text).height() <= 2 * fm.lineSpacing()) return text;
    QString shortened = text;
    while (shortened.size() > 1 &&
           fm.boundingRect(QRect(0, 0, width, 10000), Qt::TextWordWrap, shortened + QStringLiteral("…")).height() >
               2 * fm.lineSpacing())
      shortened.chop(1);
    return shortened.trimmed() + QStringLiteral("…");
  }
  static int descriptionHeight(const QFont& font, const QModelIndex& index, int width) {
    const QFontMetrics fm(font);
    return fm.boundingRect(QRect(0, 0, width, 10000), Qt::TextWordWrap, description(font, index, width)).height();
  }

  QListWidget* view_ = nullptr;
  int expectedWidth_ = 560;
};

QString fallbackDescription(const TemplateCard& t) {
  return t.mode == QLatin1String("realtime") ? QStringLiteral("Teach the AI to play a game on your screen.")
                                             : QStringLiteral("A small game Vyrn simulates very fast.");
}

}  // namespace

struct NewProjectDialog::Ui {
  QList<TemplateCard> templates;
  QListWidget* list = nullptr;
  QLineEdit* name = nullptr;
  QLabel* where = nullptr;
  QLabel* error = nullptr;
  QPushButton* create = nullptr;
  QString autoName;   // the name Vyrn filled in; replaced when the template changes, unless the user typed one
};

NewProjectDialog::NewProjectDialog(QWidget* parent, const QString& preselectTemplate)
    : QDialog(parent), ui_(new Ui) {
  setWindowTitle(QStringLiteral("New project"));
  setMinimumSize(520, 480);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(24, 22, 24, 18);
  layout->setSpacing(10);

  auto* title = new QLabel(QStringLiteral("New project"), this);
  title->setFont(theme::scaledFont(1.45, QFont::Bold));
  auto* intro = new QLabel(QStringLiteral("Pick a starting point. They all work straight away, and you can change anything later."),
                           this);
  intro->setProperty("role", "muted");
  intro->setWordWrap(true);
  layout->addWidget(title);
  layout->addWidget(intro);
  layout->addSpacing(6);

  ui_->templates = store::templates();
  ui_->list = new QListWidget(this);
  auto* delegate = new TemplateDelegate(ui_->list);
  ui_->list->setItemDelegate(delegate);
  ui_->list->setResizeMode(QListView::Adjust);   // re-measure the rows (wrapped descriptions) on resize
  ui_->list->setFrameShape(QFrame::NoFrame);
  ui_->list->setSelectionMode(QAbstractItemView::SingleSelection);
  ui_->list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  ui_->list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  ui_->list->setMouseTracking(true);
  ui_->list->viewport()->setAttribute(Qt::WA_Hover);
  ui_->list->viewport()->setAutoFillBackground(false);
  ui_->list->setAutoFillBackground(false);
  ui_->list->setStyleSheet(QStringLiteral("QListWidget { background: transparent; }"));
  ui_->list->setAccessibleName(QStringLiteral("Templates"));
  int preselectRow = -1;
  for (int i = 0; i < ui_->templates.size(); ++i) {
    const TemplateCard& t = ui_->templates[i];
    auto* item = new QListWidgetItem((t.demo ? QStringLiteral("Demo: ") : QString()) + (t.name.isEmpty() ? t.id : t.name),
                                  ui_->list);
    item->setData(IdRole, t.id);
    item->setData(DescriptionRole, t.description.isEmpty() ? fallbackDescription(t) : t.description);
    item->setData(ModeRole, t.mode);
    item->setData(IndexRole, i);
    item->setToolTip(item->data(DescriptionRole).toString());
    if (t.id == preselectTemplate) preselectRow = i;
    if (preselectRow < 0 && preselectTemplate.isEmpty() && t.id == QLatin1String("balance")) preselectRow = i;
  }
  layout->addWidget(ui_->list, 1);

  if (ui_->templates.isEmpty()) {
    ui_->list->hide();
    auto* missing = new QLabel(QStringLiteral("Vyrn can't find its templates. Reinstall it by running "
                                              "%1 in the folder you downloaded Vyrn into.")
                                   .arg(paths::installCommand()),
                               this);
    missing->setWordWrap(true);
    missing->setProperty("role", "error");
    layout->addWidget(missing, 1, Qt::AlignTop);
  }

  layout->addSpacing(6);
  auto* nameLabel = new QLabel(QStringLiteral("Name"), this);
  nameLabel->setFont(theme::scaledFont(0.95, QFont::Bold));
  ui_->name = new QLineEdit(this);
  ui_->name->setPlaceholderText(QStringLiteral("e.g. My first AI"));
  ui_->name->setMaxLength(80);
  ui_->name->setClearButtonEnabled(true);
  nameLabel->setBuddy(ui_->name);
  ui_->where = new QLabel(this);
  ui_->where->setProperty("role", "faint");
  ui_->where->setWordWrap(true);
  ui_->error = new QLabel(this);
  ui_->error->setProperty("role", "error");
  ui_->error->setWordWrap(true);
  ui_->error->hide();
  layout->addWidget(nameLabel);
  layout->addWidget(ui_->name);
  layout->addWidget(ui_->where);
  layout->addWidget(ui_->error);

  auto* buttons = new QHBoxLayout;
  buttons->addStretch(1);
  auto* cancel = new QPushButton(QStringLiteral("Cancel"), this);
  cancel->setAutoDefault(false);
  ui_->create = new QPushButton(QStringLiteral("Create"), this);
  ui_->create->setProperty("role", "primary");
  ui_->create->setDefault(true);
  buttons->addWidget(cancel);
  buttons->addWidget(ui_->create);
  layout->addSpacing(6);
  layout->addLayout(buttons);

  connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
  connect(ui_->create, &QPushButton::clicked, this, &NewProjectDialog::create);
  connect(ui_->list, &QListWidget::currentRowChanged, this, &NewProjectDialog::templateChanged);
  connect(ui_->list, &QListWidget::itemActivated, this, [this] {
    if (ui_->create->isEnabled()) create();
  });
  connect(ui_->name, &QLineEdit::textChanged, this, &NewProjectDialog::validate);

  if (!ui_->templates.isEmpty()) ui_->list->setCurrentRow(preselectRow >= 0 ? preselectRow : 0);
  validate();
  ui_->name->setFocus();
  ui_->name->selectAll();

  // Wide enough for the descriptions and tall enough to show every template, as far as the screen allows.
  constexpr int kWidth = 660;
  ensurePolished();
  const QMargins margins = layout->contentsMargins();
  delegate->setExpectedWidth(kWidth - margins.left() - margins.right() - 2 * ui_->list->frameWidth());
  int rows = 0;
  for (int i = 0; i < ui_->list->count(); ++i) rows += ui_->list->sizeHintForRow(i);
  const int screenHeight = screen() ? screen()->availableGeometry().height() : 900;
  ui_->list->setMinimumHeight(qMin(rows + 2 * ui_->list->frameWidth() + 2, screenHeight / 2));
  resize(kWidth, qMin(minimumSizeHint().height() + 8, screenHeight - 60));
}

NewProjectDialog::~NewProjectDialog() { delete ui_; }

QString NewProjectDialog::createdPath() const { return createdPath_; }

void NewProjectDialog::templateChanged() {
  const int row = ui_->list->currentRow();
  if (row < 0 || row >= ui_->templates.size()) return;
  // Follow the template with a fresh, unused name until the user types their own.
  const QString current = ui_->name->text().simplified();
  if (current.isEmpty() || current == ui_->autoName) {
    const TemplateCard& t = ui_->templates[row];
    ui_->autoName = store::uniqueName(t.name.isEmpty() ? t.id : t.name);
    ui_->name->setText(ui_->autoName);
    ui_->name->selectAll();
  }
  validate();
}

void NewProjectDialog::validate() {
  ui_->error->hide();
  const QString name = ui_->name->text().simplified();
  const bool haveTemplate = ui_->list->currentRow() >= 0 && !ui_->templates.isEmpty();
  bool ok = haveTemplate && !name.isEmpty();
  QString role = QStringLiteral("faint");
  if (name.isEmpty()) {
    ui_->where->setText(QStringLiteral("Type a name for your project."));
  } else {
    const QString folder = store::folderFor(name);
    if (QFileInfo::exists(folder)) {
      ui_->where->setText(QStringLiteral("There's already a project in %1. Pick another name.").arg(paths::pretty(folder)));
      role = QStringLiteral("error");
      ok = false;
    } else {
      ui_->where->setText(QStringLiteral("It will be saved in %1").arg(paths::pretty(folder)));
    }
  }
  if (ui_->where->property("role").toString() != role) {
    ui_->where->setProperty("role", role);
    theme::repolish(ui_->where);
  }
  const bool invalid = role == QLatin1String("error");
  if (ui_->name->property("invalid").toBool() != invalid) {
    ui_->name->setProperty("invalid", invalid);
    theme::repolish(ui_->name);
  }
  ui_->create->setEnabled(ok);
}

void NewProjectDialog::create() {
  const int row = ui_->list->currentRow();
  if (row < 0 || row >= ui_->templates.size()) return;
  QString error;
  const QString path = store::createProject(ui_->name->text(), ui_->templates[row], &error);
  if (path.isEmpty()) {
    ui_->error->setText(error.isEmpty() ? QStringLiteral("Vyrn couldn't create the project.") : error);
    ui_->error->show();
    return;
  }
  createdPath_ = path;
  accept();
}
