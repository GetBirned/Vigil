#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <set>
#include <vector>

#include "vigil/messages.hpp"
#include "vigil/sim/scenario.hpp"
#include "vigil/tracker/spatial_grid.hpp"
#include "vigil/tracker/track_manager.hpp"

namespace vigil::tracker {
namespace {

std::set<std::size_t> visited(const DetectionGrid& grid, double x0, double y0, double x1,
                              double y1) {
  std::set<std::size_t> out;
  const bool ok = grid.visit_box(x0, y0, x1, y1, 1000, [&](std::size_t i) { out.insert(i); });
  EXPECT_TRUE(ok);
  return out;
}

TEST(DetectionGrid, FindsEveryDetectionInsideABox) {
  // Random points, including negative coordinates and points exactly on cell edges.
  std::mt19937_64 rng(7);
  std::uniform_real_distribution<double> pos(-5000.0, 5000.0);
  std::vector<Detection> dets;
  for (int i = 0; i < 400; ++i) {
    dets.push_back({pos(rng), pos(rng)});
  }
  dets.push_back({0.0, 0.0});
  dets.push_back({-250.0, 250.0});

  DetectionGrid grid;
  grid.build(dets, 250.0);

  for (int trial = 0; trial < 200; ++trial) {
    double x0 = pos(rng);
    double x1 = pos(rng);
    double y0 = pos(rng);
    double y1 = pos(rng);
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    const auto found = visited(grid, x0, y0, x1, y1);
    for (std::size_t i = 0; i < dets.size(); ++i) {
      const bool inside = dets[i].x_m >= x0 && dets[i].x_m <= x1 && dets[i].y_m >= y0 &&
                          dets[i].y_m <= y1;
      if (inside) {
        EXPECT_TRUE(found.contains(i)) << "missed detection " << i << " in trial " << trial;
      }
    }
  }
}

TEST(DetectionGrid, VisitsOnlyNearbyCells) {
  const std::vector<Detection> dets{{10, 10}, {90, 90}, {5000, 5000}, {-5000, -5000}};
  DetectionGrid grid;
  grid.build(dets, 100.0);
  EXPECT_EQ(visited(grid, 0, 0, 50, 50), (std::set<std::size_t>{0, 1}));  // same cell
  EXPECT_TRUE(visited(grid, 1000, 1000, 1100, 1100).empty());
}

TEST(DetectionGrid, RefusesBoxesWiderThanTheColumnLimit) {
  const std::vector<Detection> dets{{0, 0}};
  DetectionGrid grid;
  grid.build(dets, 10.0);
  int calls = 0;
  EXPECT_FALSE(grid.visit_box(-1000, -1, 1000, 1, 32, [&](std::size_t) { ++calls; }));
  EXPECT_EQ(calls, 0);
}

// The spatial index is an optimisation only: the tracker must produce bit-for-bit the
// same picture with and without it.
void expect_identical_runs(std::size_t targets, double clutter, double region_m, int scans) {
  sim::SensorConfig sensor;
  sensor.probability_of_detection = 0.9;
  sensor.clutter_per_scan = clutter;
  sensor.region_half_width_m = region_m;
  auto scenario = sim::Scenario::make_default(targets, sensor, 99);

  TrackerConfig grid_cfg;
  grid_cfg.spatial_index = true;
  TrackerConfig brute_cfg;
  brute_cfg.spatial_index = false;
  TrackManager grid(grid_cfg);
  TrackManager brute(brute_cfg);

  for (int i = 0; i < scans; ++i) {
    const sim::Scan scan = scenario.step(1.0);
    const auto a = grid.process(scan.time_s, scan.detections);
    const auto b = brute.process(scan.time_s, scan.detections);
    ASSERT_TRUE(a && b);
    ASSERT_EQ(a->associated, b->associated) << "scan " << i;
    ASSERT_EQ(a->new_tracks, b->new_tracks) << "scan " << i;
    ASSERT_EQ(a->dropped_tracks, b->dropped_tracks) << "scan " << i;
    ASSERT_EQ(grid.tracks().size(), brute.tracks().size()) << "scan " << i;
    for (std::size_t t = 0; t < grid.tracks().size(); ++t) {
      const Track& g = grid.tracks()[t];
      const Track& b2 = brute.tracks()[t];
      ASSERT_EQ(g.id, b2.id);
      ASSERT_EQ(g.status, b2.status);
      ASSERT_EQ(g.hits, b2.hits);
      ASSERT_EQ(g.filter.state(), b2.filter.state()) << "track " << g.id << " scan " << i;
    }
  }
}

TEST(SpatialIndex, MatchesBruteForceOnTheDefaultScenario) {
  expect_identical_runs(8, 5.0, 20'000, 120);
}

TEST(SpatialIndex, MatchesBruteForceWithDenseTargetsAndHeavyClutter) {
  // Crowded and cluttered, so gates overlap and many detections compete for tracks.
  expect_identical_runs(150, 40.0, 15'000, 60);
}

}  // namespace
}  // namespace vigil::tracker
