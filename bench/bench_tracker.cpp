// Throughput of the filter and of a full tracker cycle as the target count grows.
// Run from a release build: ./build/release/bench/vigil_bench

#include <benchmark/benchmark.h>

#include <vector>

#include "vigil/messages.hpp"
#include "vigil/sim/scenario.hpp"
#include "vigil/tracker/track_manager.hpp"

namespace {

using vigil::tracker::ConstantVelocityKf;
using vigil::tracker::TrackManager;
using vigil::tracker::Vec2;

void BM_KalmanPredictUpdate(benchmark::State& state) {
  ConstantVelocityKf kf(Vec2::Zero(), {});
  double x = 0.0;
  for (auto _ : state) {
    kf.predict(1.0);
    x += 100.0;
    kf.update(Vec2(x, 0.0));
    auto estimate = kf.state();
    benchmark::DoNotOptimize(estimate);
  }
}
BENCHMARK(BM_KalmanPredictUpdate);

// One predict / gate / associate / update cycle with N real targets plus clutter, in
// steady state (tracks already confirmed). Second argument: 1 = spatial-grid gating,
// 0 = test every (track, detection) pair.
void BM_TrackerScan(benchmark::State& state) {
  const auto num_targets = static_cast<std::size_t>(state.range(0));
  vigil::tracker::TrackerConfig config;
  config.spatial_index = state.range(1) != 0;
  state.SetLabel(config.spatial_index ? "grid" : "brute force");
  vigil::sim::SensorConfig sensor;
  sensor.clutter_per_scan = 0.05 * static_cast<double>(num_targets);
  sensor.region_half_width_m = 200'000;  // keep target density realistic as N grows

  constexpr int kWarmup = 10;
  constexpr int kMeasured = 200;
  auto scenario = vigil::sim::Scenario::make_default(num_targets, sensor, 1);
  std::vector<vigil::sim::Scan> scans;
  scans.reserve(kWarmup + kMeasured);
  for (int i = 0; i < kWarmup + kMeasured; ++i) {
    scans.push_back(scenario.step(1.0));
  }

  auto warm_tracker = [&] {
    TrackManager tm(config);
    for (int i = 0; i < kWarmup; ++i) {
      tm.process(scans[static_cast<std::size_t>(i)].time_s,
                 scans[static_cast<std::size_t>(i)].detections);
    }
    return tm;
  };

  TrackManager tm = warm_tracker();
  std::size_t next = kWarmup;
  for (auto _ : state) {
    if (next == scans.size()) {
      state.PauseTiming();
      tm = warm_tracker();
      next = kWarmup;
      state.ResumeTiming();
    }
    auto stats = tm.process(scans[next].time_s, scans[next].detections);
    benchmark::DoNotOptimize(stats);
    ++next;
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
  state.counters["targets"] = static_cast<double>(num_targets);
}
BENCHMARK(BM_TrackerScan)
    ->ArgNames({"targets", "grid"})
    ->ArgsProduct({{4, 16, 64, 256, 1024}, {0, 1}})
    ->Args({4096, 1})  // brute force would take seconds per scan here
    ->Unit(benchmark::kMicrosecond);

}  // namespace
