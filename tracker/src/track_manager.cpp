#include "vigil/tracker/track_manager.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>

namespace vigil::tracker {
namespace {

// Grid cells are sized to the typical gate, within these limits.
constexpr double kMinCellM = 50.0;
constexpr double kMaxCellM = 200'000.0;
// A gate wider than this many cells (a brand-new track's, say) is cheaper to check
// against every detection than to probe cell by cell.
constexpr int kMaxGridColumns = 32;

}  // namespace

std::string_view to_string(TrackStatus status) {
  switch (status) {
    case TrackStatus::kTentative: return "TENTATIVE";
    case TrackStatus::kConfirmed: return "CONFIRMED";
    case TrackStatus::kCoasting: return "COASTING";
  }
  return "UNKNOWN";
}

TrackManager::TrackManager(TrackerConfig config) : config_(config) {}

std::size_t TrackManager::count(TrackStatus status) const {
  return static_cast<std::size_t>(
      std::ranges::count(tracks_, status, &Track::status));
}

std::vector<TrackManager::Candidate> TrackManager::gate(std::span<const Detection> detections) {
  std::vector<Candidate> candidates;
  if (tracks_.empty() || detections.empty()) {
    return candidates;
  }

  // Everything that depends only on the track is computed once, not once per detection.
  struct TrackGate {
    MeasurementPrediction prediction;
    double half_x;  // the gate ellipse's half-extent along each axis
    double half_y;
  };
  std::vector<TrackGate> gates;
  gates.reserve(tracks_.size());
  for (const Track& track : tracks_) {
    const Mat2 S = track.filter.innovation_covariance();
    // Any point with d^2 <= g lies within sqrt(g * S_ii) of the prediction on axis i, so
    // this box contains the whole gate and the grid search can't miss a candidate.
    gates.push_back({track.filter.predict_measurement(),
                     std::sqrt(config_.gate_chi2 * S(0, 0)),
                     std::sqrt(config_.gate_chi2 * S(1, 1))});
  }

  const auto score = [&](std::size_t t, std::size_t d) {
    const Vec2 z(detections[d].x_m, detections[d].y_m);
    const double d2 = gates[t].prediction.mahalanobis_sq(z);
    if (d2 <= config_.gate_chi2) {
      candidates.push_back({d2 + gates[t].prediction.log_det_covariance, t, d});
    }
  };
  const auto score_all = [&](std::size_t t) {
    for (std::size_t d = 0; d < detections.size(); ++d) {
      score(t, d);
    }
  };

  if (!config_.spatial_index) {
    for (std::size_t t = 0; t < tracks_.size(); ++t) {
      score_all(t);
    }
    return candidates;
  }

  // Size cells to the median gate width: most gates then touch only a few cells.
  std::vector<double> widths;
  widths.reserve(gates.size());
  for (const TrackGate& g : gates) {
    widths.push_back(2.0 * std::max(g.half_x, g.half_y));
  }
  const auto mid = widths.begin() + static_cast<std::ptrdiff_t>(widths.size() / 2);
  std::nth_element(widths.begin(), mid, widths.end());
  grid_.build(detections, std::clamp(*mid, kMinCellM, kMaxCellM));

  for (std::size_t t = 0; t < tracks_.size(); ++t) {
    const TrackGate& g = gates[t];
    const Vec2& p = g.prediction.z;
    const bool used_grid = grid_.visit_box(p.x() - g.half_x, p.y() - g.half_y,
                                           p.x() + g.half_x, p.y() + g.half_y, kMaxGridColumns,
                                           [&](std::size_t d) { score(t, d); });
    if (!used_grid) {
      score_all(t);
    }
  }
  return candidates;
}

std::optional<ScanStats> TrackManager::process(double time_s,
                                               std::span<const Detection> detections) {
  if (last_time_s_ && time_s < *last_time_s_) {
    return std::nullopt;
  }
  const double dt_s = last_time_s_ ? time_s - *last_time_s_ : 0.0;
  last_time_s_ = time_s;

  ScanStats stats;

  // 1. Predict every track to the scan time.
  for (Track& track : tracks_) {
    track.filter.predict(dt_s);
  }

  // 2. Gate: list every (track, detection) pair that falls inside the chi-square gate.
  //    Cost is the normalised distance d^2 + ln|S|. Plain d^2 would let a new track
  //    with a huge covariance look "close" to everything and steal good detections.
  std::vector<Candidate> candidates = gate(detections);

  // 3. Associate: greedy global nearest neighbour, cheapest pairs first.
  //    (Milestone 2 replaces this with an optimal Hungarian assignment.)
  //    Ties break on indices so the result doesn't depend on the order gating found them.
  std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) {
    return std::tie(a.cost, a.track, a.detection) < std::tie(b.cost, b.track, b.detection);
  });
  std::vector<bool> track_used(tracks_.size(), false);
  std::vector<bool> detection_used(detections.size(), false);

  for (const Candidate& c : candidates) {
    if (track_used[c.track] || detection_used[c.detection]) {
      continue;
    }
    track_used[c.track] = true;
    detection_used[c.detection] = true;
    ++stats.associated;

    Track& track = tracks_[c.track];
    track.filter.update(Vec2(detections[c.detection].x_m, detections[c.detection].y_m));
    ++track.hits;
    track.consecutive_misses = 0;
    if (track.status == TrackStatus::kCoasting ||
        (track.status == TrackStatus::kTentative && track.hits >= config_.confirm_hits)) {
      track.status = TrackStatus::kConfirmed;
    }
  }

  // 4. Tracks that got nothing this scan.
  for (std::size_t t = 0; t < tracks_.size(); ++t) {
    if (track_used[t]) {
      continue;
    }
    Track& track = tracks_[t];
    ++track.consecutive_misses;
    if (track.status == TrackStatus::kConfirmed) {
      track.status = TrackStatus::kCoasting;
    }
  }

  // 5. Drop tracks that have missed too many scans in a row.
  const auto before = tracks_.size();
  std::erase_if(tracks_, [this](const Track& track) {
    const int limit = track.status == TrackStatus::kTentative ? config_.tentative_max_misses
                                                              : config_.confirmed_max_misses;
    return track.consecutive_misses >= limit;
  });
  stats.dropped_tracks = before - tracks_.size();

  // 6. Every unclaimed detection starts a tentative track.
  for (std::size_t d = 0; d < detections.size(); ++d) {
    if (detection_used[d]) {
      continue;
    }
    tracks_.push_back(Track{
        .id = next_id_++,
        .status = TrackStatus::kTentative,
        .filter = ConstantVelocityKf(Vec2(detections[d].x_m, detections[d].y_m), config_.kalman),
        .hits = 1,
        .consecutive_misses = 0,
    });
    ++stats.new_tracks;
  }

  return stats;
}

}  // namespace vigil::tracker
