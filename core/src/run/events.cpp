#include "run/events.h"

#include <algorithm>
#include <cmath>

namespace vyrn::run {

namespace {

// Rounds to `digits` decimals so events stay short (timings, means); NaN and infinity become 0.
double tidy(double v, int digits = 4) {
  if (!std::isfinite(v)) return 0.0;
  const double scale = std::pow(10.0, digits);
  const double r = std::round(v * scale) / scale;
  return r == 0 ? 0.0 : r;   // no "-0"
}

// Keeps `digits` significant digits (learning rates: 2.5e-4, not 2.4999999368e-4 from a float).
double significant(double v, int digits) {
  if (!std::isfinite(v) || v == 0) return 0.0;
  const double scale = std::pow(10.0, digits - 1 - static_cast<int>(std::floor(std::log10(std::fabs(v)))));
  return std::round(v * scale) / scale;
}

ojson optional_number(const std::optional<double>& v) { return v ? ojson(tidy(*v)) : ojson(nullptr); }

ojson event(const char* type) {
  ojson e = ojson::object();
  e["type"] = type;
  return e;
}

}  // namespace

const char* state_name(RunState state) {
  switch (state) {
    case RunState::Starting: return "starting";
    case RunState::Countdown: return "countdown";
    case RunState::Restarting: return "restarting";
    case RunState::Running: return "running";
    case RunState::Holding: return "holding";
    case RunState::Paused: return "paused";
    case RunState::Error: return "error";
    case RunState::Stopping: return "stopping";
    case RunState::Finished: return "finished";
  }
  return "unknown";
}

ojson state_event(RunState state, const std::string& detail) {
  ojson e = event("state");
  e["state"] = state_name(state);
  e["detail"] = detail;
  return e;
}

ojson log_event(const std::string& stream, const std::string& text) {
  ojson e = event("log");
  e["stream"] = stream;
  e["text"] = text;
  return e;
}

ojson warning_event(const std::string& script, int64_t line, const std::string& message) {
  ojson e = event("warning");
  e["script"] = script;
  e["line"] = line;
  e["message"] = message;
  return e;
}

ojson round_event(const RoundRecord& r) {
  ojson e = event("round");
  e["round"] = r.round;
  e["points"] = tidy(r.points, 6);
  e["ticks"] = r.ticks;
  e["truncated"] = r.truncated;
  e["game_over"] = r.game_over;
  e["mean20"] = tidy(r.mean20);
  e["best_mean20"] = optional_number(r.best_mean20);
  e["total_ticks"] = r.total_ticks;
  e["explore"] = tidy(r.explore);
  e["seconds"] = tidy(r.seconds, 2);
  return e;
}

ojson update_event(const UpdateRecord& u) {
  ojson e = event("update");
  e["updates"] = u.updates;
  e["loss"] = tidy(u.loss, 6);
  e["q"] = tidy(u.q);
  e["explore"] = tidy(u.explore);
  e["lr"] = significant(u.lr, 6);
  e["updates_per_s"] = tidy(u.updates_per_s, 1);
  return e;
}

ojson timing_event(const TimingRecord& t) {
  ojson e = event("timing");
  e["see_ms"] = tidy(t.see_ms, 3);
  e["points_ms"] = tidy(t.points_ms, 3);
  e["respond_ms"] = tidy(t.respond_ms, 3);
  e["step_ms"] = tidy(t.step_ms, 3);
  e["brain_ms"] = tidy(t.brain_ms, 3);
  e["ipc_ms"] = tidy(t.ipc_ms, 3);
  e["ticks_per_s"] = tidy(t.ticks_per_s, 1);
  e["late_p99_ms"] = tidy(t.late_p99_ms, 2);
  e["overruns"] = t.overruns;
  e["explore"] = tidy(t.explore);
  e["total_ticks"] = t.total_ticks;
  return e;
}

ojson checkpoint_event(const std::string& which, const std::string& path, std::optional<double> best_mean20) {
  ojson e = event("checkpoint");
  e["which"] = which;
  e["path"] = path;
  e["best_mean20"] = optional_number(best_mean20);
  return e;
}

ojson countdown_event(double seconds_left) {
  ojson e = event("countdown");
  e["seconds_left"] = tidy(std::max(0.0, seconds_left), 1);
  return e;
}

ojson hold_event(bool on, const std::string& reason) {
  ojson e = event("hold");
  e["on"] = on;
  e["reason"] = reason;
  return e;
}

ojson worker_error_event(const json& worker_error, bool fatal) {
  ojson e = event("error");
  if (worker_error.is_object()) {
    for (auto it = worker_error.begin(); it != worker_error.end(); ++it) {
      if (it.key() == "t" || it.key() == "id" || it.key() == "type") continue;
      e[it.key()] = ojson::parse(it.value().dump(-1, ' ', false, json::error_handler_t::replace));
    }
  }
  if (!e.contains("kind")) e["kind"] = "exception";
  if (!e.contains("message")) e["message"] = "Python reported an error.";
  e["fatal"] = fatal;
  return e;
}

ojson app_error_event(const std::string& reason, const std::string& message, const std::string& hint, bool fatal) {
  ojson e = event("error");
  e["kind"] = "app";
  e["reason"] = reason;
  e["script"] = "";
  e["line"] = 0;
  e["message"] = message;
  e["hint"] = hint;
  e["fatal"] = fatal;
  return e;
}

ojson not_trusted_event(const std::string& message) {
  ojson e = event("error");
  e["kind"] = "not_trusted";
  e["script"] = "";
  e["line"] = 0;
  e["message"] = message;
  e["hint"] = "";
  e["fatal"] = true;
  return e;
}

std::string event_text(const ojson& event) { return event.dump(-1, ' ', false, json::error_handler_t::replace); }

void EventQueue::push(const ojson& event) {
  std::string text = event_text(event);
  std::lock_guard<std::mutex> lock(mu_);
  if (items_.size() >= capacity_) {
    items_.pop_front();
    ++unreported_drops_;
  }
  items_.push_back(std::move(text));
}

std::vector<std::string> EventQueue::drain(size_t max) {
  std::vector<std::string> out;
  std::lock_guard<std::mutex> lock(mu_);
  if (max == 0) return out;
  if (unreported_drops_ > 0) {
    out.push_back(event_text(log_event(
        "app", "(" + std::to_string(unreported_drops_) + " events were dropped because nobody read them)")));
    unreported_drops_ = 0;
  }
  const size_t n = std::min(items_.size(), max - std::min(max, out.size()));
  for (size_t i = 0; i < n; ++i) {
    out.push_back(std::move(items_.front()));
    items_.pop_front();
  }
  return out;
}

size_t EventQueue::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return items_.size();
}

}  // namespace vyrn::run
