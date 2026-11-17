// net.h on POSIX: BSD sockets, poll(2), a pipe as the waker.
#include "platform/net.h"

#include "platform/posix_util.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

namespace vyrn::platform::net {

namespace {

int fd_of(Socket s) { return static_cast<int>(s); }

void set_flags(int fd) {
  ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  ::fcntl(fd, F_SETFD, ::fcntl(fd, F_GETFD, 0) | FD_CLOEXEC);
}

bool to_address(const std::string& ipv4, int port, sockaddr_in& addr) {
  addr = sockaddr_in{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  return ::inet_pton(AF_INET, ipv4.c_str(), &addr.sin_addr) == 1;
}

}  // namespace

Socket listen_tcp(const std::string& ipv4, int port, int backlog, int& bound_port, int& err) {
  sockaddr_in addr{};
  if (!to_address(ipv4, port, addr)) {
    err = EINVAL;
    return kNoSocket;
  }
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    err = errno;
    return kNoSocket;
  }
  set_flags(fd);
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);   // restart quickly after old connections
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(fd, backlog) != 0) {
    err = errno;
    ::close(fd);
    return kNoSocket;
  }
  sockaddr_in bound{};
  socklen_t len = sizeof bound;
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len);
  bound_port = ntohs(bound.sin_port);
  err = 0;
  return fd;
}

Socket accept_tcp(Socket listener, uint32_t& peer, int& err) {
  for (;;) {
    sockaddr_in addr{};
    socklen_t len = sizeof addr;
    const int fd = ::accept(fd_of(listener), reinterpret_cast<sockaddr*>(&addr), &len);
    if (fd < 0) {
      if (errno == EINTR) continue;
      err = errno == EAGAIN ? EWOULDBLOCK : errno;
      return kNoSocket;
    }
    set_flags(fd);
    posix::no_sigpipe(fd);
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);   // small messages, sent right away
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    peer = ntohl(addr.sin_addr.s_addr);
    err = 0;
    return fd;
  }
}

Socket connect_tcp(const std::string& ipv4, int port, int& err) {
  sockaddr_in addr{};
  if (!to_address(ipv4, port, addr)) {
    err = EINVAL;
    return kNoSocket;
  }
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    err = errno;
    return kNoSocket;
  }
  ::fcntl(fd, F_SETFD, ::fcntl(fd, F_GETFD, 0) | FD_CLOEXEC);
  posix::no_sigpipe(fd);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    err = errno;
    ::close(fd);
    return kNoSocket;
  }
  err = 0;
  return fd;
}

long long send_some(Socket s, const void* data, size_t size, int& err) {
  for (;;) {
    const ssize_t n = posix::send_nosignal(fd_of(s), data, size);
    if (n >= 0) return n;
    if (errno == EINTR) continue;
    err = errno == EAGAIN ? EWOULDBLOCK : errno;
    return -1;
  }
}

long long recv_some(Socket s, void* data, size_t size, int& err) {
  for (;;) {
    const ssize_t n = ::recv(fd_of(s), data, size, 0);
    if (n >= 0) return n;
    if (errno == EINTR) continue;
    err = errno == EAGAIN ? EWOULDBLOCK : errno;
    return -1;
  }
}

bool set_blocking(Socket s, bool blocking) {
  const int flags = ::fcntl(fd_of(s), F_GETFL, 0);
  if (flags < 0) return false;
  return ::fcntl(fd_of(s), F_SETFL, blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK) == 0;
}

void shutdown_send(Socket s) { ::shutdown(fd_of(s), SHUT_WR); }

void close_socket(Socket s) {
  if (s != kNoSocket) ::close(fd_of(s));
}

int poll(std::vector<PollItem>& items, int timeout_ms, int& err) {
  std::vector<pollfd> fds(items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    fds[i].fd = fd_of(items[i].socket);
    fds[i].events = static_cast<short>((items[i].want_read ? POLLIN : 0) | (items[i].want_write ? POLLOUT : 0));
  }
  const int n = ::poll(fds.data(), fds.size(), timeout_ms);
  if (n < 0) {
    err = errno;
    return -1;
  }
  for (size_t i = 0; i < items.size(); ++i) {
    items[i].readable = (fds[i].revents & POLLIN) != 0;
    items[i].writable = (fds[i].revents & POLLOUT) != 0;
    items[i].failed = (fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
  }
  return n;
}

bool Waker::open(int& err) {
  int fds[2];
  if (::pipe(fds) != 0) {
    err = errno;
    return false;
  }
  set_flags(fds[0]);
  set_flags(fds[1]);
  read_ = fds[0];
  write_ = fds[1];
  return true;
}

void Waker::close() {
  if (read_ != kNoSocket) ::close(fd_of(read_));
  if (write_ != kNoSocket) ::close(fd_of(write_));
  read_ = write_ = kNoSocket;
}

void Waker::notify() {
  const char b = 1;
  if (write_ != kNoSocket && ::write(fd_of(write_), &b, 1) < 0) {
    // a full pipe: the loop wakes up anyway
  }
}

void Waker::drain() {
  char buf[256];
  while (read_ != kNoSocket && ::read(fd_of(read_), buf, sizeof buf) > 0) {
  }
}

bool parse_ipv4(const std::string& text, uint32_t& address) {
  in_addr addr{};
  if (::inet_pton(AF_INET, text.c_str(), &addr) != 1) return false;
  address = ntohl(addr.s_addr);
  return true;
}

std::string ipv4_text(uint32_t address) {
  in_addr addr{};
  addr.s_addr = htonl(address);
  char text[INET_ADDRSTRLEN] = "?";
  ::inet_ntop(AF_INET, &addr, text, sizeof text);
  return text;
}

std::vector<Ipv4Interface> ipv4_interfaces() {
  ifaddrs* list = nullptr;
  if (::getifaddrs(&list) != 0) return {};
  std::vector<Ipv4Interface> found;
  for (ifaddrs* i = list; i; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) {
      continue;
    }
    const in_addr addr = reinterpret_cast<const sockaddr_in*>(i->ifa_addr)->sin_addr;
    found.push_back({i->ifa_name ? i->ifa_name : "", ntohl(addr.s_addr)});
  }
  ::freeifaddrs(list);
  return found;
}

}  // namespace vyrn::platform::net
