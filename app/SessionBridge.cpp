// SessionBridge: one vyrn::Session for the UI. A 100 ms timer drains the session's events and reads its live
// view; both calls return at once, so the UI thread never waits for Python or the brain.
#include "SessionBridge.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonValue>

#include <cmath>
#include <exception>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kPollMs = 100;                // 10 polls a second: the live view refreshes at most 10 times a second
constexpr size_t kEventsPerPoll = 2000;     // a flood is spread over several polls so the UI stays responsive

void setError(QString* error, const QString& text) {
  if (error) *error = text;
}

double numberOr(const QJsonObject& object, const char* key, double fallback) {
  const QJsonValue value = object.value(QLatin1String(key));
  return value.isDouble() ? value.toDouble() : fallback;
}

// Why Train or Watch can't take the project lock, in plain words.
QString lockRefusal(const std::string& project) {
  std::optional<vyrn::ProjectLock::Holder> holder;
  try {
    holder = vyrn::ProjectLock::holder(project);
  } catch (const std::exception&) {
  }
  if (!holder) {
    return QStringLiteral("Vyrn couldn't mark this project as busy: it needs to write a small file (.vyrn-lock) in "
                          "the project folder. Check that the folder still exists and that you can write to it.");
  }
  if (holder->by == "agent") {
    return QStringLiteral("An AI agent is training this project right now. Stop it first, or wait until it is done.");
  }
  if (holder->by == "cli") {
    return QStringLiteral("vyrn-cli is training or watching this project in a terminal. Stop it there first.");
  }
  if (holder->pid == int(QCoreApplication::applicationPid())) {
    return QStringLiteral("This project is already running in another Vyrn window. Stop it there first.");
  }
  return QStringLiteral("This project is being trained in another Vyrn window. Stop it there first.");
}

}  // namespace

SessionBridge::SessionBridge(QObject* parent) : QObject(parent) {
  qRegisterMetaType<vyrn::LiveView>();
  timer_.setInterval(kPollMs);
  connect(&timer_, &QTimer::timeout, this, &SessionBridge::poll);
}

SessionBridge::~SessionBridge() {
  timer_.stop();
  if (session_) {
    session_->stop();
    session_.reset();   // waits until the run is over: Train saves the brain first
  }
  lock_.reset();
}

bool SessionBridge::start(const vyrn::RunConfig& config, QString* error) {
  if (running()) {
    setError(error, QStringLiteral("Vyrn is already running this project. Stop it first."));
    return false;
  }
  // The previous run is over: let it go (its thread has ended, so this doesn't wait).
  timer_.stop();
  session_.reset();
  lock_.reset();

  if (config.action != vyrn::RunConfig::Action::Check) {
    std::optional<vyrn::ProjectLock> lock;
    try {
      lock = vyrn::ProjectLock::acquire(config.project_dir, "app");
    } catch (const std::exception&) {
    }
    if (!lock) {
      setError(error, lockRefusal(config.project_dir));
      return false;
    }
    lock_.emplace(std::move(*lock));
  }

  try {
    session_ = std::make_unique<vyrn::Session>(config);
    session_->start();
  } catch (const std::exception& e) {
    session_.reset();
    lock_.reset();
    setError(error, QStringLiteral("Vyrn couldn't start: %1").arg(QString::fromStdString(e.what())));
    return false;
  }
  action_ = config.action;
  lastLiveVersion_ = 0;
  finishedSent_ = false;
  timer_.start();
  return true;
}

void SessionBridge::stop() {
  if (running()) session_->stop();
}

void SessionBridge::pause() {
  if (running()) session_->pause();
}

void SessionBridge::resume() {
  if (running()) session_->resume();
}

void SessionBridge::applyChanges() {
  if (running()) session_->apply_changes();
}

void SessionBridge::setHold(bool on) {
  if (running()) session_->set_hold(on);
}

bool SessionBridge::running() const { return session_ != nullptr && !finishedSent_; }

vyrn::RunConfig::Action SessionBridge::action() const { return action_; }

vyrn::RunState SessionBridge::state() const { return session_ ? session_->state() : vyrn::RunState::Finished; }

void SessionBridge::poll() {
  // A slot connected to one of the signals below may open a dialog with its own event loop, and the timer
  // keeps firing in there: never poll inside a poll, or events would be handed out of order.
  if (polling_ || !session_) return;
  polling_ = true;
  vyrn::Session* const session = session_.get();
  auto stillCurrent = [this, session] { return session_.get() == session; };

  // Read finished() before draining: once it is true, every event of the run is already in the queue.
  const bool done = session->finished();
  bool drained = false;
  for (;;) {
    const std::vector<std::string> batch = session->drain(kEventsPerPoll);
    for (const std::string& text : batch) {
      dispatch(text);
      if (!stillCurrent()) break;   // a slot started a new run: it has its own events
    }
    if (!stillCurrent()) break;
    if (batch.size() < kEventsPerPoll) {
      drained = true;
      break;
    }
    if (!done) break;   // more next time; a finished run is emptied now
  }

  if (stillCurrent()) {
    vyrn::LiveView view = session->live();
    if (view.version != lastLiveVersion_) {
      lastLiveVersion_ = view.version;
      Q_EMIT liveUpdated(view);
    }
  }
  if (stillCurrent() && done && drained && !finishedSent_) {
    timer_.stop();
    lock_.reset();   // the run is over and saved: others may train this project now
    finishedSent_ = true;
    polling_ = false;
    Q_EMIT finished();
    return;
  }
  polling_ = false;
}

void SessionBridge::dispatch(const std::string& text) {
  QJsonParseError parseError;
  const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(text), &parseError);
  if (!doc.isObject()) return;
  const QJsonObject e = doc.object();
  Q_EMIT event(e);

  const QString type = e.value(QLatin1String("type")).toString();
  if (type == QLatin1String("state")) {
    Q_EMIT stateChanged(e.value(QLatin1String("state")).toString(), e.value(QLatin1String("detail")).toString());
  } else if (type == QLatin1String("log")) {
    Q_EMIT logLine(e.value(QLatin1String("stream")).toString(QStringLiteral("app")),
                   e.value(QLatin1String("text")).toString());
  } else if (type == QLatin1String("spec")) {
    Q_EMIT specReceived(e);
  } else if (type == QLatin1String("error")) {
    Q_EMIT errorReceived(e);
  } else if (type == QLatin1String("warning")) {
    Q_EMIT warningReceived(e);
  } else if (type == QLatin1String("round")) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    Q_EMIT roundFinished(qint64(numberOr(e, "round", 0)), numberOr(e, "points", 0), numberOr(e, "mean20", 0),
                         numberOr(e, "best_mean20", nan));
  } else if (type == QLatin1String("update")) {
    Q_EMIT updateReceived(e);
  } else if (type == QLatin1String("timing")) {
    Q_EMIT timingReceived(e);
  } else if (type == QLatin1String("countdown")) {
    Q_EMIT countdown(int(std::ceil(std::max(0.0, numberOr(e, "seconds_left", 0)))));
  } else if (type == QLatin1String("hold")) {
    Q_EMIT holdChanged(e.value(QLatin1String("on")).toBool(), e.value(QLatin1String("reason")).toString());
  } else if (type == QLatin1String("checkpoint")) {
    Q_EMIT checkpointSaved(e);
  } else if (type == QLatin1String("check")) {
    Q_EMIT checkFinished(e);
  } else if (type == QLatin1String("link")) {
    Q_EMIT linkChanged(e);
  }
}
