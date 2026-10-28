// vigil-tracker: receives detection reports over UDP, maintains a track picture, and
// publishes each scan's detections and tracks to a display feed.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include "vigil/messages.hpp"
#include "vigil/tracker/publish.hpp"
#include "vigil/tracker/track_manager.hpp"
#include "vigil/udp.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int /*signum*/) { g_stop = 1; }

struct Options {
  std::string bind_address = "0.0.0.0";
  std::uint16_t port = 50'000;
  std::string display_host = "127.0.0.1";
  std::uint16_t display_port = 50'001;
  bool display = true;
  bool quiet = false;
};

void print_usage() {
  std::cout << "Usage: vigil-tracker [options]\n"
               "  --bind ADDR          address to receive detections on (default 0.0.0.0)\n"
               "  --port N             UDP port to receive detections on (default 50000)\n"
               "  --display-host ADDR  where to publish the display feed (default 127.0.0.1)\n"
               "  --display-port N     display feed UDP port (default 50001)\n"
               "  --no-display         don't publish a display feed\n"
               "  --quiet              don't print the track table each scan\n";
}

Options parse_args(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << arg << '\n';
        std::exit(2);
      }
      return argv[++i];
    };

    if (arg == "--bind") {
      opt.bind_address = value();
    } else if (arg == "--port") {
      opt.port = static_cast<std::uint16_t>(std::stoul(value()));
    } else if (arg == "--display-host") {
      opt.display_host = value();
    } else if (arg == "--display-port") {
      opt.display_port = static_cast<std::uint16_t>(std::stoul(value()));
    } else if (arg == "--no-display") {
      opt.display = false;
    } else if (arg == "--quiet") {
      opt.quiet = true;
    } else if (arg == "--help" || arg == "-h") {
      print_usage();
      std::exit(0);
    } else {
      std::cerr << "unknown option " << arg << '\n';
      std::exit(2);
    }
  }
  return opt;
}

void print_picture(const vigil::DetectionReport& report, const vigil::tracker::TrackManager& tm,
                   double process_us) {
  using vigil::tracker::TrackStatus;
  std::printf("\nseq %-6u t=%8.2fs  dets=%-3zu  confirmed=%-3zu coasting=%-3zu tentative=%-3zu  %.1f us\n",
              report.seq, static_cast<double>(report.timestamp_ns) * 1e-9,
              report.detections.size(), tm.count(TrackStatus::kConfirmed),
              tm.count(TrackStatus::kCoasting), tm.count(TrackStatus::kTentative), process_us);
  std::printf("  %-6s %-10s %10s %10s %8s %8s\n", "id", "status", "x (m)", "y (m)", "spd m/s",
              "hdg deg");
  for (const auto& track : tm.tracks()) {
    if (track.status == TrackStatus::kTentative) {
      continue;
    }
    const auto pos = track.filter.position();
    const auto vel = track.filter.velocity();
    // Compass heading: 0 = north, clockwise positive.
    double heading = std::atan2(vel.x(), vel.y()) * 180.0 / std::numbers::pi;
    if (heading < 0.0) {
      heading += 360.0;
    }
    std::printf("  %-6u %-10s %10.0f %10.0f %8.1f %8.1f\n", track.id,
                std::string(vigil::tracker::to_string(track.status)).c_str(), pos.x(), pos.y(),
                vel.norm(), heading);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const Options opt = parse_args(argc, argv);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  vigil::UdpSocket socket;
  socket.bind(opt.bind_address, opt.port);
  std::cout << "vigil-tracker: listening on " << opt.bind_address << ':' << socket.local_port()
            << " (Ctrl-C to stop)\n";
  if (opt.display) {
    std::cout << "vigil-tracker: display feed -> " << opt.display_host << ':' << opt.display_port
              << '\n';
  }

  vigil::UdpSocket display_socket;
  std::uint64_t display_errors = 0;
  // The display is optional: a send failure is counted, never fatal.
  auto publish = [&](std::span<const std::byte> datagram) {
    try {
      display_socket.send_to(opt.display_host, opt.display_port, datagram);
    } catch (const std::system_error&) {
      ++display_errors;
    }
  };

  vigil::tracker::TrackManager tracker;
  std::array<std::byte, 65'536> buffer{};

  std::uint64_t reports = 0;
  std::uint64_t decode_errors = 0;
  std::uint64_t dropped = 0;
  std::uint64_t out_of_order = 0;
  std::optional<std::uint32_t> last_seq;
  double total_us = 0.0;
  double max_us = 0.0;

  while (g_stop == 0) {
    const auto size = socket.receive(buffer, std::chrono::milliseconds(200));
    if (!size) {
      continue;
    }

    const auto report = vigil::decode(std::span(buffer).first(*size));
    if (!report) {
      ++decode_errors;
      std::cerr << "dropping datagram: " << vigil::to_string(report.error()) << '\n';
      continue;
    }

    // A report numbered 0 after traffic means the simulator restarted: start a fresh picture.
    if (last_seq && report->seq == 0) {
      std::cout << "\nsensor restarted; clearing tracks\n";
      tracker = vigil::tracker::TrackManager{};
      last_seq.reset();
    }

    // Sequence numbers wrap, so the gap is computed in unsigned 32-bit arithmetic.
    if (last_seq && report->seq != static_cast<std::uint32_t>(*last_seq + 1)) {
      dropped += static_cast<std::uint32_t>(report->seq - *last_seq - 1);
    }
    last_seq = report->seq;

    const auto start = std::chrono::steady_clock::now();
    const auto stats =
        tracker.process(static_cast<double>(report->timestamp_ns) * 1e-9, report->detections);
    const double us =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
            .count();

    if (!stats) {
      ++out_of_order;
      continue;
    }
    ++reports;
    total_us += us;
    max_us = std::max(max_us, us);

    if (opt.display) {
      // Raw detections first, then the picture they produced, so a display can draw both.
      publish(std::span(buffer).first(*size));
      publish(vigil::encode(vigil::tracker::make_track_report(
          tracker, *report, static_cast<std::uint32_t>(std::lround(us * 1000.0)))));
    }

    if (!opt.quiet) {
      print_picture(*report, tracker, us);
    }
  }

  std::cout << "\nvigil-tracker: " << reports << " reports processed, " << dropped
            << " lost in transit, " << out_of_order << " out of order, " << decode_errors
            << " malformed, " << display_errors << " display send errors\n";
  if (reports > 0) {
    std::printf("processing latency: mean %.1f us, max %.1f us\n",
                total_us / static_cast<double>(reports), max_us);
  }
  return 0;
}
