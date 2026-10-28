// Moving values between two threads: the lock-free SPSC ring against the textbook
// alternative, a std::deque guarded by a std::mutex. Both sides spin when the queue is full
// or empty, so this measures the hand-off itself rather than thread wake-ups.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

#include "vigil/spsc_queue.hpp"

namespace {

constexpr std::uint64_t kItemsPerIteration = 1 << 20;
constexpr std::size_t kCapacity = 1024;

class MutexQueue {
 public:
  bool try_push(std::uint64_t value) {
    const std::scoped_lock lock(mutex_);
    if (items_.size() == kCapacity) {
      return false;
    }
    items_.push_back(value);
    return true;
  }

  std::optional<std::uint64_t> try_pop() {
    const std::scoped_lock lock(mutex_);
    if (items_.empty()) {
      return std::nullopt;
    }
    const std::uint64_t value = items_.front();
    items_.pop_front();
    return value;
  }

 private:
  std::mutex mutex_;
  std::deque<std::uint64_t> items_;
};

template <typename Queue>
void transfer(benchmark::State& state, Queue& queue) {
  for (auto _ : state) {
    std::thread producer([&] {
      for (std::uint64_t i = 0; i < kItemsPerIteration; ++i) {
        while (!queue.try_push(std::uint64_t{i})) {
        }
      }
    });
    std::uint64_t sum = 0;
    for (std::uint64_t received = 0; received < kItemsPerIteration;) {
      if (const auto value = queue.try_pop()) {
        sum += *value;
        ++received;
      }
    }
    producer.join();
    benchmark::DoNotOptimize(sum);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kItemsPerIteration));
}

void BM_SpscQueue(benchmark::State& state) {
  vigil::SpscQueue<std::uint64_t> queue(kCapacity);
  transfer(state, queue);
}
BENCHMARK(BM_SpscQueue)->Unit(benchmark::kMillisecond)->UseRealTime();

void BM_MutexQueue(benchmark::State& state) {
  MutexQueue queue;
  transfer(state, queue);
}
BENCHMARK(BM_MutexQueue)->Unit(benchmark::kMillisecond)->UseRealTime();

}  // namespace
