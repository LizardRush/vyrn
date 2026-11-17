// Runs one vyrn::Session for the UI: polls its events and live view on a timer (never blocks the UI thread)
// and turns them into Qt signals. Takes the project lock (by "app") for Train and Watch.
#pragma once

#include <QJsonObject>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>
#include <optional>

#include "vyrn/project.h"
#include "vyrn/session.h"

Q_DECLARE_METATYPE(vyrn::LiveView)

class SessionBridge : public QObject {
  Q_OBJECT

 public:
  explicit SessionBridge(QObject* parent = nullptr);
  ~SessionBridge() override;   // stops a running session and waits for it to save (app exit only)

  // Starts Check, Train or Watch. False + plain-English error when it cannot start (e.g. the project is
  // being trained by an AI agent, or a session is already running).
  bool start(const vyrn::RunConfig& config, QString* error);
  void stop();                 // graceful; finished() is emitted once everything is saved
  void pause();
  void resume();
  void applyChanges();
  void setHold(bool on);
  bool running() const;        // a session exists and has not finished
  vyrn::RunConfig::Action action() const;
  vyrn::RunState state() const;

 Q_SIGNALS:
  void event(const QJsonObject& event);                         // every event, as received
  void stateChanged(const QString& state, const QString& detail);
  void logLine(const QString& stream, const QString& text);     // stream: "stdout" | "stderr" | "app"
  void specReceived(const QJsonObject& spec);
  void errorReceived(const QJsonObject& error);                 // worker error fields (+ "fatal")
  void warningReceived(const QJsonObject& warning);
  void roundFinished(qint64 round, double points, double mean20, double bestMean20);   // bestMean20: NaN before 20 rounds
  void updateReceived(const QJsonObject& update);
  void timingReceived(const QJsonObject& timing);
  void countdown(int secondsLeft);
  void holdChanged(bool on, const QString& reason);
  void checkpointSaved(const QJsonObject& info);
  void checkFinished(const QJsonObject& result);
  void linkChanged(const QJsonObject& link);                    // Vyrn Link: listening, a game connected or left
  void liveUpdated(const vyrn::LiveView& view);
  void finished();

 private:
  void poll();
  void dispatch(const std::string& eventText);   // one drained event -> event() + its typed signal

  std::unique_ptr<vyrn::Session> session_;
  std::optional<vyrn::ProjectLock> lock_;
  QTimer timer_;
  quint64 lastLiveVersion_ = 0;
  bool finishedSent_ = true;
  bool polling_ = false;   // a slot ran a nested event loop: don't poll inside a poll
  vyrn::RunConfig::Action action_ = vyrn::RunConfig::Action::Check;
};
