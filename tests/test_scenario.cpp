#include <gtest/gtest.h>

#include <cmath>
#include <numbers>

#include "vigil/sim/scenario.hpp"

namespace vigil::sim {
namespace {

TEST(Propagate, StraightLineMovesByVelocityTimesTime) {
  TargetState t{.id = 1, .x_m = 0, .y_m = 0, .vx_mps = 100, .vy_mps = -50, .turn_rate_rad_s = 0};
  propagate(t, 2.0);
  EXPECT_DOUBLE_EQ(t.x_m, 200.0);
  EXPECT_DOUBLE_EQ(t.y_m, -100.0);
  EXPECT_DOUBLE_EQ(t.vx_mps, 100.0);
}

TEST(Propagate, FullCircleTurnReturnsToStart) {
  const double w = 2.0 * std::numbers::pi / 60.0;  // one revolution per minute
  TargetState t{.id = 1, .x_m = 500, .y_m = -300, .vx_mps = 200, .vy_mps = 0, .turn_rate_rad_s = w};
  for (int i = 0; i < 60; ++i) {
    propagate(t, 1.0);
  }
  EXPECT_NEAR(t.x_m, 500.0, 1e-6);
  EXPECT_NEAR(t.y_m, -300.0, 1e-6);
  EXPECT_NEAR(t.vx_mps, 200.0, 1e-9);
  EXPECT_NEAR(t.vy_mps, 0.0, 1e-9);
}

TEST(Propagate, TurnPreservesSpeed) {
  TargetState t{.id = 1, .x_m = 0, .y_m = 0, .vx_mps = 150, .vy_mps = 80, .turn_rate_rad_s = 0.05};
  const double speed = std::hypot(t.vx_mps, t.vy_mps);
  propagate(t, 7.3);
  EXPECT_NEAR(std::hypot(t.vx_mps, t.vy_mps), speed, 1e-9);
}

TEST(Scenario, PerfectSensorReportsEveryTargetAndNothingElse) {
  SensorConfig sensor;
  sensor.probability_of_detection = 1.0;
  sensor.clutter_per_scan = 0.0;
  auto scenario = Scenario::make_default(8, sensor, 42);
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(scenario.step(1.0).detections.size(), 8U);
  }
}

TEST(Scenario, SameSeedGivesIdenticalScans) {
  auto a = Scenario::make_default(5, {}, 7);
  auto b = Scenario::make_default(5, {}, 7);
  for (int i = 0; i < 20; ++i) {
    const Scan sa = a.step(1.0);
    const Scan sb = b.step(1.0);
    ASSERT_EQ(sa.detections.size(), sb.detections.size());
    for (std::size_t j = 0; j < sa.detections.size(); ++j) {
      EXPECT_EQ(sa.detections[j].x_m, sb.detections[j].x_m);
      EXPECT_EQ(sa.detections[j].y_m, sb.detections[j].y_m);
    }
  }
}

TEST(Scenario, MeasurementNoiseMatchesConfiguredSigma) {
  SensorConfig sensor;
  sensor.probability_of_detection = 1.0;
  sensor.clutter_per_scan = 0.0;
  sensor.position_sigma_m = 40.0;
  Scenario scenario({TargetState{.id = 1}}, sensor, 3);

  double sum_sq = 0.0;
  constexpr int kScans = 4000;
  for (int i = 0; i < kScans; ++i) {
    const Scan s = scenario.step(1.0);
    sum_sq += s.detections[0].x_m * s.detections[0].x_m;
  }
  EXPECT_NEAR(std::sqrt(sum_sq / kScans), 40.0, 2.0);
}

}  // namespace
}  // namespace vigil::sim
