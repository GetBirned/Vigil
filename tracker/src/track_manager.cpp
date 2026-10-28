#include "vigil/tracker/track_manager.hpp"

#include <algorithm>
#include <utility>

namespace vigil::tracker {
namespace {

struct Candidate {
  double cost;
  std::size_t track;
  std::size_t detection;
};

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
  std::vector<Candidate> candidates;
  for (std::size_t t = 0; t < tracks_.size(); ++t) {
    for (std::size_t d = 0; d < detections.size(); ++d) {
      const Innovation inn =
          tracks_[t].filter.innovation(Vec2(detections[d].x_m, detections[d].y_m));
      if (inn.mahalanobis_sq <= config_.gate_chi2) {
        candidates.push_back({inn.mahalanobis_sq + inn.log_det_covariance, t, d});
      }
    }
  }

  // 3. Associate: greedy global nearest neighbour, cheapest pairs first.
  //    (Milestone 2 replaces this with an optimal Hungarian assignment.)
  std::ranges::sort(candidates, {}, &Candidate::cost);
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
