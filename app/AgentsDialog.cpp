#include "AgentsDialog.h"

#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

#include "AppPlatform.h"
#include "Paths.h"
#include "Theme.h"

namespace {

using app_platform::Os;

QString shellQuote(const QString& text) {
  static const QString safe = QStringLiteral("/._-+=:,@%~");
  const bool plain = !text.isEmpty() && std::all_of(text.begin(), text.end(), [](QChar c) {
    return c.isLetterOrNumber() || safe.contains(c);
  });
  if (plain) return text;
  QString quoted = text;
  quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
  return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

// PowerShell and Command Prompt both read a path in double quotes (Windows paths can't hold a double quote).
QString windowsQuote(const QString& path) {
  const QString native = QDir::toNativeSeparators(path);
  return native.contains(QLatin1Char(' ')) || native.contains(QLatin1Char('&')) || native.contains(QLatin1Char('('))
             ? QLatin1Char('"') + native + QLatin1Char('"')
             : native;
}

// The vyrn-cli the agent lines run, as typed in this system's terminal: the installed one with the home folder as
// "~" (Linux, macOS), else the full path, quoted when needed.
QString commandLineCli(const QString& cli, const QString& installed) {
  if (app_platform::os() == Os::Windows) return windowsQuote(cli);
  const QString home = QDir::homePath() + QLatin1Char('/');
  if (cli == installed && cli.startsWith(home)) {
    const QString rest = cli.mid(home.size());
    if (shellQuote(rest) == rest) return QStringLiteral("~/") + rest;
  }
  return shellQuote(cli);
}

QString jsonQuote(QString text) {
  text.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
  text.replace(QLatin1Char('"'), QStringLiteral("\\\""));
  return QLatin1Char('"') + text + QLatin1Char('"');
}

QLabel* paragraph(const QString& text, QWidget* parent, const char* role = "muted") {
  auto* label = new QLabel(text, parent);
  label->setWordWrap(true);
  label->setProperty("role", role);
  label->setTextFormat(Qt::RichText);
  return label;
}

QLabel* sectionTitle(const QString& text, QWidget* parent) {
  auto* label = new QLabel(text, parent);
  label->setFont(theme::scaledFont(1.02, QFont::Bold));
  return label;
}

}  // namespace

AgentsDialog::AgentsDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("Connect an AI coding agent"));
  setMinimumWidth(600);

  // The command agents start: vyrn-cli next to this program (every installer puts it there), else where this
  // system's installer puts it. In a shell line "~/.local/bin/vyrn-cli" reads best (the shell expands it); the
  // settings-file line and the button need the full path.
  const QString found = paths::cliPath();
  const QString installed = paths::installedCli();
  const QString cli = found.isEmpty() ? installed : found;
  const QString shellCli = commandLineCli(cli, installed);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(24, 22, 24, 18);
  layout->setSpacing(14);

  // ---- header ----
  auto* header = new QHBoxLayout;
  header->setSpacing(14);
  auto* badge = new QLabel(this);
  badge->setPixmap(theme::lineIcon(QStringLiteral("agent"), theme::accent(), 26).pixmap(QSize(26, 26)));
  badge->setFixedSize(44, 44);
  badge->setAlignment(Qt::AlignCenter);
  badge->setStyleSheet(QStringLiteral("QLabel { background-color: %1; border: 1px solid %2; border-radius: 12px; }")
                           .arg(theme::panel().name(), theme::border().name()));
  header->addWidget(badge, 0, Qt::AlignTop);
  auto* titles = new QVBoxLayout;
  titles->setSpacing(4);
  auto* title = new QLabel(QStringLiteral("Connect an AI coding agent"), this);
  title->setFont(theme::scaledFont(1.45, QFont::Bold));
  titles->addWidget(title);
  titles->addWidget(paragraph(QStringLiteral("Coding agents such as Claude Code, Codex and Gemini CLI can help you: "
                                             "create projects, write the three scripts, run Check, train simulations "
                                             "and look at what the AI sees."),
                              this));
  header->addLayout(titles, 1);
  layout->addLayout(header);

  auto* body = new QWidget(this);
  auto* content = new QVBoxLayout(body);
  content->setContentsMargins(0, 0, 0, 0);
  content->setSpacing(10);

  // ---- first, what agents can and can't do ----
  auto* note = new QFrame(body);
  note->setProperty("role", "callout");
  auto* noteLayout = new QVBoxLayout(note);
  noteLayout->setContentsMargins(16, 12, 16, 14);
  noteLayout->setSpacing(6);
  auto* noteTitle = new QLabel(QStringLiteral("Good to know before you connect one"), note);
  noteTitle->setFont(theme::scaledFont(1.0, QFont::Bold));
  noteTitle->setStyleSheet(QStringLiteral("QLabel { color: %1; }").arg(theme::warning().name()));
  noteLayout->addWidget(noteTitle);
  const QStringList points{
      QStringLiteral("<b>Connected agents can run Python code on this computer.</b> The scripts they write run as "
                     "you, just like when you press Check. Only connect agents you trust."),
      QStringLiteral("They can never press keys or start training a real game: you press Train for those yourself. "
                     "They can look at your screen only where you allow it (project Settings → AI coding agents)."),
      QStringLiteral("What an agent reads through Vyrn (your scripts, the console, pictures of what the AI sees) is "
                     "sent to that agent's online service, like anything else you show it."),
  };
  for (const QString& point : points) {
    auto* row = new QHBoxLayout;
    row->setSpacing(8);
    auto* dot = new QLabel(QStringLiteral("•"), note);
    auto* text = new QLabel(point, note);
    text->setWordWrap(true);
    text->setTextFormat(Qt::RichText);
    row->addWidget(dot, 0, Qt::AlignTop);
    row->addWidget(text, 1);
    noteLayout->addLayout(row);
  }
  content->addWidget(note);
  content->addSpacing(6);

  // ---- step 1: one line per agent ----
  auto* step1 = new QHBoxLayout;
  step1->addWidget(sectionTitle(QStringLiteral("1.  Add Vyrn to your agent (once)"), body), 1);
  // "Add to Claude Code" exists only when the claude command is installed, and runs nothing until clicked.
  const QString claude = QStandardPaths::findExecutable(QStringLiteral("claude"));
  QPushButton* addToClaude = nullptr;
  if (!claude.isEmpty()) {
    addToClaude = new QPushButton(QStringLiteral("Add to Claude Code"), body);
    addToClaude->setProperty("role", "primary");
    addToClaude->setAutoDefault(false);
    addToClaude->setCursor(Qt::PointingHandCursor);
    addToClaude->setToolTip(QStringLiteral("Runs the Claude Code line below for you"));
    if (!QFileInfo(cli).isExecutable()) {
      addToClaude->setEnabled(false);
      addToClaude->setToolTip(QStringLiteral("Install vyrn-cli first (%1)").arg(paths::installCommand()));
    }
    step1->addWidget(addToClaude);
  }
  content->addLayout(step1);
  if (found.isEmpty()) {
    content->addWidget(paragraph(QStringLiteral("vyrn-cli isn't installed yet: run <code>%1</code> in the Vyrn folder "
                                                "first. These lines use the place it installs to.")
                                     .arg(paths::installCommand()),
                                 body, "faint"));
  }

  auto* agents = new QFrame(body);
  agents->setProperty("role", "card");
  auto* grid = new QGridLayout(agents);
  grid->setContentsMargins(16, 12, 16, 12);
  grid->setHorizontalSpacing(14);
  grid->setVerticalSpacing(8);
  grid->setColumnStretch(1, 1);
  int row = 0;
  auto addLine = [&](const QString& name, const QString& command) {
    auto* label = new QLabel(name, agents);
    label->setFont(theme::scaledFont(0.96, QFont::Bold));
    grid->addWidget(label, row, 0, Qt::AlignVCenter);
    grid->addWidget(theme::copyLine(command, agents), row, 1);
    ++row;
  };
  addLine(QStringLiteral("Claude Code"), QStringLiteral("claude mcp add --scope user vyrn -- %1 mcp").arg(shellCli));
  addLine(QStringLiteral("Codex CLI"), QStringLiteral("codex mcp add vyrn -- %1 mcp").arg(shellCli));
  addLine(QStringLiteral("Gemini CLI"), QStringLiteral("gemini mcp add vyrn %1 mcp").arg(shellCli));
  addLine(QStringLiteral("Others"),
          QStringLiteral("{\"mcpServers\": {\"vyrn\": {\"command\": %1, \"args\": [\"mcp\"]}}}")
              .arg(jsonQuote(QDir::toNativeSeparators(cli))));
  content->addWidget(agents);
  content->addWidget(paragraph(QStringLiteral("Paste a line into a terminal. “Others” is for agents that use an MCP "
                                              "settings file, such as Cursor."),
                               body, "faint"));

  QPlainTextEdit* output = nullptr;
  QLabel* outcome = nullptr;
  if (addToClaude) {
    output = new QPlainTextEdit(body);
    output->setReadOnly(true);
    output->setProperty("role", "log");
    output->setFont(theme::monoFont(9));
    output->setFixedHeight(92);
    output->hide();
    outcome = new QLabel(body);
    outcome->setWordWrap(true);
    outcome->hide();
    content->addWidget(output);
    content->addWidget(outcome);
  }

  // ---- step 2 ----
  content->addSpacing(6);
  content->addWidget(sectionTitle(QStringLiteral("2.  Ask it for help"), body));
  content->addWidget(paragraph(QStringLiteral("Restart the agent, then try something like: <i>“Use the vyrn tools to "
                                              "make a Catch project that learns faster, then train it for 5 minutes "
                                              "and tell me how it did.”</i>"),
                               body));
  content->addStretch(1);

  auto* scroll = new QScrollArea(this);
  scroll->setWidget(body);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll->viewport()->setAutoFillBackground(false);
  body->setAutoFillBackground(false);
  layout->addWidget(scroll, 1);

  auto* buttons = new QHBoxLayout;
  buttons->addStretch(1);
  auto* close = new QPushButton(QStringLiteral("Close"), this);
  close->setDefault(true);
  buttons->addWidget(close);
  layout->addLayout(buttons);
  connect(close, &QPushButton::clicked, this, &QDialog::accept);

  if (addToClaude) {
    const QStringList args{QStringLiteral("mcp"), QStringLiteral("add"), QStringLiteral("--scope"),
                           QStringLiteral("user"), QStringLiteral("vyrn"), QStringLiteral("--"),
                           QDir::toNativeSeparators(cli), QStringLiteral("mcp")};
    connect(addToClaude, &QPushButton::clicked, this, [this, claude, args, addToClaude, output, outcome] {
      addToClaude->setEnabled(false);
      addToClaude->setText(QStringLiteral("Adding…"));
      output->setPlainText(QStringLiteral("$ claude %1\n").arg(args.join(QLatin1Char(' '))));
      output->show();
      outcome->hide();

      auto* process = new QProcess(this);
      process->setProcessChannelMode(QProcess::MergedChannels);
      process->setStandardInputFile(QProcess::nullDevice());
      auto* timer = new QTimer(process);
      timer->setSingleShot(true);
      auto finish = [addToClaude, outcome](bool ok, const QString& text) {
        outcome->setText(text);
        outcome->setStyleSheet(
            QStringLiteral("QLabel { color: %1; }").arg((ok ? theme::success() : theme::danger()).name()));
        outcome->show();
        addToClaude->setText(ok ? QStringLiteral("Added") : QStringLiteral("Try again"));
        addToClaude->setEnabled(!ok);
      };
      connect(process, &QProcess::readyRead, this, [process, output] {
        output->moveCursor(QTextCursor::End);
        output->insertPlainText(QString::fromUtf8(process->readAll()));
        output->ensureCursorVisible();
      });
      connect(timer, &QTimer::timeout, process, [process] {
        process->setProperty("vyrnTimedOut", true);
        process->kill();
      });
      connect(process, &QProcess::errorOccurred, this, [process, finish](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        finish(false, QStringLiteral("Vyrn couldn't run the claude command."));
        process->deleteLater();
      });
      connect(process, &QProcess::finished, this, [process, output, finish](int code, QProcess::ExitStatus status) {
        if (process->property("vyrnTimedOut").toBool())
          finish(false, QStringLiteral("Claude Code didn't answer within a minute."));
        else if (status == QProcess::NormalExit && code == 0)
          finish(true, QStringLiteral("Done. Restart Claude Code, then ask it to use Vyrn."));
        else if (output->toPlainText().contains(QLatin1String("already exists"), Qt::CaseInsensitive))
          finish(true, QStringLiteral("Vyrn was already added to Claude Code. Nothing to do."));
        else
          finish(false, QStringLiteral("That didn't work (exit code %1). The output above says why.").arg(code));
        process->deleteLater();
      });
      timer->start(60000);
      process->start(claude, args);
    });
  }

  // Tall enough to show everything without scrolling, when the screen allows it.
  theme::fitDialogHeight(this, 840);
}

AgentsDialog::~AgentsDialog() {
  // Never kill "claude mcp add" halfway through writing its settings: let it finish without the dialog.
  for (QProcess* process : findChildren<QProcess*>()) {
    disconnect(process, nullptr, this, nullptr);
    if (process->state() == QProcess::NotRunning) continue;
    process->setParent(nullptr);
    connect(process, &QProcess::finished, process, &QObject::deleteLater);
    connect(process, &QProcess::errorOccurred, process, [process](QProcess::ProcessError error) {
      if (error == QProcess::FailedToStart) process->deleteLater();
    });
  }
}
