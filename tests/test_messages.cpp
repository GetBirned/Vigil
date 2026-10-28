#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include "vigil/messages.hpp"

namespace vigil {
namespace {

DetectionReport sample_report() {
  DetectionReport r;
  r.seq = 0xDEADBEEF;
  r.sensor_id = 7;
  r.timestamp_ns = 1'234'567'890'123ULL;
  r.detections = {{1.5, -2.25}, {10'000.0, 20'000.0}, {-0.0, 3.0e-12}};
  return r;
}

TEST(Messages, RoundTripPreservesEveryField) {
  const DetectionReport in = sample_report();
  const auto bytes = encode(in);
  ASSERT_EQ(bytes.size(), wire::kHeaderSize + 3 * wire::kDetectionSize);

  const auto out = decode(bytes);
  ASSERT_TRUE(out.has_value()) << to_string(out.error());
  EXPECT_EQ(out->seq, in.seq);
  EXPECT_EQ(out->sensor_id, in.sensor_id);
  EXPECT_EQ(out->timestamp_ns, in.timestamp_ns);
  ASSERT_EQ(out->detections.size(), in.detections.size());
  for (std::size_t i = 0; i < in.detections.size(); ++i) {
    EXPECT_EQ(out->detections[i].x_m, in.detections[i].x_m);
    EXPECT_EQ(out->detections[i].y_m, in.detections[i].y_m);
  }
}

TEST(Messages, HeaderLayoutMatchesIcd) {
  const auto bytes = encode(sample_report());
  // Magic "VGL1" as a little-endian u32.
  EXPECT_EQ(bytes[0], std::byte{0x31});
  EXPECT_EQ(bytes[1], std::byte{0x4C});
  EXPECT_EQ(bytes[2], std::byte{0x47});
  EXPECT_EQ(bytes[3], std::byte{0x56});
  // Version 1, message type 1.
  EXPECT_EQ(bytes[4], std::byte{0x01});
  EXPECT_EQ(bytes[5], std::byte{0x00});
  EXPECT_EQ(bytes[6], std::byte{0x01});
  // Sequence 0xDEADBEEF at offset 8, least significant byte first.
  EXPECT_EQ(bytes[8], std::byte{0xEF});
  EXPECT_EQ(bytes[11], std::byte{0xDE});
  // Detection count at offset 14.
  EXPECT_EQ(bytes[14], std::byte{0x03});
  EXPECT_EQ(bytes[15], std::byte{0x00});
}

TEST(Messages, EmptyReportIsJustAHeader) {
  DetectionReport r;
  r.seq = 1;
  const auto bytes = encode(r);
  EXPECT_EQ(bytes.size(), wire::kHeaderSize);
  const auto out = decode(bytes);
  ASSERT_TRUE(out.has_value());
  EXPECT_TRUE(out->detections.empty());
}

TEST(Messages, MaxSizeReportFitsOneEthernetFrame) {
  DetectionReport r;
  r.detections.resize(wire::kMaxDetections);
  EXPECT_LE(encode(r).size(), wire::kMaxDatagramSize);

  r.detections.emplace_back();
  EXPECT_THROW((void)encode(r), std::length_error);
}

TEST(Messages, RejectsShortDatagram) {
  const std::vector<std::byte> bytes(wire::kHeaderSize - 1);
  EXPECT_EQ(decode(bytes).error(), DecodeError::kTooShort);
}

TEST(Messages, RejectsBadMagic) {
  auto bytes = encode(sample_report());
  bytes[0] = std::byte{0x00};
  EXPECT_EQ(decode(bytes).error(), DecodeError::kBadMagic);
}

TEST(Messages, RejectsUnknownVersion) {
  auto bytes = encode(sample_report());
  bytes[4] = std::byte{0x02};
  EXPECT_EQ(decode(bytes).error(), DecodeError::kUnsupportedVersion);
}

TEST(Messages, RejectsUnknownMessageType) {
  auto bytes = encode(sample_report());
  bytes[6] = std::byte{0x09};
  EXPECT_EQ(decode(bytes).error(), DecodeError::kUnknownMessageType);
}

TEST(Messages, RejectsTruncatedBody) {
  auto bytes = encode(sample_report());
  bytes.pop_back();
  EXPECT_EQ(decode(bytes).error(), DecodeError::kLengthMismatch);
}

TEST(Messages, RejectsCountLargerThanBody) {
  auto bytes = encode(sample_report());
  bytes[14] = std::byte{0x04};
  EXPECT_EQ(decode(bytes).error(), DecodeError::kLengthMismatch);
}

TEST(Messages, RejectsNonFiniteCoordinates) {
  DetectionReport r = sample_report();
  r.detections[1].y_m = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(decode(encode(r)).error(), DecodeError::kNonFiniteValue);

  r.detections[1].y_m = std::numeric_limits<double>::infinity();
  EXPECT_EQ(decode(encode(r)).error(), DecodeError::kNonFiniteValue);
}

}  // namespace
}  // namespace vigil
