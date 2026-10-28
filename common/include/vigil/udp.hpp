#pragma once

// Minimal RAII wrapper over a POSIX IPv4 UDP socket.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace vigil {

class UdpSocket {
 public:
  // Opens an unbound socket. Throws std::system_error on failure.
  UdpSocket();
  ~UdpSocket();

  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;
  UdpSocket(UdpSocket&& other) noexcept;
  UdpSocket& operator=(UdpSocket&& other) noexcept;

  // Binds to the given IPv4 address and port. Port 0 picks an ephemeral port.
  void bind(const std::string& address, std::uint16_t port);

  // The port this socket is bound to (useful after binding to port 0).
  [[nodiscard]] std::uint16_t local_port() const;

  // Sends one datagram. Throws std::system_error on failure.
  void send_to(const std::string& address, std::uint16_t port, std::span<const std::byte> data);

  // Waits up to `timeout` for a datagram. Returns the number of bytes written into
  // `buffer`, or std::nullopt if the timeout expired or a signal interrupted the wait.
  std::optional<std::size_t> receive(std::span<std::byte> buffer,
                                     std::chrono::milliseconds timeout);

 private:
  int fd_ = -1;
};

}  // namespace vigil
