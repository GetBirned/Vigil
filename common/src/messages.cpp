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

void put_f32(std::vector<std::byte>& out, float value) {
  put_le(out, std::bit_cast<std::uint32_t>(value));
}

float get_f32(std::span<const std::byte> in, std::size_t offset) {
  return std::bit_cast<float>(get_le<std::uint32_t>(in, offset));
}

// The 24-byte header shared by every message type.
struct Header {
  std::uint32_t seq;
  std::uint16_t source_id;
  std::uint16_t count;
  std::uint64_t timestamp_ns;
};

void put_header(std::vector<std::byte>& out, std::uint16_t msg_type, const Header& h) {
  put_le(out, wire::kMagic);
  put_le(out, wire::kVersion);
  put_le(out, msg_type);
  put_le(out, h.seq);
  put_le(out, h.source_id);
  put_le(out, h.count);
  put_le(out, h.timestamp_ns);
}

std::expected<Header, DecodeError> get_header(std::span<const std::byte> datagram,
                                              std::uint16_t expected_type) {
  if (datagram.size() < wire::kHeaderSize) {
    return std::unexpected(DecodeError::kTooShort);
  }
  if (get_le<std::uint32_t>(datagram, 0) != wire::kMagic) {
    return std::unexpected(DecodeError::kBadMagic);
  }
  if (get_le<std::uint16_t>(datagram, 4) != wire::kVersion) {
    return std::unexpected(DecodeError::kUnsupportedVersion);
  }
  if (get_le<std::uint16_t>(datagram, 6) != expected_type) {
    return std::unexpected(DecodeError::kUnknownMessageType);
  }
  return Header{
      .seq = get_le<std::uint32_t>(datagram, 8),
      .source_id = get_le<std::uint16_t>(datagram, 12),
      .count = get_le<std::uint16_t>(datagram, 14),
      .timestamp_ns = get_le<std::uint64_t>(datagram, 16),
  };
}

}  // namespace

std::string_view to_string(DecodeError error) {
  switch (error) {
    case DecodeError::kTooShort: return "datagram shorter than header";
    case DecodeError::kBadMagic: return "bad magic number";
    case DecodeError::kUnsupportedVersion: return "unsupported protocol version";
    case DecodeError::kUnknownMessageType: return "unknown message type";
    case DecodeError::kLengthMismatch: return "detection count does not match datagram length";
    case DecodeError::kNonFiniteValue: return "record contains NaN or infinity";
    case DecodeError::kBadTrackStatus: return "track status is not a defined value";
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

  put_header(out, wire::kMsgDetectionReport,
             {.seq = report.seq,
              .source_id = report.sensor_id,
              .count = static_cast<std::uint16_t>(report.detections.size()),
              .timestamp_ns = report.timestamp_ns});

  for (const Detection& d : report.detections) {
    put_f64(out, d.x_m);
    put_f64(out, d.y_m);
  }
  return out;
}

std::expected<DetectionReport, DecodeError> decode(std::span<const std::byte> datagram) {
  const auto header = get_header(datagram, wire::kMsgDetectionReport);
  if (!header) {
    return std::unexpected(header.error());
  }

  DetectionReport report;
  report.seq = header->seq;
  report.sensor_id = header->source_id;
  report.timestamp_ns = header->timestamp_ns;
  const std::size_t count = header->count;

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

std::vector<std::byte> encode(const TrackReport& report) {
  if (report.tracks.size() > wire::kMaxTracks) {
    throw std::length_error("TrackReport has " + std::to_string(report.tracks.size()) +
                            " tracks; max per datagram is " + std::to_string(wire::kMaxTracks));
  }

  std::vector<std::byte> out;
  out.reserve(wire::kHeaderSize + wire::kTrackSummarySize +
              report.tracks.size() * wire::kTrackSize);

  put_header(out, wire::kMsgTrackReport,
             {.seq = report.seq,
              .source_id = report.source_id,
              .count = static_cast<std::uint16_t>(report.tracks.size()),
              .timestamp_ns = report.timestamp_ns});
  put_le(out, report.processing_ns);
  put_le(out, report.detections_in_scan);
  put_le(out, std::uint16_t{0});  // reserved

  for (const TrackState& t : report.tracks) {
    put_le(out, t.id);
    put_le(out, static_cast<std::uint8_t>(t.status));
    put_le(out, std::uint8_t{0});  // reserved
    put_le(out, t.hits);
    put_f64(out, t.x_m);
    put_f64(out, t.y_m);
    put_f64(out, t.vx_mps);
    put_f64(out, t.vy_mps);
    put_f32(out, t.position_sigma_m);
    put_f32(out, t.velocity_sigma_mps);
  }
  return out;
}

std::expected<TrackReport, DecodeError> decode_track_report(std::span<const std::byte> datagram) {
  const auto header = get_header(datagram, wire::kMsgTrackReport);
  if (!header) {
    return std::unexpected(header.error());
  }
  const std::size_t count = header->count;
  if (datagram.size() != wire::kHeaderSize + wire::kTrackSummarySize + count * wire::kTrackSize) {
    return std::unexpected(DecodeError::kLengthMismatch);
  }

  TrackReport report;
  report.seq = header->seq;
  report.source_id = header->source_id;
  report.timestamp_ns = header->timestamp_ns;
  report.processing_ns = get_le<std::uint32_t>(datagram, wire::kHeaderSize);
  report.detections_in_scan = get_le<std::uint16_t>(datagram, wire::kHeaderSize + 4);

  report.tracks.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t o = wire::kHeaderSize + wire::kTrackSummarySize + i * wire::kTrackSize;
    const auto status = get_le<std::uint8_t>(datagram, o + 4);
    if (status > static_cast<std::uint8_t>(WireTrackStatus::kCoasting)) {
      return std::unexpected(DecodeError::kBadTrackStatus);
    }
    const TrackState t{
        .id = get_le<std::uint32_t>(datagram, o),
        .status = static_cast<WireTrackStatus>(status),
        .hits = get_le<std::uint16_t>(datagram, o + 6),
        .x_m = get_f64(datagram, o + 8),
        .y_m = get_f64(datagram, o + 16),
        .vx_mps = get_f64(datagram, o + 24),
        .vy_mps = get_f64(datagram, o + 32),
        .position_sigma_m = get_f32(datagram, o + 40),
        .velocity_sigma_mps = get_f32(datagram, o + 44),
    };
    if (!std::isfinite(t.x_m) || !std::isfinite(t.y_m) || !std::isfinite(t.vx_mps) ||
        !std::isfinite(t.vy_mps) || !std::isfinite(t.position_sigma_m) ||
        !std::isfinite(t.velocity_sigma_mps)) {
      return std::unexpected(DecodeError::kNonFiniteValue);
    }
    report.tracks.push_back(t);
  }
  return report;
}

}  // namespace vigil
