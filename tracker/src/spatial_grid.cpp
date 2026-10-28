#include "vigil/tracker/spatial_grid.hpp"

namespace vigil::tracker {

void DetectionGrid::build(std::span<const Detection> detections, double cell_m) {
  cell_m_ = cell_m;
  entries_.clear();
  entries_.reserve(detections.size());
  for (std::size_t i = 0; i < detections.size(); ++i) {
    entries_.emplace_back(key(cell_of(detections[i].x_m), cell_of(detections[i].y_m)),
                          static_cast<std::uint32_t>(i));
  }
  // Sorting by (key, index) keeps each cell's detections in input order.
  std::ranges::sort(entries_);
}

}  // namespace vigil::tracker
