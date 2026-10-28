#include <gtest/gtest.h>

#include <vector>

#include "vigil/messages.hpp"
#include "vigil/tracker/publish.hpp"
#include "vigil/tracker/track_manager.hpp"

namespace vigil::tracker {
namespace {

TEST(Publish, TrackReportMirrorsTrackerState) {
  TrackManager tm;
  DetectionReport scan;
  for (std::uint32_t i = 0; i < 4; ++i) {
    scan.seq = i;
    scan.timestamp_ns = static_cast<std::uint64_t>(i) * 1'000'000'000ULL;
    scan.detections = {{100.0 * i, 50.0 * i}, {-20'000.0 + 3000.0 * i, 9000.0}};
    tm.process(static_cast<double>(i), scan.detections);
  }

  const TrackReport report = make_track_report(tm, scan, 12'000);
  EXPECT_EQ(report.seq, 3U);
  EXPECT_EQ(report.timestamp_ns, 3'000'000'000ULL);
  EXPECT_EQ(report.processing_ns, 12'000U);
  EXPECT_EQ(report.detections_in_scan, 2U);
  ASSERT_EQ(report.tracks.size(), tm.tracks().size());

  for (std::size_t i = 0; i < report.tracks.size(); ++i) {
    const Track& t = tm.tracks()[i];
    const TrackState& w = report.tracks[i];
    EXPECT_EQ(w.id, t.id);
    EXPECT_EQ(w.x_m, t.filter.position().x());
    EXPECT_EQ(w.vy_mps, t.filter.velocity().y());
    EXPECT_GT(w.position_sigma_m, 0.0F);
    if (t.status == TrackStatus::kConfirmed) {
      EXPECT_EQ(w.status, WireTrackStatus::kConfirmed);
    }
  }

  // And it survives the wire.
  const auto decoded = decode_track_report(encode(report));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->tracks.size(), report.tracks.size());
}

}  // namespace
}  // namespace vigil::tracker
