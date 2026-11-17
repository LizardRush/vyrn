#include "SettingsDialog.h"

#include <QCheckBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

#include "Paths.h"
#include "ProjectStore.h"
#include "Theme.h"
#include "Updater.h"
#include "vyrn/platform.h"

namespace {

constexpr int kDefaultDecisions = 10;
constexpr int kDefaultRoundSeconds = 60;
constexpr int kDefaultRoundTicks = 1000;
constexpr int kFieldWidth = 150;

// A titled, rounded panel; returns the panel and its grid (label | field, hints below the field). Every card uses
// the same label column width, so the fields line up from card to card.
QFrame* sectionCard(const QString& title, QWidget* parent, QGridLayout** grid) {
  auto* frame = new QFrame(parent);
  frame->setProperty("role", "card");
  auto* layout = new QVBoxLayout(frame);
  layout->setContentsMargins(18, 14, 18, 16);
  layout->setSpacing(10);
  auto* heading = new QLabel(title, frame);
  heading->setFont(theme::scaledFont(1.0, QFont::Bold));
  layout->addWidget(heading);
  *grid = new QGridLayout;
  (*grid)->setHorizontalSpacing(16);
  (*grid)->setVerticalSpacing(4);
  (*grid)->setColumnStretch(1, 1);
  (*grid)->setColumnMinimumWidth(0, frame->fontMetrics().horizontalAdvance(QStringLiteral("Decisions per second")) + 4);
  layout->addLayout(*grid);
  return frame;
}

QLabel* hint(const QString& text, QWidget* parent) {
  auto* label = new QLabel(text, parent);
  label->setProperty("role", "faint");
  label->setWordWrap(true);
  return label;
}

int intSetting(const QJsonObject& settings, const char* key, int fallback) {
  const QJsonValue v = settings.value(QLatin1String(key));
  return v.isDouble() && std::isfinite(v.toDouble()) ? int(std::lround(v.toDouble())) : fallback;
}

}  // namespace

struct SettingsDialog::Ui {
  bool realtime = false;
  QJsonObject agents;   // the existing "agents" object (other keys are kept)
  QJsonObject linkObject;   // the existing "link" object (other keys are kept)
  LinkSettings link0;       // Vyrn Link as the dialog opened

  // Vyrn Link (real games only)
  QCheckBox* linkOn = nullptr;
  QSpinBox* linkPort = nullptr;
  QLabel* linkAddress = nullptr;
  QCheckBox* linkLockstep = nullptr;
  QSpinBox* linkFrames = nullptr;
  QLabel* linkCode = nullptr;
  QPushButton* linkNewCode = nullptr;
  QList<QWidget*> linkFields;   // enabled while linkOn is checked

  QSpinBox* decisions = nullptr;
  QSpinBox* round = nullptr;
  QLineEdit* startProgram = nullptr;
  QCheckBox* agentsScreen = nullptr;
  QLineEdit* python = nullptr;
  QPushButton* test = nullptr;
  QLabel* testResult = nullptr;
  QLabel* error = nullptr;

  // What the fields showed when the dialog opened: only values the user changed are written.
  int decisions0 = 0, round0 = 0;
  QString startProgram0, python0;
  bool agentsScreen0 = false;
  QCheckBox* autoUpdate = nullptr;
};

SettingsDialog::SettingsDialog(const QString& projectDir, QWidget* parent)
    : QDialog(parent), ui_(new Ui), projectDir_(projectDir) {
  const ProjectCard project = store::project(projectDir);
  const QJsonObject settings = store::settings(projectDir);
  ui_->realtime = project.mode == QLatin1String("realtime");
  ui_->agents = settings.value(QLatin1String("agents")).toObject();
  ui_->linkObject = settings.value(QLatin1String("link")).toObject();
  ui_->link0 = store::linkSettings(settings);
  const QString name = project.name.isEmpty() ? QFileInfo(projectDir).fileName() : project.name;

  setWindowTitle(QStringLiteral("Settings — %1").arg(name));
  setMinimumWidth(560);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(24, 22, 24, 18);
  layout->setSpacing(14);

  auto* title = new QLabel(QStringLiteral("Settings"), this);
  title->setFont(theme::scaledFont(1.45, QFont::Bold));
  auto* subtitle = new QLabel(QStringLiteral("%1  ·  %2  ·  saved in vyrn.json")
                                  .arg(name, ui_->realtime ? QStringLiteral("Real game") : QStringLiteral("Simulation")),
                              this);
  subtitle->setProperty("role", "muted");
  auto* titles = new QVBoxLayout;
  titles->setSpacing(2);
  titles->addWidget(title);
  titles->addWidget(subtitle);
  layout->addLayout(titles);

  auto* body = new QWidget(this);
  auto* cards = new QVBoxLayout(body);
  cards->setContentsMargins(0, 0, 0, 0);
  cards->setSpacing(12);

  // ---- timing ----
  QGridLayout* grid = nullptr;
  QFrame* timing = sectionCard(QStringLiteral("Timing"), body, &grid);
  ui_->decisions = new QSpinBox(timing);
  ui_->decisions->setRange(1, 60);
  ui_->decisions->setValue(intSetting(settings, "decisions_per_second", kDefaultDecisions));
  ui_->decisions->setFixedWidth(kFieldWidth);
  ui_->decisions->setAccessibleName(QStringLiteral("Decisions per second"));
  ui_->round = new QSpinBox(timing);
  ui_->round->setFixedWidth(kFieldWidth);
  if (ui_->realtime) {
    ui_->round->setRange(1, 86400);
    ui_->round->setSuffix(QStringLiteral(" s"));
    ui_->round->setValue(intSetting(settings, "round_seconds", kDefaultRoundSeconds));
  } else {
    ui_->round->setRange(1, 10000000);
    ui_->round->setSuffix(QStringLiteral(" ticks"));
    ui_->round->setSingleStep(100);
    ui_->round->setValue(intSetting(settings, "round_ticks", kDefaultRoundTicks));
  }
  ui_->round->setAccessibleName(QStringLiteral("Round length"));
  auto* decisionsLabel = new QLabel(QStringLiteral("Decisions per second"), timing);
  decisionsLabel->setBuddy(ui_->decisions);
  auto* roundLabel = new QLabel(QStringLiteral("Round length"), timing);
  roundLabel->setBuddy(ui_->round);
  grid->addWidget(decisionsLabel, 0, 0);
  grid->addWidget(ui_->decisions, 0, 1, Qt::AlignLeft);
  grid->addWidget(hint(ui_->realtime ? QStringLiteral("How often the AI looks at your game and picks a choice.")
                                     : QStringLiteral("How fast Watch plays at 1×. Training always runs as fast as "
                                                      "it can."),
                       timing),
                  1, 1);
  grid->setRowMinimumHeight(2, 10);
  grid->addWidget(roundLabel, 3, 0);
  grid->addWidget(ui_->round, 3, 1, Qt::AlignLeft);
  grid->addWidget(hint(ui_->realtime ? QStringLiteral("The longest a round lasts. It ends sooner when game_over() "
                                                      "is true, and then Vyrn calls restart().")
                                     : QStringLiteral("The longest a round lasts, in ticks (one tick = one choice). "
                                                      "It ends sooner when game_over() is true."),
                       timing),
                  4, 1);
  cards->addWidget(timing);

  // ---- real games: the program to start, and whether coding agents may look at the screen ----
  if (ui_->realtime) {
    QFrame* game = sectionCard(QStringLiteral("Your game"), body, &grid);
    ui_->startProgram = new QLineEdit(settings.value(QLatin1String("start_program")).toString(), game);
    ui_->startProgram->setPlaceholderText(QStringLiteral("e.g. python3 demo_game.py or chromium chrome://dino"));
    ui_->startProgram->setClearButtonEnabled(true);
    auto* startLabel = new QLabel(QStringLiteral("Start program"), game);
    startLabel->setBuddy(ui_->startProgram);
    grid->addWidget(startLabel, 0, 0);
    grid->addWidget(ui_->startProgram, 0, 1);
    grid->addWidget(hint(QStringLiteral("Vyrn runs this command (in the project folder) when Check, Train or Watch "
                                        "starts, unless the one it started is still running, and closes it again on "
                                        "Stop. Leave it empty to start your game yourself."),
                         game),
                    1, 1);
    cards->addWidget(game);

    // Vyrn Link: a game (or a mod inside it) connects over the network, sends its numbers and gets the buttons.
    QFrame* linkCard = sectionCard(QStringLiteral("Vyrn Link"), body, &grid);
    linkCard->setObjectName(QStringLiteral("linkCard"));
    ui_->linkOn = new QCheckBox(QStringLiteral("Let a game connect with Vyrn Link"), linkCard);
    ui_->linkOn->setChecked(ui_->link0.on);
    grid->addWidget(ui_->linkOn, 0, 0, 1, 2);
    auto* linkHint = hint(QStringLiteral("For games with a mod that talks to Vyrn, like Geometry Dash with Geode: the "
                                         "game sends its numbers and the AI holds its buttons. Nothing is read from "
                                         "the screen or pressed on this computer."),
                          linkCard);
    linkHint->setContentsMargins(27, 0, 0, 0);
    grid->addWidget(linkHint, 1, 0, 1, 2);
    grid->setRowMinimumHeight(2, 8);

    ui_->linkPort = new QSpinBox(linkCard);
    ui_->linkPort->setRange(1024, 65535);
    ui_->linkPort->setValue(ui_->link0.port);
    ui_->linkPort->setFixedWidth(kFieldWidth);
    ui_->linkPort->setAccessibleName(QStringLiteral("Vyrn Link port"));
    auto* portLabel = new QLabel(QStringLiteral("Port"), linkCard);
    portLabel->setBuddy(ui_->linkPort);
    grid->addWidget(portLabel, 3, 0);
    grid->addWidget(ui_->linkPort, 3, 1, Qt::AlignLeft);
    ui_->linkAddress = hint(QString(), linkCard);
    ui_->linkAddress->setTextInteractionFlags(Qt::TextSelectableByMouse);
    grid->addWidget(ui_->linkAddress, 4, 1);

    auto* codeLabel = new QLabel(QStringLiteral("Pairing code"), linkCard);
    ui_->linkCode = new QLabel(linkCard);
    ui_->linkCode->setObjectName(QStringLiteral("linkCode"));
    ui_->linkCode->setFont(theme::scaledFont(1.15, QFont::Bold));
    ui_->linkCode->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ui_->linkNewCode = new QPushButton(QStringLiteral("New code"), linkCard);
    ui_->linkNewCode->setAutoDefault(false);
    ui_->linkNewCode->setToolTip(QStringLiteral("Make a new pairing code: games have to use the new one"));
    auto* codeRow = new QHBoxLayout;
    codeRow->setSpacing(12);
    codeRow->addWidget(ui_->linkCode);
    codeRow->addWidget(ui_->linkNewCode);
    codeRow->addStretch(1);
    grid->setRowMinimumHeight(5, 6);
    grid->addWidget(codeLabel, 6, 0);
    grid->addLayout(codeRow, 6, 1);
    grid->addWidget(hint(QStringLiteral("Type it into your game's mod. A game on this computer (127.0.0.1) doesn't "
                                        "need it. It stays on this computer: shared projects never include it."),
                         linkCard),
                    7, 1);

    ui_->linkLockstep = new QCheckBox(QStringLiteral("Lockstep: the game waits for the AI at every step"), linkCard);
    ui_->linkLockstep->setChecked(ui_->link0.lockstep);
    grid->setRowMinimumHeight(8, 8);
    grid->addWidget(ui_->linkLockstep, 9, 0, 1, 2);
    auto* lockstepHint = hint(QStringLiteral("Training runs like a simulation: no timing jitter, and it can go "
                                             "faster than real time. The game's mod has to support it."),
                              linkCard);
    lockstepHint->setContentsMargins(27, 0, 0, 0);
    grid->addWidget(lockstepHint, 10, 0, 1, 2);
    ui_->linkFrames = new QSpinBox(linkCard);
    ui_->linkFrames->setRange(1, 1000);
    ui_->linkFrames->setValue(ui_->link0.framesPerStep);
    ui_->linkFrames->setFixedWidth(kFieldWidth);
    ui_->linkFrames->setAccessibleName(QStringLiteral("Frames per step"));
    auto* framesLabel = new QLabel(QStringLiteral("Frames per step"), linkCard);
    framesLabel->setBuddy(ui_->linkFrames);
    grid->addWidget(framesLabel, 11, 0);
    grid->addWidget(ui_->linkFrames, 11, 1, Qt::AlignLeft);
    grid->addWidget(hint(QStringLiteral("Lockstep: how many of the game's frames pass between two decisions."),
                         linkCard),
                    12, 1);
    cards->addWidget(linkCard);
    ui_->linkFields = {portLabel, ui_->linkPort, ui_->linkAddress, codeLabel, ui_->linkCode, ui_->linkNewCode,
                       ui_->linkLockstep, lockstepHint, framesLabel, ui_->linkFrames};

    auto showAddress = [this] {
      ui_->linkAddress->setText(QStringLiteral("Your game connects to %1").arg(store::linkUrl(ui_->linkPort->value())));
    };
    auto enableFields = [this] {
      const bool on = ui_->linkOn->isChecked();
      for (QWidget* field : ui_->linkFields) field->setEnabled(on);
      ui_->linkFrames->setEnabled(on && ui_->linkLockstep->isChecked());
      if (on && ui_->linkCode->text().isEmpty()) ui_->linkCode->setText(store::linkCode(projectDir_));
    };
    showAddress();
    enableFields();
    connect(ui_->linkPort, &QSpinBox::valueChanged, this, showAddress);
    connect(ui_->linkOn, &QCheckBox::toggled, this, enableFields);
    connect(ui_->linkLockstep, &QCheckBox::toggled, this, enableFields);
    connect(ui_->linkNewCode, &QPushButton::clicked, this, [this] {
      QString error;
      const QString code = store::newLinkCode(projectDir_, &error);
      if (code.isEmpty()) {
        ui_->error->setText(error);
        ui_->error->show();
        return;
      }
      ui_->linkCode->setText(code);
      ui_->linkNewCode->setToolTip(QStringLiteral("Saved. Type the new code into your game's mod; a run that is "
                                                  "going on keeps the old one until it stops."));
    });

    QFrame* agentsCard = sectionCard(QStringLiteral("AI coding agents"), body, &grid);
    ui_->agentsScreen = new QCheckBox(QStringLiteral("Let AI coding agents look at the screen"), agentsCard);
    ui_->agentsScreen->setChecked(ui_->agents.value(QLatin1String("screen")).toBool(false));
    grid->addWidget(ui_->agentsScreen, 0, 0, 1, 2);
    auto* agentsHint = hint(QStringLiteral("A connected agent's Check can then take pictures of your screen. "
                                           "Without this, its Check works only if the scripts don't look at the "
                                           "screen. Agents can never press keys or start training a real game: you "
                                           "always do that yourself."),
                            agentsCard);
    agentsHint->setContentsMargins(27, 0, 0, 0);   // lines up with the check box text
    grid->addWidget(agentsHint, 1, 0, 1, 2);
    cards->addWidget(agentsCard);
  }

  // ---- updates (an app setting, not a project one) ----
  QFrame* updatesCard = sectionCard(QStringLiteral("Updates"), body, &grid);
  ui_->autoUpdate = new QCheckBox(QStringLiteral("Check for updates automatically"), updatesCard);
  ui_->autoUpdate->setChecked(Updater::autoEnabled());
  grid->addWidget(ui_->autoUpdate, 0, 0, 1, 2);
  cards->addWidget(updatesCard);

  // ---- advanced ----
  QFrame* advanced = sectionCard(QStringLiteral("Advanced"), body, &grid);
  ui_->python = new QLineEdit(settings.value(QLatin1String("python")).toString(), advanced);
  ui_->python->setPlaceholderText(
      QStringLiteral("%1 (the default)").arg(QString::fromStdString(vyrn::platform::default_python())));
  ui_->python->setClearButtonEnabled(true);
  ui_->test = new QPushButton(QStringLiteral("Test"), advanced);
  ui_->test->setAutoDefault(false);
  ui_->test->setToolTip(QStringLiteral("Check that this Python starts and has numpy and Pillow"));
  auto* pythonRow = new QHBoxLayout;
  pythonRow->setSpacing(8);
  pythonRow->addWidget(ui_->python, 1);
  pythonRow->addWidget(ui_->test);
  auto* pythonLabel = new QLabel(QStringLiteral("Python"), advanced);
  pythonLabel->setBuddy(ui_->python);
  grid->addWidget(pythonLabel, 0, 0);
  grid->addLayout(pythonRow, 0, 1);
  ui_->testResult = new QLabel(advanced);
  ui_->testResult->setWordWrap(true);
  ui_->testResult->setTextInteractionFlags(Qt::TextSelectableByMouse);
  ui_->testResult->hide();
  grid->addWidget(ui_->testResult, 1, 1);
  grid->addWidget(hint(QStringLiteral("The Python that runs your scripts. Leave it empty unless you need a "
                                      "different one, for example with extra packages."),
                       advanced),
                  2, 1);
  cards->addWidget(advanced);
  cards->addStretch(1);
  connect(ui_->test, &QPushButton::clicked, this, &SettingsDialog::testPython);

  auto* scroll = new QScrollArea(this);
  scroll->setWidget(body);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll->viewport()->setAutoFillBackground(false);
  body->setAutoFillBackground(false);
  layout->addWidget(scroll, 1);

  ui_->error = new QLabel(this);
  ui_->error->setProperty("role", "error");
  ui_->error->setWordWrap(true);
  ui_->error->hide();
  layout->addWidget(ui_->error);

  auto* buttons = new QHBoxLayout;
  buttons->addStretch(1);
  auto* cancel = new QPushButton(QStringLiteral("Cancel"), this);
  cancel->setAutoDefault(false);
  auto* save = new QPushButton(QStringLiteral("Save"), this);
  save->setProperty("role", "primary");
  save->setDefault(true);
  buttons->addWidget(cancel);
  buttons->addWidget(save);
  layout->addLayout(buttons);
  connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
  connect(save, &QPushButton::clicked, this, &SettingsDialog::accept);

  ui_->decisions0 = ui_->decisions->value();
  ui_->round0 = ui_->round->value();
  ui_->python0 = ui_->python->text().trimmed();
  if (ui_->startProgram) ui_->startProgram0 = ui_->startProgram->text().trimmed();
  if (ui_->agentsScreen) ui_->agentsScreen0 = ui_->agentsScreen->isChecked();

  // Tall enough to show every section without scrolling, when the screen allows it.
  theme::fitDialogHeight(this, 620);
}

SettingsDialog::~SettingsDialog() {
  // A Python test that is still running is only a check: stop it, and let it clean up after itself (no waiting).
  for (QProcess* process : findChildren<QProcess*>()) {
    disconnect(process, nullptr, this, nullptr);
    if (process->state() == QProcess::NotRunning) continue;
    process->setParent(nullptr);
    connect(process, &QProcess::finished, process, &QObject::deleteLater);
    connect(process, &QProcess::errorOccurred, process, [process](QProcess::ProcessError error) {
      if (error == QProcess::FailedToStart) process->deleteLater();
    });
    process->kill();
  }
  delete ui_;
}

void SettingsDialog::accept() {
  if (ui_->autoUpdate && ui_->autoUpdate->isChecked() != Updater::autoEnabled())
    Updater::setAutoEnabled(ui_->autoUpdate->isChecked());
  QJsonObject changes;
  if (ui_->decisions->value() != ui_->decisions0)
    changes.insert(QStringLiteral("decisions_per_second"), ui_->decisions->value());
  if (ui_->round->value() != ui_->round0)
    changes.insert(ui_->realtime ? QStringLiteral("round_seconds") : QStringLiteral("round_ticks"), ui_->round->value());
  if (ui_->python->text().trimmed() != ui_->python0) changes.insert(QStringLiteral("python"), ui_->python->text().trimmed());
  if (ui_->startProgram && ui_->startProgram->text().trimmed() != ui_->startProgram0)
    changes.insert(QStringLiteral("start_program"), ui_->startProgram->text().trimmed());
  if (ui_->linkOn) {
    const bool on = ui_->linkOn->isChecked();
    const LinkSettings& was = ui_->link0;
    const bool changed = on != was.on || (on && (ui_->linkPort->value() != was.port ||
                                                 ui_->linkLockstep->isChecked() != was.lockstep ||
                                                 ui_->linkFrames->value() != was.framesPerStep));
    if (changed && on) {
      QJsonObject link = ui_->linkObject;   // keep any other keys in "link"
      link.insert(QStringLiteral("port"), ui_->linkPort->value());
      link.insert(QStringLiteral("lockstep"), ui_->linkLockstep->isChecked());
      link.insert(QStringLiteral("frames_per_step"), ui_->linkFrames->value());
      changes.insert(QStringLiteral("link"), link);
    } else if (changed) {
      changes.insert(QStringLiteral("link"), false);
    }
  }
  if (ui_->agentsScreen && ui_->agentsScreen->isChecked() != ui_->agentsScreen0) {
    QJsonObject agents = ui_->agents;   // keep any other keys in "agents"
    agents.insert(QStringLiteral("screen"), ui_->agentsScreen->isChecked());
    changes.insert(QStringLiteral("agents"), agents);
  }
  if (!changes.isEmpty()) {
    QString error;
    if (!store::mergeSettings(projectDir_, changes, &error)) {
      ui_->error->setText(QStringLiteral("Vyrn couldn't save the settings: %1").arg(error));
      ui_->error->show();
      return;
    }
  }
  QDialog::accept();
}

void SettingsDialog::testPython() {
  const QString chosen = ui_->python->text().trimmed();
  const QString program = chosen.isEmpty() ? QString::fromStdString(vyrn::platform::default_python()) : chosen;
  ui_->test->setEnabled(false);
  ui_->testResult->setStyleSheet(QString());
  ui_->testResult->setText(QStringLiteral("Testing %1…").arg(program));
  ui_->testResult->setProperty("role", "muted");
  theme::repolish(ui_->testResult);
  ui_->testResult->show();

  auto* process = new QProcess(this);
  auto* timer = new QTimer(process);
  timer->setSingleShot(true);
  auto show = [this](bool ok, const QString& text) {
    ui_->testResult->setText((ok ? QStringLiteral("✓  ") : QStringLiteral("✗  ")) + text);
    ui_->testResult->setProperty("role", QString());
    ui_->testResult->setStyleSheet(
        QStringLiteral("QLabel { color: %1; }").arg((ok ? theme::success() : theme::danger()).name()));
    ui_->test->setEnabled(true);
  };
  connect(timer, &QTimer::timeout, process, [process] {
    process->setProperty("vyrnTimedOut", true);
    process->kill();
  });
  connect(process, &QProcess::errorOccurred, this, [process, show](QProcess::ProcessError error) {
    if (error != QProcess::FailedToStart) return;
    show(false, QStringLiteral("Vyrn couldn't start “%1”. Check the path.").arg(process->program()));
    process->deleteLater();
  });
  connect(process, &QProcess::finished, this, [process, show](int code, QProcess::ExitStatus status) {
    const QString out = QString::fromUtf8(process->readAllStandardOutput()).trimmed();
    const QStringList errLines =
        QString::fromUtf8(process->readAllStandardError()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (process->property("vyrnTimedOut").toBool())
      show(false, QStringLiteral("It didn't answer within 20 seconds."));
    else if (status == QProcess::NormalExit && code == 0)
      show(true, QStringLiteral("Python %1 works, with numpy and Pillow.").arg(out));
    else if (const QRegularExpressionMatch missing =
                 QRegularExpression(QStringLiteral("No module named '([^']+)'")).match(errLines.value(errLines.size() - 1));
             missing.hasMatch()) {
      const QString module = missing.captured(1) == QLatin1String("PIL") ? QStringLiteral("Pillow") : missing.captured(1);
      show(false, QStringLiteral("It starts, but %1 is missing. Vyrn needs numpy and Pillow.").arg(module));
    } else
      show(false, errLines.isEmpty() ? QStringLiteral("It stopped with an error.") : errLines.last().trimmed());
    process->deleteLater();
  });
  process->setStandardInputFile(QProcess::nullDevice());
  timer->start(20000);
  process->start(program, {QStringLiteral("-c"),
                           QStringLiteral("import sys, numpy, PIL; print(sys.version.split()[0])")});
}
