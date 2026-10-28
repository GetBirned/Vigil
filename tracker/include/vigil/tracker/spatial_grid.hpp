#pragma once

// Uniform grid over a scan's detections, for finding the detections near a track without
// testing every one. Built once per scan; lookups are by axis-aligned box.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "vigil/messages.hpp"

namespace vigil::tracker {

class DetectionGrid {
 public:
  // Buckets `detections` into square cells `cell_m` metres wide. Indices refer to the span.
  void build(std::span<const Detection> detections, double cell_m);

  // Calls visit(index) for every detection in a cell that overlaps the box
  // [x0, x1] x [y0, y1]. That is a superset of the detections inside the box; callers
  // still apply their exact test. Returns false, visiting nothing, if the box spans more
  // than `max_columns` cell columns: the caller should scan everything instead, which is
  // cheaper than probing that many cells.
  template <typename Visit>
  bool visit_box(double x0, double y0, double x1, double y1, int max_columns, Visit&& visit) const;

  [[nodiscard]] double cell_size() const { return cell_m_; }

 private:
  // Cell coordinates are biased into unsigned range so that, for a fixed column, keys
  // increase with the row. One column of a box is then one contiguous run of the sorted
  // entries, found with two binary searches.
  static constexpr std::int64_t kBias = std::int64_t{1} << 31;
  static constexpr std::int64_t kMaxCell = kBias - 1;

  [[nodiscard]] std::int64_t cell_of(double v) const {
    return std::clamp(static_cast<std::int64_t>(std::floor(v / cell_m_)), -kBias, kMaxCell);
  }
  static std::uint64_t key(std::int64_t col, std::int64_t row) {
    return (static_cast<std::uint64_t>(col + kBias) << 32) | static_cast<std::uint64_t>(row + kBias);
  }

  double cell_m_ = 1.0;
  std::vector<std::pair<std::uint64_t, std::uint32_t>> entries_;  // (cell key, detection index)
};

template <typename Visit>
bool DetectionGrid::visit_box(double x0, double y0, double x1, double y1, int max_columns,
                              Visit&& visit) const {
  const std::int64_t c0 = cell_of(x0);
  const std::int64_t c1 = cell_of(x1);
  if (c1 - c0 + 1 > max_columns) {
    return false;
  }
  const std::int64_t r0 = cell_of(y0);
  const std::int64_t r1 = cell_of(y1);
  const auto by_key = [](const auto& entry, std::uint64_t k) { return entry.first < k; };
  for (std::int64_t c = c0; c <= c1; ++c) {
    auto it = std::lower_bound(entries_.begin(), entries_.end(), key(c, r0), by_key);
    const std::uint64_t last = key(c, r1);
    for (; it != entries_.end() && it->first <= last; ++it) {
      visit(static_cast<std::size_t>(it->second));
    }
  }
  return true;
}

}  // namespace vigil::tracker
