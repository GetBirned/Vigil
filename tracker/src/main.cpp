// vigil-tracker: receives detection reports over UDP and maintains a track picture.

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

#include "vigil/messages.hpp"
#include "vigil/tracker/track_manager.hpp"
#include "vigil/udp.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int /*signum*/) { g_stop = 1; }

struct Options {
  std::string bind_address = "0.0.0.0";
  std::uint16_t port = 50'000;
  bool quiet = false;
};

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
    } else if (arg == "--quiet") {
      opt.quiet = true;
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: vigil-tracker [--bind ADDR] [--port N] [--quiet]\n";
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

    if (!opt.quiet) {
      print_picture(*report, tracker, us);
    }
  }

  std::cout << "\nvigil-tracker: " << reports << " reports processed, " << dropped
            << " lost in transit, " << out_of_order << " out of order, " << decode_errors
            << " malformed\n";
  if (reports > 0) {
    std::printf("processing latency: mean %.1f us, max %.1f us\n",
                total_us / static_cast<double>(reports), max_us);
  }
  return 0;
}
