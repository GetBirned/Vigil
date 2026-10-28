#include "vigil/messages.hpp"

#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace vigil {
namespace {

// Byte-at-a-time little-endian helpers: correct on any host byte order and free of
// alignment or strict-aliasing assumptions.
template <typename T>
void put_le(std::vector<std::byte>& out, T value) {
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFF));
  }
}

template <typename T>
T get_le(std::span<const std::byte> in, std::size_t offset) {
  T value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    value = static_cast<T>(value | (static_cast<T>(in[offset + i]) << (8 * i)));
  }
  return value;
}

void put_f64(std::vector<std::byte>& out, double value) {
  put_le(out, std::bit_cast<std::uint64_t>(value));
}

double get_f64(std::span<const std::byte> in, std::size_t offset) {
  return std::bit_cast<double>(get_le<std::uint64_t>(in, offset));
}

}  // namespace

std::string_view to_string(DecodeError error) {
  switch (error) {
    case DecodeError::kTooShort: return "datagram shorter than header";
    case DecodeError::kBadMagic: return "bad magic number";
    case DecodeError::kUnsupportedVersion: return "unsupported protocol version";
    case DecodeError::kUnknownMessageType: return "unknown message type";
    case DecodeError::kLengthMismatch: return "detection count does not match datagram length";
    case DecodeError::kNonFiniteValue: return "detection contains NaN or infinity";
  }
  return "unknown decode error";
}

std::vector<std::byte> encode(const DetectionReport& report) {
  if (report.detections.size() > wire::kMaxDetections) {
    throw std::length_error("DetectionReport has " + std::to_string(report.detections.size()) +
                            " detections; max per datagram is " +
                            std::to_string(wire::kMaxDetections));
  }

  std::vector<std::byte> out;
  out.reserve(wire::kHeaderSize + report.detections.size() * wire::kDetectionSize);

  put_le(out, wire::kMagic);
  put_le(out, wire::kVersion);
  put_le(out, wire::kMsgDetectionReport);
  put_le(out, report.seq);
  put_le(out, report.sensor_id);
  put_le(out, static_cast<std::uint16_t>(report.detections.size()));
  put_le(out, report.timestamp_ns);

  for (const Detection& d : report.detections) {
    put_f64(out, d.x_m);
    put_f64(out, d.y_m);
  }
  return out;
}

std::expected<DetectionReport, DecodeError> decode(std::span<const std::byte> datagram) {
  if (datagram.size() < wire::kHeaderSize) {
    return std::unexpected(DecodeError::kTooShort);
  }
  if (get_le<std::uint32_t>(datagram, 0) != wire::kMagic) {
    return std::unexpected(DecodeError::kBadMagic);
  }
  if (get_le<std::uint16_t>(datagram, 4) != wire::kVersion) {
    return std::unexpected(DecodeError::kUnsupportedVersion);
  }
  if (get_le<std::uint16_t>(datagram, 6) != wire::kMsgDetectionReport) {
    return std::unexpected(DecodeError::kUnknownMessageType);
  }

  DetectionReport report;
  report.seq = get_le<std::uint32_t>(datagram, 8);
  report.sensor_id = get_le<std::uint16_t>(datagram, 12);
  const std::size_t count = get_le<std::uint16_t>(datagram, 14);
  report.timestamp_ns = get_le<std::uint64_t>(datagram, 16);

  if (datagram.size() != wire::kHeaderSize + count * wire::kDetectionSize) {
    return std::unexpected(DecodeError::kLengthMismatch);
  }

  report.detections.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t offset = wire::kHeaderSize + i * wire::kDetectionSize;
    const Detection d{get_f64(datagram, offset), get_f64(datagram, offset + 8)};
    if (!std::isfinite(d.x_m) || !std::isfinite(d.y_m)) {
      return std::unexpected(DecodeError::kNonFiniteValue);
    }
    report.detections.push_back(d);
  }
  return report;
}

}  // namespace vigil
