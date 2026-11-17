#include "run/check.h"

#include "run/observation.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vyrn::run {

std::pair<std::string, int64_t> split_location(const std::string& where) {
  const auto colon = where.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= where.size()) return {"", 0};
  int64_t line = 0;
  for (size_t i = colon + 1; i < where.size(); ++i) {
    if (where[i] < '0' || where[i] > '9') return {"", 0};
    line = line * 10 + (where[i] - '0');
  }
  return {where.substr(0, colon), line};
}

CheckReport::CheckReport(const json& spec) : spec_(spec) {
  auto obs = spec.find("obs");
  if (obs != spec.end() && obs->is_object()) {
    auto names = obs->find("names");
    if (names != obs->end() && names->is_array()) {
      for (const auto& n : *names) number_names_.push_back(n.is_string() ? n.get<std::string>() : "");
    }
  }
  auto warnings = spec.find("warnings");
  if (warnings != spec.end() && warnings->is_array()) {
    for (const auto& w : *warnings) add_warning(w);
  }
}

void CheckReport::add_warning(const json& w) {
  if (!w.is_object()) return;
  CheckWarning cw;
  cw.script = w.value("script", "");
  cw.line = w.value("line", int64_t{0});
  cw.message = w.value("message", "");
  if (cw.message.empty()) return;
  const bool seen = std::any_of(warnings_.begin(), warnings_.end(), [&](const CheckWarning& o) {
    return o.script == cw.script && o.line == cw.line && o.message == cw.message;
  });
  if (!seen) warnings_.push_back(std::move(cw));
}

void CheckReport::track_obs(const ipc::Frame& frame, const char* key) {
  ObsView o;
  if (!read_obs(frame, key, o)) return;
  ++observations_;
  if (o.picture) {
    const size_t bytes = static_cast<size_t>(o.h) * static_cast<size_t>(o.w) * 3;
    if (bytes > 0) brightest_ = std::max(brightest_, static_cast<int>(*std::max_element(o.rgb, o.rgb + bytes)));
    if (observations_ == 1) {
      first_picture_.assign(o.rgb, o.rgb + bytes);
      first_h_ = o.h;
      first_w_ = o.w;
    } else if (!picture_changed_) {
      picture_changed_ = o.h != first_h_ || o.w != first_w_ || std::memcmp(o.rgb, first_picture_.data(), bytes) != 0;
    }
    return;
  }
  if (observations_ == 1 || first_numbers_.size() != o.numbers.size()) {
    first_numbers_ = o.numbers;
    number_changed_.assign(o.numbers.size(), false);
    return;
  }
  for (size_t i = 0; i < o.numbers.size(); ++i) {
    if (o.numbers[i] != first_numbers_[i]) number_changed_[i] = true;
  }
}

void CheckReport::add_result(const ipc::Frame& result) {
  const json& j = result.j;
  auto ms = j.find("ms");
  if (ms != j.end() && ms->is_object()) {
    for (auto it = ms->begin(); it != ms->end(); ++it) {
      if (!it.value().is_number()) continue;
      auto& slot = ms_[it.key()];
      slot.first += it.value().get<double>();
      slot.second += 1;
    }
  }
  auto warnings = j.find("warnings");
  if (warnings != j.end() && warnings->is_array()) {
    for (const auto& w : *warnings) add_warning(w);
  }
  auto did = j.find("did");
  if (did != j.end() && did->is_array()) {
    for (const auto& d : *did) {
      if (d.is_string() && std::find(did_.begin(), did_.end(), d.get<std::string>()) == did_.end()) {
        did_.push_back(d.get<std::string>());
      }
    }
  }
  auto points = j.find("points");
  if (points != j.end() && points->is_number()) {
    points_sum_ += points->get<double>();
    ++points_count_;
  }
  if (j.value("game_over", false) == true) {
    game_over_seen_ = true;
    ++rounds_ended_;
  } else if (j.value("truncated", false) == true) {
    ++rounds_ended_;
  }
  track_obs(result, "final_obs");
  track_obs(result, "obs");
}

void CheckReport::add_ok(const json& reply) {
  ipc::Frame frame;
  frame.j = reply;
  frame.j.erase("obs");
  frame.j.erase("final_obs");
  add_result(frame);
}

void CheckReport::add_restart(const json& reply) {
  // Its timings count like any other hook's; what it would press is reported apart from the choices'.
  ipc::Frame frame;
  frame.j = json::object();
  for (const char* key : {"ms", "warnings"}) {
    auto it = reply.find(key);
    if (it != reply.end()) frame.j[key] = *it;
  }
  add_result(frame);
  restart_ran_ = true;
  auto did = reply.find("did");
  if (did != reply.end() && did->is_array()) {
    for (const auto& d : *did) {
      if (d.is_string() && std::find(restart_did_.begin(), restart_did_.end(), d.get<std::string>()) == restart_did_.end()) {
        restart_did_.push_back(d.get<std::string>());
      }
    }
  }
}

std::string CheckReport::restart_summary() const {
  if (!restart_ran_) return "";
  if (restart_did_.empty()) return "restart(): ran (it has no keys to press).";
  std::string text;
  for (const auto& d : restart_did_) text += (text.empty() ? "" : ", ") + d;
  return "restart(): " + text + " (nothing was pressed).";
}

std::vector<CheckWarning> CheckReport::take_new_warnings() {
  std::vector<CheckWarning> out(warnings_.begin() + static_cast<std::ptrdiff_t>(handed_out_), warnings_.end());
  handed_out_ = warnings_.size();
  return out;
}

CheckWarning CheckReport::at_hook(const char* hook, const std::string& message) const {
  CheckWarning w;
  w.message = message;
  auto hooks = spec_.find("hooks");
  if (hooks != spec_.end() && hooks->is_object()) {
    auto where = hooks->find(hook);
    if (where != hooks->end() && where->is_string()) {
      auto loc = split_location(where->get<std::string>());
      w.script = loc.first;
      w.line = loc.second;
    }
  }
  return w;
}

std::vector<std::string> CheckReport::constant_inputs() const {
  std::vector<std::string> out;
  if (observations_ < 2 || !constant_inputs_meaningful_) return out;
  if (!first_picture_.empty()) {
    if (!picture_changed_) out.push_back("picture");
    return out;
  }
  for (size_t i = 0; i < number_changed_.size(); ++i) {
    if (number_changed_[i]) continue;
    const bool named = i < number_names_.size() && !number_names_[i].empty();
    out.push_back(named ? number_names_[i] : "input " + std::to_string(i + 1));
  }
  return out;
}

std::vector<CheckWarning> CheckReport::findings(bool realtime, double decisions_per_second) const {
  std::vector<CheckWarning> out;
  const std::string what = std::to_string(steps_) + (realtime ? " ticks" : " steps");
  auto hooks = spec_.find("hooks");
  const bool has_game_over = hooks != spec_.end() && hooks->is_object() && hooks->contains("game_over");
  if (!realtime && has_game_over && !game_over_seen_ && steps_ > 0) {   // real time: a note (notes())
    out.push_back(at_hook("game_over", "game_over() was never True in " + what +
                                           ". Make sure it becomes True when a round really is over."));
  }
  if (brightest_ >= 0 && brightest_ < kAlmostBlack) {
    out.push_back(at_hook("see", "The picture is almost black (brightest value " + std::to_string(brightest_) +
                                     " of 255): the AI can hardly see anything."));
  }
  for (const auto& name : constant_inputs()) {
    if (name == "picture" && !first_picture_.empty()) {
      out.push_back(at_hook("see", "The picture never changed in " + what +
                                       ": the AI can't learn from a picture that always looks the same."));
    } else {
      out.push_back(at_hook("see", "\"" + name + "\" never changed in " + what + ", so the AI would ignore it."));
    }
  }
  auto see = ms_.find("see");
  if (realtime && see != ms_.end() && see->second.second > 0 && decisions_per_second > 0) {
    const double mean = see->second.first / static_cast<double>(see->second.second);
    const double tick_ms = 1000.0 / decisions_per_second;
    if (mean > 0.8 * tick_ms) {
      out.push_back(at_hook("see", "see() takes " + std::to_string(static_cast<int>(std::lround(mean))) +
                                       " ms, over 80% of a tick (" +
                                       std::to_string(static_cast<int>(std::lround(tick_ms))) +
                                       " ms). Use a smaller screen area, or fewer decisions per second."));
    }
  }
  return out;
}

std::vector<std::string> CheckReport::notes(bool realtime, double decisions_per_second) const {
  std::vector<std::string> out;
  auto hooks = spec_.find("hooks");
  const bool has_game_over = hooks != spec_.end() && hooks->is_object() && hooks->contains("game_over");
  if (realtime && has_game_over && !game_over_seen_ && steps_ > 0) {
    // A real game rarely ends within a check this short.
    const double seconds = decisions_per_second > 0 ? static_cast<double>(steps_) / decisions_per_second : 0.0;
    out.push_back("game_over() stayed False during this short check (" + short_number(seconds) +
                  " s). That is normal if the game didn't end. If your game-over screen was showing, game_over() "
                  "needs fixing.");
  }
  out.insert(out.end(), extra_notes_.begin(), extra_notes_.end());
  return out;
}

ojson CheckReport::event(bool ok, bool realtime, double decisions_per_second) const {
  ojson e = ojson::object();
  e["type"] = "check";
  e["ok"] = ok;
  e["mode"] = realtime ? "realtime" : "sim";
  ojson warnings = ojson::array();
  std::vector<CheckWarning> all = warnings_;
  for (auto& w : findings(realtime, decisions_per_second)) all.push_back(std::move(w));
  for (const auto& w : all) warnings.push_back(ojson{{"script", w.script}, {"line", w.line}, {"message", w.message}});
  e["warnings"] = warnings;
  e["notes"] = notes(realtime, decisions_per_second);
  ojson ms = ojson::object();
  for (const auto& [hook, total] : ms_) {
    const double mean = total.second > 0 ? total.first / static_cast<double>(total.second) : 0.0;
    ms[hook] = std::round(mean * 1000.0) / 1000.0;
  }
  e["ms"] = ms;
  e["mean_points"] = points_count_ > 0 ? points_sum_ / static_cast<double>(points_count_) : 0.0;
  e["game_over_seen"] = game_over_seen_;
  e["constant_inputs"] = constant_inputs();
  e["steps"] = steps_;
  e["rounds_ended"] = rounds_ended_;
  std::vector<std::string> did(did_.begin(), did_.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(6, did_.size())));
  e["did"] = did;
  if (restart_ran_) e["restart_did"] = restart_did_;
  return e;
}

}  // namespace vyrn::run
