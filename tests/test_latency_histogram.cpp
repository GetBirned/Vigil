#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <random>

#include "vigil/latency_histogram.hpp"

namespace vigil {
namespace {

TEST(LatencyHistogram, IsEmptyToStartWith) {
  const LatencyHistogram h;
  EXPECT_EQ(h.count(), 0U);
  EXPECT_EQ(h.min(), 0U);
  EXPECT_EQ(h.max(), 0U);
  EXPECT_EQ(h.mean(), 0.0);
  EXPECT_EQ(h.quantile(0.99), 0U);
}

TEST(LatencyHistogram, SmallValuesAreExact) {
  LatencyHistogram h;
  for (std::uint64_t v = 1; v <= 10; ++v) {
    h.record(v);
  }
  EXPECT_EQ(h.count(), 10U);
  EXPECT_EQ(h.min(), 1U);
  EXPECT_EQ(h.max(), 10U);
  EXPECT_DOUBLE_EQ(h.mean(), 5.5);
  EXPECT_EQ(h.quantile(0.0), 1U);
  EXPECT_EQ(h.quantile(0.5), 5U);
  EXPECT_EQ(h.quantile(0.9), 9U);
  EXPECT_EQ(h.quantile(1.0), 10U);
}

TEST(LatencyHistogram, QuantilesAreWithinBucketPrecisionAndNeverLow) {
  // 1..100,000 ns uniformly: the true q-quantile is q * 100,000.
  LatencyHistogram h;
  for (std::uint64_t v = 1; v <= 100'000; ++v) {
    h.record(v);
  }
  for (const double q : {0.5, 0.9, 0.99, 0.999}) {
    const double truth = q * 100'000;
    const auto got = static_cast<double>(h.quantile(q));
    EXPECT_GE(got, truth) << "q=" << q;
    EXPECT_LE(got, truth * (1.0 + 1.0 / 16)) << "q=" << q;
  }
}

TEST(LatencyHistogram, OneSlowOutlierShowsUpOnlyAtTheTail) {
  LatencyHistogram h;
  for (int i = 0; i < 999; ++i) {
    h.record(1'000);
  }
  h.record(5'000'000);
  EXPECT_LE(h.quantile(0.99), 1'063U);
  EXPECT_EQ(h.quantile(1.0), 5'000'000U);
  EXPECT_EQ(h.max(), 5'000'000U);
}

TEST(LatencyHistogram, HandlesTheWholeRangeOfUint64) {
  LatencyHistogram h;
  constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
  h.record(0);
  h.record(kMax);
  EXPECT_EQ(h.quantile(0.0), 0U);
  EXPECT_EQ(h.quantile(1.0), kMax);

  // Every power of two and its neighbours lands in a bucket whose upper edge bounds it
  // within 1/16.
  for (int bit = 4; bit < 64; ++bit) {
    for (const std::uint64_t v : {(std::uint64_t{1} << bit) - 1, std::uint64_t{1} << bit,
                                  (std::uint64_t{1} << bit) + 1}) {
      LatencyHistogram one;
      one.record(v);
      one.record(kMax);  // so the result isn't simply clamped to max()
      const std::uint64_t edge = one.quantile(0.5);
      EXPECT_GE(edge, v);
      EXPECT_LE(static_cast<long double>(edge - v), static_cast<long double>(v) / 16) << v;
    }
  }
}

}  // namespace
}  // namespace vigil
