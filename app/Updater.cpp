#include "Updater.h"

#include "AppPlatform.h"
#include "Paths.h"

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QRegularExpression>
#include <QSettings>
#include <QStringList>
#include <QTimer>

namespace {

const char kApiUrl[] = "https://api.github.com/repos/LizardRush/vyrn/commits/main";
const char kAutoKey[] = "updates/auto";
const char kLastKey[] = "updates/lastCheck";

QString& stampDir() {
  static QString dir;
  return dir;
}

std::function<bool(const QString&)>& hook() {
  static std::function<bool(const QString&)> h;
  return h;
}

QString firstLine(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  return QString::fromUtf8(file.readLine()).trimmed();
}

}  // namespace

Updater::Updater(QObject* parent) : QObject(parent) {}

bool Updater::validCommit(const QString& text) {
  static const QRegularExpression hex(QStringLiteral("^[0-9a-fA-F]{40}$"));
  return hex.match(text).hasMatch();
}

QString Updater::installedCommit() {
  const QString dir = stampDir().isEmpty() ? paths::dataDir() : stampDir();
  if (dir.isEmpty()) return {};
  for (const char* name : {"commit", ".vyrn-commit"}) {
    const QString line = firstLine(dir + QLatin1Char('/') + QLatin1String(name));
    if (validCommit(line)) return line.toLower();
  }
  return {};
}

bool Updater::autoEnabled() { return QSettings().value(kAutoKey, true).toBool(); }

void Updater::setAutoEnabled(bool on) { QSettings().setValue(kAutoKey, on); }

void Updater::setStampDir(const QString& dir) { stampDir() = dir; }

QString Updater::updateUrl() {
  const QString url = qEnvironmentVariable("VYRN_UPDATE_URL");
  return url.isEmpty() ? QString::fromLatin1(kApiUrl) : url;
}

QString Updater::oneLiner() { return app_platform::updateCommand(); }

bool Updater::startUpdate() {
  if (hook()) return hook()(oneLiner());
  return app_platform::openUpdateTerminal(oneLiner());
}

void Updater::setTerminalHook(std::function<bool(const QString&)> h) { hook() = std::move(h); }

void Updater::check(bool manual) {
  if (process_) return;
  if (!manual) {
    if (qEnvironmentVariableIntValue("VYRN_UPDATE_NO_AUTO") == 1 || !autoEnabled()) return;
    const QDateTime last = QSettings().value(kLastKey).toDateTime();
    if (last.isValid() && last.secsTo(QDateTime::currentDateTimeUtc()) < 24 * 60 * 60) return;
  }
  installed_ = installedCommit();
  if (installed_.isEmpty()) {
    if (manual) Q_EMIT unknown();
    return;
  }
  manual_ = manual;
  process_ = new QProcess(this);
  timeout_ = new QTimer(this);
  timeout_->setSingleShot(true);
  connect(timeout_, &QTimer::timeout, this, [this] {
    if (!process_) return;
    process_->disconnect(this);
    process_->kill();
    process_->deleteLater();
    process_ = nullptr;
    finish(false, QStringLiteral("GitHub took too long to answer."));
  });
  connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error != QProcess::FailedToStart || !process_) return;
    process_->deleteLater();
    process_ = nullptr;
    finish(false, QStringLiteral("Vyrn needs the curl program to check, and it isn't installed."));
  });
  connect(process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
    if (!process_) return;
    const QString out = QString::fromUtf8(process_->readAllStandardOutput()).trimmed();
    process_->deleteLater();
    process_ = nullptr;
    if (status != QProcess::NormalExit || code != 0) {
      finish(false, code == 6 || code == 7 || code == 28
                        ? QStringLiteral("Vyrn couldn't reach GitHub. Is this computer online?")
                        : QStringLiteral("GitHub didn't give a usable answer (curl stopped with code %1).").arg(code));
      return;
    }
    if (!validCommit(out)) {
      finish(false, QStringLiteral("GitHub's answer wasn't what Vyrn expected."));
      return;
    }
    QSettings().setValue(kLastKey, QDateTime::currentDateTimeUtc());
    if (out.toLower() == installed_) {
      if (manual_) Q_EMIT upToDate();
    } else {
      Q_EMIT updateAvailable(out.toLower());
    }
  });
  process_->start(QStringLiteral("curl"),
                  {QStringLiteral("-fsSL"), QStringLiteral("-m"), QStringLiteral("12"), QStringLiteral("-H"),
                   QStringLiteral("Accept: application/vnd.github.sha"), updateUrl()});
  timeout_->start(15000);
}

void Updater::finish(bool ok, const QString& why) {
  if (timeout_) timeout_->stop();
  if (!ok && manual_) Q_EMIT failed(why);
}
