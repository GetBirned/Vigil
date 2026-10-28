#pragma once

// Track lifecycle and measurement-to-track association.

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "vigil/messages.hpp"
#include "vigil/tracker/kalman.hpp"
#include "vigil/tracker/spatial_grid.hpp"

namespace vigil::tracker {

enum class TrackStatus {
  kTentative,  // started from an unassociated detection; might be clutter
  kConfirmed,  // enough hits to be believed
  kCoasting,   // confirmed, but missed recent scans; predicting without updates
};

std::string_view to_string(TrackStatus status);

struct Track {
  std::uint32_t id = 0;
  TrackStatus status = TrackStatus::kTentative;
  ConstantVelocityKf filter;
  int hits = 0;
  int consecutive_misses = 0;
};

struct TrackerConfig {
  KalmanConfig kalman;
  // Chi-square gate on the innovation; 9.21 keeps 99% of true returns with 2 dof.
  double gate_chi2 = 9.21;
  int confirm_hits = 3;          // tentative -> confirmed after this many hits
  int tentative_max_misses = 2;  // drop a tentative track after this many misses in a row
  int confirmed_max_misses = 5;  // drop a coasting track after this many misses in a row
  // Find gate candidates through a spatial grid instead of testing every
  // (track, detection) pair. Results are identical either way; only the cost differs.
  bool spatial_index = true;
};

struct ScanStats {
  std::size_t associated = 0;
  std::size_t new_tracks = 0;
  std::size_t dropped_tracks = 0;
};

class TrackManager {
 public:
  explicit TrackManager(TrackerConfig config = {});

  // Runs one predict / associate / update / manage cycle for a scan taken at `time_s`.
  // Returns std::nullopt (and changes nothing) if the scan is older than the last one.
  std::optional<ScanStats> process(double time_s, std::span<const Detection> detections);

  [[nodiscard]] const std::vector<Track>& tracks() const { return tracks_; }
  [[nodiscard]] std::size_t count(TrackStatus status) const;

 private:
  struct Candidate {
    double cost;
    std::size_t track;
    std::size_t detection;
  };

  // Every (track, detection) pair inside the gate, in no particular order.
  std::vector<Candidate> gate(std::span<const Detection> detections);

  TrackerConfig config_;
  std::vector<Track> tracks_;
  std::optional<double> last_time_s_;
  std::uint32_t next_id_ = 1;
  DetectionGrid grid_;  // rebuilt every scan; kept as a member to reuse its storage
};

}  // namespace vigil::tracker
