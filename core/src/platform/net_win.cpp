// net.h on Windows: Winsock 2 (WSAStartup once, closesocket, WSAPoll, ioctlsocket for non-blocking mode), a
// connected loopback socket pair as the waker (WSAPoll only takes sockets), GetAdaptersAddresses for the interfaces.
#include "platform/net.h"

#include "platform/win_util.h"

#include <iphlpapi.h>
#include <ws2tcpip.h>

#include <cerrno>
#include <climits>
#include <vector>

namespace vyrn::platform::net {

namespace {

SOCKET sock_of(Socket s) { return static_cast<SOCKET>(s); }

int last_error() { return win::errno_from_wsa(WSAGetLastError()); }

bool to_address(const std::string& ipv4, int port, sockaddr_in& addr) {
  addr = sockaddr_in{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<u_short>(port));
  return inet_pton(AF_INET, ipv4.c_str(), &addr.sin_addr) == 1;
}

// A socket that child processes don't inherit.
SOCKET new_socket() { return WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT); }

}  // namespace

Socket listen_tcp(const std::string& ipv4, int port, int backlog, int& bound_port, int& err) {
  sockaddr_in addr{};
  if (!win::winsock_ready()) {
    err = ENETDOWN;
    return kNoSocket;
  }
  if (!to_address(ipv4, port, addr)) {
    err = EINVAL;
    return kNoSocket;
  }
  const SOCKET s = new_socket();
  if (s == INVALID_SOCKET) {
    err = last_error();
    return kNoSocket;
  }
  // No SO_REUSEADDR (on Windows it would let another program take over the port), and SO_EXCLUSIVEADDRUSE so that
  // no other program can bind the same address while Vyrn listens.
  const BOOL exclusive = TRUE;
  setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
  if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(s, backlog) != 0) {
    err = last_error();
    closesocket(s);
    return kNoSocket;
  }
  set_blocking(static_cast<Socket>(s), false);
  sockaddr_in bound{};
  int len = sizeof bound;
  getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len);
  bound_port = ntohs(bound.sin_port);
  err = 0;
  return static_cast<Socket>(s);
}

Socket accept_tcp(Socket listener, uint32_t& peer, int& err) {
  sockaddr_in addr{};
  int len = sizeof addr;
  const SOCKET s = accept(sock_of(listener), reinterpret_cast<sockaddr*>(&addr), &len);
  if (s == INVALID_SOCKET) {
    err = last_error();
    return kNoSocket;
  }
  SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
  set_blocking(static_cast<Socket>(s), false);
  const BOOL one = TRUE;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
  setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&one), sizeof one);
  peer = ntohl(addr.sin_addr.s_addr);
  err = 0;
  return static_cast<Socket>(s);
}

Socket connect_tcp(const std::string& ipv4, int port, int& err) {
  sockaddr_in addr{};
  if (!win::winsock_ready()) {
    err = ENETDOWN;
    return kNoSocket;
  }
  if (!to_address(ipv4, port, addr)) {
    err = EINVAL;
    return kNoSocket;
  }
  const SOCKET s = new_socket();
  if (s == INVALID_SOCKET) {
    err = last_error();
    return kNoSocket;
  }
  if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    err = last_error();
    closesocket(s);
    return kNoSocket;
  }
  err = 0;
  return static_cast<Socket>(s);
}

long long send_some(Socket s, const void* data, size_t size, int& err) {
  const int n = send(sock_of(s), static_cast<const char*>(data), static_cast<int>(size < INT_MAX ? size : INT_MAX), 0);
  if (n == SOCKET_ERROR) {
    err = last_error();
    return -1;
  }
  return n;
}

long long recv_some(Socket s, void* data, size_t size, int& err) {
  const int n = recv(sock_of(s), static_cast<char*>(data), static_cast<int>(size < INT_MAX ? size : INT_MAX), 0);
  if (n == SOCKET_ERROR) {
    err = last_error();
    return -1;
  }
  return n;
}

bool set_blocking(Socket s, bool blocking) {
  u_long mode = blocking ? 0 : 1;
  return ioctlsocket(sock_of(s), FIONBIO, &mode) == 0;
}

void shutdown_send(Socket s) { shutdown(sock_of(s), SD_SEND); }

void close_socket(Socket s) {
  if (s != kNoSocket) closesocket(sock_of(s));
}

int poll(std::vector<PollItem>& items, int timeout_ms, int& err) {
  std::vector<WSAPOLLFD> fds(items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    fds[i].fd = sock_of(items[i].socket);
    fds[i].events = static_cast<SHORT>((items[i].want_read ? POLLRDNORM : 0) | (items[i].want_write ? POLLWRNORM : 0));
  }
  const int n = fds.empty() ? (Sleep(static_cast<DWORD>(timeout_ms < 0 ? 0 : timeout_ms)), 0)
                            : WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeout_ms);
  if (n == SOCKET_ERROR) {
    err = last_error();
    return -1;
  }
  for (size_t i = 0; i < items.size(); ++i) {
    items[i].readable = (fds[i].revents & POLLRDNORM) != 0;
    items[i].writable = (fds[i].revents & POLLWRNORM) != 0;
    items[i].failed = (fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
  }
  return n;
}

// A loopback connection: a listener on 127.0.0.1:0, a client, the accepted end.
bool Waker::open(int& err) {
  int port = 0;
  const Socket listener = listen_tcp("127.0.0.1", 0, 1, port, err);
  if (listener == kNoSocket) return false;
  set_blocking(listener, true);
  const Socket writer = connect_tcp("127.0.0.1", port, err);
  if (writer == kNoSocket) {
    close_socket(listener);
    return false;
  }
  const SOCKET reader = accept(sock_of(listener), nullptr, nullptr);
  if (reader == INVALID_SOCKET) err = last_error();
  close_socket(listener);
  if (reader == INVALID_SOCKET) {
    close_socket(writer);
    return false;
  }
  SetHandleInformation(reinterpret_cast<HANDLE>(reader), HANDLE_FLAG_INHERIT, 0);
  read_ = static_cast<Socket>(reader);
  write_ = writer;
  set_blocking(read_, false);
  set_blocking(write_, false);
  return true;
}

void Waker::close() {
  close_socket(read_);
  close_socket(write_);
  read_ = write_ = kNoSocket;
}

void Waker::notify() {
  const char b = 1;
  if (write_ != kNoSocket) send(sock_of(write_), &b, 1, 0);   // a full buffer: the loop wakes up anyway
}

void Waker::drain() {
  char buf[256];
  while (read_ != kNoSocket && recv(sock_of(read_), buf, sizeof buf, 0) > 0) {
  }
}

bool parse_ipv4(const std::string& text, uint32_t& address) {
  in_addr addr{};
  if (inet_pton(AF_INET, text.c_str(), &addr) != 1) return false;
  address = ntohl(addr.s_addr);
  return true;
}

std::string ipv4_text(uint32_t address) {
  in_addr addr{};
  addr.s_addr = htonl(address);
  char text[INET_ADDRSTRLEN] = "?";
  inet_ntop(AF_INET, &addr, text, sizeof text);
  return text;
}

std::vector<Ipv4Interface> ipv4_interfaces() {
  ULONG size = 16 * 1024;
  std::vector<unsigned char> buf;
  ULONG rc = ERROR_BUFFER_OVERFLOW;
  for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; ++attempt) {
    buf.resize(size);
    rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                              nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
  }
  std::vector<Ipv4Interface> found;
  if (rc != NO_ERROR) return found;
  for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
      const auto* in = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
      found.push_back({win::narrow(a->FriendlyName), ntohl(in->sin_addr.s_addr)});
    }
  }
  return found;
}

}  // namespace vyrn::platform::net
