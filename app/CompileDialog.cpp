// CompileDialog: vyrn::export_standalone on a background thread (docs/dev/standalone.md section 1).
#include "CompileDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <string>

#include "Background.h"
#include "Paths.h"
#include "ProjectStore.h"
#include "Sharing.h"
#include "Theme.h"
#include "vyrn/package.h"

namespace {

QLabel* mutedLabel(const QString& text, QWidget* parent) {
  auto* label = new QLabel(text, parent);
  label->setProperty("role", "muted");
  label->setWordWrap(true);
  label->setTextFormat(Qt::PlainText);
  return label;
}

}  // namespace

struct CompileDialog::Ui {
  CompileDialog* d = nullptr;
  QString dir, name, folderName;   // folderName: "Catch-numbers-play"
  bool sim = true;
  bool wheelAvailable = false;     // this Vyrn has the vyrn package's wheel (vyrn::find_wheel)

  QStackedWidget* pages = nullptr;
  QComboBox* brain = nullptr;
  QLabel* brainHint = nullptr;
  QLineEdit* out = nullptr;
  QCheckBox* wheel = nullptr;
  QLabel* wheelHint = nullptr;
  QLabel* error = nullptr;
  QPushButton* compile = nullptr;
  QPushButton* cancel = nullptr;

  QLabel* doneTitle = nullptr;
  QLabel* doneText = nullptr;
  QLabel* doneNotes = nullptr;
  QString compiledFolder, lastError;
  QStringList compiledFiles;
  bool compiling = false;

  void build();
  QWidget* buildForm();
  QWidget* buildDone();
  QString target() const;
  void chooseParent();
  void startCompile();
  void compileTo(const QString& folder, bool overwrite);
  void setCompiling(bool on);
  void showError(const QString& text);
  void showDone(const vyrn::StandaloneResult& result);
};

void CompileDialog::Ui::build() {
  const ProjectCard card = store::project(dir);
  name = card.name.isEmpty() ? QFileInfo(dir).fileName() : card.name;
  sim = card.mode != QLatin1String("realtime");
  try {
    folderName = sharing::qs(vyrn::suggested_file_name(dir.toStdString(), "standalone"));
  } catch (const std::exception&) {
    folderName = QStringLiteral("AI-play");
  }
  d->setWindowTitle(QStringLiteral("Compile"));
  auto* layout = new QVBoxLayout(d);
  layout->setContentsMargins(18, 16, 18, 14);
  pages = new QStackedWidget(d);
  pages->addWidget(buildForm());
  pages->addWidget(buildDone());
  layout->addWidget(pages);
  d->resize(580, d->sizeHint().height());
}

QWidget* CompileDialog::Ui::buildForm() {
  auto* page = new QWidget(pages);
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(12);

  auto* title = new QLabel(QStringLiteral("Compile “%1”").arg(name), page);
  title->setFont(theme::scaledFont(1.25, QFont::Bold));
  title->setTextFormat(Qt::PlainText);
  title->setWordWrap(true);
  layout->addWidget(title);
  QString about = QStringLiteral("Turns the trained AI into a small Python program (play.py) that plays without Vyrn, "
                                 "on any computer with Python 3.10 or newer. It plays but doesn't learn.");
  if (!sim)
    about += QStringLiteral(" This AI plays a real game: play.py presses keys only when you start it with "
                            "--allow-keys.");
  layout->addWidget(mutedLabel(about, page));

  auto* form = new QFormLayout;
  form->setLabelAlignment(Qt::AlignLeft);
  form->setHorizontalSpacing(14);
  form->setVerticalSpacing(8);

  brain = new QComboBox(page);
  brain->setObjectName(QStringLiteral("compileBrain"));
  brain->addItem(QStringLiteral("Best: the brain at its best 20-round average"));
  brain->addItem(QStringLiteral("Latest: the brain as it was last saved"));
  auto* brainBox = new QVBoxLayout;
  brainBox->setSpacing(4);
  brainBox->addWidget(brain);
  const QList<BrainCard> brains = store::brains(dir);
  const BrainCard* active = nullptr;
  for (const BrainCard& b : brains)
    if (b.active && store::isTrained(b)) active = &b;
  QString hint = QStringLiteral("No trained AI yet: press Train to make one.");
  if (active) {
    QStringList parts{QStringLiteral("trained %1").arg(store::formatDuration(active->timeTrained))};
    if (active->hasBest) parts.append(QStringLiteral("best 20-round average %1").arg(store::formatPoints(active->best)));
    hint = QStringLiteral("The brain Watch uses: %1.").arg(parts.join(QStringLiteral(" · ")));
  }
  brainHint = mutedLabel(hint, page);
  brainBox->addWidget(brainHint);
  form->addRow(QStringLiteral("Which brain"), brainBox);

  out = new QLineEdit(QDir(QDir::homePath()).filePath(folderName), page);
  out->setObjectName(QStringLiteral("compileFolder"));
  out->setToolTip(QStringLiteral("A new folder: Compile puts play.py, the brain and the scripts in it"));
  auto* choose = new QPushButton(QStringLiteral("Choose…"), page);
  choose->setToolTip(QStringLiteral("Pick the folder the new folder %1 goes in").arg(folderName));
  auto* outRow = new QHBoxLayout;
  outRow->setSpacing(6);
  outRow->addWidget(out, 1);
  outRow->addWidget(choose);
  form->addRow(QStringLiteral("Save to"), outRow);

  const QString wheelFile = sharing::qs(vyrn::wheel_file_name());
  QString wheelPath;
  try {
    wheelPath = sharing::qs(vyrn::find_wheel());
  } catch (const std::exception&) {
  }
  wheelAvailable = !wheelPath.isEmpty();
  wheel = new QCheckBox(QStringLiteral("Include the vyrn package (.whl)"), page);
  wheel->setObjectName(QStringLiteral("compileWheel"));
  wheel->setChecked(wheelAvailable);
  wheel->setEnabled(wheelAvailable);
  auto* wheelBox = new QVBoxLayout;
  wheelBox->setSpacing(4);
  wheelBox->addWidget(wheel);
  wheelHint = mutedLabel(
      wheelPath.isEmpty()
          ? QStringLiteral("This Vyrn has no %1 to include, so each computer needs the vyrn package installed "
                           "another way.")
                .arg(wheelFile)
          : QStringLiteral("%1 (%2): other computers install it with pip, no internet needed.")
                .arg(wheelFile, sharing::sizeText(quint64(QFileInfo(wheelPath).size()))),
      page);
  wheelBox->addWidget(wheelHint);
  form->addRow(QStringLiteral("Python package"), wheelBox);
  layout->addLayout(form);

  error = new QLabel(page);
  error->setObjectName(QStringLiteral("compileError"));
  error->setWordWrap(true);
  error->setTextFormat(Qt::PlainText);
  error->setStyleSheet(QStringLiteral("color: %1;").arg(theme::danger().name()));
  error->hide();
  layout->addWidget(error);
  layout->addStretch(1);

  auto* buttons = new QDialogButtonBox(page);
  compile = buttons->addButton(QStringLiteral("Compile"), QDialogButtonBox::AcceptRole);
  compile->setProperty("role", "primary");
  theme::repolish(compile);   // the button box polished it already
  compile->setDefault(true);
  compile->setEnabled(active != nullptr);
  cancel = buttons->addButton(QDialogButtonBox::Cancel);
  layout->addWidget(buttons);
  QObject::connect(choose, &QPushButton::clicked, d, [this] { chooseParent(); });
  QObject::connect(compile, &QPushButton::clicked, d, [this] { startCompile(); });
  QObject::connect(cancel, &QPushButton::clicked, d, &QDialog::reject);
  return page;
}

QWidget* CompileDialog::Ui::buildDone() {
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
  doneTitle->setObjectName(QStringLiteral("compileDone"));
  doneTitle->setFont(theme::scaledFont(1.15, QFont::Bold));
  doneTitle->setTextFormat(Qt::PlainText);
  doneTitle->setWordWrap(true);
  doneText = new QLabel(page);
  doneText->setTextFormat(Qt::PlainText);
  doneText->setWordWrap(true);
  doneText->setTextInteractionFlags(Qt::TextSelectableByMouse);   // the commands can be copied
  doneNotes = mutedLabel(QString(), page);
  texts->addWidget(doneTitle);
  texts->addWidget(doneText);
  texts->addWidget(doneNotes);
  row->addLayout(texts, 1);
  layout->addLayout(row);
  layout->addStretch(1);
  auto* buttons = new QDialogButtonBox(page);
  QPushButton* folder = buttons->addButton(QStringLiteral("Show in folder"), QDialogButtonBox::ActionRole);
  QPushButton* done = buttons->addButton(QStringLiteral("Done"), QDialogButtonBox::AcceptRole);
  done->setDefault(true);
  layout->addWidget(buttons);
  QObject::connect(folder, &QPushButton::clicked, d,
                   [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(compiledFolder)); });
  QObject::connect(done, &QPushButton::clicked, d, &QDialog::accept);
  return page;
}

QString CompileDialog::Ui::target() const {
  QString path = QDir::fromNativeSeparators(out->text().trimmed());
  if (path == QLatin1String("~")) path = QDir::homePath();
  if (path.startsWith(QLatin1String("~/"))) path = QDir::homePath() + path.mid(1);
  return path.isEmpty() ? QString() : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

// The folder picker chooses where the new folder goes; its name stays <Name>-play (or the one typed).
void CompileDialog::Ui::chooseParent() {
  const QFileInfo current(target());
  const QString start = current.absolutePath().isEmpty() ? QDir::homePath() : current.absolutePath();
  const QString parent = QFileDialog::getExistingDirectory(d, QStringLiteral("Where should the compiled AI go?"), start);
  if (parent.isEmpty()) return;
  const QString leaf = current.fileName().isEmpty() ? folderName : current.fileName();
  out->setText(QDir::toNativeSeparators(QDir(parent).filePath(leaf)));
}

void CompileDialog::Ui::startCompile() {
  if (compiling) return;
  error->hide();
  const QString folder = target();
  if (folder.isEmpty()) {
    showError(QStringLiteral("Please choose a folder for the compiled AI."));
    return;
  }
  bool overwrite = false;
  if (QFileInfo(folder).isDir()) {
    const auto answer = QMessageBox::question(
        d, QStringLiteral("Replace the folder?"),
        QStringLiteral("There is already a folder called %1. Replace it with the newly compiled AI?\n\nVyrn only "
                       "replaces a folder that Compile made before; everything in it is replaced.")
            .arg(QFileInfo(folder).fileName()),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) return;
    overwrite = true;
  }
  compileTo(folder, overwrite);
}

void CompileDialog::Ui::compileTo(const QString& folder, bool overwrite) {
  if (compiling) return;
  error->hide();
  lastError.clear();
  compiledFolder.clear();
  compiledFiles.clear();
  vyrn::StandaloneOptions o;
  o.which = brain->currentIndex() == 1 ? "latest" : "best";
  o.include_wheel = wheel->isChecked();
  o.overwrite = overwrite;
  setCompiling(true);
  const std::string project = dir.toStdString(), outDir = QDir::toNativeSeparators(folder).toStdString();
  background::run<vyrn::StandaloneResult>(
      d, [project, outDir, o] { return vyrn::export_standalone(project, outDir, o); },
      [this](background::Outcome<vyrn::StandaloneResult> outcome) {
        setCompiling(false);
        if (!outcome.value) {
          lastError = outcome.error;
          showError(outcome.error);   // the core's own words (PackageError)
          return;
        }
        showDone(*outcome.value);
      });
}

void CompileDialog::Ui::setCompiling(bool on) {
  compiling = on;
  compile->setEnabled(!on);
  cancel->setEnabled(!on);
  compile->setText(on ? QStringLiteral("Compiling…") : QStringLiteral("Compile"));
  brain->setEnabled(!on);
  out->setEnabled(!on);
  wheel->setEnabled(!on && wheelAvailable);
}

void CompileDialog::Ui::showError(const QString& text) {
  error->setText(text);
  error->show();
}

void CompileDialog::Ui::showDone(const vyrn::StandaloneResult& r) {
  compiledFolder = sharing::qs(r.folder);
  for (const vyrn::PackageFile& f : r.files) compiledFiles.append(sharing::qs(f.path));
  doneTitle->setText(QStringLiteral("Compiled to %1.").arg(paths::pretty(QDir::fromNativeSeparators(compiledFolder))));
  QString text = QStringLiteral("Run it with: python3 play.py\n(in that folder; on Windows: py play.py). Ctrl+C stops it.");
  if (!r.wheel.empty())
    text += QStringLiteral("\n\nOn another computer, copy the whole folder and install the vyrn package once:\n"
                           "pip install %1")
                .arg(sharing::qs(r.wheel));
  doneText->setText(text);
  QStringList notes;
  for (const std::string& note : r.notes) notes.append(sharing::qs(note));
  notes.append(QStringLiteral("README.txt in the folder says how to run it and what its options do."));
  doneNotes->setText(notes.join(QLatin1Char('\n')));
  QWidget* donePage = pages->widget(1);
  pages->removeWidget(pages->widget(0));   // the form is kept (hidden), so the dialog shrinks to this page
  pages->setCurrentWidget(donePage);
  d->resize(d->width(), d->hasHeightForWidth() ? d->heightForWidth(d->width()) : d->sizeHint().height());
}

CompileDialog::CompileDialog(const QString& projectDir, QWidget* parent) : QDialog(parent), ui_(new Ui) {
  ui_->d = this;
  ui_->dir = projectDir;
  setObjectName(QStringLiteral("compileDialog"));
  ui_->build();
}

CompileDialog::~CompileDialog() { delete ui_; }

QString CompileDialog::folder() const { return ui_->target(); }
void CompileDialog::compileTo(const QString& folder, bool overwrite) { ui_->compileTo(folder, overwrite); }
bool CompileDialog::compiling() const { return ui_->compiling; }
QString CompileDialog::compiledFolder() const { return ui_->compiledFolder; }
QStringList CompileDialog::compiledFiles() const { return ui_->compiledFiles; }
QString CompileDialog::errorText() const { return ui_->lastError; }

QString CompileDialog::doneMessage() const {
  return ui_->compiledFolder.isEmpty() ? QString() : ui_->doneTitle->text() + QLatin1Char(' ') + ui_->doneText->text();
}
