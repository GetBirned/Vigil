#pragma once

// Bounded single-producer / single-consumer queue: a lock-free ring buffer that hands values
// from exactly one producer thread to exactly one consumer thread.
//
// Each index has one writer: the producer owns tail_ and the consumer owns head_. A side
// publishes its index with a release store and reads the other side's with an acquire load,
// and that pairing is all the synchronisation a slot needs. Push and pop never take a lock
// or do a read-modify-write. Each side also caches the last index it saw from the other side
// and re-reads the shared one only when the queue looks full (or empty), so in steady state
// the two cores don't bounce the same cache line back and forth on every call.

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vigil {

// Keeps the producer's and the consumer's fields on separate cache lines (no false sharing).
inline constexpr std::size_t kCacheLineBytes = 64;

template <typename T>
class SpscQueue {
 public:
  // `capacity` is rounded up to a power of two so an index maps to a slot with a mask.
  explicit SpscQueue(std::size_t capacity)
      : slots_(std::bit_ceil(std::max<std::size_t>(capacity, 1))),
        mask_(static_cast<std::uint32_t>(slots_.size() - 1)) {
    if (slots_.size() > kMaxCapacity) {
      throw std::length_error("SpscQueue capacity too large");
    }
  }

  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  [[nodiscard]] std::size_t capacity() const { return slots_.size(); }

  // Producer only. Returns false, leaving `value` untouched, if the queue is full.
  bool try_push(T&& value) {
    const std::uint32_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - head_cache_ == slots_.size()) {
      // Acquire: the consumer has finished moving out of the slot we're about to reuse.
      head_cache_ = head_.load(std::memory_order_acquire);
      if (tail - head_cache_ == slots_.size()) {
        return false;
      }
    }
    slots_[tail & mask_] = std::move(value);
    // Release: the slot's new contents are visible before the consumer can see the index.
    tail_.store(tail + 1, std::memory_order_release);
    tail_.notify_one();  // no system call unless the consumer is blocked in wait()
    return true;
  }

  // Consumer only. Returns std::nullopt if the queue is empty.
  std::optional<T> try_pop() {
    const std::uint32_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_cache_) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (head == tail_cache_) {
        return std::nullopt;
      }
    }
    std::optional<T> value(std::move(slots_[head & mask_]));
    head_.store(head + 1, std::memory_order_release);
    return value;
  }

  // Consumer only. Blocks while the queue is empty, sleeping in the kernel rather than
  // spinning, and wakes as soon as the producer pushes.
  void wait() const {
    tail_.wait(head_.load(std::memory_order_relaxed), std::memory_order_acquire);
  }

  // Approximate number of queued values; exact only when called from the consumer while the
  // producer is idle. For monitoring (queue depth, high-water mark), not for control flow.
  [[nodiscard]] std::size_t size_approx() const {
    return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
  }

 private:
  // Indices count up forever and wrap at 2^32; unsigned subtraction keeps `tail - head`
  // correct across the wrap as long as the capacity is at most 2^31. 32 bits (rather than
  // 64) also lets atomic wait/notify map straight onto a Linux futex.
  static constexpr std::size_t kMaxCapacity = std::size_t{1} << 31;

  std::vector<T> slots_;
  std::uint32_t mask_;

  // Consumer-owned: its index, and its cached copy of the producer's.
  alignas(kCacheLineBytes) std::atomic<std::uint32_t> head_{0};
  std::uint32_t tail_cache_ = 0;

  // Producer-owned: its index, and its cached copy of the consumer's.
  alignas(kCacheLineBytes) std::atomic<std::uint32_t> tail_{0};
  std::uint32_t head_cache_ = 0;
};

}  // namespace vigil
