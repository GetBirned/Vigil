#pragma once

// Turns the tracker's internal state into the TrackReport message sent to displays.

#include <cstdint>

#include "vigil/messages.hpp"
#include "vigil/tracker/track_manager.hpp"

namespace vigil::tracker {

// Snapshot of every track (tentative ones included) after processing `scan`.
TrackReport make_track_report(const TrackManager& tracker, const DetectionReport& scan,
                              std::uint32_t processing_ns);

}  // namespace vigil::tracker
