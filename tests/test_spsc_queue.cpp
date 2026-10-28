#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <thread>

#include "vigil/spsc_queue.hpp"

namespace vigil {
namespace {

TEST(SpscQueue, RoundsCapacityUpToAPowerOfTwo) {
  EXPECT_EQ(SpscQueue<int>(5).capacity(), 8U);
  EXPECT_EQ(SpscQueue<int>(8).capacity(), 8U);
  EXPECT_EQ(SpscQueue<int>(0).capacity(), 1U);
}

TEST(SpscQueue, IsFirstInFirstOut) {
  SpscQueue<int> q(4);
  EXPECT_FALSE(q.try_pop());
  EXPECT_TRUE(q.try_push(1));
  EXPECT_TRUE(q.try_push(2));
  EXPECT_EQ(q.size_approx(), 2U);
  EXPECT_EQ(q.try_pop(), 1);
  EXPECT_EQ(q.try_pop(), 2);
  EXPECT_FALSE(q.try_pop());
}

TEST(SpscQueue, RefusesPushWhenFullAndKeepsTheValue) {
  SpscQueue<std::unique_ptr<int>> q(2);
  EXPECT_TRUE(q.try_push(std::make_unique<int>(1)));
  EXPECT_TRUE(q.try_push(std::make_unique<int>(2)));
  auto extra = std::make_unique<int>(3);
  EXPECT_FALSE(q.try_push(std::move(extra)));
  ASSERT_NE(extra, nullptr);  // a refused push must not consume the value
  EXPECT_EQ(*extra, 3);

  EXPECT_EQ(**q.try_pop(), 1);
  EXPECT_TRUE(q.try_push(std::move(extra)));  // room again after one pop
  EXPECT_EQ(**q.try_pop(), 2);
  EXPECT_EQ(**q.try_pop(), 3);
}

TEST(SpscQueue, WrapsAroundTheRingManyTimes) {
  SpscQueue<std::uint64_t> q(4);
  for (std::uint64_t i = 0; i < 10'000; ++i) {
    ASSERT_TRUE(q.try_push(std::uint64_t{i}));
    ASSERT_EQ(q.try_pop(), i);
  }
}

// Two real threads through a tiny queue, so it is constantly full or empty and every race
// between the indices gets exercised. The ThreadSanitizer preset runs this too.
TEST(SpscQueue, DeliversEveryValueInOrderAcrossThreads) {
  constexpr std::uint64_t kCount = 1'000'000;
  SpscQueue<std::uint64_t> q(8);

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kCount; ++i) {
      while (!q.try_push(std::uint64_t{i})) {
        std::this_thread::yield();
      }
    }
  });

  std::uint64_t expected = 0;
  while (expected < kCount) {
    if (const auto value = q.try_pop()) {
      ASSERT_EQ(*value, expected);
      ++expected;
    }
  }
  producer.join();
  EXPECT_FALSE(q.try_pop());
}

TEST(SpscQueue, WaitBlocksUntilAPush) {
  SpscQueue<int> q(4);
  std::thread producer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_TRUE(q.try_push(42));
  });
  q.wait();  // must not return before the push...
  EXPECT_EQ(q.try_pop(), 42);  // ...and must have seen it when it does
  producer.join();
}

TEST(SpscQueue, WaitReturnsAtOnceWhenNotEmpty) {
  SpscQueue<int> q(4);
  ASSERT_TRUE(q.try_push(7));
  q.wait();
  EXPECT_EQ(q.try_pop(), 7);
}

}  // namespace
}  // namespace vigil
