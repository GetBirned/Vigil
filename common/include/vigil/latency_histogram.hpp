#pragma once

// Fixed-size latency histogram with log-linear buckets, in the style of HdrHistogram.
//
// Values are grouped by power of two, and each power of two is split into 16 equal
// sub-buckets, so a bucket is never wider than 1/16 (6.25%) of the values in it. That covers
// everything from 1 ns to centuries in under 8 KiB. Recording is a few integer operations
// and an increment, with no allocation, so it is safe to call on a hot path.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace vigil {

class LatencyHistogram {
 public:
  void record(std::uint64_t value_ns);

  [[nodiscard]] std::uint64_t count() const { return count_; }
  [[nodiscard]] std::uint64_t min() const { return count_ == 0 ? 0 : min_; }
  [[nodiscard]] std::uint64_t max() const { return max_; }
  [[nodiscard]] double mean() const;

  // The value at quantile `q` (0..1): the smallest bucket that holds at least q of all
  // recorded values, reported as that bucket's upper edge so latency is never understated,
  // and clamped to the observed min and max. Returns 0 if nothing has been recorded.
  [[nodiscard]] std::uint64_t quantile(double q) const;

 private:
  static constexpr int kSubBucketBits = 4;
  static constexpr std::size_t kSubBuckets = std::size_t{1} << kSubBucketBits;
  // Values below kSubBuckets get one exact bucket each; every power of two above that gets
  // kSubBuckets more.
  static constexpr std::size_t kBuckets = (64 - kSubBucketBits + 1) * kSubBuckets;

  static std::size_t bucket_of(std::uint64_t value);
  static std::uint64_t upper_edge(std::size_t bucket);

  std::array<std::uint64_t, kBuckets> counts_{};
  std::uint64_t count_ = 0;
  std::uint64_t min_ = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t max_ = 0;
  long double sum_ = 0;
};

}  // namespace vigil
