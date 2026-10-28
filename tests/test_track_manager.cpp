#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

#include "vigil/sim/scenario.hpp"
#include "vigil/tracker/track_manager.hpp"

namespace vigil::tracker {
namespace {

std::vector<Detection> one(double x, double y) { return {Detection{x, y}}; }

TEST(TrackManager, ConfirmsTrackAfterThreeHits) {
  TrackManager tm;
  tm.process(0.0, one(0, 0));
  EXPECT_EQ(tm.count(TrackStatus::kTentative), 1U);
  tm.process(1.0, one(100, 0));
  EXPECT_EQ(tm.count(TrackStatus::kTentative), 1U);
  tm.process(2.0, one(200, 0));
  EXPECT_EQ(tm.count(TrackStatus::kConfirmed), 1U);
  EXPECT_EQ(tm.tracks().size(), 1U);
}

TEST(TrackManager, LoneFalseAlarmIsDroppedQuickly) {
  TrackManager tm;
  tm.process(0.0, one(5000, 5000));
  tm.process(1.0, {});
  tm.process(2.0, {});
  EXPECT_TRUE(tm.tracks().empty());
}

TEST(TrackManager, ConfirmedTrackCoastsThenDrops) {
  TrackerConfig config;
  TrackManager tm(config);
  for (int i = 0; i < 5; ++i) {
    tm.process(i, one(100.0 * i, 0));
  }
  ASSERT_EQ(tm.count(TrackStatus::kConfirmed), 1U);

  tm.process(5.0, {});
  EXPECT_EQ(tm.count(TrackStatus::kCoasting), 1U);

  // Reacquired while coasting: back to confirmed, same track.
  const auto id = tm.tracks().front().id;
  tm.process(6.0, one(600, 0));
  EXPECT_EQ(tm.count(TrackStatus::kConfirmed), 1U);
  EXPECT_EQ(tm.tracks().front().id, id);

  for (int i = 0; i < config.confirmed_max_misses; ++i) {
    tm.process(7.0 + i, {});
  }
  EXPECT_TRUE(tm.tracks().empty());
}

TEST(TrackManager, RejectsScanOlderThanTheLastOne) {
  TrackManager tm;
  EXPECT_TRUE(tm.process(5.0, one(0, 0)).has_value());
  EXPECT_FALSE(tm.process(4.0, one(0, 0)).has_value());
  EXPECT_EQ(tm.tracks().size(), 1U);
}

TEST(TrackManager, KeepsCrossingTargetsSeparate) {
  // Two targets fly an X through the origin; they must not swap identities.
  TrackManager tm;
  for (int i = -20; i <= 20; ++i) {
    const double t = i + 20.0;
    const std::vector<Detection> dets{{150.0 * i, 150.0 * i}, {150.0 * i, -150.0 * i}};
    tm.process(t, dets);
  }
  ASSERT_EQ(tm.count(TrackStatus::kConfirmed), 2U);
  for (const Track& track : tm.tracks()) {
    const Vec2 v = track.filter.velocity();
    EXPECT_NEAR(std::abs(v.x()), 150.0, 20.0);
    EXPECT_NEAR(std::abs(v.y()), 150.0, 20.0);
  }
}

// End to end against the simulator: noisy sensor, missed detections and clutter.
TEST(TrackManager, TracksEverySimulatedTargetThroughClutter) {
  sim::SensorConfig sensor;
  sensor.position_sigma_m = 25.0;
  sensor.probability_of_detection = 0.9;
  sensor.clutter_per_scan = 5.0;
  constexpr std::size_t kTargets = 8;
  auto scenario = sim::Scenario::make_default(kTargets, sensor, 2026);

  TrackManager tm;
  sim::Scan scan;
  for (int i = 0; i < 120; ++i) {
    scan = scenario.step(1.0);
    tm.process(scan.time_s, scan.detections);
  }

  std::size_t firm = 0;
  for (const Track& track : tm.tracks()) {
    if (track.status != TrackStatus::kTentative) {
      ++firm;
    }
  }
  EXPECT_EQ(firm, kTargets) << "expected exactly one firm track per target";

  // Every target has a firm track nearby, and the overall position error is small.
  double sum_sq = 0.0;
  for (const sim::TargetState& truth : scan.truth) {
    double best = std::numeric_limits<double>::infinity();
    for (const Track& track : tm.tracks()) {
      if (track.status == TrackStatus::kTentative) {
        continue;
      }
      best = std::min(best, (track.filter.position() - Vec2(truth.x_m, truth.y_m)).norm());
    }
    EXPECT_LT(best, 150.0) << "target " << truth.id << " has no track nearby";
    sum_sq += best * best;
  }
  EXPECT_LT(std::sqrt(sum_sq / kTargets), 60.0);
}

}  // namespace
}  // namespace vigil::tracker
