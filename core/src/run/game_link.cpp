// Vyrn Link inside a run (game_link.h).
#include "run/game_link.h"

#include "ipc/proto.h"
#include "run/runner.h"
#include "util/clock.h"
#include "vyrn/link.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <utility>
#include <vector>

namespace vyrn::run {

namespace test_hooks {
std::string link_bind_address = "0.0.0.0";
}  // namespace test_hooks

namespace {

constexpr double kWatchEvery = 0.1;      // the helper thread looks for notices and connection changes 10x a second
constexpr size_t kRequestRoom = 4096;    // a request's own keys ("t", "id", "first", "connected"...)

std::vector<std::string> names(const json& reply, const char* key) {
  std::vector<std::string> out;
  const auto it = reply.find(key);
  if (it == reply.end() || !it->is_array()) return out;
  for (const auto& v : *it) {
    if (v.is_string()) out.push_back(v.get<std::string>());
  }
  return out;
}

std::string quoted_list(const std::vector<std::string>& keys) {
  std::string text;
  for (size_t i = 0; i < keys.size() && i < 8; ++i) text += (i ? ", \"" : "\"") + keys[i] + "\"";
  if (keys.size() > 8) text += " and " + std::to_string(keys.size() - 8) + " more";
  return text;
}

}  // namespace

std::unique_ptr<GameLink> GameLink::for_project(RunContext& ctx, const std::string& project_dir,
                                                const util::ProjectSettings& settings, double tick_hz) {
  std::vector<std::string> warnings;
  const std::optional<link::LinkSettings> ls = link::link_settings(json(settings.raw), &warnings);
  for (const auto& w : warnings) emit_settings_warning(ctx, w);
  if (!ls) return nullptr;
  link::LinkConfig cfg;
  cfg.port = ls->port;
  cfg.bind_address = test_hooks::link_bind_address;
  bool saved = true;
  cfg.code = link_code(project_dir, &saved);
  if (!saved) {
    ctx.emit(warning_event("", 0,
                           std::string("Vyrn couldn't save the Vyrn Link pairing code in ") + kLinkCodeFile +
                               " (is the project folder read-only?), so the code changes every time Vyrn starts."));
  }
  cfg.welcome.project =
      settings.name.empty() ? std::filesystem::path(project_dir).filename().string() : settings.name;
  cfg.welcome.lockstep = ls->lockstep;
  cfg.welcome.frames_per_step = ls->frames_per_step;
  cfg.welcome.tick_hz = tick_hz;
  return std::unique_ptr<GameLink>(new GameLink(ctx, *ls, std::move(cfg)));
}

GameLink::GameLink(RunContext& ctx, link::LinkSettings settings, link::LinkConfig config)
    : ctx_(ctx), settings_(settings), code_(config.code) {
  server_ = std::make_unique<link::LinkServer>(std::move(config));
}

GameLink::~GameLink() { stop(); }

void GameLink::start() {
  server_->start();
  started_at_ = util::now_seconds();
  const std::string bind = test_hooks::link_bind_address;
  if (bind == "0.0.0.0") {
    const std::vector<std::string> lan = lan_addresses();
    address_ = lan.empty() ? "127.0.0.1" : lan.front();
  } else {
    address_ = bind;
  }
  url_ = "ws://" + address_ + ":" + std::to_string(server_->port()) + "/";
  emit_status(true);
  thread_ = std::thread([this] { watch(); });
}

void GameLink::stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    quit_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  if (!server_->running()) return;
  server_->stop();
  for (const auto& n : server_->take_notices()) ctx_.app_log("Vyrn Link: " + n.text);
  const link::LinkStats s = server_->stats();
  if (s.connections > 0) {
    ctx_.app_log("Vyrn Link: " + std::to_string(s.states) + " states and " + std::to_string(s.events) +
                 " events from the game, " + std::to_string(s.inputs) + " inputs sent.");
  }
}

void GameLink::set_buttons(const json& spec) {
  link::WelcomeInfo w = server_->welcome();
  w.buttons = names(spec, "buttons");
  server_->set_welcome(w);
}

std::string GameLink::waiting_text() const {
  std::string text = "Waiting for the game: connect it to " + url_ + " with the pairing code " + code_;
  if (address_ != "127.0.0.1") text += " (on this computer: ws://127.0.0.1:" + std::to_string(server_->port()) + "/)";
  return text + ".";
}

void GameLink::attach(json& request, const link::MessagePtr& state, bool with_events) {
  request["connected"] = server_->connected();
  std::vector<link::MessagePtr> events;
  if (with_events) events = server_->drain_events();
  const size_t limit = ipc::kMaxJsonBytes - kRequestRoom;
  // Quick estimate from the texts the game sent (the parsed values print about as long): almost always tiny.
  size_t estimate = state ? state->text.size() : 2;
  for (const auto& e : events) estimate += e->text.size() + 1;
  if (estimate + estimate / 4 < limit) {
    request["game"] = state ? state->value : json::object();
    if (with_events) {
      json list = json::array();
      for (const auto& e : events) list.push_back(e->value);
      request["game_events"] = std::move(list);
    }
    return;
  }
  // Too big for one request: the state first (its biggest fields go, "type" and "frame" stay), then the newest
  // events that still fit.
  json game = state ? state->value : json::object();
  size_t game_size = game.dump().size();
  if (game_size > limit && game.is_object()) {
    std::vector<std::pair<size_t, std::string>> fields;
    for (auto it = game.begin(); it != game.end(); ++it) {
      if (it.key() == "type" || it.key() == "frame") continue;
      fields.emplace_back(it.key().size() + it.value().dump().size() + 4, it.key());
    }
    std::sort(fields.begin(), fields.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::string> dropped;
    for (const auto& [size, key] : fields) {
      if (game_size <= limit) break;
      game.erase(key);
      game_size -= std::min(game_size, size);
      dropped.push_back(key);
    }
    game_size = game.dump().size();
    warn_once(warned_state_, "The game's state is too big to pass to your scripts (at most 1 MiB in all), so Vyrn "
                             "leaves out its biggest fields: " + quoted_list(dropped) +
                                 ". Send fewer or smaller fields from the game.");
  }
  request["game"] = std::move(game);
  if (!with_events) return;
  const size_t room = limit > game_size ? limit - game_size : 0;
  size_t keep_from = events.size(), used = 2;
  while (keep_from > 0) {
    const size_t size = events[keep_from - 1]->value.dump().size() + 1;
    if (used + size > room) break;
    used += size;
    --keep_from;
  }
  if (keep_from > 0) {
    warn_once(warned_events_, "The game sent more events in one tick than fit next to its state (at most 1 MiB in "
                              "all), so Vyrn dropped the oldest " + std::to_string(keep_from) +
                                  ". Send fewer or smaller events.");
  }
  json list = json::array();
  for (size_t i = keep_from; i < events.size(); ++i) list.push_back(events[i]->value);
  request["game_events"] = std::move(list);
}

void GameLink::forward_sends(const json& reply) {
  const auto it = reply.find("send");
  if (it == reply.end() || !it->is_array()) return;
  for (const auto& message : *it) server_->send_custom(message);
}

void GameLink::send_input(const link::MessagePtr& state, const json& act_reply) {
  if (!state) return;   // nothing to answer yet: the game hasn't sent a state
  const auto client = server_->client();
  if (!client || client->connection != state->connection) return;   // that game left; a new one gets the next tick
  const auto it = act_reply.find("game_input");
  const json input = it != act_reply.end() && it->is_object() ? *it : json::object();
  server_->send_input(state->frame, names(input, "hold"), names(input, "tap"));
}

void GameLink::release(const link::MessagePtr& state) {
  if (!state) return;
  const auto client = server_->client();
  if (client && client->connection == state->connection) server_->send_input(state->frame, {}, {});
}

void GameLink::watch() {
  std::unique_lock<std::mutex> lock(mu_);
  while (!quit_) {
    cv_.wait_for(lock, std::chrono::duration<double>(kWatchEvery));
    if (quit_) break;
    lock.unlock();
    for (const auto& n : server_->take_notices()) ctx_.app_log("Vyrn Link: " + n.text);
    emit_status(false);
    lock.lock();
  }
}

void GameLink::emit_status(bool force) {
  const std::optional<link::ClientInfo> client = server_->client();
  const uint64_t connection = client ? client->connection : 0;
  if (!force && connection == shown_connection_) return;
  shown_connection_ = connection;
  ojson e = ojson::object();
  e["type"] = "link";
  e["url"] = url_;
  e["address"] = address_;
  e["port"] = server_->port();
  e["code"] = code_;
  e["lockstep"] = settings_.lockstep;
  e["frames_per_step"] = settings_.frames_per_step;
  e["connected"] = client.has_value();
  if (client) {
    e["game"] = client->game;
    e["client"] = client->client;
    e["from"] = client->address;
  }
  ctx_.emit(e);
}

void GameLink::warn_once(std::atomic<bool>& said, const std::string& message) {
  if (!said.exchange(true)) ctx_.emit(warning_event("", 0, message));
}

}  // namespace vyrn::run
