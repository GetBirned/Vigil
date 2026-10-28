#pragma once

// Ground-truth target motion plus a simple position sensor with noise, missed
// detections and uniform clutter.

#include <cstdint>
#include <random>
#include <vector>

#include "vigil/messages.hpp"

namespace vigil::sim {

struct TargetState {
  std::uint32_t id = 0;
  double x_m = 0.0;
  double y_m = 0.0;
  double vx_mps = 0.0;
  double vy_mps = 0.0;
  double turn_rate_rad_s = 0.0;  // 0 = straight line; otherwise a coordinated turn
};

struct SensorConfig {
  double position_sigma_m = 25.0;       // 1-sigma noise on each axis
  double probability_of_detection = 0.95;
  double clutter_per_scan = 2.0;        // mean false alarms per scan (Poisson)
  double region_half_width_m = 20'000;  // clutter is spread over a square this size
};

struct Scan {
  double time_s = 0.0;
  std::vector<Detection> detections;  // shuffled: truth order is not leaked
  std::vector<TargetState> truth;     // for scoring only; never goes on the wire
};

// Moves a target forward by dt seconds using the exact coordinated-turn solution.
void propagate(TargetState& target, double dt_s);

class Scenario {
 public:
  Scenario(std::vector<TargetState> targets, SensorConfig sensor, std::uint64_t seed);

  // Spreads `num_targets` targets across the region with varied speeds and headings;
  // every third target flies a gentle turn. Fully determined by `seed`.
  static Scenario make_default(std::size_t num_targets, SensorConfig sensor, std::uint64_t seed);

  // Advances truth by dt seconds, then returns what the sensor reports at the new time.
  Scan step(double dt_s);

  [[nodiscard]] double time_s() const { return time_s_; }
  [[nodiscard]] const std::vector<TargetState>& targets() const { return targets_; }
  [[nodiscard]] const SensorConfig& sensor() const { return sensor_; }

 private:
  std::vector<TargetState> targets_;
  SensorConfig sensor_;
  std::mt19937_64 rng_;
  double time_s_ = 0.0;
};

}  // namespace vigil::sim
