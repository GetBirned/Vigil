#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <span>

#include "vigil/messages.hpp"
#include "vigil/udp.hpp"

namespace vigil {
namespace {

using std::chrono::milliseconds;

TEST(Udp, LoopbackDeliversAnEncodedReport) {
  UdpSocket receiver;
  receiver.bind("127.0.0.1", 0);
  const std::uint16_t port = receiver.local_port();
  ASSERT_NE(port, 0);

  DetectionReport sent;
  sent.seq = 42;
  sent.detections = {{100.0, 200.0}};
  UdpSocket sender;
  sender.send_to("127.0.0.1", port, encode(sent));

  std::array<std::byte, 2048> buffer{};
  const auto size = receiver.receive(buffer, milliseconds(1000));
  ASSERT_TRUE(size.has_value());

  const auto got = decode(std::span(buffer).first(*size));
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->seq, 42U);
  ASSERT_EQ(got->detections.size(), 1U);
  EXPECT_EQ(got->detections[0].x_m, 100.0);
}

TEST(Udp, ReceiveTimesOutWhenNothingArrives) {
  UdpSocket receiver;
  receiver.bind("127.0.0.1", 0);
  std::array<std::byte, 64> buffer{};
  EXPECT_FALSE(receiver.receive(buffer, milliseconds(20)).has_value());
}

TEST(Udp, RejectsMalformedAddress) {
  UdpSocket socket;
  EXPECT_THROW(socket.bind("not-an-ip", 0), std::invalid_argument);
}

}  // namespace
}  // namespace vigil
