// OpenPackageDialog: reads a shared file with vyrn::inspect_package() on a background thread (it checks every
// entry and weight, which takes a moment for big files), shows what it holds and imports it.
#include "OpenPackageDialog.h"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>
#include <optional>
#include <string>

#include "Background.h"
#include "Paths.h"
#include "PlotWidget.h"
#include "Sharing.h"
#include "Theme.h"
#include "vyrn/package.h"

namespace {

using sharing::qs;

constexpr int kPreviewSide = 128;
constexpr int kRoleProject = Qt::UserRole;        // target list: a project folder
constexpr int kRoleTemplate = Qt::UserRole + 1;   // target list: a template id ("New <template> project")

QLabel* textLabel(QWidget* parent, const QString& role = QString()) {
  auto* label = new QLabel(parent);
  label->setWordWrap(true);
  label->setTextFormat(Qt::PlainText);
  if (!role.isEmpty()) label->setProperty("role", role);
  return label;
}

QLabel* sectionTitle(const QString& text, QWidget* parent) {
  auto* label = new QLabel(text, parent);
  label->setFont(theme::scaledFont(1.0, QFont::Bold));
  return label;
}

QFrame* callout(QWidget* parent, QLabel** text) {
  auto* frame = new QFrame(parent);
  frame->setProperty("role", "callout");
  auto* layout = new QVBoxLayout(frame);
  layout->setContentsMargins(12, 9, 12, 9);
  *text = textLabel(frame);
  layout->addWidget(*text);
  return frame;
}

// "a 10×10 picture" / "3 numbers" from a brain's input (picture shape {height, width}).
QString inputText(const vyrn::BrainSummary& b) {
  if (b.input == "picture" && b.input_shape.size() >= 2)
    return QStringLiteral("a %1×%2 picture").arg(b.input_shape[1]).arg(b.input_shape[0]);
  if (b.input == "numbers" && !b.input_shape.empty())
    return b.input_shape[0] == 1 ? QStringLiteral("1 number") : QStringLiteral("%1 numbers").arg(b.input_shape[0]);
  return {};
}

QString createdDate(const std::string& created) {   // "2026-11-13T19:20:00Z" -> "2026-11-13"
  return qs(created.substr(0, std::min<size_t>(10, created.size())));
}

}  // namespace

struct OpenPackageDialog::Ui {
  OpenPackageDialog* d = nullptr;
  QString file;
  Mode mode = Mode::Project;
  QString intoProject;
  QString root;
  bool isLoading = true, isBusy = false;
  QString error;
  std::optional<vyrn::PackageInfo> info;
  QString imported, importError;
  QStringList notes;
  QString chosenPath, chosenTemplateId;

  QLabel* status = nullptr;
  QWidget* content = nullptr;
  QLabel* preview = nullptr;
  QLabel* title = nullptr;
  QLabel* byline = nullptr;
  QLabel* modeLabel = nullptr;
  QLabel* description = nullptr;
  QLabel* brainStats = nullptr;
  QLabel* brainSees = nullptr;
  PlotWidget* chart = nullptr;
  QWidget* filesBox = nullptr;
  QLabel* filesTitle = nullptr;
  QTreeWidget* files = nullptr;
  QFrame* startBox = nullptr;
  QLabel* startText = nullptr;
  QFrame* codeBox = nullptr;
  QLabel* codeText = nullptr;
  QLabel* brainNote = nullptr;
  QWidget* targetsBox = nullptr;
  QListWidget* targets = nullptr;
  QLabel* problem = nullptr;
  QPushButton* go = nullptr;
  QPushButton* cancel = nullptr;

  void build();
  void load();
  void fill(const vyrn::PackageInfo& p);
  void fillTargets(const vyrn::PackageInfo& p);
  void updateGo();
};

void OpenPackageDialog::Ui::build() {
  const QString name = QFileInfo(file).fileName();
  d->setWindowTitle(mode == Mode::Project ? QStringLiteral("Open a shared project")
                                          : QStringLiteral("Add a trained AI"));
  d->setObjectName(QStringLiteral("openPackageDialog"));
  auto* outer = new QVBoxLayout(d);
  outer->setContentsMargins(18, 16, 18, 14);
  outer->setSpacing(10);

  status = textLabel(d, QStringLiteral("muted"));
  status->setText(QStringLiteral("Reading %1…").arg(name));
  outer->addWidget(status);

  content = new QWidget;
  auto* layout = new QVBoxLayout(content);
  layout->setContentsMargins(0, 0, 6, 0);
  layout->setSpacing(12);

  auto* header = new QHBoxLayout;
  header->setSpacing(14);
  preview = new QLabel(content);
  preview->setObjectName(QStringLiteral("packagePreview"));
  preview->hide();
  header->addWidget(preview, 0, Qt::AlignTop);
  auto* titles = new QVBoxLayout;
  titles->setSpacing(4);
  title = textLabel(content);
  title->setFont(theme::scaledFont(1.35, QFont::Bold));
  byline = textLabel(content, QStringLiteral("muted"));
  modeLabel = textLabel(content);
  description = textLabel(content);
  description->setTextInteractionFlags(Qt::TextSelectableByMouse);
  titles->addWidget(title);
  titles->addWidget(byline);
  titles->addWidget(modeLabel);
  titles->addSpacing(4);
  titles->addWidget(description);
  titles->addStretch(1);
  header->addLayout(titles, 1);
  layout->addLayout(header);

  layout->addWidget(sectionTitle(QStringLiteral("The trained AI"), content));
  brainStats = textLabel(content);
  brainSees = textLabel(content, QStringLiteral("muted"));
  layout->addWidget(brainStats);
  layout->addWidget(brainSees);
  chart = new PlotWidget(content);
  chart->setObjectName(QStringLiteral("packageChart"));
  chart->setMeanOnly(true);
  chart->setMinimumHeight(130);
  chart->setMaximumHeight(150);
  layout->addWidget(chart);

  filesBox = new QWidget(content);
  auto* filesLayout = new QVBoxLayout(filesBox);
  filesLayout->setContentsMargins(0, 0, 0, 0);
  filesLayout->setSpacing(6);
  filesTitle = sectionTitle(QStringLiteral("Files"), filesBox);
  files = new QTreeWidget(filesBox);
  files->setColumnCount(2);
  files->setHeaderLabels({QStringLiteral("File"), QStringLiteral("Size")});
  files->setRootIsDecorated(false);
  files->setSelectionMode(QAbstractItemView::NoSelection);
  files->setFocusPolicy(Qt::NoFocus);
  files->header()->setStretchLastSection(false);
  files->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  files->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  files->setFixedHeight(120);
  filesLayout->addWidget(filesTitle);
  filesLayout->addWidget(files);
  layout->addWidget(filesBox);

  startBox = callout(content, &startText);
  layout->addWidget(startBox);
  codeBox = callout(content, &codeText);
  codeBox->setObjectName(QStringLiteral("codeWarning"));
  codeText->setTextFormat(Qt::RichText);
  layout->addWidget(codeBox);
  brainNote = textLabel(content, QStringLiteral("muted"));
  layout->addWidget(brainNote);

  targetsBox = new QWidget(content);
  auto* targetsLayout = new QVBoxLayout(targetsBox);
  targetsLayout->setContentsMargins(0, 0, 0, 0);
  targetsLayout->setSpacing(6);
  targetsLayout->addWidget(sectionTitle(QStringLiteral("Add it to"), targetsBox));
  targets = new QListWidget(targetsBox);
  targets->setObjectName(QStringLiteral("brainTargets"));
  targets->setMinimumHeight(140);
  targetsLayout->addWidget(targets);
  layout->addWidget(targetsBox);
  layout->addStretch(1);

  auto* scroll = new QScrollArea(d);
  scroll->setWidget(content);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll->hide();
  outer->addWidget(scroll, 1);
  QObject::connect(d, &OpenPackageDialog::loaded, d, [this, scroll] {
    scroll->setVisible(info.has_value());
    if (!info) return;
    // As tall as what it shows (wrapped text included), plus the buttons and margins; scrolls beyond 760 px.
    QLayout* l = content->layout();
    const int width = d->width() - 36 - 6;
    const int needed = l->hasHeightForWidth() ? l->totalHeightForWidth(width) : l->totalSizeHint().height();
    d->resize(d->width(), std::min(760, needed + 76));
  });

  problem = textLabel(d);
  problem->setStyleSheet(QStringLiteral("color: %1;").arg(theme::danger().name()));
  problem->hide();
  outer->addWidget(problem);

  auto* buttons = new QDialogButtonBox(d);
  go = buttons->addButton(mode == Mode::Project              ? QStringLiteral("Import")
                          : mode == Mode::BrainIntoProject ? QStringLiteral("Add to this project")
                                                           : QStringLiteral("Add brain"),
                          QDialogButtonBox::AcceptRole);
  go->setProperty("role", "primary");
  theme::repolish(go);   // the button box polished it already
  go->setDefault(true);
  go->setEnabled(false);
  cancel = buttons->addButton(QDialogButtonBox::Cancel);
  outer->addWidget(buttons);
  QObject::connect(go, &QPushButton::clicked, d, [this] { d->importNow(); });
  QObject::connect(cancel, &QPushButton::clicked, d, &QDialog::reject);
  QObject::connect(targets, &QListWidget::currentRowChanged, d, [this](int) { updateGo(); });
  QObject::connect(targets, &QListWidget::itemDoubleClicked, d, [this](QListWidgetItem*) { d->importNow(); });

  d->resize(640, 260);
}

void OpenPackageDialog::Ui::load() {
  const std::string path = file.toStdString();
  background::run<vyrn::PackageInfo>(
      d, [path] { return vyrn::inspect_package(path); },
      [this](background::Outcome<vyrn::PackageInfo> outcome) {
        isLoading = false;
        const QString name = QFileInfo(file).fileName();
        if (!outcome.value) {
          error = outcome.error.isEmpty() ? QStringLiteral("It isn't a file Vyrn can read.") : outcome.error;
          status->setText(QStringLiteral("Vyrn can't open %1. %2").arg(name, error));
          status->setStyleSheet(QStringLiteral("color: %1;").arg(theme::danger().name()));
        } else if ((mode == Mode::Project) != (outcome.value->kind == "project")) {
          error = mode == Mode::Project ? QStringLiteral("It holds a trained AI, not a project.")
                                        : QStringLiteral("It holds a whole project, not just a trained AI: open it "
                                                         "from the Vyrn home screen.");
          status->setText(QStringLiteral("Vyrn can't open %1 here. %2").arg(name, error));
          status->setStyleSheet(QStringLiteral("color: %1;").arg(theme::danger().name()));
        } else {
          info = std::move(outcome.value);
          status->hide();
          fill(*info);
        }
        updateGo();
        Q_EMIT d->loaded();
      });
}

void OpenPackageDialog::Ui::fill(const vyrn::PackageInfo& p) {
  title->setText(qs(p.name).isEmpty() ? QFileInfo(file).completeBaseName() : qs(p.name));
  QStringList by;
  by.append(p.author.empty() ? QStringLiteral("The author didn't give a name") : QStringLiteral("By %1").arg(qs(p.author)));
  if (!p.created.empty()) by.append(QStringLiteral("shared %1").arg(createdDate(p.created)));
  by.append(sharing::sizeText(p.file_size));
  byline->setText(by.join(QStringLiteral(" · ")));
  QString playing = sharing::modeText(p.mode);
  if (p.uses_link) playing += QStringLiteral(" (a game connects to Vyrn over Vyrn Link)");
  modeLabel->setText(playing);
  modeLabel->setVisible(!playing.isEmpty());
  description->setText(qs(p.description));
  description->setVisible(!p.description.empty());

  const QImage picture = sharing::imageFromPng(p.preview_png);
  if (!picture.isNull()) {
    const int longest = std::max(picture.width(), picture.height());
    const QImage scaled = longest > kPreviewSide
                              ? picture.scaled(kPreviewSide, kPreviewSide, Qt::KeepAspectRatio, Qt::FastTransformation)
                              : picture.scaled(picture.size() * std::max(1, kPreviewSide / longest),
                                               Qt::KeepAspectRatio, Qt::FastTransformation);
    preview->setPixmap(QPixmap::fromImage(scaled));
    preview->setFixedSize(scaled.size());
    preview->setToolTip(QStringLiteral("What the AI sees"));
    preview->show();
  }

  // The trained AI: the active brain comes first.
  chart->clear();
  if (p.brains.empty()) {
    brainStats->setText(QStringLiteral("No trained AI is included: press Train after importing to train one."));
    brainSees->hide();
    chart->hide();
  } else {
    const vyrn::BrainSummary& b = p.brains.front();
    QStringList stats{QStringLiteral("Trained for %1").arg(store::formatDuration(b.time_trained_s))};
    stats.append(b.rounds == 1 ? QStringLiteral("1 round")
                               : QStringLiteral("%1 rounds").arg(QLocale().toString(qint64(b.rounds))));
    if (b.has_best) stats.append(QStringLiteral("best 20-round average %1").arg(store::formatPoints(b.best_mean20)));
    brainStats->setText(stats.join(QStringLiteral(" · ")));
    QStringList sees;
    const QString input = inputText(b);
    if (!input.isEmpty()) sees.append(QStringLiteral("Sees %1").arg(input));
    QStringList choices;
    for (const std::string& c : b.choices) choices.append(qs(c));
    if (!choices.isEmpty()) sees.append(QStringLiteral("choices: %1").arg(choices.join(QStringLiteral(", "))));
    brainSees->setText(sees.join(QStringLiteral(" · ")));
    brainSees->setVisible(!sees.isEmpty());
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (const vyrn::HistoryPoint& h : b.history) chart->addRound(h.round, nan, h.mean20);
    chart->setVisible(!b.history.empty());
  }

  const bool project = mode == Mode::Project;
  filesBox->setVisible(project);
  startBox->setVisible(project && !p.start_program.empty());
  codeBox->setVisible(project);
  brainNote->setVisible(!project);
  targetsBox->setVisible(mode == Mode::BrainChooseProject);
  if (project) {
    files->clear();
    int scripts = 0;
    quint64 total = 0;
    for (const vyrn::PackageFile& f : p.files) {
      const QString path = qs(f.path);
      new QTreeWidgetItem(files, {path, sharing::sizeText(f.size)});
      if (path.endsWith(QLatin1String(".py"), Qt::CaseInsensitive)) ++scripts;
      total += f.size;
    }
    filesTitle->setText(QStringLiteral("Files (%1, %2)")
                            .arg(p.files.size() == 1 ? QStringLiteral("1 file")
                                                     : QStringLiteral("%1 files").arg(p.files.size()),
                                 sharing::sizeText(total)));
    startText->setText(QStringLiteral("The author starts “%1” with this project. Vyrn never starts it by itself: to "
                                      "do the same, set it in Settings → Start program.")
                           .arg(qs(p.start_program)));
    codeText->setText(
        QStringLiteral("<b>This project contains Python code</b> (%1) that will run on this computer once you trust "
                       "it, with the same access to your files as any program you run. Vyrn opens it untrusted: read "
                       "its scripts first, and only trust projects from people you trust.")
            .arg(scripts == 1 ? QStringLiteral("1 script") : QStringLiteral("%1 scripts").arg(scripts)));
  } else {
    brainNote->setText(
        mode == Mode::BrainIntoProject
            ? QStringLiteral("Vyrn adds it to “%1” and makes it the brain Watch uses; your other brains are kept. A "
                             "trained AI holds no code, and Vyrn checks that it fits the project's scripts first.")
                  .arg(intoProject)
            : QStringLiteral("A trained AI holds no code: adding it runs nothing. It works in a project whose "
                             "scripts see the same things and have the same choices, and Vyrn checks that first."));
    if (mode == Mode::BrainChooseProject) fillTargets(p);
  }
}

void OpenPackageDialog::Ui::fillTargets(const vyrn::PackageInfo& p) {
  targets->clear();
  const QString templateId = qs(p.template_id);
  if (!templateId.isEmpty()) {
    for (const TemplateCard& t : store::templates()) {
      if (t.id != templateId) continue;
      auto* item = new QListWidgetItem(theme::lineIcon(QStringLiteral("plus"), theme::accent()),
                                       QStringLiteral("New %1 project with this brain").arg(t.name), targets);
      item->setData(kRoleTemplate, t.id);
      item->setToolTip(QStringLiteral("Makes a new project from Vyrn's “%1” template and adds the brain to it")
                           .arg(t.name));
    }
  }
  QList<ProjectCard> projects = store::projects();
  std::stable_partition(projects.begin(), projects.end(),
                        [&templateId](const ProjectCard& c) { return !templateId.isEmpty() && c.templateId == templateId; });
  for (const ProjectCard& c : std::as_const(projects)) {
    QString text = c.name;
    if (!templateId.isEmpty() && c.templateId == templateId) text += QStringLiteral(" (same template)");
    auto* item = new QListWidgetItem(theme::lineIcon(QStringLiteral("folder"), theme::muted()), text, targets);
    item->setData(kRoleProject, c.path);
    item->setToolTip(paths::pretty(c.path));
  }
  if (targets->count() == 0) {
    auto* item = new QListWidgetItem(QStringLiteral("You have no projects yet: create one with the same scripts "
                                                    "first."),
                                     targets);
    item->setFlags(Qt::NoItemFlags);
  } else {
    targets->setCurrentRow(0);
  }
}

void OpenPackageDialog::Ui::updateGo() {
  bool ok = !isLoading && !isBusy && info.has_value();
  if (ok && mode == Mode::BrainChooseProject) {
    const QListWidgetItem* item = targets->currentItem();
    ok = item && (item->data(kRoleProject).isValid() || item->data(kRoleTemplate).isValid());
  }
  go->setEnabled(ok);
  cancel->setEnabled(!isBusy);
}

OpenPackageDialog::OpenPackageDialog(const QString& file, Mode mode, QWidget* parent, const QString& intoProject)
    : QDialog(parent), ui_(new Ui) {
  ui_->d = this;
  ui_->file = QFileInfo(file).absoluteFilePath();
  ui_->mode = mode;
  ui_->intoProject = intoProject;
  ui_->root = paths::projectsRoot();
  ui_->build();
  ui_->load();
}

OpenPackageDialog::~OpenPackageDialog() { delete ui_; }

void OpenPackageDialog::reject() {
  if (!ui_->isBusy) QDialog::reject();   // an import that is being written finishes first
}

bool OpenPackageDialog::loading() const { return ui_->isLoading; }
QString OpenPackageDialog::loadError() const { return ui_->error; }
QString OpenPackageDialog::packageName() const { return ui_->info ? qs(ui_->info->name) : QString(); }
bool OpenPackageDialog::busy() const { return ui_->isBusy; }
QString OpenPackageDialog::importError() const { return ui_->importError; }
void OpenPackageDialog::setProjectsRoot(const QString& root) { ui_->root = root; }
QString OpenPackageDialog::importedProject() const { return ui_->imported; }
QStringList OpenPackageDialog::notes() const { return ui_->notes; }
QString OpenPackageDialog::chosenProject() const { return ui_->chosenPath; }

bool OpenPackageDialog::chosenTemplate(TemplateCard* out) const {
  if (ui_->chosenTemplateId.isEmpty()) return false;
  for (const TemplateCard& t : store::templates()) {
    if (t.id != ui_->chosenTemplateId) continue;
    if (out) *out = t;
    return true;
  }
  return false;
}

void OpenPackageDialog::importNow() {
  Ui& u = *ui_;
  if (u.isLoading || u.isBusy || !u.info) return;
  u.problem->hide();
  if (u.mode == Mode::BrainIntoProject) {
    accept();
    return;
  }
  if (u.mode == Mode::BrainChooseProject) {
    const QListWidgetItem* item = u.targets->currentItem();
    if (!item) return;
    u.chosenPath = item->data(kRoleProject).toString();
    u.chosenTemplateId = item->data(kRoleTemplate).toString();
    if (u.chosenPath.isEmpty() && u.chosenTemplateId.isEmpty()) return;
    accept();
    return;
  }
  u.isBusy = true;
  u.importError.clear();
  u.go->setText(QStringLiteral("Importing…"));
  u.updateGo();
  const std::string file = u.file.toStdString(), root = u.root.toStdString();
  background::run<vyrn::ImportResult>(
      this, [file, root] { return vyrn::import_project(file, root); },
      [this](background::Outcome<vyrn::ImportResult> outcome) {
        Ui& u = *ui_;
        u.isBusy = false;
        u.go->setText(QStringLiteral("Import"));
        u.updateGo();
        if (!outcome.value) {
          u.importError = outcome.error;
          u.problem->setText(QStringLiteral("Vyrn couldn't import it. %1").arg(outcome.error));
          u.problem->show();
          return;
        }
        u.imported = qs(outcome.value->project);
        u.notes.clear();
        for (const std::string& note : outcome.value->notes) u.notes.append(qs(note));
        accept();
      });
}
