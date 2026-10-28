#include "vigil/sim/scenario.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace vigil::sim {

void propagate(TargetState& t, double dt_s) {
  const double w = t.turn_rate_rad_s;
  if (std::abs(w) < 1e-9) {
    t.x_m += t.vx_mps * dt_s;
    t.y_m += t.vy_mps * dt_s;
    return;
  }

  const double s = std::sin(w * dt_s);
  const double c = std::cos(w * dt_s);
  const double vx = t.vx_mps;
  const double vy = t.vy_mps;
  t.x_m += (vx * s - vy * (1.0 - c)) / w;
  t.y_m += (vx * (1.0 - c) + vy * s) / w;
  t.vx_mps = vx * c - vy * s;
  t.vy_mps = vx * s + vy * c;
}

Scenario::Scenario(std::vector<TargetState> targets, SensorConfig sensor, std::uint64_t seed)
    : targets_(std::move(targets)), sensor_(sensor), rng_(seed) {}

Scenario Scenario::make_default(std::size_t num_targets, SensorConfig sensor, std::uint64_t seed) {
  std::mt19937_64 rng(seed ^ 0x9E3779B97F4A7C15ULL);
  const double spawn = 0.6 * sensor.region_half_width_m;
  std::uniform_real_distribution<double> position(-spawn, spawn);
  std::uniform_real_distribution<double> speed(100.0, 250.0);
  std::uniform_real_distribution<double> heading(0.0, 2.0 * std::numbers::pi);
  std::uniform_real_distribution<double> turn_deg_s(1.0, 3.0);

  std::vector<TargetState> targets;
  targets.reserve(num_targets);
  for (std::size_t i = 0; i < num_targets; ++i) {
    const double v = speed(rng);
    const double h = heading(rng);
    TargetState t;
    t.id = static_cast<std::uint32_t>(i + 1);
    t.x_m = position(rng);
    t.y_m = position(rng);
    t.vx_mps = v * std::cos(h);
    t.vy_mps = v * std::sin(h);
    if (i % 3 == 2) {
      const double sign = (i % 2 == 0) ? 1.0 : -1.0;
      t.turn_rate_rad_s = sign * turn_deg_s(rng) * std::numbers::pi / 180.0;
    }
    targets.push_back(t);
  }
  return {std::move(targets), sensor, seed};
}

Scan Scenario::step(double dt_s) {
  time_s_ += dt_s;

  std::normal_distribution<double> noise(0.0, sensor_.position_sigma_m);
  std::bernoulli_distribution detected(sensor_.probability_of_detection);
  std::uniform_real_distribution<double> clutter_pos(-sensor_.region_half_width_m,
                                                     sensor_.region_half_width_m);

  Scan scan;
  scan.time_s = time_s_;

  for (TargetState& t : targets_) {
    propagate(t, dt_s);
    if (detected(rng_)) {
      scan.detections.push_back({t.x_m + noise(rng_), t.y_m + noise(rng_)});
    }
  }

  // poisson_distribution requires a strictly positive mean, so "no clutter" is a special case.
  const int false_alarms = sensor_.clutter_per_scan > 0.0
                               ? std::poisson_distribution<int>(sensor_.clutter_per_scan)(rng_)
                               : 0;
  for (int i = 0; i < false_alarms; ++i) {
    scan.detections.push_back({clutter_pos(rng_), clutter_pos(rng_)});
  }

  // A real sensor gives no hint which return is which.
  std::ranges::shuffle(scan.detections, rng_);

  scan.truth = targets_;
  return scan;
}

}  // namespace vigil::sim
