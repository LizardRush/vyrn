// TrustDialog: the person's decision to let Vyrn run an imported project's Python code.
#include "TrustDialog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <optional>

#include "Sharing.h"
#include "Theme.h"
#include "vyrn/package.h"

namespace {

constexpr int kMaxDepth = 8;   // the same depth a .vrlp may hold

const std::array<QLatin1StringView, 4> kMainScripts = {QLatin1StringView("inputs.py"), QLatin1StringView("responses.py"),
                                                       QLatin1StringView("points.py"), QLatin1StringView("world.py")};

int rank(const QString& file) {
  for (size_t i = 0; i < kMainScripts.size(); ++i)
    if (file == kMainScripts[i]) return int(i);
  return int(kMainScripts.size());
}

QString purpose(const QString& file) {
  switch (rank(file)) {
    case 0: return QStringLiteral("what the AI sees");
    case 1: return QStringLiteral("what its choices do");
    case 2: return QStringLiteral("how it scores");
    case 3: return QStringLiteral("the simulated game");
    default: return {};
  }
}

// Every .py file Python could import from the project: the main scripts first, then the others by name.
QStringList scriptsOf(const QString& dir) {
  QStringList scripts;
  const QDir root(dir);
  QDirIterator it(dir, {QStringLiteral("*.py")}, QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    const QString relative = root.relativeFilePath(it.next());
    const QStringList parts = relative.split(QLatin1Char('/'));
    if (parts.size() > kMaxDepth + 1) continue;
    const bool skip = std::any_of(parts.begin(), parts.end() - 1, [](const QString& part) {
      return part == QLatin1String("__pycache__") || part == QLatin1String("brains") || part == QLatin1String("runs");
    });
    if (!skip) scripts.append(relative);
  }
  std::sort(scripts.begin(), scripts.end(), [](const QString& a, const QString& b) {
    const int ra = rank(a), rb = rank(b);
    if (ra != rb) return ra < rb;
    return a.compare(b, Qt::CaseInsensitive) < 0;
  });
  return scripts;
}

int lineCount(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return -1;
  const QByteArray text = file.read(4 * 1024 * 1024);
  return int(text.count('\n')) + (text.isEmpty() || text.endsWith('\n') ? 0 : 1);
}

QLabel* wrapped(const QString& text, QWidget* parent, Qt::TextFormat format = Qt::PlainText) {
  auto* label = new QLabel(text, parent);
  label->setWordWrap(true);
  label->setTextFormat(format);
  return label;
}

}  // namespace

TrustDialog::TrustDialog(const QString& projectDir, bool andRun, QWidget* parent) : QDialog(parent), dir_(projectDir) {
  setObjectName(QStringLiteral("trustDialog"));
  setWindowTitle(QStringLiteral("Trust this project?"));
  std::optional<vyrn::ImportOrigin> origin;
  try {
    origin = vyrn::import_origin(dir_.toStdString());
  } catch (const std::exception&) {
  }

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(18, 16, 18, 14);
  layout->setSpacing(10);

  const QString name = origin && !origin->name.empty() ? sharing::qs(origin->name) : QFileInfo(dir_).fileName();
  QString from = origin && !origin->file.empty() ? sharing::qs(origin->file) : QStringLiteral("a shared file");
  if (origin && !origin->author.empty()) from += QStringLiteral(", shared by %1").arg(sharing::qs(origin->author));
  if (origin && !origin->imported.empty()) from += QStringLiteral(" (imported %1)").arg(sharing::qs(origin->imported));
  auto* title = wrapped(QStringLiteral("“%1” came from %2.").arg(name, from), this);
  title->setFont(theme::scaledFont(1.12, QFont::Bold));
  layout->addWidget(title);
  layout->addWidget(wrapped(
      QStringLiteral("Its Python code hasn't run on this computer yet. Once you trust it, Vyrn runs these scripts "
                     "whenever you press Check, Train or Watch, with the same access to your files as any program you "
                     "run. Read them first, and only trust projects from people you trust."),
      this));

  auto* listTitle = new QLabel(QStringLiteral("Its scripts (click one to read it):"), this);
  listTitle->setFont(theme::scaledFont(1.0, QFont::Bold));
  layout->addWidget(listTitle);
  auto* list = new QListWidget(this);
  list->setObjectName(QStringLiteral("trustScripts"));
  for (const QString& script : scriptsOf(dir_)) {
    const int lines = lineCount(QDir(dir_).filePath(script));
    QStringList parts;
    if (!purpose(script).isEmpty()) parts.append(purpose(script));
    if (lines >= 0) parts.append(lines == 1 ? QStringLiteral("1 line") : QStringLiteral("%1 lines").arg(lines));
    auto* item = new QListWidgetItem(
        parts.isEmpty() ? script : QStringLiteral("%1 — %2").arg(script, parts.join(QStringLiteral(", "))), list);
    item->setData(Qt::UserRole, script);
    item->setToolTip(QStringLiteral("Open %1 in the editor").arg(script));
  }
  if (list->count() == 0) new QListWidgetItem(QStringLiteral("No Python scripts found"), list);
  list->setMaximumHeight(std::min(220, 26 * std::max(3, list->count()) + 8));
  layout->addWidget(list);
  connect(list, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
    const QString script = item->data(Qt::UserRole).toString();
    if (script.isEmpty()) return;
    Q_EMIT scriptClicked(script);
    reject();
  });

  if (origin && !origin->start_program.empty())
    layout->addWidget(wrapped(QStringLiteral("The author started “%1” with this project. Vyrn never starts it by "
                                             "itself: to do the same, set it in Settings → Start program.")
                                  .arg(sharing::qs(origin->start_program)),
                              this));
  if (!QFileInfo::exists(QDir(dir_).filePath(QStringLiteral("world.py")))) {
    auto* keys = wrapped(QStringLiteral("<b>This is a real game:</b> this AI presses keys in the window you choose "
                                        "while it plays."),
                         this, Qt::RichText);
    keys->setStyleSheet(QStringLiteral("color: %1;").arg(theme::warning().name()));
    layout->addWidget(keys);
  }

  error_ = wrapped(QString(), this);
  error_->setStyleSheet(QStringLiteral("color: %1;").arg(theme::danger().name()));
  error_->hide();
  layout->addWidget(error_);

  auto* buttons = new QDialogButtonBox(this);
  QPushButton* trust = buttons->addButton(andRun ? QStringLiteral("Trust and run") : QStringLiteral("Trust this project"),
                                          QDialogButtonBox::AcceptRole);
  trust->setObjectName(QStringLiteral("trustButton"));
  trust->setAutoDefault(false);
  QPushButton* cancel = buttons->addButton(QDialogButtonBox::Cancel);
  cancel->setDefault(true);   // Enter never trusts by accident
  layout->addWidget(buttons);
  connect(trust, &QPushButton::clicked, this, &TrustDialog::trustNow);
  connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
  resize(560, sizeHint().height());
}

void TrustDialog::trustNow() {
  try {
    vyrn::trust_project(dir_.toStdString());
  } catch (const std::exception& e) {
    error_->setText(QStringLiteral("Vyrn couldn't record that you trust it: %1").arg(sharing::qs(e.what())));
    error_->show();
    return;
  }
  accept();
}
