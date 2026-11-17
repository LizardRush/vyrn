// The app's side of the Windows worker transport (platform/worker_listener.h), exercised on every platform with
// real loopback connections: a fake worker connects "main" and "ctl" with the token, in any order; strangers, wrong
// tokens and extra connections are closed; it gives up on time, or when the worker is gone.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cerrno>
#include <string>
#include <thread>

#include "platform/net.h"
#include "platform/transport.h"
#include "platform/worker_listener.h"

namespace {

namespace net = vyrn::platform::net;
using vyrn::platform::ConnectionRole;
using vyrn::platform::WorkerListener;

const std::string kToken = "00112233445566778899aabbccddeeff";

// One blocking client connection; closed when it goes.
class Client {
 public:
  explicit Client(int port) {
    int err = 0;
    s_ = net::connect_tcp("127.0.0.1", port, err);
    err_ = err;
  }
  ~Client() { net::close_socket(s_); }
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  bool connected() const { return s_ != net::kNoSocket; }
  int error() const { return err_; }
  void send(const std::string& bytes) {
    int err = 0;
    size_t done = 0;
    while (done < bytes.size()) {
      const long long n = net::send_some(s_, bytes.data() + done, bytes.size() - done, err);
      if (n <= 0) return;
      done += static_cast<size_t>(n);
    }
  }
  // True when the other side closed this connection within `ms` (EOF or a reset).
  bool closed_by_peer(int ms) {
    std::vector<net::PollItem> items(1);
    items[0].socket = s_;
    items[0].want_read = true;
    int err = 0;
    if (net::poll(items, ms, err) <= 0) return false;
    char b;
    return net::recv_some(s_, &b, 1, err) <= 0;
  }

 private:
  net::Socket s_ = net::kNoSocket;
  int err_ = 0;
};

// Everything that arrives on `s` within `ms`, after `early`.
std::string read_all(net::Socket s, std::string early, size_t want, int ms) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (early.size() < want && std::chrono::steady_clock::now() < end) {
    std::vector<net::PollItem> items(1);
    items[0].socket = s;
    items[0].want_read = true;
    int err = 0;
    if (net::poll(items, 50, err) <= 0) continue;
    char buf[256];
    const long long n = net::recv_some(s, buf, sizeof buf, err);
    if (n <= 0) break;
    early.append(buf, static_cast<size_t>(n));
  }
  return early;
}

TEST(WorkerListener, TakesMainAndCtlInEitherOrderAndKeepsTheFirstFrames) {
  WorkerListener listener;
  int err = 0;
  ASSERT_TRUE(listener.open(kToken, err)) << err;
  ASSERT_GT(listener.port(), 0);
  EXPECT_EQ(listener.token(), kToken);
  std::atomic<bool> done{false};
  std::thread worker([&] {
    Client ctl(listener.port());
    ctl.send(vyrn::platform::connection_line(kToken, ConnectionRole::Ctl));
    Client main(listener.port());
    main.send(vyrn::platform::connection_line(kToken, ConnectionRole::Main) + "VYRN-hello-frame");
    while (!done) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  });
  const bool accepted = listener.accept(5000, [] { return false; });
  EXPECT_TRUE(accepted);
  EXPECT_TRUE(listener.complete());
  const net::Socket main = listener.take_main();
  const net::Socket ctl = listener.take_ctl();
  EXPECT_NE(main, net::kNoSocket);
  EXPECT_NE(ctl, net::kNoSocket);
  if (main != net::kNoSocket) {
    EXPECT_EQ(read_all(main, listener.take_early(), 16, 2000), "VYRN-hello-frame");
  }
  done = true;
  worker.join();
  net::close_socket(main);
  net::close_socket(ctl);
}

TEST(WorkerListener, ClosesStrangersAndExtraConnectionsThenRefusesNewOnes) {
  WorkerListener listener;
  int err = 0;
  ASSERT_TRUE(listener.open(kToken, err));
  std::atomic<bool> done{false};
  std::atomic<int> closed{0};
  std::thread worker([&] {
    Client stranger(listener.port());
    stranger.send("ffffffffffffffffffffffffffffffff main\n");
    if (stranger.closed_by_peer(3000)) ++closed;
    Client garbage(listener.port());
    garbage.send("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    if (garbage.closed_by_peer(3000)) ++closed;
    Client main(listener.port());
    main.send(kToken + " main\n");
    Client second_main(listener.port());
    second_main.send(kToken + " main\n");
    if (second_main.closed_by_peer(3000)) ++closed;
    Client ctl(listener.port());
    ctl.send(kToken + " ct");   // in two pieces
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ctl.send("l\n");
    while (!done) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  });
  EXPECT_TRUE(listener.accept(10000, nullptr));
  done = true;
  worker.join();
  EXPECT_EQ(closed.load(), 3) << "the stranger, the garbage and the second main connection are closed";
  net::close_socket(listener.take_main());
  net::close_socket(listener.take_ctl());
  // Both connections are there: the listener is closed, so a third connection is refused.
  Client late(listener.port());
  EXPECT_FALSE(late.connected());
}

TEST(WorkerListener, GivesUpAfterItsTime) {
  WorkerListener listener;
  int err = 0;
  ASSERT_TRUE(listener.open(kToken, err));
  Client only_main(listener.port());
  only_main.send(kToken + " main\n");   // the ctl connection never comes
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(listener.accept(300, nullptr));
  const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  EXPECT_GE(waited, 0.25);
  EXPECT_LT(waited, 3.0);
  EXPECT_FALSE(listener.complete());
}

TEST(WorkerListener, GivesUpWhenTheWorkerIsGone) {
  WorkerListener listener;
  int err = 0;
  ASSERT_TRUE(listener.open(kToken, err));
  const auto start = std::chrono::steady_clock::now();
  std::atomic<int> asked{0};
  EXPECT_FALSE(listener.accept(-1, [&] { return ++asked > 3; }));   // "exited" on the fourth look
  const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  EXPECT_LT(waited, 3.0);
  EXPECT_GE(asked.load(), 4);
}

}  // namespace
