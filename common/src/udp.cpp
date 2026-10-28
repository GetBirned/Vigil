#include "vigil/udp.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace vigil {
namespace {

[[noreturn]] void throw_errno(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}

sockaddr_in make_address(const std::string& address, std::uint16_t port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
    throw std::invalid_argument("not an IPv4 address: " + address);
  }
  return addr;
}

}  // namespace

UdpSocket::UdpSocket() : fd_(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) {
  if (fd_ < 0) {
    throw_errno("socket");
  }
}

UdpSocket::~UdpSocket() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = std::exchange(other.fd_, -1);
  }
  return *this;
}

void UdpSocket::bind(const std::string& address, std::uint16_t port) {
  const int reuse = 1;
  ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  const sockaddr_in addr = make_address(address, port);
  if (::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    throw_errno("bind");
  }
}

std::uint16_t UdpSocket::local_port() const {
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    throw_errno("getsockname");
  }
  return ntohs(addr.sin_port);
}

void UdpSocket::send_to(const std::string& address, std::uint16_t port,
                        std::span<const std::byte> data) {
  const sockaddr_in addr = make_address(address, port);
  const ssize_t sent = ::sendto(fd_, data.data(), data.size(), 0,
                                reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
  if (sent < 0) {
    throw_errno("sendto");
  }
}

std::optional<std::size_t> UdpSocket::receive(std::span<std::byte> buffer,
                                              std::chrono::milliseconds timeout) {
  pollfd pfd{.fd = fd_, .events = POLLIN, .revents = 0};
  const int ready = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
  if (ready == 0 || (ready < 0 && errno == EINTR)) {
    return std::nullopt;
  }
  if (ready < 0) {
    throw_errno("poll");
  }

  const ssize_t received = ::recv(fd_, buffer.data(), buffer.size(), 0);
  if (received < 0) {
    if (errno == EINTR) {
      return std::nullopt;
    }
    throw_errno("recv");
  }
  return static_cast<std::size_t>(received);
}

}  // namespace vigil
