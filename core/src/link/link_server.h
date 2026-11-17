// Vyrn Link: the WebSocket server a game (or a mod inside a game) connects to. The game streams its state and
// events; Vyrn sends the AI's buttons. Protocol v1 is docs/user/vyrn-link.md (frozen: mods are written against
// it); the design is docs/dev/link.md.
//
// One IO thread (poll) owns every socket: it answers the HTTP upgrade, checks the hello and the pairing code,
// keeps the newest state and the events, and writes what the API queues. Every public method is thread-safe; the
// runner calls them from its own thread. Start a server only for projects whose vyrn.json has "link"
// (link_settings() says whether it does).
//
//   LinkServer server(config);
//   server.start();                                   // throws LinkError (e.g. the port is in use)
//   // real time, once per AI tick:
//   MessagePtr state = server.latest_state();         // null until the game sends one
//   auto events = server.drain_events();
//   server.send_input(state->frame, {"jump"}, {});
//   // lockstep: answer each state, then wait for the next one
//   StepResult next = server.step(*state, {"jump"}, {}, 1.0);
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace vyrn::link {

using json = nlohmann::json;

inline constexpr int kProtocol = 1;               // Vyrn Link protocol version
inline constexpr int kDefaultPort = 7457;
inline constexpr size_t kMaxMessage = 1u << 20;   // 1 MiB per message, both ways

// The project's vyrn.json "link": {"port": 7457, "lockstep": false, "frames_per_step": 1}.
struct LinkSettings {
  int port = kDefaultPort;
  bool lockstep = false;
  int frames_per_step = 1;
};

// The link settings of a project, from its whole vyrn.json object. nullopt when it has no "link" (missing, null
// or false): then Vyrn never listens. "link": true means the defaults. A bad value falls back to its default and
// adds a plain-English line to *warnings.
std::optional<LinkSettings> link_settings(const json& vyrn_json, std::vector<std::string>* warnings = nullptr);

// What the welcome message tells the game.
struct WelcomeInfo {
  std::string project;                 // the project's name
  std::string vyrn;                    // Vyrn's version; empty = this build's version
  std::vector<std::string> buttons;    // the game buttons the project's choices use
  bool lockstep = false;
  int frames_per_step = 1;
  double tick_hz = 10;                 // AI decisions per second (real time)
};

struct LinkConfig {
  int port = kDefaultPort;               // 0 = any free port (tests); LinkServer::port() tells which
  std::string bind_address = "0.0.0.0";  // every network interface: the game usually runs on another computer
  std::string code;                      // the 6-digit pairing code; empty = make a new one
  WelcomeInfo welcome;
  // Limits.
  size_t max_message = kMaxMessage;      // bytes per message (fragments together)
  double hello_timeout_s = 10;           // from connecting to the hello
  double ping_after_s = 5;               // ping a game that has been quiet this long (0 = never)
  double idle_timeout_s = 20;            // drop a game that has been silent this long, pongs included (0 = never)
  int bad_codes_per_address = 5;         // wrong pairing codes allowed from one address per window...
  int bad_codes_total = 20;              // ...and from all addresses together
  double bad_code_window_s = 60;
  int max_pending = 8;                   // connections that haven't said hello yet
  size_t max_events = 1000;              // events kept for the runner; the oldest are dropped beyond this
  size_t max_unsent = 8u << 20;          // bytes waiting for a game that stopped reading: then it is dropped
  bool local_needs_code = false;         // tests only: ask for the code from 127.0.0.1 too
  // A correct hello while another game is connected: when that game has been quiet this long, Vyrn pings it, and
  // if it doesn't answer within replace_probe_s the new game takes its place (a game that crashed or lost the
  // network can come back at once). A game that answers keeps its place: the new one gets "busy".
  double replace_quiet_s = 1.0;
  double replace_probe_s = 1.5;
};

// A server-side problem: the port is in use, the pairing code isn't 6 digits...
struct LinkError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// The game that is connected.
struct ClientInfo {
  std::string game;         // hello "game", e.g. "Geometry Dash"
  std::string client;       // hello "client", e.g. "my-mod 0.1"
  std::string address;      // where it connected from, e.g. "192.168.1.20"
  uint64_t connection = 0;  // 1, 2, ...: a new number for every connection that was welcomed
};

// A state or an event from the game.
struct GameMessage {
  uint64_t seq = 0;          // arrival order of states and events together (1, 2, ...), never reused
  uint64_t connection = 0;   // ClientInfo::connection of the sender
  int64_t frame = 0;         // a state's "frame"
  double received_s = 0;     // when it arrived (steady clock, seconds)
  std::string text;          // the JSON exactly as the game sent it
  json value;                // the parsed object (with "type")
};
using MessagePtr = std::shared_ptr<const GameMessage>;

enum class Wait {
  Ok,            // a newer state arrived
  Timeout,       // nothing newer in time
  Disconnected,  // no game, or the game that sent the answered state went away
  Interrupted,   // interrupt_waits() was called
  Stopped,       // the server stopped
};

struct StepResult {
  Wait status = Wait::Timeout;
  MessagePtr state;   // the newest state (Ok); null otherwise
};

struct LinkStats {
  uint64_t connections = 0;      // games welcomed
  uint64_t refused = 0;          // connections refused (wrong code, busy, broken protocol, ...)
  uint64_t states = 0;
  uint64_t events = 0;
  uint64_t events_dropped = 0;   // events dropped because the runner didn't drain them
  uint64_t inputs = 0;           // input messages queued
  uint64_t sent = 0;             // other messages queued (stop, vyrn.send)
  uint64_t bytes_in = 0;
  uint64_t bytes_out = 0;
};

// One line for the app's console: "Geometry Dash connected from 192.168.1.20", "Wrong pairing code from ...".
struct LinkNotice {
  bool warning = false;
  std::string text;
};

class LinkServer {
 public:
  explicit LinkServer(LinkConfig config);
  ~LinkServer();   // stop()
  LinkServer(const LinkServer&) = delete;
  LinkServer& operator=(const LinkServer&) = delete;

  // Listens and starts the IO thread. Throws LinkError with a plain-English reason (port in use, bad code...).
  void start();
  // Tells a connected game to stop ({"type":"stop"}), closes every connection and the port, joins the IO thread.
  // Safe to call twice, and from any thread except a waiting one's own callback.
  void stop();
  bool running() const;
  int port() const;            // the port it listens on (after start())
  std::string code() const;    // the pairing code (made by the constructor when LinkConfig::code is empty)
  // Changes the welcome for games that connect from now on (a connected game keeps its welcome).
  void set_welcome(const WelcomeInfo& welcome);
  WelcomeInfo welcome() const;

  // ---- the game ----
  bool connected() const;                     // a game said hello and was welcomed, and is still there
  std::optional<ClientInfo> client() const;   // nullopt when no game is connected
  MessagePtr latest_state() const;            // the newest state of the connected game; null until it sends one
  std::vector<MessagePtr> drain_events();     // events since the last call, oldest first

  // ---- Vyrn -> game (false when no game is connected; nothing is queued then) ----
  // {"type":"input","frame":frame,"hold":[...],"tap":[...]}: hold = every button down until the next input.
  bool send_input(int64_t frame, const std::vector<std::string>& hold, const std::vector<std::string>& tap = {});
  bool send_stop();   // {"type":"stop"}: release everything and play normally (leave lockstep waiting)
  // A message from the user's scripts (vyrn.send): an object with a string "type" that isn't one Vyrn sends
  // itself (input, stop, welcome, error) or one the game sends (hello, state, event); at most 1 MiB.
  bool send_custom(const json& message);

  // ---- waiting (lockstep) ----
  // The newest state once one with seq > newer_than_seq is there (Ok), or why not.
  StepResult wait_state(uint64_t newer_than_seq, double timeout_s) const;
  // Answers `answered` with an input for its frame, then waits for the next state. Disconnected right away when
  // `answered` came from an earlier connection.
  StepResult step(const GameMessage& answered, const std::vector<std::string>& hold,
                  const std::vector<std::string>& tap, double timeout_s);
  // True once a game is connected; false on timeout, interrupt_waits() or stop().
  bool wait_connected(double timeout_s) const;
  // Makes every wait that is running now return Interrupted (e.g. the person pressed Stop).
  void interrupt_waits();

  // ---- for the app ----
  LinkStats stats() const;
  std::vector<LinkNotice> take_notices();   // the console lines since the last call

  // A new random pairing code: 6 digits, never starting with 0.
  static std::string make_code();

 private:
  struct State;
  std::unique_ptr<State> s_;
};

}  // namespace vyrn::link
