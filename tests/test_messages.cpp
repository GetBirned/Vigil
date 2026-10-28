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

TrackReport sample_track_report() {
  TrackReport r;
  r.seq = 99;
  r.source_id = 2;
  r.timestamp_ns = 42'000'000'000ULL;
  r.processing_ns = 12'345;
  r.detections_in_scan = 9;
  r.tracks = {
      {.id = 1, .status = WireTrackStatus::kConfirmed, .hits = 17, .x_m = 9569.5, .y_m = -8602.25,
       .vx_mps = -45.0, .vy_mps = -85.5, .position_sigma_m = 14.5F, .velocity_sigma_mps = 6.25F},
      {.id = 6, .status = WireTrackStatus::kCoasting, .hits = 4, .x_m = 0.0, .y_m = 1.0,
       .vx_mps = 2.0, .vy_mps = 3.0, .position_sigma_m = 80.0F, .velocity_sigma_mps = 30.0F},
  };
  return r;
}

TEST(TrackReports, RoundTripPreservesEveryField) {
  const TrackReport in = sample_track_report();
  const auto bytes = encode(in);
  ASSERT_EQ(bytes.size(), wire::kHeaderSize + wire::kTrackSummarySize + 2 * wire::kTrackSize);
  EXPECT_EQ(bytes[6], std::byte{0x02});  // message type

  const auto out = decode_track_report(bytes);
  ASSERT_TRUE(out.has_value()) << to_string(out.error());
  EXPECT_EQ(out->seq, in.seq);
  EXPECT_EQ(out->source_id, in.source_id);
  EXPECT_EQ(out->timestamp_ns, in.timestamp_ns);
  EXPECT_EQ(out->processing_ns, in.processing_ns);
  EXPECT_EQ(out->detections_in_scan, in.detections_in_scan);
  ASSERT_EQ(out->tracks.size(), 2U);
  for (std::size_t i = 0; i < 2; ++i) {
    EXPECT_EQ(out->tracks[i].id, in.tracks[i].id);
    EXPECT_EQ(out->tracks[i].status, in.tracks[i].status);
    EXPECT_EQ(out->tracks[i].hits, in.tracks[i].hits);
    EXPECT_EQ(out->tracks[i].x_m, in.tracks[i].x_m);
    EXPECT_EQ(out->tracks[i].y_m, in.tracks[i].y_m);
    EXPECT_EQ(out->tracks[i].vx_mps, in.tracks[i].vx_mps);
    EXPECT_EQ(out->tracks[i].vy_mps, in.tracks[i].vy_mps);
    EXPECT_EQ(out->tracks[i].position_sigma_m, in.tracks[i].position_sigma_m);
    EXPECT_EQ(out->tracks[i].velocity_sigma_mps, in.tracks[i].velocity_sigma_mps);
  }
}

TEST(TrackReports, MessageTypesAreNotInterchangeable) {
  EXPECT_EQ(decode(encode(sample_track_report())).error(), DecodeError::kUnknownMessageType);
  EXPECT_EQ(decode_track_report(encode(sample_report())).error(),
            DecodeError::kUnknownMessageType);
}

TEST(TrackReports, RejectsTruncatedBody) {
  auto bytes = encode(sample_track_report());
  bytes.pop_back();
  EXPECT_EQ(decode_track_report(bytes).error(), DecodeError::kLengthMismatch);
}

TEST(TrackReports, RejectsUndefinedStatus) {
  auto bytes = encode(sample_track_report());
  bytes[wire::kHeaderSize + wire::kTrackSummarySize + 4] = std::byte{0x07};
  EXPECT_EQ(decode_track_report(bytes).error(), DecodeError::kBadTrackStatus);
}

TEST(TrackReports, RejectsNonFiniteState) {
  TrackReport r = sample_track_report();
  r.tracks[1].vx_mps = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(decode_track_report(encode(r)).error(), DecodeError::kNonFiniteValue);
}

TEST(TrackReports, EnforcesTrackLimit) {
  TrackReport r;
  r.tracks.resize(wire::kMaxTracks);
  EXPECT_LE(encode(r).size(), wire::kMaxTrackDatagramSize);
  r.tracks.emplace_back();
  EXPECT_THROW((void)encode(r), std::length_error);
}

}  // namespace
}  // namespace vigil
