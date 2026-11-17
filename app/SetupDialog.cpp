#include "SetupDialog.h"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QList>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QShowEvent>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QXmlStreamReader>

#include <algorithm>

#include "AppPlatform.h"
#include "Paths.h"
#include "Theme.h"
#include "vyrn/platform.h"

namespace {

using Status = SetupChecker::Status;
using app_platform::Os;

// The Python a project uses when it names none (vyrn::platform::default_python()), or python3 from PATH when that
// file is missing.
QString pythonProgram() {
  const QString python = QString::fromStdString(vyrn::platform::default_python());
  return QFileInfo(python).isAbsolute() && !QFileInfo::exists(python) ? QStringLiteral("python3") : python;
}

// How Python and its packages get installed on this system: a line to copy for each, and where to paste it.
struct PythonFix {
  QString python;     // Python itself (missing, or older than 3.10 on macOS and Windows)
  QString packages;   // numpy and Pillow
  QString note;
};

PythonFix pythonFix(const QString& python) {
  switch (app_platform::os()) {
    case Os::MacOS: {
      const QString brew = QStringLiteral("brew install python numpy pillow");
      return {brew, brew,
              QStringLiteral("Paste this into Terminal (it needs Homebrew, brew.sh), then press Check again.")};
    }
    case Os::Windows: {
      const QString exe = QDir::toNativeSeparators(python);
      const QString quoted = exe.contains(QLatin1Char(' ')) ? QStringLiteral("\"%1\"").arg(exe) : exe;
      return {QStringLiteral("winget install -e --id Python.Python.3.12"),
              quoted + QStringLiteral(" -m pip install numpy Pillow"),
              QStringLiteral("Paste this into PowerShell or Command Prompt. After installing Python itself, start Vyrn "
                             "again.")};
    }
    case Os::Linux:
      break;
  }
  return {QStringLiteral("sudo apt install python3 python3-numpy python3-pil"),
          QStringLiteral("sudo apt install python3-numpy python3-pil"),
          QStringLiteral("Paste this into a terminal. It asks for your password.")};
}

// macOS: System Settings → Privacy & Security at one of its lists (Privacy_Accessibility, Privacy_ScreenCapture).
void openPrivacySettings(const QString& list) {
  QDesktopServices::openUrl(QUrl(QStringLiteral("x-apple.systempreferences:com.apple.preference.security?") + list));
}

QString shellQuote(const QString& text) {
  static const QString safe = QStringLiteral("/._-+=:,@%");
  const bool plain = std::all_of(text.begin(), text.end(), [](QChar c) { return c.isLetterOrNumber() || safe.contains(c); });
  if (plain && !text.isEmpty()) return text;
  QString quoted = text;
  quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
  return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

// "./install.sh --keyboard", with the full path when this build knows where its source folder is (so the line
// works from any terminal).
QString installCommand(const QString& flag) {
  const QString script = paths::installScript();
  return (script.isEmpty() ? QStringLiteral("./install.sh") : shellQuote(script)) + QLatin1Char(' ') + flag;
}

QString installNote(bool relogin) {
  QString note = paths::installScript().isEmpty()
                     ? QStringLiteral("Run this in a terminal, inside the folder you downloaded Vyrn into.")
                     : QStringLiteral("Paste this into a terminal. It asks for your password.");
  if (relogin) note += QStringLiteral(" Afterwards, log out and back in so the change takes effect.");
  return note;
}

QString firstLine(const QByteArray& bytes) {
  const QStringList lines = QString::fromUtf8(bytes).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
  return lines.isEmpty() ? QString() : lines.first().trimmed();
}

QString lastLine(const QByteArray& bytes) {
  const QStringList lines = QString::fromUtf8(bytes).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
  return lines.isEmpty() ? QString() : lines.last().trimmed();
}

QString sentence(QString text) {
  text = text.trimmed();
  if (!text.isEmpty() && !text.endsWith(QLatin1Char('.')) && !text.endsWith(QLatin1Char(')'))) text += QLatin1Char('.');
  return text;
}

// "S-C-A-Escape" and "C-A-S-Escape" are the same key: sort the modifiers.
QString normalizeKeybind(const QString& key) {
  QStringList parts = key.trimmed().split(QLatin1Char('-'), Qt::SkipEmptyParts);
  if (parts.isEmpty()) return {};
  const QString last = parts.takeLast();
  std::sort(parts.begin(), parts.end());
  parts << last;
  return parts.join(QLatin1Char('-'));
}

QString whyItMatters(int check) {
  switch (check) {
    case SetupChecker::Python: return QStringLiteral("runs your three scripts");
    case SetupChecker::Screen: return QStringLiteral("lets the AI see a real game");
    case SetupChecker::Keyboard: return QStringLiteral("lets the AI press keys in a real game");
    case SetupChecker::FocusHelper: return QStringLiteral("keys only ever go to your game's window");
    case SetupChecker::PanicKeys: return QStringLiteral("stops the AI at once, from anywhere");
    default: return {};
  }
}

// A tick, a cross, a dash, or a spinner while the check runs.
class StatusIcon : public QWidget {
 public:
  explicit StatusIcon(QWidget* parent = nullptr) : QWidget(parent) {
    setFixedSize(24, 24);
    spin_.setInterval(40);
    QObject::connect(&spin_, &QTimer::timeout, this, [this] {
      angle_ = (angle_ + 14) % 360;
      update();
    });
  }

  void setStatus(Status status) {
    status_ = status;
    if (status == Status::Running)
      spin_.start();
    else
      spin_.stop();
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF circle = QRectF(rect()).adjusted(2, 2, -2, -2);
    const QPointF c = circle.center();
    const qreal r = circle.width() / 2;
    auto softFill = [&](QColor color) {
      color.setAlpha(42);
      p.setPen(Qt::NoPen);
      p.setBrush(color);
      p.drawEllipse(circle);
    };
    switch (status_) {
      case Status::Ok: {
        softFill(theme::success());
        p.setPen(QPen(theme::success(), 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(QList<QPointF>{c + QPointF(-0.42 * r, 0.02 * r), c + QPointF(-0.1 * r, 0.34 * r),
                                      c + QPointF(0.45 * r, -0.3 * r)});
        break;
      }
      case Status::Failed: {
        softFill(theme::danger());
        p.setPen(QPen(theme::danger(), 2.0, Qt::SolidLine, Qt::RoundCap));
        const qreal d = 0.32 * r;
        p.drawLine(c + QPointF(-d, -d), c + QPointF(d, d));
        p.drawLine(c + QPointF(d, -d), c + QPointF(-d, d));
        break;
      }
      case Status::NotNeeded: {
        p.setPen(QPen(theme::faint(), 1.2));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(circle.adjusted(0.5, 0.5, -0.5, -0.5));
        p.setPen(QPen(theme::faint(), 2.0, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(c + QPointF(-0.35 * r, 0), c + QPointF(0.35 * r, 0));
        break;
      }
      case Status::Waiting: {
        p.setPen(QPen(theme::faint(), 1.2));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(circle.adjusted(0.5, 0.5, -0.5, -0.5));
        break;
      }
      case Status::Running: {
        p.setPen(QPen(theme::border(), 2.0));
        p.setBrush(Qt::NoBrush);
        const QRectF ring = circle.adjusted(1.5, 1.5, -1.5, -1.5);
        p.drawEllipse(ring);
        p.setPen(QPen(theme::accent(), 2.0, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(ring, -angle_ * 16, 100 * 16);
        break;
      }
    }
  }

 private:
  Status status_ = Status::Waiting;
  QTimer spin_;
  int angle_ = 0;
};

// One check: status, title + why it matters, what Vyrn found, and a copyable fix when it failed.
class CheckRow : public QFrame {
 public:
  CheckRow(int check, QWidget* parent) : QFrame(parent) {
    setProperty("role", "card");
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(14, 12, 14, 12);
    outer->setSpacing(12);

    icon_ = new StatusIcon(this);
    outer->addWidget(icon_, 0, Qt::AlignTop);

    auto* text = new QVBoxLayout;
    text->setSpacing(4);
    title_ = new QLabel(this);
    title_->setTextFormat(Qt::RichText);
    title_->setText(QStringLiteral("<span style='font-weight:bold'>%1</span>"
                                   "<span style='color:%2'>&nbsp;&nbsp;·&nbsp;&nbsp;%3</span>")
                        .arg(SetupChecker::title(SetupChecker::Check(check)).toHtmlEscaped(),
                             theme::muted().name(), whyItMatters(check).toHtmlEscaped()));
    summary_ = new QLabel(this);
    summary_->setWordWrap(true);
    summary_->setProperty("role", "muted");
    summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    text->addWidget(title_);
    text->addWidget(summary_);

    fixBox_ = new QWidget(this);
    auto* fixLayout = new QVBoxLayout(fixBox_);
    fixLayout->setContentsMargins(0, 6, 0, 0);
    fixLayout->setSpacing(4);
    fixNote_ = new QLabel(fixBox_);
    fixNote_->setWordWrap(true);
    fixNote_->setProperty("role", "faint");
    fixLayout->addWidget(fixNote_);
    text->addWidget(fixBox_);
    text->addStretch(1);   // keep the text at the top when the test picture makes the row taller
    fixBox_->hide();
    outer->addLayout(text, 1);

    picture_ = new QLabel(this);
    picture_->setFixedSize(72, 72);
    picture_->setAlignment(Qt::AlignCenter);
    picture_->setToolTip(QStringLiteral("Vyrn's test picture of the top-left corner of your screen"));
    picture_->setStyleSheet(QStringLiteral("QLabel { background-color: %1; border: 1px solid %2; border-radius: 6px; }")
                                .arg(theme::base().name(), theme::border().name()));
    picture_->hide();
    outer->addWidget(picture_, 0, Qt::AlignTop);
  }

  void setResult(const SetupChecker::Result& result) {
    icon_->setStatus(result.status);
    QString summary = result.summary;
    if (summary.isEmpty()) {
      summary = result.status == Status::Running   ? QStringLiteral("Checking…")
                : result.status == Status::Waiting ? QStringLiteral("Waiting…")
                                                   : QString();
    }
    summary_->setText(summary);
    summary_->setVisible(!summary.isEmpty());

    const bool showFix = result.status == Status::Failed &&
                         (!result.fix.isEmpty() || !result.fixNote.isEmpty() || !result.buttons.isEmpty());
    for (QWidget** old : {&fixLine_, &buttons_}) {
      if (!*old) continue;
      (*old)->hide();
      (*old)->deleteLater();
      *old = nullptr;
    }
    if (showFix && !result.fix.isEmpty()) {
      fixLine_ = theme::copyLine(result.fix, fixBox_);
      static_cast<QVBoxLayout*>(fixBox_->layout())->insertWidget(0, fixLine_);
    }
    if (showFix && !result.buttons.isEmpty()) {
      buttons_ = new QWidget(fixBox_);
      auto* row = new QHBoxLayout(buttons_);
      row->setContentsMargins(0, 2, 0, 0);
      for (const SetupChecker::Button& b : result.buttons) {
        auto* button = new QPushButton(b.label, buttons_);
        button->setAutoDefault(false);
        button->setCursor(Qt::PointingHandCursor);
        QObject::connect(button, &QPushButton::clicked, button, b.action);
        row->addWidget(button);
      }
      row->addStretch(1);
      fixBox_->layout()->addWidget(buttons_);
    }
    fixNote_->setText(result.fixNote);
    fixNote_->setVisible(!result.fixNote.isEmpty());
    fixBox_->setVisible(showFix);

    if (!result.picture.isNull()) {
      const qreal dpr = devicePixelRatioF();
      QPixmap pm = QPixmap::fromImage(result.picture.scaled(QSize(64, 64) * dpr, Qt::KeepAspectRatio,
                                                            Qt::SmoothTransformation));
      pm.setDevicePixelRatio(dpr);
      picture_->setPixmap(pm);
      picture_->show();
    } else {
      picture_->clear();
      picture_->hide();
    }
    setEnabled(result.status != Status::NotNeeded);
  }

 private:
  StatusIcon* icon_ = nullptr;
  QLabel* title_ = nullptr;
  QLabel* summary_ = nullptr;
  QWidget* fixBox_ = nullptr;
  QWidget* fixLine_ = nullptr;
  QWidget* buttons_ = nullptr;
  QLabel* fixNote_ = nullptr;
  QLabel* picture_ = nullptr;
};

}  // namespace

// ==== SetupChecker ===============================================================================================

SetupChecker::SetupChecker(QObject* parent) : QObject(parent) {}

SetupChecker::~SetupChecker() { cancel(); }

QString SetupChecker::title(Check check) {
  const bool mac = app_platform::os() == Os::MacOS;
  switch (check) {
    case Python: return QStringLiteral("Python with numpy and Pillow");
    case Screen: return mac ? QStringLiteral("Screen Recording") : QStringLiteral("Screen capture");
    case Keyboard: return mac ? QStringLiteral("Accessibility") : QStringLiteral("Keyboard access");
    case FocusHelper: return QStringLiteral("Focus helper");
    case PanicKeys: return QStringLiteral("Panic key (%1)").arg(app_platform::panicShortcut());
    default: return {};
  }
}

bool SetupChecker::appliesHere(Check check) {
  switch (app_platform::os()) {
    case Os::Linux: return check >= Python && check < CheckCount;
    case Os::MacOS: return check == Python || check == Screen || check == Keyboard;   // permissions; the app's hotkey
    case Os::Windows: return check == Python;   // Pillow sees the screen and SendInput presses keys without setup
  }
  return false;
}

void SetupChecker::start(bool realGame, bool grabScreen) {
  cancel();
  realGame_ = realGame;
  finishedSent_ = false;
  for (int i = 0; i < CheckCount; ++i) {
    const bool here = appliesHere(Check(i));
    const bool needed = here && (i == Python || realGame);
    results_[i] = Result{};
    results_[i].status = needed ? Status::Running : Status::NotNeeded;
    if (here && !needed) results_[i].summary = QStringLiteral("Only needed for real games.");
    Q_EMIT changed(i);
  }
  checkPython();
  if (realGame) {
    switch (app_platform::os()) {
      case Os::Linux:
        checkScreen(grabScreen);
        checkKeyboard();
        checkFocusHelper();
        checkPanicKeys();
        break;
      case Os::MacOS:
        checkScreenRecording();
        checkAccessibility();
        break;
      case Os::Windows:
        break;
    }
  }
  maybeFinished();
}

void SetupChecker::cancel() {
  ++generation_;
  for (QProcess* process : findChildren<QProcess*>(Qt::FindDirectChildrenOnly)) {
    disconnect(process, nullptr, this, nullptr);
    if (process->state() == QProcess::NotRunning) {
      process->deleteLater();
      continue;
    }
    // Stop it without waiting for it: it deletes itself once it has really ended.
    process->setParent(nullptr);
    connect(process, &QProcess::finished, process, &QObject::deleteLater);
    connect(process, &QProcess::errorOccurred, process, [process](QProcess::ProcessError error) {
      if (error == QProcess::FailedToStart) process->deleteLater();
    });
    process->kill();
  }
}

bool SetupChecker::running() const {
  return std::any_of(results_.begin(), results_.end(),
                     [](const Result& r) { return r.status == Status::Running || r.status == Status::Waiting; });
}

const SetupChecker::Result& SetupChecker::result(Check check) const { return results_[check]; }

int SetupChecker::passed() const {
  return int(std::count_if(results_.begin(), results_.end(), [](const Result& r) { return r.status == Status::Ok; }));
}

int SetupChecker::total() const {
  return int(std::count_if(results_.begin(), results_.end(),
                           [](const Result& r) { return r.status != Status::NotNeeded; }));
}

void SetupChecker::run(const QString& program, const QStringList& args, int timeoutMs, Done done) {
  auto* process = new QProcess(this);
  auto* timer = new QTimer(process);
  timer->setSingleShot(true);
  const int generation = generation_;
  auto report = [this, process, timer, generation, done](bool started) {
    if (process->property("vyrnReported").toBool()) return;   // finished() and errorOccurred() may both fire
    process->setProperty("vyrnReported", true);
    timer->stop();
    ProcessResult r;
    r.started = started;
    r.timedOut = process->property("vyrnTimedOut").toBool();
    r.exitCode = started && process->exitStatus() == QProcess::NormalExit ? process->exitCode() : -1;
    r.out = process->readAllStandardOutput();
    r.err = process->readAllStandardError();
    process->deleteLater();
    if (generation == generation_) done(r);
  };
  connect(process, &QProcess::finished, this, [report] { report(true); });
  connect(process, &QProcess::errorOccurred, this, [report](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) report(false);   // the other errors also emit finished()
  });
  connect(timer, &QTimer::timeout, process, [process] {
    process->setProperty("vyrnTimedOut", true);
    process->kill();
  });
  process->setStandardInputFile(QProcess::nullDevice());
  timer->start(timeoutMs);   // before start(): a program that can't start reports (and stops the timer) right away
  process->start(program, args, QIODevice::ReadOnly);
}

void SetupChecker::set(Check check, Status status, const QString& summary, const QString& fix, const QString& fixNote,
                       const QImage& picture, const QList<Button>& buttons) {
  Result& r = results_[check];
  r.status = status;
  r.summary = summary;
  r.fix = fix;
  r.fixNote = fixNote;
  r.picture = picture;
  r.buttons = buttons;
  Q_EMIT changed(check);
  maybeFinished();
}

void SetupChecker::maybeFinished() {
  if (finishedSent_ || running()) return;
  finishedSent_ = true;
  Q_EMIT finished(passed(), total());
}

void SetupChecker::checkPython() {
  // Linux runs the line it always ran. macOS and Windows also need Python 3.10 or newer (docs/dev/platforms.md): the
  // line prints the version first and exits with status 3 when it is older.
  const bool versionGate = app_platform::os() != Os::Linux;
  const QString code =
      versionGate
          ? QStringLiteral("import sys; v = sys.version.split()[0]; print(v, flush=True); "
                           "sys.exit(3) if sys.version_info < (3, 10) else None; "
                           "import numpy, PIL; print(v, numpy.__version__, PIL.__version__)")
          : QStringLiteral("import sys, numpy, PIL; print(sys.version.split()[0], numpy.__version__, PIL.__version__)");
  const QString python = pythonProgram();
  run(python, {QStringLiteral("-c"), code}, 30000, [this, versionGate, python](const ProcessResult& r) {
    const PythonFix fixes = pythonFix(python);
    const QString fix = fixes.packages;
    const QString note = fixes.note;
    if (!r.started) {
      set(Python, Status::Failed, QStringLiteral("Vyrn can't find Python 3 on this computer."), fixes.python, note);
      return;
    }
    if (r.timedOut) {
      set(Python, Status::Failed, QStringLiteral("Python took too long to start. Try “Check again” in a moment."));
      return;
    }
    if (versionGate && r.exitCode == 3) {
      set(Python, Status::Failed,
          QStringLiteral("Python %1 is too old: Vyrn needs Python 3.10 or newer.").arg(firstLine(r.out)),
          fixes.python, note);
      return;
    }
    if (r.exitCode == 0) {
      const QStringList v = lastLine(r.out).simplified().split(QLatin1Char(' '));
      set(Python, Status::Ok,
          v.size() >= 3 ? QStringLiteral("Python %1 with numpy %2 and Pillow %3.").arg(v[0], v[1], v[2])
                        : QStringLiteral("Python, numpy and Pillow are ready."));
      return;
    }
    const QString err = QString::fromUtf8(r.err);
    QString summary;
    if (err.contains(QLatin1String("'numpy'")))
      summary = QStringLiteral("Python is there, but numpy is missing.");
    else if (err.contains(QLatin1String("'PIL'")))
      summary = QStringLiteral("Python is there, but Pillow is missing.");
    else
      summary = QStringLiteral("Python couldn't load numpy and Pillow: %1").arg(sentence(lastLine(r.err)));
    set(Python, Status::Failed, summary, fix, note);
  });
}

void SetupChecker::checkScreen(bool grab) {
  const QString fix = QStringLiteral("sudo apt install grim");
  const QString note = QStringLiteral("Paste this into a terminal. It asks for your password.");
  const QString grim = QStandardPaths::findExecutable(QStringLiteral("grim"));
  if (grim.isEmpty()) {
    set(Screen, Status::Failed, QStringLiteral("grim isn't installed. Vyrn uses it to take pictures of the screen."),
        fix, note);
    return;
  }
  if (!grab) {
    set(Screen, Status::Ok, QStringLiteral("grim is installed."));
    return;
  }
  run(grim, {QStringLiteral("-g"), QStringLiteral("0,0 64x64"), QStringLiteral("-")}, 10000,
      [this, fix, note](const ProcessResult& r) {
        if (!r.started) {
          set(Screen, Status::Failed, QStringLiteral("grim couldn't be started."), fix, note);
          return;
        }
        if (r.timedOut) {
          set(Screen, Status::Failed, QStringLiteral("Taking a test picture took too long. Try “Check again”."));
          return;
        }
        QImage picture;
        if (r.exitCode == 0 && picture.loadFromData(r.out, "PNG")) {
          set(Screen, Status::Ok, QStringLiteral("Vyrn took a test picture of the top-left corner of your screen."),
              {}, {}, picture);
          return;
        }
        const QString why = firstLine(r.err);
        if (qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") || why.contains(QLatin1String("display"))) {
          set(Screen, Status::Failed,
              QStringLiteral("Screen pictures need the Raspberry Pi desktop, and Vyrn isn't running on it."), {},
              QStringLiteral("Start Vyrn from the desktop menu (Programming → Vyrn)."));
        } else {
          set(Screen, Status::Failed,
              QStringLiteral("grim couldn't take a test picture: %1").arg(sentence(why.isEmpty() ? QStringLiteral("no reason given") : why)));
        }
      });
}

// macOS: the worker's screen pictures (Pillow ImageGrab) show other apps' windows only with Screen Recording.
void SetupChecker::checkScreenRecording() {
  if (app_platform::screenRecordingAllowed()) {
    set(Screen, Status::Ok,
        QStringLiteral("Vyrn may take pictures of the screen (only while you train or watch a real game)."));
    return;
  }
  set(Screen, Status::Failed,
      QStringLiteral("Vyrn isn't allowed to record the screen yet, so the AI can't see your game."), {},
      QStringLiteral("Turn on Vyrn in System Settings → Privacy & Security → Screen Recording, then quit and reopen "
                     "Vyrn. Started Vyrn from Terminal? Then turn on Terminal."),
      {},
      {{QStringLiteral("Open Screen Recording settings"),
        [] { openPrivacySettings(QStringLiteral("Privacy_ScreenCapture")); }},
       {QStringLiteral("Ask for permission"), [] { app_platform::requestScreenRecording(); }}});
}

// macOS: the worker presses keys with CGEventPost, which needs Accessibility.
void SetupChecker::checkAccessibility() {
  if (app_platform::accessibilityAllowed()) {
    set(Keyboard, Status::Ok, QStringLiteral("Vyrn may press keys (only while you train or watch a real game)."));
    return;
  }
  set(Keyboard, Status::Failed, QStringLiteral("Vyrn isn't allowed to press keys yet."), {},
      QStringLiteral("Turn on Vyrn in System Settings → Privacy & Security → Accessibility, then press Check again. "
                     "Started Vyrn from Terminal? Then turn on Terminal."),
      {},
      {{QStringLiteral("Open Accessibility settings"),
        [] { openPrivacySettings(QStringLiteral("Privacy_Accessibility")); }}});
}

void SetupChecker::checkKeyboard() {
  run(QStringLiteral("test"), {QStringLiteral("-w"), QStringLiteral("/dev/uinput")}, 5000,
      [this](const ProcessResult& r) {
        if (r.started && !r.timedOut && r.exitCode == 0) {
          checkEvdev();
          return;
        }
        const QString fix = installCommand(QStringLiteral("--keyboard"));
        if (!QFileInfo::exists(QStringLiteral("/dev/uinput"))) {
          set(Keyboard, Status::Failed, QStringLiteral("The virtual keyboard device (/dev/uinput) isn't there yet."),
              fix, installNote(false));
        } else {
          set(Keyboard, Status::Failed,
              QStringLiteral("Vyrn isn't allowed to use the virtual keyboard (/dev/uinput) yet."), fix,
              installNote(true));
        }
      });
}

void SetupChecker::checkEvdev() {
  run(pythonProgram(), {QStringLiteral("-c"), QStringLiteral("import evdev")}, 20000, [this](const ProcessResult& r) {
    if (r.started && !r.timedOut && r.exitCode == 0) {
      set(Keyboard, Status::Ok,
          QStringLiteral("Vyrn can press keys with a virtual keyboard (only while you train a real game)."));
      return;
    }
    set(Keyboard, Status::Failed, QStringLiteral("The Python evdev module is missing, so Vyrn can't press keys."),
        QStringLiteral("sudo apt install python3-evdev"),
        QStringLiteral("Paste this into a terminal. It asks for your password."));
  });
}

void SetupChecker::checkFocusHelper() {
  const QString helper = paths::focusHelper();
  if (helper.isEmpty()) {
    set(FocusHelper, Status::Failed, QStringLiteral("The focus helper (vyrn-focus) isn't installed."),
        installCommand(QStringLiteral("--app")), installNote(false));
    return;
  }
  run(helper, {QStringLiteral("--once")}, 5000, [this](const ProcessResult& r) {
    const QStringList lines = QString::fromUtf8(r.out).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (r.started && !r.timedOut && r.exitCode == 0 && lines.contains(QStringLiteral("ready"))) {
      set(FocusHelper, Status::Ok, QStringLiteral("Vyrn can see which window is active."));
      return;
    }
    QString why;
    for (const QString& line : lines) {
      if (line.startsWith(QLatin1String("error\t"))) why = line.mid(6).trimmed();
    }
    if (why.isEmpty()) why = firstLine(r.err);
    if (why.isEmpty())
      why = r.timedOut ? QStringLiteral("it didn't answer in time") : QStringLiteral("it stopped without saying why");
    if (why.contains(QLatin1String("Wayland"))) {
      set(FocusHelper, Status::Failed, QStringLiteral("The focus helper can't reach the desktop: %1").arg(sentence(why)),
          {}, QStringLiteral("Start Vyrn from the Raspberry Pi desktop menu (Programming → Vyrn)."));
    } else {
      set(FocusHelper, Status::Failed, QStringLiteral("The focus helper doesn't work here: %1").arg(sentence(why)), {},
          QStringLiteral("Vyrn needs the labwc desktop that comes with Raspberry Pi OS."));
    }
  });
}

void SetupChecker::checkPanicKeys() {
  const QString fix = installCommand(QStringLiteral("--keyboard"));
  const QString rcPath = QDir::home().filePath(QStringLiteral(".config/labwc/rc.xml"));
  QFile file(rcPath);
  if (!file.open(QIODevice::ReadOnly)) {
    set(PanicKeys, Status::Failed, QStringLiteral("The panic key isn't set up yet."), fix, installNote(false));
    return;
  }

  // Find <keybind key="..."> elements whose Execute action runs vyrn-panic (as an attribute or a <command> child).
  QSet<QString> bound;
  QString command;
  QXmlStreamReader xml(&file);
  QString currentKey;
  bool inAction = false;
  while (!xml.atEnd()) {
    xml.readNext();
    if (xml.isStartElement()) {
      const auto name = xml.name();
      if (name == QLatin1String("keybind")) {
        currentKey = normalizeKeybind(xml.attributes().value(QLatin1String("key")).toString());
      } else if (name == QLatin1String("action") && !currentKey.isEmpty()) {
        inAction = true;
        const QString cmd = xml.attributes().value(QLatin1String("command")).toString();
        if (cmd.contains(QLatin1String("vyrn-panic"))) {
          bound.insert(currentKey);
          command = cmd;
        }
      } else if (name == QLatin1String("command") && inAction) {
        const QString cmd = xml.readElementText();
        if (cmd.contains(QLatin1String("vyrn-panic"))) {
          bound.insert(currentKey);
          command = cmd;
        }
      }
    } else if (xml.isEndElement()) {
      if (xml.name() == QLatin1String("keybind")) currentKey.clear();
      if (xml.name() == QLatin1String("action")) inAction = false;
    }
  }
  if (xml.hasError() && bound.isEmpty()) {
    set(PanicKeys, Status::Failed, QStringLiteral("Vyrn couldn't read %1.").arg(paths::pretty(rcPath)), fix,
        installNote(false));
    return;
  }
  const bool plain = bound.contains(normalizeKeybind(QStringLiteral("C-A-Escape")));
  const bool shifted = bound.contains(normalizeKeybind(QStringLiteral("S-C-A-Escape")));
  if (!plain) {
    set(PanicKeys, Status::Failed, QStringLiteral("The panic key isn't set up yet."), fix, installNote(false));
    return;
  }
  if (!shifted) {
    set(PanicKeys, Status::Failed,
        QStringLiteral("Only half of the panic key is set up: Shift+Ctrl+Alt+Esc is missing (it's needed while the "
                       "AI holds Shift)."),
        fix, installNote(false));
    return;
  }
  QString program = command.trimmed().section(QLatin1Char(' '), 0, 0);
  if (program.startsWith(QLatin1String("~/"))) program = QDir::home().filePath(program.mid(2));
  if (QFileInfo(program).isAbsolute() && !QFileInfo(program).isExecutable()) {
    set(PanicKeys, Status::Failed,
        QStringLiteral("The panic key runs %1, but that file is missing.").arg(paths::pretty(program)), fix,
        installNote(false));
    return;
  }
  set(PanicKeys, Status::Ok,
      QStringLiteral("%1 stops the AI at once, even when Vyrn is hidden.").arg(app_platform::panicShortcut()));
}

// ==== SetupDialog ================================================================================================

struct SetupDialog::Ui {
  SetupChecker* checker = nullptr;
  bool includeRealGame = true;
  bool started = false;
  QLabel* summary = nullptr;
  QList<CheckRow*> rows;
  QWidget* realGameHeader = nullptr;
  QPushButton* alsoRealGame = nullptr;
  QPushButton* again = nullptr;
};

SetupDialog::SetupDialog(QWidget* parent, bool includeRealGameChecks) : QDialog(parent), ui_(new Ui) {
  ui_->includeRealGame = includeRealGameChecks;
  ui_->checker = new SetupChecker(this);
  setWindowTitle(QStringLiteral("Setup check"));
  setMinimumWidth(620);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(24, 22, 24, 18);
  layout->setSpacing(14);

  // Header: title + what is going on.
  auto* header = new QHBoxLayout;
  header->setSpacing(14);
  auto* logo = new QLabel(this);
  const qreal dpr = devicePixelRatioF();
  QPixmap icon = QPixmap(QStringLiteral(":/vyrn.png")).scaled(QSize(44, 44) * dpr, Qt::KeepAspectRatio,
                                                              Qt::SmoothTransformation);
  icon.setDevicePixelRatio(dpr);
  logo->setPixmap(icon);
  header->addWidget(logo, 0, Qt::AlignTop);
  auto* titles = new QVBoxLayout;
  titles->setSpacing(2);
  auto* title = new QLabel(QStringLiteral("Setup check"), this);
  title->setFont(theme::scaledFont(1.45, QFont::Bold));
  ui_->summary = new QLabel(this);
  ui_->summary->setProperty("role", "muted");
  ui_->summary->setWordWrap(true);
  titles->addWidget(title);
  titles->addWidget(ui_->summary);
  header->addLayout(titles, 1);
  layout->addLayout(header);

  // Rows, in two groups.
  auto* list = new QWidget(this);
  auto* rows = new QVBoxLayout(list);
  rows->setContentsMargins(0, 0, 0, 0);
  rows->setSpacing(8);
  auto sectionLabel = [list](const QString& text) {
    auto* label = new QLabel(text, list);
    label->setFont(theme::scaledFont(0.92, QFont::Bold));
    label->setProperty("role", "muted");
    return label;
  };
  rows->addWidget(sectionLabel(QStringLiteral("Every project needs")));
  for (int i = 0; i < SetupChecker::CheckCount; ++i) {
    const bool here = SetupChecker::appliesHere(SetupChecker::Check(i));
    if (i == SetupChecker::Screen) {
      ui_->realGameHeader = new QWidget(list);
      if (!here) ui_->realGameHeader->hide();   // Windows: real games need nothing more
      auto* h = new QHBoxLayout(ui_->realGameHeader);
      h->setContentsMargins(0, 10, 0, 0);
      h->addWidget(sectionLabel(QStringLiteral("Real games also need (the AI looks at your screen and presses keys)")), 1);
      ui_->alsoRealGame = new QPushButton(QStringLiteral("Check these too"), ui_->realGameHeader);
      ui_->alsoRealGame->setProperty("role", "link");
      ui_->alsoRealGame->setCursor(Qt::PointingHandCursor);
      h->addWidget(ui_->alsoRealGame);
      rows->addWidget(ui_->realGameHeader);
      connect(ui_->alsoRealGame, &QPushButton::clicked, this, [this] {
        ui_->includeRealGame = true;
        runChecks();
      });
    }
    auto* row = new CheckRow(i, list);
    ui_->rows << row;
    rows->addWidget(row);
    if (!here) row->hide();   // a check another system needs
  }
  rows->addStretch(1);

  auto* scroll = new QScrollArea(this);
  scroll->setWidget(list);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll->viewport()->setAutoFillBackground(false);
  list->setAutoFillBackground(false);
  layout->addWidget(scroll, 1);

  // Buttons.
  auto* buttons = new QHBoxLayout;
  ui_->again = new QPushButton(QStringLiteral("Check again"), this);
  ui_->again->setAutoDefault(false);
  auto* close = new QPushButton(QStringLiteral("Close"), this);
  close->setDefault(true);
  buttons->addWidget(ui_->again);
  buttons->addStretch(1);
  buttons->addWidget(close);
  layout->addLayout(buttons);
  connect(ui_->again, &QPushButton::clicked, this, &SetupDialog::runChecks);
  connect(close, &QPushButton::clicked, this, &QDialog::accept);

  connect(ui_->checker, &SetupChecker::changed, this, [this](int check) {
    updateRow(check);
    updateSummary();
  });
  connect(ui_->checker, &SetupChecker::finished, this, [this](int passed, int total) {
    updateSummary();
    // Fixes appeared under the rows that failed: grow to show them (never shrink under the user's hands).
    QTimer::singleShot(0, this, [this] { theme::fitDialogHeight(this, width(), /*growOnly=*/true); });
    Q_EMIT checksFinished(passed, total);
  });

  for (int i = 0; i < SetupChecker::CheckCount; ++i) {
    SetupChecker::Result waiting;
    waiting.status = i == SetupChecker::Python || ui_->includeRealGame ? Status::Waiting : Status::NotNeeded;
    if (waiting.status == Status::NotNeeded) waiting.summary = QStringLiteral("Only needed for real games.");
    ui_->rows[i]->setResult(waiting);
  }
  updateSummary();
  theme::fitDialogHeight(this, 680);
}

SetupDialog::~SetupDialog() { delete ui_; }

void SetupDialog::runChecks() {
  ui_->started = true;
  ui_->checker->start(ui_->includeRealGame, /*grabScreen=*/true);
}

bool SetupDialog::allPassed() const {
  return ui_->started && !ui_->checker->running() && ui_->checker->passed() == ui_->checker->total();
}

void SetupDialog::showEvent(QShowEvent* event) {
  QDialog::showEvent(event);
  if (!ui_->started) QTimer::singleShot(0, this, &SetupDialog::runChecks);
}

void SetupDialog::updateRow(int check) {
  if (check >= 0 && check < ui_->rows.size())
    ui_->rows[check]->setResult(ui_->checker->result(SetupChecker::Check(check)));
}

void SetupDialog::updateSummary() {
  const SetupChecker* c = ui_->checker;
  ui_->alsoRealGame->setVisible(!ui_->includeRealGame);
  ui_->again->setEnabled(ui_->started && !c->running());
  if (!ui_->started || c->running()) {
    ui_->summary->setText(QStringLiteral("Vyrn is checking what it needs on this computer…"));
    return;
  }
  const int failed = c->total() - c->passed();
  if (failed == 0) {
    ui_->summary->setText(ui_->includeRealGame
                              ? QStringLiteral("Everything is ready, for simulations and real games.")
                              : QStringLiteral("Everything simulations need is ready."));
  } else {
    ui_->summary->setText(failed == 1 ? QStringLiteral("1 thing needs fixing. Copy the line below it into a terminal.")
                                      : QStringLiteral("%1 things need fixing. Copy the line below each one into a "
                                                       "terminal.")
                                            .arg(failed));
  }
}
