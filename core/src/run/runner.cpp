#include "run/runner.h"

#include "brain/preprocess.h"
#include "util/clock.h"
#include "util/files.h"

#include <algorithm>
#include <filesystem>

namespace vyrn::run {

const char* action_name(RunConfig::Action action) {
  switch (action) {
    case RunConfig::Action::Check: return "check";
    case RunConfig::Action::Train: return "train";
    case RunConfig::Action::Watch: return "watch";
  }
  return "?";
}

ojson spec_event(const json& spec_msg, RunConfig::Action action, const std::string& brain_status,
                 const std::string& why, const std::string& brain_id, const std::string& net,
                 const brain::BrainShape& shape, uint64_t seed) {
  ojson e = ojson::object();
  e["type"] = "spec";
  if (spec_msg.is_object()) {
    for (auto it = spec_msg.begin(); it != spec_msg.end(); ++it) {
      const std::string& key = it.key();
      if (key == "t" || key == "id" || key == "type" || key == "preview") continue;
      e[key] = ojson::parse(it.value().dump(-1, ' ', false, json::error_handler_t::replace));
    }
  }
  e["action"] = action_name(action);
  e["brain"] = brain_status;
  e["why"] = why;
  e["brain_id"] = brain_id;
  e["net"] = net;
  e["frames"] = shape.kind == brain::ObsSpec::Kind::Picture ? std::max(1, shape.frames) : 1;
  e["seed"] = seed;
  return e;
}

LiveView check_live_view(const ObsView& obs, const brain::Spec& spec) {
  LiveView view;
  view.choices = spec.labels;
  if (obs.picture) {
    const brain::Geometry g = brain::choose_geometry(obs.h, obs.w);
    std::vector<uint8_t> chw(static_cast<size_t>(3) * static_cast<size_t>(g.h) * static_cast<size_t>(g.w));
    brain::to_frame(obs.rgb, obs.h, obs.w, g, chw.data());
    view.kind = "picture";
    view.input = picture_from_chw(chw.data(), g.h, g.w);
    return view;
  }
  view.kind = "numbers";
  view.numbers = obs.numbers;
  view.number_names = spec.obs.names;
  if (view.number_names.size() != view.numbers.size()) {
    view.number_names.clear();
    for (size_t i = 0; i < view.numbers.size(); ++i) view.number_names.push_back("input " + std::to_string(i + 1));
  }
  return view;
}

void emit_result_warnings(RunContext& ctx, const json& result) {
  auto warnings = result.find("warnings");
  if (warnings == result.end() || !warnings->is_array()) return;
  for (const auto& w : *warnings) {
    if (!w.is_object()) continue;
    ctx.emit(warning_event(w.value("script", ""), w.value("line", int64_t{0}), w.value("message", "")));
  }
}

void emit_settings_warning(RunContext& ctx, const std::string& message) {
  const std::string prefix = "vyrn.json: ";
  ctx.emit(warning_event("vyrn.json", 0, message.rfind(prefix, 0) == 0 ? message.substr(prefix.size()) : message));
}

void open_run_logs(RunContext& ctx, const std::string& project_dir, const std::string& brain_id,
                   RunConfig::Action action, util::CsvWriter& rounds, util::CsvWriter& updates) {
  namespace fs = std::filesystem;
  const fs::path runs = fs::path(project_dir) / "runs";
  std::error_code ec;
  fs::create_directories(runs, ec);
  const fs::path dir = runs / util::unique_child_name(runs.string(), util::local_stamp());
  std::string why;
  if (!fs::create_directories(dir, ec) ||
      !rounds.open((dir / "rounds.csv").string(),
                   {"round", "points", "ticks", "truncated", "game_over", "mean20", "best_mean20", "total_ticks",
                    "explore", "seconds"},
                   &why) ||
      !updates.open((dir / "updates.csv").string(),
                    {"seconds", "total_ticks", "updates", "loss", "q", "explore", "lr", "updates_per_s",
                     "ticks_per_s"},
                    &why)) {
    ctx.emit(warning_event("", 0, "Vyrn can't write its logs in " + dir.string() +
                                      (why.empty() ? std::string() : " (" + why + ")") + "; training goes on."));
    return;
  }
  // Which brain these logs belong to: sharing (.vrl) reads the points chart of a brain from its runs.
  try {
    const ojson run = {{"brain", brain_id}, {"action", action_name(action)}, {"started", util::local_iso_time()}};
    util::write_text_file_atomic((dir / "run.json").string(), run.dump(2) + "\n");
  } catch (const std::exception& e) {
    ctx.emit(warning_event("", 0, std::string("Vyrn can't write run.json in ") + dir.string() + " (" + e.what() +
                                      "); training goes on, but this run's points won't be shared with the brain."));
  }
}

void write_round_row(util::CsvWriter& rounds, const RoundRecord& rec) {
  rounds.row({util::csv_number(static_cast<long long>(rec.round)), util::csv_number(rec.points),
              util::csv_number(static_cast<long long>(rec.ticks)), rec.truncated ? "1" : "0",
              rec.game_over ? "1" : "0", util::csv_number(rec.mean20),
              rec.best_mean20 ? util::csv_number(*rec.best_mean20) : "",
              util::csv_number(static_cast<long long>(rec.total_ticks)), util::csv_number(rec.explore),
              util::csv_number(rec.seconds)});
}

void write_update_row(util::CsvWriter& updates, double seconds, int64_t total_ticks, const UpdateRecord& u,
                      double ticks_per_s) {
  updates.row({util::csv_number(seconds), util::csv_number(static_cast<long long>(total_ticks)),
               util::csv_number(static_cast<long long>(u.updates)), util::csv_number(u.loss), util::csv_number(u.q),
               util::csv_number(u.explore), util::csv_number(u.lr), util::csv_number(u.updates_per_s),
               util::csv_number(ticks_per_s)});
}

}  // namespace vyrn::run
