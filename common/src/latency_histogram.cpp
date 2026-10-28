#include "vigil/latency_histogram.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace vigil {

std::size_t LatencyHistogram::bucket_of(std::uint64_t value) {
  if (value < kSubBuckets) {
    return static_cast<std::size_t>(value);
  }
  // For a value in [2^k, 2^(k+1)) with k >= kSubBucketBits, keep its top kSubBucketBits + 1
  // bits: the leading 1 picks the power of two, the bits after it pick the sub-bucket.
  const int shift = std::bit_width(value) - 1 - kSubBucketBits;
  const auto sub = static_cast<std::size_t>((value >> shift) & (kSubBuckets - 1));
  return static_cast<std::size_t>(shift + 1) * kSubBuckets + sub;
}

std::uint64_t LatencyHistogram::upper_edge(std::size_t bucket) {
  if (bucket < kSubBuckets) {
    return bucket;
  }
  const auto shift = static_cast<int>(bucket / kSubBuckets) - 1;
  const std::uint64_t lower = (kSubBuckets + bucket % kSubBuckets) << shift;
  return lower + ((std::uint64_t{1} << shift) - 1);
}

void LatencyHistogram::record(std::uint64_t value_ns) {
  ++counts_[bucket_of(value_ns)];
  ++count_;
  min_ = std::min(min_, value_ns);
  max_ = std::max(max_, value_ns);
  sum_ += static_cast<long double>(value_ns);
}

double LatencyHistogram::mean() const {
  return count_ == 0 ? 0.0 : static_cast<double>(sum_ / static_cast<long double>(count_));
}

std::uint64_t LatencyHistogram::quantile(double q) const {
  if (count_ == 0) {
    return 0;
  }
  // The rank (1-based) of the value we want, so q = 0 means the minimum and q = 1 the maximum.
  const auto wanted = std::max<std::uint64_t>(
      1, static_cast<std::uint64_t>(std::ceil(std::clamp(q, 0.0, 1.0) *
                                              static_cast<double>(count_))));
  std::uint64_t seen = 0;
  for (std::size_t bucket = 0; bucket < kBuckets; ++bucket) {
    seen += counts_[bucket];
    if (seen >= wanted) {
      return std::clamp(upper_edge(bucket), min_, max_);
    }
  }
  return max_;
}

}  // namespace vigil
