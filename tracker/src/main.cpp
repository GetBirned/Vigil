// vigil-tracker: receives detection reports over UDP, maintains a track picture, and
// publishes each scan's detections and tracks to a display feed.
//
// Two threads, joined by a lock-free single-producer / single-consumer queue:
//   vigil-rx     reads datagrams off the socket, stamps their arrival time and queues them.
//                It does nothing else, so the socket is drained promptly even while the
//                tracker is busy or the terminal is slow to print.
//   vigil-track  (the main thread) decodes, tracks, publishes and prints, and keeps a
//                latency histogram for each of those stages.

#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <iostream>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "vigil/auth.hpp"
#include "vigil/latency_histogram.hpp"
#include "vigil/messages.hpp"
#include "vigil/spsc_queue.hpp"
#include "vigil/tracker/publish.hpp"
#include "vigil/tracker/track_manager.hpp"
#include "vigil/udp.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// Set from the signal handler and read by both threads, so it must be atomic, and lock-free
// so that touching it from a signal handler is safe.
std::atomic<bool> g_stop{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void on_signal(int /*signum*/) { g_stop.store(true, std::memory_order_relaxed); }

// Reports that can wait for the tracker. At one report per scan that is minutes of backlog;
// it only fills if the tracker falls badly behind.
constexpr std::size_t kQueueDepth = 256;

// One datagram as it came off the socket, stamped on arrival so queueing delay is measured.
struct Datagram {
  std::vector<std::byte> bytes;
  Clock::time_point received;
  Clock::time_point authenticated;  // same as `received` when running --insecure
  bool end_of_stream = false;  // queued once, last, when the receive thread stops
};

struct ReceiveStats {
  std::uint64_t datagrams = 0;
  std::uint64_t queue_full = 0;  // dropped because the tracker had fallen behind
  // Failed authentication, by reason (indexed by vigil::auth::AuthError).
  std::array<std::uint64_t, vigil::auth::kAuthErrorCount> rejected{};
};

// Shows up in top -H, perf and gdb's thread list.
void name_this_thread(const char* name) { pthread_setname_np(pthread_self(), name); }

std::uint64_t ns_between(Clock::time_point from, Clock::time_point to) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(to - from).count());
}

struct Options {
  std::string bind_address = "0.0.0.0";
  std::uint16_t port = 50'000;
  std::string display_host = "127.0.0.1";
  std::uint16_t display_port = 50'001;
  bool display = true;
  bool quiet = false;
  bool spin = false;
  std::string key_file;   // accept only datagrams sealed with this key
  bool insecure = false;  // or explicitly accept unauthenticated datagrams
};

void print_usage() {
  std::cout << "Usage: vigil-tracker [options]\n"
               "  --bind ADDR          address to receive detections on (default 0.0.0.0)\n"
               "  --port N             UDP port to receive detections on (default 50000)\n"
               "  --display-host ADDR  where to publish the display feed (default 127.0.0.1)\n"
               "  --display-port N     display feed UDP port (default 50001)\n"
               "  --no-display         don't publish a display feed\n"
               "  --quiet              don't print the track table each scan\n"
               "  --spin               busy-poll the receive queue instead of sleeping:\n"
               "                       lower latency, but keeps one core at 100%\n"
               "  --key-file PATH      accept only datagrams authenticated with this key, and\n"
               "                       authenticate the display feed with it too\n"
               "  --insecure           accept unauthenticated datagrams instead\n";
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
    } else if (arg == "--spin") {
      opt.spin = true;
    } else if (arg == "--key-file") {
      opt.key_file = value();
    } else if (arg == "--insecure") {
      opt.insecure = true;
    } else if (arg == "--help" || arg == "-h") {
      print_usage();
      std::exit(0);
    } else {
      std::cerr << "unknown option " << arg << '\n';
      std::exit(2);
    }
  }
  // No silent default: running without authentication has to be asked for.
  if (opt.key_file.empty() == !opt.insecure) {
    std::cerr << "choose one: --key-file PATH to require authenticated datagrams, or --insecure\n";
    std::exit(2);
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

// The vigil-rx thread. Never blocks on the tracker: if the queue is full, the newest datagram
// is dropped and counted. Stalling here instead would only move the loss into the kernel's
// socket buffer, where nobody can see it.
//
// Authentication happens here, before the queue, so forged traffic costs one HMAC and can't
// crowd genuine reports out of the queue or reach the decoder at all.
ReceiveStats receive_loop(const std::stop_token& stop, vigil::UdpSocket& socket,
                          vigil::SpscQueue<Datagram>& queue,
                          std::optional<vigil::auth::Opener> opener) {
  name_this_thread("vigil-rx");
  ReceiveStats stats;
  std::array<std::byte, 65'536> buffer{};
  std::optional<Clock::time_point> last_warning;
  while (!stop.stop_requested() && !g_stop.load(std::memory_order_relaxed)) {
    const auto size = socket.receive(buffer, std::chrono::milliseconds(200));
    if (!size) {
      continue;
    }
    const auto received = Clock::now();
    ++stats.datagrams;

    std::span<const std::byte> message = std::span(buffer).first(*size);
    if (opener) {
      const auto opened = opener->open(message);
      if (!opened) {
        const auto reason = static_cast<std::size_t>(opened.error());
        ++stats.rejected[reason];
        // At most one line a second, so a flood of forgeries can't flood the log too.
        if (!last_warning || received - *last_warning >= std::chrono::seconds(1)) {
          last_warning = received;
          std::fprintf(stderr, "vigil-rx: rejected datagram: %s (%llu for this reason so far)\n",
                       std::string(vigil::auth::to_string(opened.error())).c_str(),
                       static_cast<unsigned long long>(stats.rejected[reason]));
        }
        continue;
      }
      message = *opened;
    }

    Datagram datagram{.bytes = {message.begin(), message.end()},
                      .received = received,
                      .authenticated = opener ? Clock::now() : received};
    if (!queue.try_push(std::move(datagram))) {
      ++stats.queue_full;
    }
  }
  // Tell the tracker nothing more is coming. It is draining the queue, so this only waits
  // while the queue is full, and gives up if the tracker itself is shutting down.
  Datagram end{
      .bytes = {}, .received = Clock::now(), .authenticated = Clock::now(), .end_of_stream = true};
  while (!queue.try_push(std::move(end)) && !stop.stop_requested()) {
    std::this_thread::yield();
  }
  return stats;
}

struct StageLatency {
  vigil::LatencyHistogram authenticate;  // HMAC and replay check, on the receive thread
  vigil::LatencyHistogram queue;    // authenticated -> picked up by the tracker
  vigil::LatencyHistogram decode;   // datagram -> DetectionReport
  vigil::LatencyHistogram track;    // predict, gate, associate, update, manage
  vigil::LatencyHistogram publish;  // encode and send to the display
  vigil::LatencyHistogram total;    // arrival on the socket -> track picture sent
};

void print_latency(const StageLatency& latency) {
  if (latency.total.count() == 0) {
    return;
  }
  std::printf("\nlatency per report (us)       p50       p90       p99     p99.9       max\n");
  const auto row = [](const char* name, const vigil::LatencyHistogram& h) {
    const auto us = [](std::uint64_t ns) { return static_cast<double>(ns) * 1e-3; };
    std::printf("  %-22s %9.1f %9.1f %9.1f %9.1f %9.1f\n", name, us(h.quantile(0.5)),
                us(h.quantile(0.9)), us(h.quantile(0.99)), us(h.quantile(0.999)), us(h.max()));
  };
  if (latency.authenticate.max() > 0) {
    row("authenticate", latency.authenticate);
  }
  row("queue wait", latency.queue);
  row("decode", latency.decode);
  row("track", latency.track);
  row("publish", latency.publish);
  row("socket to display", latency.total);
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

  // The receive thread verifies incoming datagrams; this thread seals the display feed.
  std::optional<vigil::auth::Opener> opener;
  std::optional<vigil::auth::Sealer> display_sealer;
  if (opt.insecure) {
    std::cout << "vigil-tracker: WARNING: --insecure: accepting unauthenticated datagrams\n";
  } else {
    const auto key = vigil::auth::load_key_file(opt.key_file);
    if (!key) {
      std::cerr << "vigil-tracker: " << key.error() << '\n';
      return 2;
    }
    opener.emplace(*key);
    display_sealer.emplace(*key, vigil::auth::epoch_now());
    std::cout << "vigil-tracker: accepting only datagrams authenticated with key "
              << std::format("{:04x}", key->id()) << '\n';
  }

  vigil::UdpSocket display_socket;
  std::uint64_t display_errors = 0;
  // The display is optional: a send failure is counted, never fatal.
  auto publish = [&](std::span<const std::byte> message) {
    try {
      if (display_sealer) {
        display_socket.send_to(opt.display_host, opt.display_port, display_sealer->seal(message));
      } else {
        display_socket.send_to(opt.display_host, opt.display_port, message);
      }
    } catch (const std::system_error&) {
      ++display_errors;
    }
  };

  vigil::SpscQueue<Datagram> queue(kQueueDepth);
  ReceiveStats rx_stats;
  // Asked to stop and joined when it goes out of scope, including if the tracker throws.
  std::jthread receiver([&, opener = std::move(opener)](const std::stop_token& stop) mutable {
    rx_stats = receive_loop(stop, socket, queue, std::move(opener));
  });
  name_this_thread("vigil-track");

  vigil::tracker::TrackManager tracker;
  StageLatency latency;

  std::uint64_t reports = 0;
  std::uint64_t decode_errors = 0;
  std::uint64_t missing = 0;
  std::uint64_t out_of_order = 0;
  std::size_t max_queue_depth = 0;
  std::optional<std::uint32_t> last_seq;

  for (;;) {
    auto datagram = queue.try_pop();
    if (!datagram) {
      // Sleeping costs a kernel wake-up (tens of microseconds) per report; spinning trades a
      // core for not paying it.
      if (!opt.spin) {
        queue.wait();
      }
      continue;
    }
    if (datagram->end_of_stream) {
      break;
    }
    const auto dequeued = Clock::now();
    max_queue_depth = std::max(max_queue_depth, queue.size_approx() + 1);
    latency.authenticate.record(ns_between(datagram->received, datagram->authenticated));
    latency.queue.record(ns_between(datagram->authenticated, dequeued));

    const auto report = vigil::decode(datagram->bytes);
    const auto decoded = Clock::now();
    if (!report) {
      ++decode_errors;
      std::cerr << "dropping datagram: " << vigil::to_string(report.error()) << '\n';
      continue;
    }
    latency.decode.record(ns_between(dequeued, decoded));

    // A report numbered 0 after traffic means the simulator restarted: start a fresh picture.
    if (last_seq && report->seq == 0) {
      std::cout << "\nsensor restarted; clearing tracks\n";
      tracker = vigil::tracker::TrackManager{};
      last_seq.reset();
    }

    // Sequence numbers wrap, so the gap is computed in unsigned 32-bit arithmetic.
    if (last_seq && report->seq != static_cast<std::uint32_t>(*last_seq + 1)) {
      missing += static_cast<std::uint32_t>(report->seq - *last_seq - 1);
    }
    last_seq = report->seq;

    const auto stats =
        tracker.process(static_cast<double>(report->timestamp_ns) * 1e-9, report->detections);
    const auto tracked = Clock::now();
    if (!stats) {
      ++out_of_order;
      continue;
    }
    ++reports;
    const std::uint64_t track_ns = ns_between(decoded, tracked);
    latency.track.record(track_ns);

    if (opt.display) {
      // Raw detections first, then the picture they produced, so a display can draw both.
      publish(datagram->bytes);
      publish(vigil::encode(vigil::tracker::make_track_report(
          tracker, *report,
          static_cast<std::uint32_t>(
              std::min<std::uint64_t>(track_ns, std::numeric_limits<std::uint32_t>::max())))));
    }
    const auto published = Clock::now();
    latency.publish.record(ns_between(tracked, published));
    latency.total.record(ns_between(datagram->received, published));

    // Printing is outside the measured path, and the receive thread keeps draining the
    // socket however long the terminal takes.
    if (!opt.quiet) {
      print_picture(*report, tracker, static_cast<double>(track_ns) * 1e-3);
    }
  }
  receiver.join();  // it has already finished: it sent the end-of-stream marker

  // Queue drops are also sequence gaps, except for any after the last report processed.
  std::cout << "\nvigil-tracker: " << rx_stats.datagrams << " datagrams received, " << reports
            << " reports processed, " << rx_stats.queue_full
            << " dropped at a full receive queue, " << missing
            << " missing by sequence number (queue drops included), " << out_of_order
            << " out of order, " << decode_errors << " malformed, " << display_errors
            << " display send errors\n";
  std::cout << "receive queue: " << queue.capacity() << " slots, high-water mark "
            << max_queue_depth << '\n';
  for (std::size_t reason = 0; reason < rx_stats.rejected.size(); ++reason) {
    if (rx_stats.rejected[reason] > 0) {
      std::cout << "rejected: " << rx_stats.rejected[reason] << ' '
                << vigil::auth::to_string(static_cast<vigil::auth::AuthError>(reason)) << '\n';
    }
  }
  print_latency(latency);
  return 0;
}
