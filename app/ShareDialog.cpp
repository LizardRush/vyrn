// ShareDialog: writes a .vrlp (a whole project) or a .vrl (one trained brain) with vyrn::export_project /
// vyrn::export_brain on a background thread. For projects, a trial export into the cache folder lists exactly
// what goes in and how big it is.
#include "ShareDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <filesystem>
#include <string>
#include <vector>

#include "Background.h"
#include "Paths.h"
#include "ProjectStore.h"
#include "Sharing.h"
#include "Theme.h"
#include "vyrn/package.h"

namespace {

constexpr int kMaxDescription = 2000;   // docs/dev/package.md: ExportOptions::description
constexpr int kMaxAuthor = 100;

QLabel* mutedLabel(const QString& text, QWidget* parent) {
  auto* label = new QLabel(text, parent);
  label->setProperty("role", "muted");
  label->setWordWrap(true);
  label->setTextFormat(Qt::PlainText);
  return label;
}

}  // namespace

struct ShareDialog::Ui {
  ShareDialog* d = nullptr;
  Kind kind = Kind::Project;
  QString dir, name;
  std::vector<uint8_t> preview;   // PNG of what the AI sees; empty when there's no picture
  bool hasBrain = false;

  QStackedWidget* pages = nullptr;
  QLineEdit* author = nullptr;
  QPlainTextEdit* description = nullptr;
  QComboBox* brain = nullptr;
  QLabel* brainHint = nullptr;
  QCheckBox* usePreview = nullptr;
  QTreeWidget* files = nullptr;
  QLabel* size = nullptr;
  QLabel* error = nullptr;
  QPushButton* save = nullptr;
  QPushButton* cancel = nullptr;

  QLabel* doneTitle = nullptr;
  QLabel* doneText = nullptr;
  QLabel* doneWhere = nullptr;
  QString savedFile, lastError;

  int estimateRun = 0;   // only the newest trial export is shown
  bool saving = false;

  void build(const QImage& seen);
  QWidget* buildForm(const QImage& seen);
  QWidget* buildDone();
  vyrn::ExportOptions options() const;
  QString extension() const { return kind == Kind::Project ? QStringLiteral(".vrlp") : QStringLiteral(".vrl"); }
  void estimate();
  void showEstimate(const background::Outcome<vyrn::PackageInfo>& outcome);
  void startSave();
  void saveTo(QString out, bool overwrite);
  void showError(const QString& text);
  void setSaving(bool on);
  void showDone(const QString& file, const vyrn::PackageInfo& info);
};

void ShareDialog::Ui::build(const QImage& seen) {
  const ProjectCard card = store::project(dir);
  name = card.name.isEmpty() ? QFileInfo(dir).fileName() : card.name;
  const QList<BrainCard> brains = store::brains(dir);
  hasBrain = !brains.isEmpty();
  preview = sharing::previewPng(seen);

  d->setWindowTitle(kind == Kind::Project ? QStringLiteral("Share project") : QStringLiteral("Export this brain"));
  auto* layout = new QVBoxLayout(d);
  layout->setContentsMargins(18, 16, 18, 14);
  pages = new QStackedWidget(d);
  pages->addWidget(buildForm(seen));
  pages->addWidget(buildDone());
  layout->addWidget(pages);

  // The brain this would share: the active one (else the newest).
  const BrainCard* active = nullptr;
  for (const BrainCard& b : brains)
    if (b.active) active = &b;
  if (!active && !brains.isEmpty()) active = &brains.first();
  if (active) {
    QStringList parts{QStringLiteral("Trained for %1").arg(store::formatDuration(active->timeTrained))};
    if (active->hasBest) parts.append(QStringLiteral("best 20-round average %1").arg(store::formatPoints(active->best)));
    brainHint->setText(QStringLiteral("The brain Watch uses: %1.").arg(parts.join(QStringLiteral(" · "))));
  } else {
    brainHint->setText(QStringLiteral("No trained AI yet: press Train to make one. The project can be shared "
                                      "without it."));
  }
  if (kind == Kind::Project && !hasBrain) {
    const QSignalBlocker quiet(brain);   // estimate() runs once, below
    brain->setCurrentIndex(2);
    brain->setEnabled(false);
  }
  d->resize(kind == Kind::Project ? 600 : 540, d->sizeHint().height());
  estimate();
}

QWidget* ShareDialog::Ui::buildForm(const QImage& seen) {
  auto* page = new QWidget(pages);
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(12);

  auto* title = new QLabel(kind == Kind::Project ? QStringLiteral("Share “%1”").arg(name)
                                                 : QStringLiteral("Export the brain of “%1”").arg(name),
                           page);
  title->setFont(theme::scaledFont(1.25, QFont::Bold));
  title->setTextFormat(Qt::PlainText);
  title->setWordWrap(true);
  layout->addWidget(title);
  layout->addWidget(mutedLabel(
      kind == Kind::Project
          ? QStringLiteral("Saves the project as one .vrlp file: its scripts, pictures and settings, and its trained "
                           "AI if you include it. Anyone with Vyrn can open it and watch your AI play.")
          : QStringLiteral("Saves the trained AI as one .vrl file. Anyone with Vyrn can add it to a project with the "
                           "same scripts and watch it play. A .vrl file holds no code."),
      page));

  auto* form = new QFormLayout;
  form->setLabelAlignment(Qt::AlignLeft);
  form->setHorizontalSpacing(14);
  form->setVerticalSpacing(8);
  author = new QLineEdit(sharing::rememberedAuthor(), page);   // remembered from last time; never the system name
  author->setMaxLength(kMaxAuthor);
  author->setPlaceholderText(QStringLiteral("Your name or nickname (optional)"));
  author->setToolTip(QStringLiteral("Shown to the people who open the file. Vyrn remembers it for next time."));
  form->addRow(QStringLiteral("Author"), author);

  description = new QPlainTextEdit(page);
  description->setPlaceholderText(QStringLiteral("What it plays, how well it does, anything people should know "
                                                 "(optional)"));
  description->setTabChangesFocus(true);
  description->setFixedHeight(QFontMetrics(description->font()).lineSpacing() * 4 + 16);
  form->addRow(QStringLiteral("Description"), description);

  brain = new QComboBox(page);
  brain->addItem(QStringLiteral("Best: the brain at its best 20-round average"));
  brain->addItem(QStringLiteral("Latest: the brain as it was last saved"));
  if (kind == Kind::Project) brain->addItem(QStringLiteral("Don't include it (scripts, pictures and settings only)"));
  auto* brainBox = new QVBoxLayout;
  brainBox->setSpacing(4);
  brainBox->addWidget(brain);
  brainHint = mutedLabel(QString(), page);
  brainBox->addWidget(brainHint);
  form->addRow(kind == Kind::Project ? QStringLiteral("Include the trained AI") : QStringLiteral("Which brain"),
               brainBox);

  if (!preview.empty()) {
    auto* previewRow = new QHBoxLayout;
    previewRow->setSpacing(12);
    auto* picture = new QLabel(page);
    const QImage scaled = seen.scaled(96, 96, Qt::KeepAspectRatio, Qt::FastTransformation);
    picture->setPixmap(QPixmap::fromImage(scaled));
    picture->setFixedSize(scaled.size());
    usePreview = new QCheckBox(QStringLiteral("Include this picture of what the AI sees"), page);
    usePreview->setChecked(true);
    previewRow->addWidget(picture, 0, Qt::AlignTop);
    previewRow->addWidget(usePreview, 1, Qt::AlignVCenter);
    form->addRow(QStringLiteral("Preview"), previewRow);
  }
  layout->addLayout(form);

  if (kind == Kind::Project) {
    auto* filesTitle = new QLabel(QStringLiteral("What goes in"), page);
    filesTitle->setFont(theme::scaledFont(1.0, QFont::Bold));
    layout->addWidget(filesTitle);
    files = new QTreeWidget(page);
    files->setObjectName(QStringLiteral("shareFiles"));
    files->setColumnCount(2);
    files->setHeaderLabels({QStringLiteral("File"), QStringLiteral("Size")});
    files->setRootIsDecorated(false);
    files->setSelectionMode(QAbstractItemView::NoSelection);
    files->setFocusPolicy(Qt::NoFocus);
    files->header()->setStretchLastSection(false);
    files->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    files->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    files->setFixedHeight(150);
    layout->addWidget(files);
    size = mutedLabel(QString(), page);
    layout->addWidget(size);
    QObject::connect(brain, &QComboBox::currentIndexChanged, d, [this](int) { estimate(); });
    if (usePreview) QObject::connect(usePreview, &QCheckBox::toggled, d, [this](bool) { estimate(); });
  }

  error = new QLabel(page);
  error->setWordWrap(true);
  error->setTextFormat(Qt::PlainText);
  error->setStyleSheet(QStringLiteral("color: %1;").arg(theme::danger().name()));
  error->hide();
  layout->addWidget(error);

  auto* buttons = new QDialogButtonBox(page);
  save = buttons->addButton(QStringLiteral("Save…"), QDialogButtonBox::AcceptRole);
  save->setProperty("role", "primary");
  theme::repolish(save);   // the button box polished it already
  save->setDefault(true);
  cancel = buttons->addButton(QDialogButtonBox::Cancel);
  layout->addWidget(buttons);
  QObject::connect(save, &QPushButton::clicked, d, [this] { startSave(); });
  QObject::connect(cancel, &QPushButton::clicked, d, &QDialog::reject);
  return page;
}

QWidget* ShareDialog::Ui::buildDone() {
  auto* page = new QWidget(pages);
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(10);
  auto* row = new QHBoxLayout;
  row->setSpacing(12);
  auto* tick = new QLabel(page);
  tick->setPixmap(theme::lineIcon(QStringLiteral("check"), theme::success(), 28).pixmap(28, 28));
  row->addWidget(tick, 0, Qt::AlignTop);
  auto* texts = new QVBoxLayout;
  texts->setSpacing(6);
  doneTitle = new QLabel(page);
  doneTitle->setObjectName(QStringLiteral("shareSaved"));
  doneTitle->setFont(theme::scaledFont(1.15, QFont::Bold));
  doneTitle->setTextFormat(Qt::PlainText);
  doneTitle->setWordWrap(true);
  doneText = new QLabel(page);
  doneText->setTextFormat(Qt::PlainText);
  doneText->setWordWrap(true);
  doneWhere = mutedLabel(QString(), page);
  texts->addWidget(doneTitle);
  texts->addWidget(doneText);
  texts->addWidget(doneWhere);
  row->addLayout(texts, 1);
  layout->addLayout(row);
  layout->addStretch(1);
  auto* buttons = new QDialogButtonBox(page);
  QPushButton* folder = buttons->addButton(QStringLiteral("Show in folder"), QDialogButtonBox::ActionRole);
  QPushButton* done = buttons->addButton(QStringLiteral("Done"), QDialogButtonBox::AcceptRole);
  done->setDefault(true);
  layout->addWidget(buttons);
  QObject::connect(folder, &QPushButton::clicked, d, [this] {
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(savedFile).absolutePath()));
  });
  QObject::connect(done, &QPushButton::clicked, d, &QDialog::accept);
  return page;
}

vyrn::ExportOptions ShareDialog::Ui::options() const {
  vyrn::ExportOptions o;
  o.author = author->text().trimmed().toStdString();
  o.description = description->toPlainText().trimmed().toStdString();
  o.which = brain->currentIndex() == 1 ? "latest" : "best";
  o.include_brain = kind == Kind::Brain || (hasBrain && brain->currentIndex() != 2);
  if (usePreview && usePreview->isChecked()) o.preview_png = preview;
  return o;
}

// A trial export into the cache folder: exactly the files that go in, and the size (give or take the text).
void ShareDialog::Ui::estimate() {
  if (kind != Kind::Project) return;
  const int run = ++estimateRun;
  files->clear();
  size->setText(QStringLiteral("Working out what goes in…"));
  vyrn::ExportOptions o = options();
  o.overwrite = true;
  const std::string project = dir.toStdString();
  const std::string out = QDir(paths::cacheDir())
                              .filePath(QStringLiteral("share-estimate-%1-%2.vrlp")
                                            .arg(QCoreApplication::applicationPid())
                                            .arg(run))
                              .toStdString();
  background::run<vyrn::PackageInfo>(
      d,
      [project, out, o] {
        struct Cleanup {
          std::string file;
          ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove(file, ec);
            std::filesystem::remove(file + ".part", ec);
          }
        } cleanup{out};
        return vyrn::export_project(project, out, o);
      },
      [this, run](background::Outcome<vyrn::PackageInfo> outcome) {
        if (run == estimateRun) showEstimate(outcome);
      });
}

void ShareDialog::Ui::showEstimate(const background::Outcome<vyrn::PackageInfo>& outcome) {
  files->clear();
  if (!outcome.value) {
    size->setText(outcome.error);
    return;
  }
  const vyrn::PackageInfo& info = *outcome.value;
  for (const vyrn::PackageFile& f : info.files)
    new QTreeWidgetItem(files, {sharing::qs(f.path), sharing::sizeText(f.size)});
  for (const vyrn::BrainSummary& b : info.brains) {
    QStringList parts{b.which == "latest" ? QStringLiteral("latest") : QStringLiteral("best")};
    if (b.time_trained_s > 0) parts.append(QStringLiteral("trained %1").arg(store::formatDuration(b.time_trained_s)));
    new QTreeWidgetItem(files, {QStringLiteral("The trained AI (%1)").arg(parts.join(QStringLiteral(", "))), QString()});
  }
  if (!info.preview_png.empty())
    new QTreeWidgetItem(files, {QStringLiteral("Preview picture"), sharing::sizeText(info.preview_png.size())});
  const qsizetype count = qsizetype(info.files.size());
  size->setText(QStringLiteral("%1, about %2 in all.")
                    .arg(count == 1 ? QStringLiteral("1 file") : QStringLiteral("%1 files").arg(count),
                         sharing::sizeText(info.file_size)));
}

void ShareDialog::Ui::showError(const QString& text) {
  error->setText(text);
  error->show();
}

void ShareDialog::Ui::setSaving(bool on) {
  saving = on;
  save->setEnabled(!on);
  cancel->setEnabled(!on);
  save->setText(on ? QStringLiteral("Saving…") : QStringLiteral("Save…"));
  for (QWidget* field : {static_cast<QWidget*>(author), static_cast<QWidget*>(description)}) field->setEnabled(!on);
}

void ShareDialog::Ui::startSave() {
  if (saving) return;
  error->hide();
  if (description->toPlainText().trimmed().size() > kMaxDescription) {
    showError(QStringLiteral("The description is too long. Keep it under %1 characters.")
                  .arg(QLocale().toString(kMaxDescription)));
    return;
  }
  QString suggested;
  try {
    suggested = sharing::qs(vyrn::suggested_file_name(dir.toStdString(), kind == Kind::Project ? "project" : "brain"));
  } catch (const std::exception& e) {
    showError(sharing::qs(e.what()));
    return;
  }
  const QString out = QFileDialog::getSaveFileName(
      d, kind == Kind::Project ? QStringLiteral("Save the shared project") : QStringLiteral("Save the brain"),
      QDir::home().filePath(suggested),
      kind == Kind::Project ? QStringLiteral("Vyrn project (*.vrlp)") : QStringLiteral("Vyrn brain (*.vrl)"));
  if (!out.isEmpty()) saveTo(out, true);   // the save dialog already asked about replacing that file
}

void ShareDialog::Ui::saveTo(QString out, bool overwrite) {
  if (saving) return;
  error->hide();
  lastError.clear();
  savedFile.clear();
  if (description->toPlainText().trimmed().size() > kMaxDescription) {
    lastError = QStringLiteral("The description is too long. Keep it under %1 characters.")
                    .arg(QLocale().toString(kMaxDescription));
    showError(lastError);
    return;
  }
  vyrn::ExportOptions o = options();
  o.overwrite = overwrite;
  if (!out.endsWith(extension(), Qt::CaseInsensitive)) {
    out += extension();
    o.overwrite = false;   // a name nobody saw: never replace a file without asking
  }
  setSaving(true);
  const std::string project = dir.toStdString(), file = out.toStdString();
  const bool whole = kind == Kind::Project;
  background::run<vyrn::PackageInfo>(
      d,
      [project, file, o, whole] {
        return whole ? vyrn::export_project(project, file, o) : vyrn::export_brain(project, file, o);
      },
      [this, out](background::Outcome<vyrn::PackageInfo> outcome) {
        setSaving(false);
        if (!outcome.value) {
          lastError = outcome.error;
          showError(outcome.error);
          return;
        }
        sharing::rememberAuthor(author->text());
        showDone(out, *outcome.value);
      });
}

void ShareDialog::Ui::showDone(const QString& file, const vyrn::PackageInfo& info) {
  savedFile = file;
  const quint64 bytes = info.file_size > 0 ? info.file_size : quint64(QFileInfo(file).size());
  doneTitle->setText(QStringLiteral("Saved %1 (%2).").arg(QFileInfo(file).fileName(), sharing::sizeText(bytes)));
  if (kind == Kind::Brain)
    doneText->setText(QStringLiteral("Anyone with Vyrn can add it to a project with the same scripts and watch your "
                                     "AI."));
  else if (info.brains.empty())
    doneText->setText(QStringLiteral("Anyone with Vyrn can double-click it to open your project."));
  else
    doneText->setText(QStringLiteral("Anyone with Vyrn can double-click it to open your project and watch your AI."));
  doneWhere->setText(QStringLiteral("In %1").arg(paths::pretty(QFileInfo(file).absolutePath())));
  QWidget* done = pages->widget(1);
  pages->removeWidget(pages->widget(0));   // the form is kept (hidden), so the dialog shrinks to this page
  pages->setCurrentWidget(done);
  d->resize(d->width(), d->hasHeightForWidth() ? d->heightForWidth(d->width()) : d->sizeHint().height());
}

ShareDialog::ShareDialog(Kind kind, const QString& projectDir, const QImage& seen, QWidget* parent)
    : QDialog(parent), ui_(new Ui) {
  ui_->d = this;
  ui_->kind = kind;
  ui_->dir = projectDir;
  setObjectName(QStringLiteral("shareDialog"));
  ui_->build(seen);
}

ShareDialog::~ShareDialog() { delete ui_; }

void ShareDialog::saveTo(const QString& file, bool overwrite) { ui_->saveTo(file, overwrite); }
bool ShareDialog::saving() const { return ui_->saving; }
QString ShareDialog::savedFile() const { return ui_->savedFile; }
QString ShareDialog::errorText() const { return ui_->lastError; }

QString ShareDialog::savedMessage() const {
  return ui_->savedFile.isEmpty() ? QString() : ui_->doneTitle->text() + QLatin1Char(' ') + ui_->doneText->text();
}
