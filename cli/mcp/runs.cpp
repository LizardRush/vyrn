// Sessions the MCP server runs: background trainings, and Checks and Watches run to the end.
#include "runs.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

namespace vyrn::mcp {

namespace {

std::string str(const json& e, const char* key) {
  auto it = e.find(key);
  return it != e.end() && it->is_string() ? it->get<std::string>() : std::string();
}

int64_t integer(const json& e, const char* key) {
  auto it = e.find(key);
  if (it == e.end() || !it->is_number()) return 0;
  return it->is_number_float() ? static_cast<int64_t>(it->get<double>()) : it->get<int64_t>();
}

std::optional<double> number(const json& e, const char* key) {
  auto it = e.find(key);
  if (it == e.end() || !it->is_number()) return std::nullopt;
  const double v = it->get<double>();
  return std::isfinite(v) ? std::optional<double>(v) : std::nullopt;
}

void add_once(std::vector<std::string>& list, const std::string& item, size_t cap) {
  if (item.empty() || list.size() >= cap) return;
  if (std::find(list.begin(), list.end(), item) == list.end()) list.push_back(item);
}

}  // namespace

double now_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string format_error(const json& e) {
  const std::string script = str(e, "script"), message = str(e, "message"), hint = str(e, "hint");
  const std::string exc = str(e, "exc_type"), kind = str(e, "kind");
  const int64_t line = integer(e, "line"), col = integer(e, "col");
  std::string text = message.empty() ? "Something went wrong." : message;
  if (!exc.empty() && kind == "exception" && text.rfind(exc, 0) != 0) text = exc + ": " + text;
  std::string where = script;
  if (!where.empty() && line > 0) {
    where += ":" + std::to_string(line);
    if (col > 0) where += ":" + std::to_string(col);
  }
  std::string out = where.empty() ? text : where + " " + text;
  if (!hint.empty()) out += " — " + hint;
  return out;
}

std::string format_warning(const json& e) {
  const std::string script = str(e, "script"), message = str(e, "message");
  const int64_t line = integer(e, "line");
  if (message.empty()) return "";
  if (script.empty() || message.rfind(script, 0) == 0) return message;   // "vyrn.json isn't valid JSON ..."
  return script + (line > 0 ? ":" + std::to_string(line) : std::string()) + ": " + message;
}

void Digest::add(const std::string& event_text) {
  const json e = json::parse(event_text, nullptr, false);
  if (e.is_discarded() || !e.is_object()) return;
  const std::string type = str(e, "type");
  if (type == "state") {
    state = str(e, "state");
    detail = str(e, "detail");
  } else if (type == "log") {
    const std::string stream = str(e, "stream");
    std::string line = str(e, "text");
    if (line.size() > 400) line = line.substr(0, 400) + "...";
    if (stream == "stderr") line = "[stderr] " + line;
    if (stream == "app") line = "[vyrn] " + line;
    console.push_back(std::move(line));
    while (console.size() > 30) console.pop_front();
  } else if (type == "spec") {
    spec = e;
    if (auto w = e.find("warnings"); w != e.end() && w->is_array()) {   // Train shows these only in the spec
      for (const auto& item : *w) {
        if (item.is_object()) add_once(warnings, format_warning(item), 50);
      }
    }
  } else if (type == "error") {
    last_error = e;
    const std::string text = format_error(e);
    if (errors.empty() || errors.back() != text) add_once(errors, text, 20);
    if (str(e, "reason") == "save_failed") save_failed = true;
  } else if (type == "warning") {
    add_once(warnings, format_warning(e), 50);
  } else if (type == "round") {
    ++rounds;
    last_round = std::max(last_round, integer(e, "round"));
    const double points = number(e, "points").value_or(0.0);
    recent_points.push_back(points);
    while (recent_points.size() > 50) recent_points.pop_front();
    if (round_points.size() < 1000) round_points.push_back(points);
    if (auto m = number(e, "mean20")) mean20 = m;
    if (auto b = number(e, "best_mean20")) best_mean20 = b;
    if (auto x = number(e, "explore")) explore = x;
    if (auto t = number(e, "total_ticks")) total_ticks = static_cast<int64_t>(*t);
  } else if (type == "update") {
    if (auto x = number(e, "explore")) explore = x;
  } else if (type == "timing") {
    if (auto s = number(e, "ticks_per_s")) ticks_per_s = s;
    if (auto x = number(e, "explore")) explore = x;
    if (auto t = number(e, "total_ticks")) total_ticks = static_cast<int64_t>(*t);
  } else if (type == "checkpoint") {
    ++checkpoints;
    if (auto b = number(e, "best_mean20")) best_mean20 = b;
  } else if (type == "check") {
    check = e;
  }
}

double Training::minutes_left() const {
  if (finished) return 0;
  return std::max(0.0, minutes - running_s / 60.0);
}

Runs::Runs(std::string data_dir, std::string python, int max_trainings)
    : data_dir_(std::move(data_dir)), python_(std::move(python)), max_(std::max(1, max_trainings)) {}

Runs::~Runs() {
  stop_all();
  trainings_.clear();   // each Session's destructor waits for its run to end (Train saves first)
}

RunConfig Runs::config(RunConfig::Action action, const std::string& project) const {
  RunConfig c;
  c.action = action;
  c.project_dir = project;
  c.data_dir = data_dir_;
  if (!python_.empty()) c.python = python_;
  c.allow_input = false;   // docs/dev/mcp.md section 4: keys are never pressed through MCP
  return c;
}

int Runs::running() const {
  int n = 0;
  for (const auto& t : trainings_) n += t->finished ? 0 : 1;
  return n;
}

std::vector<std::string> Runs::running_folders() const {
  std::vector<std::string> out;
  for (const auto& t : trainings_) {
    if (!t->finished) out.push_back(t->folder);
  }
  return out;
}

Training* Runs::find(const std::string& path) {
  for (auto it = trainings_.rbegin(); it != trainings_.rend(); ++it) {
    if ((*it)->path == path) return it->get();
  }
  return nullptr;
}

Training& Runs::start(const std::string& path, const std::string& folder, double minutes, bool new_brain,
                      ProjectLock lock) {
  // An older, finished record of this project goes; so do the oldest finished ones beyond 32.
  trainings_.erase(std::remove_if(trainings_.begin(), trainings_.end(),
                                  [&](const std::unique_ptr<Training>& t) { return t->finished && t->path == path; }),
                   trainings_.end());
  while (trainings_.size() >= 32) {
    auto oldest = std::find_if(trainings_.begin(), trainings_.end(),
                               [](const std::unique_ptr<Training>& t) { return t->finished; });
    if (oldest == trainings_.end()) break;
    trainings_.erase(oldest);
  }
  auto t = std::make_unique<Training>();
  t->path = path;
  t->folder = folder;
  t->minutes = minutes;
  t->new_brain = new_brain;
  t->lock.emplace(std::move(lock));
  RunConfig cfg = config(RunConfig::Action::Train, path);
  cfg.max_seconds = minutes * 60.0;
  cfg.new_brain = new_brain;
  t->session = std::make_unique<Session>(cfg);
  t->started = t->last_pump = now_s();
  t->session->start();
  std::fprintf(stderr, "vyrn mcp: training %s for %g minutes%s\n", folder.c_str(), minutes,
               new_brain ? " with a new brain" : "");
  trainings_.push_back(std::move(t));
  return *trainings_.back();
}

void Runs::stop(Training& t) {
  if (t.finished || t.stop_requested) return;
  t.stop_requested = true;
  t.checkpoints_at_stop = t.digest.checkpoints;
  t.session->stop();
}

void Runs::pump() {
  for (auto& t : trainings_) {
    if (t->finished) continue;
    for (const auto& e : t->session->drain(2000)) t->digest.add(e);
    const double now = now_s();
    if (t->was_running) t->running_s += now - t->last_pump;
    t->last_pump = now;
    t->was_running = t->session->state() == RunState::Running;
    if (!t->session->finished()) continue;
    for (const auto& e : t->session->drain(static_cast<size_t>(1) << 30)) t->digest.add(e);
    t->final_view = t->session->live();
    t->finished = true;
    t->finished_at = now;
    t->lock.reset();   // the project is free again
    std::fprintf(stderr, "vyrn mcp: training %s ended: %s\n", t->folder.c_str(), t->digest.detail.c_str());
  }
}

void Runs::stop_all() {
  for (auto& t : trainings_) stop(*t);
}

bool Runs::all_finished() const { return running() == 0; }

Outcome run_to_end(const RunConfig& config, Waiter& waiter, double seconds,
                   const std::function<bool(const Digest&)>& enough) {
  Outcome out;
  Session session(config);
  session.start();
  const double start = now_s();
  double stop_at = 0;
  bool stopping = false;
  auto take_view = [&] {
    LiveView v = session.live();
    if (!v.kind.empty() && (out.last_view.kind.empty() || v.version != out.last_view.version)) {
      out.last_view = std::move(v);
    }
  };
  for (;;) {
    for (const auto& e : session.drain(2000)) out.digest.add(e);
    if (session.finished()) {
      for (const auto& e : session.drain(static_cast<size_t>(1) << 30)) out.digest.add(e);
      take_view();
      out.finished = true;
      break;
    }
    take_view();
    const double now = now_s();
    if (!stopping) {
      if (enough && enough(out.digest)) {
        stopping = true;
      } else if (now - start > seconds) {
        out.timed_out = true;
        stopping = true;
      }
      if (stopping) {
        session.stop();
        stop_at = now;
      }
    } else if (now - stop_at > 30) {
      break;   // the Session's destructor waits for the rest
    }
    if (!waiter.wait(0.05) && !stopping) {
      out.interrupted = true;
      stopping = true;
      session.stop();
      stop_at = now_s();
    }
  }
  return out;
}

std::vector<uint8_t> picture_png(const Picture& p) {
  if (p.w <= 0 || p.h <= 0 || p.rgb.size() != static_cast<size_t>(p.w) * static_cast<size_t>(p.h) * 3) return {};
  const int biggest = std::max(p.w, p.h);
  const int scale = std::clamp(256 / biggest, 1, 16);
  try {
    return png_encode(p.w, p.h, p.rgb, scale);
  } catch (const std::exception&) {
    return {};
  }
}

}  // namespace vyrn::mcp
