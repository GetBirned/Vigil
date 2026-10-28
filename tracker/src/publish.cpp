#include "vigil/tracker/publish.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vigil::tracker {
namespace {

WireTrackStatus to_wire(TrackStatus status) {
  switch (status) {
    case TrackStatus::kTentative: return WireTrackStatus::kTentative;
    case TrackStatus::kConfirmed: return WireTrackStatus::kConfirmed;
    case TrackStatus::kCoasting: return WireTrackStatus::kCoasting;
  }
  return WireTrackStatus::kTentative;
}

}  // namespace

TrackReport make_track_report(const TrackManager& tracker, const DetectionReport& scan,
                              std::uint32_t processing_ns) {
  TrackReport report;
  report.seq = scan.seq;
  report.source_id = 1;
  report.timestamp_ns = scan.timestamp_ns;
  report.processing_ns = processing_ns;
  report.detections_in_scan = static_cast<std::uint16_t>(
      std::min<std::size_t>(scan.detections.size(), std::numeric_limits<std::uint16_t>::max()));

  const auto& tracks = tracker.tracks();
  const std::size_t count = std::min(tracks.size(), wire::kMaxTracks);
  report.tracks.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const Track& t = tracks[i];
    const Vec4& x = t.filter.state();
    const Mat4& P = t.filter.covariance();
    report.tracks.push_back({
        .id = t.id,
        .status = to_wire(t.status),
        .hits = static_cast<std::uint16_t>(std::min(t.hits, 0xFFFF)),
        .x_m = x(0),
        .y_m = x(1),
        .vx_mps = x(2),
        .vy_mps = x(3),
        .position_sigma_m = static_cast<float>(std::sqrt((P(0, 0) + P(1, 1)) / 2.0)),
        .velocity_sigma_mps = static_cast<float>(std::sqrt((P(2, 2) + P(3, 3)) / 2.0)),
    });
  }
  return report;
}

}  // namespace vigil::tracker
