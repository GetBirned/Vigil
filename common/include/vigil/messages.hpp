#pragma once

// Wire formats: sensor -> tracker detection reports, and tracker -> display track reports.
// The authoritative description is docs/ICD.md; keep the two in sync.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace vigil {

// One position measurement in the sensor's local east/north frame.
struct Detection {
  double x_m = 0.0;  // east, metres
  double y_m = 0.0;  // north, metres
};

// Every detection from one sensor scan, stamped with the scan time.
struct DetectionReport {
  std::uint32_t seq = 0;           // increments by one per report, wraps at 2^32
  std::uint16_t sensor_id = 0;
  std::uint64_t timestamp_ns = 0;  // scan time, nanoseconds since scenario start
  std::vector<Detection> detections;
};

// Track status as it goes on the wire. Values are fixed by the ICD.
enum class WireTrackStatus : std::uint8_t {
  kTentative = 0,
  kConfirmed = 1,
  kCoasting = 2,
};

// One track's state estimate, as published to displays.
struct TrackState {
  std::uint32_t id = 0;
  WireTrackStatus status = WireTrackStatus::kTentative;
  std::uint16_t hits = 0;
  double x_m = 0.0;
  double y_m = 0.0;
  double vx_mps = 0.0;
  double vy_mps = 0.0;
  float position_sigma_m = 0.0F;  // RMS 1-sigma position uncertainty
  float velocity_sigma_mps = 0.0F;  // RMS 1-sigma velocity uncertainty
};

// The tracker's full picture after processing one scan.
struct TrackReport {
  std::uint32_t seq = 0;            // copied from the DetectionReport that produced it
  std::uint16_t source_id = 0;      // which tracker produced the picture
  std::uint64_t timestamp_ns = 0;   // scan time of that DetectionReport
  std::uint32_t processing_ns = 0;  // time the tracker spent on the scan
  std::uint16_t detections_in_scan = 0;
  std::vector<TrackState> tracks;
};

namespace wire {

inline constexpr std::uint32_t kMagic = 0x56474C31;  // "VGL1"
inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::uint16_t kMsgDetectionReport = 1;
inline constexpr std::uint16_t kMsgTrackReport = 2;

inline constexpr std::size_t kHeaderSize = 24;
inline constexpr std::size_t kDetectionSize = 16;
// Largest UDP payload that fits a 1500-byte Ethernet MTU without fragmenting.
inline constexpr std::size_t kMaxDatagramSize = 1472;
inline constexpr std::size_t kMaxDetections = (kMaxDatagramSize - kHeaderSize) / kDetectionSize;

inline constexpr std::size_t kTrackSummarySize = 8;
inline constexpr std::size_t kTrackSize = 48;
// The display feed may fragment at the IP layer, so it uses the full UDP payload limit.
inline constexpr std::size_t kMaxTrackDatagramSize = 65'507;
inline constexpr std::size_t kMaxTracks =
    (kMaxTrackDatagramSize - kHeaderSize - kTrackSummarySize) / kTrackSize;

}  // namespace wire

enum class DecodeError {
  kTooShort,
  kBadMagic,
  kUnsupportedVersion,
  kUnknownMessageType,
  kLengthMismatch,
  kNonFiniteValue,
  kBadTrackStatus,
};

std::string_view to_string(DecodeError error);

// Serializes a report into a little-endian datagram. Throws std::length_error if the
// report holds more than wire::kMaxDetections detections.
std::vector<std::byte> encode(const DetectionReport& report);

// Parses a datagram. Never throws on malformed input; returns the reason instead.
std::expected<DetectionReport, DecodeError> decode(std::span<const std::byte> datagram);

// Track report counterparts. encode throws std::length_error above wire::kMaxTracks.
std::vector<std::byte> encode(const TrackReport& report);
std::expected<TrackReport, DecodeError> decode_track_report(std::span<const std::byte> datagram);

}  // namespace vigil
