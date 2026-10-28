// vigil-sim: flies a scenario and streams detection reports to the tracker over UDP.

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#include "vigil/messages.hpp"
#include "vigil/sim/scenario.hpp"
#include "vigil/udp.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int /*signum*/) { g_stop = 1; }

struct Options {
  std::string host = "127.0.0.1";
  std::uint16_t port = 50'000;
  double rate_hz = 1.0;
  std::size_t targets = 5;
  std::uint64_t seed = 1;
  std::uint64_t scans = 0;  // 0 = run until Ctrl-C
  bool fast = false;        // send as fast as possible instead of in real time
  vigil::sim::SensorConfig sensor;
};

void print_usage() {
  std::cout << "Usage: vigil-sim [options]\n"
               "  --host ADDR      tracker IPv4 address   (default 127.0.0.1)\n"
               "  --port N         tracker UDP port       (default 50000)\n"
               "  --rate HZ        scans per second       (default 1)\n"
               "  --targets N      number of targets      (default 5)\n"
               "  --seed N         random seed            (default 1)\n"
               "  --scans N        stop after N scans     (default 0 = forever)\n"
               "  --pd P           probability of detection (default 0.95)\n"
               "  --clutter N      mean false alarms per scan (default 2)\n"
               "  --sigma M        position noise, metres (default 25)\n"
               "  --fast           don't pace to real time\n";
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

    if (arg == "--host") {
      opt.host = value();
    } else if (arg == "--port") {
      opt.port = static_cast<std::uint16_t>(std::stoul(value()));
    } else if (arg == "--rate") {
      opt.rate_hz = std::stod(value());
    } else if (arg == "--targets") {
      opt.targets = std::stoul(value());
    } else if (arg == "--seed") {
      opt.seed = std::stoull(value());
    } else if (arg == "--scans") {
      opt.scans = std::stoull(value());
    } else if (arg == "--pd") {
      opt.sensor.probability_of_detection = std::stod(value());
    } else if (arg == "--clutter") {
      opt.sensor.clutter_per_scan = std::stod(value());
    } else if (arg == "--sigma") {
      opt.sensor.position_sigma_m = std::stod(value());
    } else if (arg == "--fast") {
      opt.fast = true;
    } else if (arg == "--help" || arg == "-h") {
      print_usage();
      std::exit(0);
    } else {
      std::cerr << "unknown option " << arg << '\n';
      print_usage();
      std::exit(2);
    }
  }
  if (opt.rate_hz <= 0.0) {
    std::cerr << "--rate must be positive\n";
    std::exit(2);
  }
  return opt;
}

}  // namespace

int main(int argc, char** argv) {
  const Options opt = parse_args(argc, argv);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  auto scenario = vigil::sim::Scenario::make_default(opt.targets, opt.sensor, opt.seed);
  vigil::UdpSocket socket;

  const double dt_s = 1.0 / opt.rate_hz;
  const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(dt_s));
  auto next_send = std::chrono::steady_clock::now();

  std::cout << "vigil-sim: " << opt.targets << " targets -> " << opt.host << ':' << opt.port
            << " at " << opt.rate_hz << " Hz" << (opt.fast ? " (fast)" : "") << '\n';

  std::uint32_t seq = 0;
  for (std::uint64_t n = 0; g_stop == 0 && (opt.scans == 0 || n < opt.scans); ++n) {
    const vigil::sim::Scan scan = scenario.step(dt_s);

    vigil::DetectionReport report;
    report.seq = seq++;
    report.sensor_id = 1;
    report.timestamp_ns = static_cast<std::uint64_t>(std::llround(scan.time_s * 1e9));
    report.detections = scan.detections;
    // One report must fit one unfragmented datagram (see docs/ICD.md). The detections are
    // already shuffled, so truncating drops a random subset.
    if (report.detections.size() > vigil::wire::kMaxDetections) {
      std::cerr << "scan " << report.seq << ": truncating " << report.detections.size()
                << " detections to " << vigil::wire::kMaxDetections << '\n';
      report.detections.resize(vigil::wire::kMaxDetections);
    }
    socket.send_to(opt.host, opt.port, vigil::encode(report));

    if (report.seq % 10 == 0) {
      std::cout << "scan " << report.seq << "  t=" << scan.time_s << "s  "
                << report.detections.size() << " detections\n";
    }

    if (!opt.fast) {
      next_send += period;
      std::this_thread::sleep_until(next_send);
    }
  }

  std::cout << "vigil-sim: sent " << seq << " reports\n";
  return 0;
}
