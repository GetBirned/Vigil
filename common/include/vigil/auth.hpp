#pragma once

// Authenticated datagrams: an envelope that wraps any Vigil message so the receiver can
// reject forged, tampered and replayed traffic. The authoritative description is the
// "Security" section of docs/ICD.md.
//
//   offset  size  field
//        0     4  magic "VGLS"
//        4     2  envelope version
//        6     2  key id (identifies the shared key without revealing it)
//        8     8  epoch: the sender's start time, which must increase across restarts
//       16     8  counter: 0, 1, 2, ... per datagram within an epoch
//       24     n  payload: an ordinary Vigil message, unchanged
//     24+n    16  tag: HMAC-SHA256 over bytes [0, 24+n), truncated to 128 bits
//
// The receiver checks, in order: the envelope parses and names its key, the (epoch, counter)
// pair is new according to a sliding replay window, and the tag matches (compared in
// constant time). Only then does the replay window move. Integrity and authenticity only:
// the payload is not encrypted.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "vigil/messages.hpp"

namespace vigil::auth {

inline constexpr std::uint32_t kMagic = 0x56474C53;  // "VGLS"
inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::size_t kHeaderSize = 24;
inline constexpr std::size_t kTagSize = 16;
inline constexpr std::size_t kOverhead = kHeaderSize + kTagSize;
inline constexpr std::size_t kKeySize = 32;

// Detections that still fit one unfragmented datagram once the envelope is added.
inline constexpr std::size_t kMaxDetections =
    (wire::kMaxDatagramSize - kOverhead - wire::kHeaderSize) / wire::kDetectionSize;

class Key {
 public:
  explicit Key(const std::array<std::byte, kKeySize>& bytes);

  // A 16-bit fingerprint derived from the key (an HMAC of a fixed label), so the two ends
  // can tell they hold different keys without the key itself going on the wire.
  [[nodiscard]] std::uint16_t id() const { return id_; }
  [[nodiscard]] std::span<const std::byte, kKeySize> bytes() const { return bytes_; }

 private:
  std::array<std::byte, kKeySize> bytes_;
  std::uint16_t id_;
};

// HMAC-SHA256 with the key and algorithm set up once. OpenSSL 3's one-shot HMAC() looks the
// algorithm up again on every call, which costs more than hashing a short datagram.
class HmacSha256 {
 public:
  static constexpr std::size_t kSize = 32;

  explicit HmacSha256(const Key& key);
  ~HmacSha256();
  HmacSha256(HmacSha256&&) noexcept;
  HmacSha256& operator=(HmacSha256&&) noexcept;

  std::array<std::byte, kSize> operator()(std::span<const std::byte> data);

 private:
  struct Context;
  std::unique_ptr<Context> ctx_;
};

// Reads a key file holding 64 hex digits (32 random bytes), e.g. from
// `openssl rand -hex 32`. Refuses a file that other users can read or write.
std::expected<Key, std::string> load_key_file(const std::filesystem::path& path);

// Wraps payloads for one sender. Not thread-safe: one Sealer per sending thread.
class Sealer {
 public:
  Sealer(Key key, std::uint64_t epoch);

  std::vector<std::byte> seal(std::span<const std::byte> payload);

 private:
  Key key_;
  HmacSha256 hmac_;
  std::uint64_t epoch_;
  std::uint64_t counter_ = 0;
};

// The sender's epoch for a fresh start: wall-clock nanoseconds since the Unix epoch.
std::uint64_t epoch_now();

enum class AuthError {
  kTooShort,
  kNotAuthenticated,  // no envelope: plain (or forged) traffic
  kUnsupportedVersion,
  kWrongKey,          // sealed with a key we don't hold
  kStaleEpoch,        // from an earlier run of the sender
  kTooOld,            // behind the replay window
  kReplayed,          // already accepted once
  kBadTag,            // forged or modified in transit
};
inline constexpr std::size_t kAuthErrorCount = 8;

std::string_view to_string(AuthError error);

// Sliding-window replay protection in the style of IPsec (RFC 4303 section 3.4.3): accepts
// each (epoch, counter) once, tolerates reordering within the last kWidth counters, and
// rejects anything older. A newer epoch means the sender restarted, and starts over.
class ReplayWindow {
 public:
  static constexpr std::uint64_t kWidth = 64;

  // Would (epoch, counter) be accepted? Doesn't change anything.
  [[nodiscard]] std::expected<void, AuthError> check(std::uint64_t epoch,
                                                     std::uint64_t counter) const;
  // Records (epoch, counter) as seen. Call only after check() passed and the tag verified,
  // so that unauthenticated traffic can never move the window.
  void accept(std::uint64_t epoch, std::uint64_t counter);

 private:
  bool started_ = false;
  std::uint64_t epoch_ = 0;
  std::uint64_t top_ = 0;   // highest counter accepted in this epoch
  std::uint64_t seen_ = 0;  // bit i set: counter top_ - i has been accepted
};

// Verifies datagrams from one sender. Not thread-safe: one Opener per receiving thread.
class Opener {
 public:
  explicit Opener(Key key);

  // On success, the payload: a view into `datagram`.
  std::expected<std::span<const std::byte>, AuthError> open(std::span<const std::byte> datagram);

 private:
  Key key_;
  HmacSha256 hmac_;
  ReplayWindow window_;
};

}  // namespace vigil::auth
