#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "vigil/auth.hpp"
#include "vigil/messages.hpp"

namespace vigil::auth {
namespace {

Key key_from_seed(std::uint8_t seed) {
  std::array<std::byte, kKeySize> bytes{};
  for (std::size_t i = 0; i < kKeySize; ++i) {
    bytes[i] = static_cast<std::byte>(seed + i);
  }
  return Key(bytes);
}

std::vector<std::byte> payload(std::string_view text) {
  const auto bytes = std::as_bytes(std::span(text));
  return {bytes.begin(), bytes.end()};
}

std::vector<std::byte> from_hex(std::string_view hex) {
  std::vector<std::byte> out;
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    out.push_back(static_cast<std::byte>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
  }
  return out;
}

TEST(Auth, MatchesTheIcdWorkedExample) {
  // Key 00 01 .. 1f, epoch 1, counter 0, payload "abc". Generated independently with
  // Python's hmac module; the display gateway's tests use the same vector.
  const Key key = key_from_seed(0);
  EXPECT_EQ(key.id(), 0x1495);
  Sealer sealer(key, 1);
  EXPECT_EQ(sealer.seal(payload("abc")),
            from_hex("534c4756010095140100000000000000000000000000000061626363358950b813e954bc"
                     "d5c07a59da7d7e"));
}

TEST(Auth, RoundTripsThePayload) {
  Sealer sealer(key_from_seed(1), 42);
  Opener opener(key_from_seed(1));
  for (const std::string_view text : {"", "a", "detection report bytes"}) {
    const auto sealed = sealer.seal(payload(text));
    EXPECT_EQ(sealed.size(), text.size() + kOverhead);
    const auto opened = opener.open(sealed);
    ASSERT_TRUE(opened) << to_string(opened.error());
    EXPECT_TRUE(std::ranges::equal(*opened, payload(text)));
  }
}

TEST(Auth, RejectsPlainMessages) {
  Opener opener(key_from_seed(1));
  DetectionReport report;
  report.detections = {{1.0, 2.0}};
  EXPECT_EQ(opener.open(encode(report)).error(), AuthError::kNotAuthenticated);
  EXPECT_EQ(opener.open({}).error(), AuthError::kTooShort);
}

TEST(Auth, EveryFlippedBitIsRejectedAndLeavesTheWindowAlone) {
  Sealer sealer(key_from_seed(1), 7);
  Opener opener(key_from_seed(1));
  const auto good = sealer.seal(payload("tracks"));
  for (std::size_t byte = 0; byte < good.size(); ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      auto bad = good;
      bad[byte] ^= static_cast<std::byte>(1 << bit);
      EXPECT_FALSE(opener.open(bad)) << "byte " << byte << " bit " << bit;
    }
  }
  // None of the forgeries, including ones claiming huge counters or later epochs, moved
  // the replay window, so the genuine datagram is still accepted.
  EXPECT_TRUE(opener.open(good));
}

TEST(Auth, RejectsEveryTruncation) {
  Sealer sealer(key_from_seed(1), 7);
  Opener opener(key_from_seed(1));
  const auto good = sealer.seal(payload("tracks"));
  for (std::size_t n = 0; n < good.size(); ++n) {
    EXPECT_FALSE(opener.open(std::span(good).first(n))) << n << " bytes";
  }
}

TEST(Auth, RejectsAnotherKeyEvenWithACopiedKeyId) {
  Sealer attacker(key_from_seed(99), 7);
  Opener opener(key_from_seed(1));
  auto forged = attacker.seal(payload("fake target"));
  EXPECT_EQ(opener.open(forged).error(), AuthError::kWrongKey);

  // The key id is public, so an attacker can copy it. The tag still gives them away.
  const std::uint16_t id = key_from_seed(1).id();
  forged[6] = static_cast<std::byte>(id & 0xFF);
  forged[7] = static_cast<std::byte>(id >> 8);
  EXPECT_EQ(opener.open(forged).error(), AuthError::kBadTag);
}

TEST(Auth, RejectsReplays) {
  Sealer sealer(key_from_seed(1), 7);
  Opener opener(key_from_seed(1));
  const auto first = sealer.seal(payload("one"));
  const auto second = sealer.seal(payload("two"));
  EXPECT_TRUE(opener.open(first));
  EXPECT_TRUE(opener.open(second));
  EXPECT_EQ(opener.open(first).error(), AuthError::kReplayed);
  EXPECT_EQ(opener.open(second).error(), AuthError::kReplayed);
}

TEST(Auth, AcceptsALaterRunOfTheSenderAndRejectsTheEarlierOne) {
  Opener opener(key_from_seed(1));
  Sealer first_run(key_from_seed(1), 1'000);
  const auto old = first_run.seal(payload("before restart"));
  const auto old_unseen = first_run.seal(payload("captured, never delivered"));
  EXPECT_TRUE(opener.open(old));

  Sealer second_run(key_from_seed(1), 2'000);  // restarted: counter back to 0
  EXPECT_TRUE(opener.open(second_run.seal(payload("after restart"))));
  EXPECT_EQ(opener.open(old_unseen).error(), AuthError::kStaleEpoch);
}

TEST(ReplayWindow, AcceptsReorderingWithinTheWindowExactlyOnce) {
  std::vector<std::uint64_t> counters(ReplayWindow::kWidth);
  std::iota(counters.begin(), counters.end(), 100);
  std::ranges::shuffle(counters, std::mt19937_64(3));

  ReplayWindow window;
  window.accept(1, 100 + ReplayWindow::kWidth - 1);  // the newest arrives first
  for (const std::uint64_t c : counters) {
    if (c == 100 + ReplayWindow::kWidth - 1) continue;
    ASSERT_TRUE(window.check(1, c)) << c;
    window.accept(1, c);
    EXPECT_EQ(window.check(1, c).error(), AuthError::kReplayed) << c;
  }
  EXPECT_EQ(window.check(1, 99).error(), AuthError::kTooOld);
}

TEST(ReplayWindow, SlidesForwardAndForgetsWhatFellOut) {
  ReplayWindow window;
  window.accept(1, 0);
  window.accept(1, 10);
  EXPECT_EQ(window.check(1, 0).error(), AuthError::kReplayed);
  EXPECT_TRUE(window.check(1, 5));  // a gap inside the window can still arrive late

  window.accept(1, 10 + ReplayWindow::kWidth);  // jump further than the window is wide
  EXPECT_EQ(window.check(1, 10).error(), AuthError::kTooOld);
  EXPECT_TRUE(window.check(1, 10 + ReplayWindow::kWidth - 1));
  EXPECT_EQ(window.check(1, 10 + ReplayWindow::kWidth).error(), AuthError::kReplayed);
  EXPECT_EQ(window.check(0, 1'000'000).error(), AuthError::kStaleEpoch);
}

TEST(Auth, AFullDetectionReportStillFitsOneDatagram) {
  DetectionReport report;
  report.detections.resize(kMaxDetections);
  Sealer sealer(key_from_seed(1), 1);
  EXPECT_LE(sealer.seal(encode(report)).size(), wire::kMaxDatagramSize);
  report.detections.push_back({});
  EXPECT_GT(sealer.seal(encode(report)).size(), wire::kMaxDatagramSize);
}

class KeyFile : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = std::filesystem::temp_directory_path() /
            ("vigil_key_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
             "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
  }
  void TearDown() override { std::filesystem::remove(path_); }

  void write(const std::string& text, std::filesystem::perms perms) {
    std::ofstream(path_) << text;
    std::filesystem::permissions(path_, perms);
  }

  std::filesystem::path path_;
};

constexpr auto kOwnerOnly = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
const std::string kHexKey = "000102030405060708090a0b0c0d0e0f101112131415161718191A1B1C1D1E1F";

TEST_F(KeyFile, LoadsAHexKey) {
  write(kHexKey + "\n", kOwnerOnly);
  const auto key = load_key_file(path_);
  ASSERT_TRUE(key) << key.error();
  EXPECT_EQ(key->id(), key_from_seed(0).id());
}

TEST_F(KeyFile, RefusesAKeyOtherUsersCanRead) {
  write(kHexKey, kOwnerOnly | std::filesystem::perms::others_read);
  const auto key = load_key_file(path_);
  ASSERT_FALSE(key);
  EXPECT_NE(key.error().find("chmod 600"), std::string::npos);
}

TEST_F(KeyFile, RefusesMalformedKeys) {
  write(kHexKey.substr(2), kOwnerOnly);
  EXPECT_FALSE(load_key_file(path_));
  write("zz" + kHexKey.substr(2), kOwnerOnly);
  EXPECT_FALSE(load_key_file(path_));
  EXPECT_FALSE(load_key_file(path_.string() + ".missing"));
}

}  // namespace
}  // namespace vigil::auth
