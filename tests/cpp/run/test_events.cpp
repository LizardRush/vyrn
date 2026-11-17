// Session events: the JSON each builder makes and the queue drain() reads.
#include "run/events.h"

#include <gtest/gtest.h>

using vyrn::RunState;
using namespace vyrn::run;

namespace {

json parsed(const ojson& e) { return json::parse(event_text(e)); }

}  // namespace

TEST(Events, TypeComesFirst) {
  const std::string text = event_text(state_event(RunState::Running, "Training."));
  EXPECT_EQ(text, R"({"type":"state","state":"running","detail":"Training."})");
}

TEST(Events, StateNames) {
  const std::pair<RunState, const char*> names[] = {
      {RunState::Starting, "starting"}, {RunState::Countdown, "countdown"}, {RunState::Restarting, "restarting"},
      {RunState::Running, "running"},   {RunState::Holding, "holding"},     {RunState::Paused, "paused"},
      {RunState::Error, "error"},       {RunState::Stopping, "stopping"},   {RunState::Finished, "finished"}};
  for (const auto& [state, name] : names) {
    EXPECT_STREQ(state_name(state), name);
    EXPECT_STREQ(vyrn::to_string(state), name);
  }
}

TEST(Events, LogAndWarning) {
  EXPECT_EQ(parsed(log_event("stdout", "hello")), (json{{"type", "log"}, {"stream", "stdout"}, {"text", "hello"}}));
  const json w = parsed(warning_event("points.py", 7, "points() returned None"));
  EXPECT_EQ(w["type"], "warning");
  EXPECT_EQ(w["script"], "points.py");
  EXPECT_EQ(w["line"], 7);
  EXPECT_EQ(w["message"], "points() returned None");
}

TEST(Events, RoundHasEveryField) {
  RoundRecord r;
  r.round = 12;
  r.points = -1;
  r.ticks = 9;
  r.game_over = true;
  r.mean20 = 0.123456789;
  r.total_ticks = 108;
  r.explore = 0.5;
  r.seconds = 1.234;
  json e = parsed(round_event(r));
  EXPECT_EQ(e["type"], "round");
  EXPECT_EQ(e["round"], 12);
  EXPECT_DOUBLE_EQ(e["points"].get<double>(), -1.0);
  EXPECT_EQ(e["ticks"], 9);
  EXPECT_EQ(e["truncated"], false);
  EXPECT_EQ(e["game_over"], true);
  EXPECT_DOUBLE_EQ(e["mean20"].get<double>(), 0.1235);   // rounded for display
  EXPECT_TRUE(e["best_mean20"].is_null());                // no best before 20 rounds
  EXPECT_EQ(e["total_ticks"], 108);
  EXPECT_DOUBLE_EQ(e["explore"].get<double>(), 0.5);

  r.best_mean20 = 0.75;
  e = parsed(round_event(r));
  EXPECT_DOUBLE_EQ(e["best_mean20"].get<double>(), 0.75);
}

TEST(Events, UpdateAndTiming) {
  UpdateRecord u;
  u.updates = 250;
  u.loss = 0.0123;
  u.q = 1.5;
  u.explore = 0.2;
  u.lr = 2.5e-4;
  u.updates_per_s = 61.27;
  const json e = parsed(update_event(u));
  for (const char* key : {"type", "updates", "loss", "q", "explore", "lr", "updates_per_s"}) {
    EXPECT_TRUE(e.contains(key)) << key;
  }
  EXPECT_EQ(e["updates"], 250);
  EXPECT_DOUBLE_EQ(e["lr"].get<double>(), 2.5e-4);
  u.lr = static_cast<float>(1e-4);   // a float's rounding noise doesn't show
  EXPECT_EQ(event_text(update_event(u)).find("9.99999"), std::string::npos);
  EXPECT_DOUBLE_EQ(parsed(update_event(u))["lr"].get<double>(), 1e-4);

  TimingRecord t;
  t.see_ms = 0.25;
  t.ticks_per_s = 1234.56;
  t.overruns = 2;
  const json tj = parsed(timing_event(t));
  for (const char* key : {"see_ms", "points_ms", "respond_ms", "brain_ms", "ipc_ms", "ticks_per_s", "late_p99_ms",
                          "overruns"}) {
    EXPECT_TRUE(tj.contains(key)) << key;
  }
  EXPECT_DOUBLE_EQ(tj["ticks_per_s"].get<double>(), 1234.6);
  EXPECT_EQ(tj["overruns"], 2);
}

TEST(Events, NonFiniteNumbersBecomeZero) {
  TimingRecord t;
  t.see_ms = std::numeric_limits<double>::quiet_NaN();
  t.ipc_ms = std::numeric_limits<double>::infinity();
  const json e = parsed(timing_event(t));
  EXPECT_EQ(e["see_ms"].get<double>(), 0.0);
  EXPECT_EQ(e["ipc_ms"].get<double>(), 0.0);
}

TEST(Events, WorkerErrorKeepsItsFieldsAndDropsTheEnvelope) {
  const json reply = {{"t", "error"},      {"id", 9},          {"fatal", true},          {"kind", "exception"},
                      {"script", "points.py"}, {"line", 7},    {"col", 12},              {"func", "points"},
                      {"exc_type", "NameError"}, {"message", "name 'scor' is not defined"},
                      {"hint", "Is it spelled the same as where you made it?"}, {"trace", {"points.py:7 in points"}}};
  const json e = parsed(worker_error_event(reply, false));
  EXPECT_EQ(e["type"], "error");
  EXPECT_FALSE(e.contains("t"));
  EXPECT_FALSE(e.contains("id"));
  EXPECT_EQ(e["fatal"], false);   // the session decides: it keeps the brain and pauses
  EXPECT_EQ(e["script"], "points.py");
  EXPECT_EQ(e["line"], 7);
  EXPECT_EQ(e["col"], 12);
  EXPECT_EQ(e["exc_type"], "NameError");
  EXPECT_EQ(e["trace"], json::array({"points.py:7 in points"}));
  // "type" comes first in the text.
  EXPECT_EQ(event_text(worker_error_event(reply, true)).rfind(R"({"type":"error")", 0), 0u);
}

TEST(Events, AppError) {
  const json e = parsed(app_error_event("no_brain", "Nothing to watch.", "Train first.", true));
  EXPECT_EQ(e["type"], "error");
  EXPECT_EQ(e["kind"], "app");
  EXPECT_EQ(e["reason"], "no_brain");
  EXPECT_EQ(e["message"], "Nothing to watch.");
  EXPECT_EQ(e["hint"], "Train first.");
  EXPECT_EQ(e["fatal"], true);
  EXPECT_EQ(e["script"], "");
}

TEST(Events, CheckpointCountdownHold) {
  json e = parsed(checkpoint_event("best", "/p/brains/x/best", 0.9));
  EXPECT_EQ(e, (json{{"type", "checkpoint"}, {"which", "best"}, {"path", "/p/brains/x/best"}, {"best_mean20", 0.9}}));
  e = parsed(countdown_event(2.0));
  EXPECT_EQ(e["seconds_left"].get<double>(), 2.0);
  e = parsed(hold_event(true, "unfocused"));
  EXPECT_EQ(e, (json{{"type", "hold"}, {"on", true}, {"reason", "unfocused"}}));
}

TEST(Events, InvalidUtf8IsReplacedNotThrown) {
  const std::string bad = std::string("caf") + static_cast<char>(0xE9) + " ok";
  std::string text;
  EXPECT_NO_THROW(text = event_text(log_event("stdout", bad)));
  EXPECT_NE(text.find("ok"), std::string::npos);
  json back;
  EXPECT_NO_THROW(back = json::parse(text));
  EXPECT_EQ(back["type"], "log");
}

TEST(EventQueue, DrainsOldestFirstUpToMax) {
  EventQueue q;
  for (int i = 0; i < 5; ++i) q.push(log_event("app", std::to_string(i)));
  auto first = q.drain(2);
  ASSERT_EQ(first.size(), 2u);
  EXPECT_EQ(json::parse(first[0])["text"], "0");
  EXPECT_EQ(json::parse(first[1])["text"], "1");
  auto rest = q.drain(100);
  ASSERT_EQ(rest.size(), 3u);
  EXPECT_EQ(json::parse(rest[2])["text"], "4");
  EXPECT_TRUE(q.drain(100).empty());
}

TEST(EventQueue, DropsTheOldestWhenNobodyReadsAndSaysSo) {
  EventQueue q(3);
  for (int i = 0; i < 5; ++i) q.push(log_event("app", std::to_string(i)));
  EXPECT_EQ(q.size(), 3u);
  auto out = q.drain(100);
  ASSERT_EQ(out.size(), 4u);
  EXPECT_EQ(json::parse(out[0])["text"], "(2 events were dropped because nobody read them)");
  EXPECT_EQ(json::parse(out[1])["text"], "2");
  EXPECT_EQ(json::parse(out[3])["text"], "4");
  EXPECT_TRUE(q.drain(100).empty());
}
