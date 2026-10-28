#pragma once

// Wire format for sensor -> tracker detection reports.
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

namespace wire {

inline constexpr std::uint32_t kMagic = 0x56474C31;  // "VGL1"
inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::uint16_t kMsgDetectionReport = 1;

inline constexpr std::size_t kHeaderSize = 24;
inline constexpr std::size_t kDetectionSize = 16;
// Largest UDP payload that fits a 1500-byte Ethernet MTU without fragmenting.
inline constexpr std::size_t kMaxDatagramSize = 1472;
inline constexpr std::size_t kMaxDetections = (kMaxDatagramSize - kHeaderSize) / kDetectionSize;

}  // namespace wire

enum class DecodeError {
  kTooShort,
  kBadMagic,
  kUnsupportedVersion,
  kUnknownMessageType,
  kLengthMismatch,
  kNonFiniteValue,
};

std::string_view to_string(DecodeError error);

// Serializes a report into a little-endian datagram. Throws std::length_error if the
// report holds more than wire::kMaxDetections detections.
std::vector<std::byte> encode(const DetectionReport& report);

// Parses a datagram. Never throws on malformed input; returns the reason instead.
std::expected<DetectionReport, DecodeError> decode(std::span<const std::byte> datagram);

}  // namespace vigil
