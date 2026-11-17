// A tiny socket shim for Vyrn Link (docs/dev/platforms.md section 3): BSD sockets on POSIX, Winsock on Windows
// (WSAStartup, closesocket, WSAPoll, ioctlsocket). IPv4 TCP only. Error codes are errno values (EWOULDBLOCK: try
// again later; Winsock's errors are mapped), so messages can use std::strerror().
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vyrn::platform::net {

using Socket = std::intptr_t;   // a descriptor (POSIX) or a SOCKET (Windows)
constexpr Socket kNoSocket = -1;

// A listening socket on ipv4:port (port 0: any free one): non-blocking, close-on-exec, SO_REUSEADDR (POSIX only:
// on Windows it would let another program take the port). bound_port: the port it got.
Socket listen_tcp(const std::string& ipv4, int port, int backlog, int& bound_port, int& err);
// The next waiting connection: non-blocking, close-on-exec, TCP_NODELAY and SO_KEEPALIVE. kNoSocket when none is
// waiting (err = EWOULDBLOCK) or on an error. peer: the other side's IPv4 address (host byte order).
Socket accept_tcp(Socket listener, uint32_t& peer, int& err);
// A blocking connection to ipv4:port (tests and tools). kNoSocket + err when it fails.
Socket connect_tcp(const std::string& ipv4, int port, int& err);
long long send_some(Socket s, const void* data, size_t size, int& err);   // -1 + err; never raises SIGPIPE
long long recv_some(Socket s, void* data, size_t size, int& err);         // 0: the other side closed; -1 + err
bool set_blocking(Socket s, bool blocking);
void shutdown_send(Socket s);
void close_socket(Socket s);

struct PollItem {
  Socket socket = kNoSocket;
  bool want_read = false, want_write = false;
  bool readable = false, writable = false, failed = false;   // failed: hang-up or error (a read tells which)
};
// poll / WSAPoll. Returns how many items are ready, 0 after timeout_ms (-1: no limit), -1 + err.
int poll(std::vector<PollItem>& items, int timeout_ms, int& err);

// Wakes a poll loop from another thread: notify() makes socket() readable until drain().
class Waker {
 public:
  Waker() = default;
  ~Waker() { close(); }
  Waker(const Waker&) = delete;
  Waker& operator=(const Waker&) = delete;
  bool open(int& err);   // POSIX: a pipe; Windows: a connected loopback socket pair
  void close();
  void notify();         // any thread
  void drain();          // the polling thread
  Socket socket() const { return read_; }

 private:
  Socket read_ = kNoSocket, write_ = kNoSocket;
};

bool parse_ipv4(const std::string& text, uint32_t& address);   // dotted quad -> host byte order
std::string ipv4_text(uint32_t address);

struct Ipv4Interface {
  std::string name;   // the interface's name (Linux "eth0", Windows the adapter's friendly name)
  uint32_t address = 0;
};
std::vector<Ipv4Interface> ipv4_interfaces();   // interfaces that are up, without loopback

}  // namespace vyrn::platform::net
