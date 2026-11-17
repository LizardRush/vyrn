// Runs slow core calls (writing, reading and checking .vrl/.vrlp files) on their own thread, so the UI keeps
// painting, and hands the outcome back on the UI thread.
#pragma once

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThread>

#include <atomic>
#include <exception>
#include <optional>
#include <thread>
#include <utility>

namespace background {

template <typename T>
struct Outcome {
  std::optional<T> value;   // set when the work returned
  QString error;            // else the exception's message (the core's are plain English)
};

inline std::atomic<int>& jobsRunning() {
  static std::atomic<int> count{0};
  return count;
}

// work() runs on a new thread; done(Outcome<T>) then runs on the UI thread, unless `context` is gone by then.
template <typename T, typename Work, typename Done>
void run(QObject* context, Work work, Done done) {
  QPointer<QObject> guard(context);
  ++jobsRunning();
  std::thread([guard, work = std::move(work), done = std::move(done)]() mutable {
    Outcome<T> outcome;
    try {
      outcome.value.emplace(work());
    } catch (const std::exception& e) {
      outcome.error = QString::fromStdString(e.what());
    } catch (...) {
      outcome.error = QStringLiteral("Something unexpected went wrong.");
    }
    if (QCoreApplication* app = QCoreApplication::instance()) {
      QMetaObject::invokeMethod(
          app,
          [guard, done = std::move(done), outcome = std::move(outcome)]() mutable {
            if (guard) done(std::move(outcome));
          },
          Qt::QueuedConnection);
    }
    --jobsRunning();
  }).detach();
}

// Before the app exits: lets a file that is being written finish (at most `ms`).
inline void waitForJobs(int ms) {
  QElapsedTimer clock;
  clock.start();
  while (jobsRunning().load() > 0 && clock.elapsed() < ms) QThread::msleep(20);
}

}  // namespace background
