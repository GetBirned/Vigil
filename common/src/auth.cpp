#include "vigil/auth.hpp"

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/params.h>

#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

namespace vigil::auth {
namespace {

template <typename T>
void put_le(std::vector<std::byte>& out, T value) {
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFF));
  }
}

template <typename T>
T get_le(std::span<const std::byte> in, std::size_t offset) {
  T value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    value = static_cast<T>(value | (static_cast<T>(in[offset + i]) << (8 * i)));
  }
  return value;
}

using Mac = std::array<unsigned char, EVP_MAX_MD_SIZE>;

// One-shot HMAC-SHA256 (32 bytes of the returned array are used). Only for one-off use;
// per-datagram work goes through HmacSha256.
Mac hmac_sha256(std::span<const std::byte, kKeySize> key, std::span<const std::byte> data) {
  Mac mac{};
  unsigned int length = 0;
  if (HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
           reinterpret_cast<const unsigned char*>(data.data()), data.size(), mac.data(),
           &length) == nullptr) {
    throw std::runtime_error("HMAC-SHA256 failed");
  }
  return mac;
}

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

// ------------------------------------------------------------------ Key

Key::Key(const std::array<std::byte, kKeySize>& bytes) : bytes_(bytes), id_(0) {
  constexpr std::string_view kLabel = "vigil key id";
  const Mac mac = hmac_sha256(bytes_, std::as_bytes(std::span(kLabel)));
  id_ = static_cast<std::uint16_t>(mac[0] | (mac[1] << 8));
}

std::expected<Key, std::string> load_key_file(const std::filesystem::path& path) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::file_status status = fs::status(path, ec);
  if (ec || !fs::is_regular_file(status)) {
    return std::unexpected("cannot read key file " + path.string());
  }
  // Like ssh with a private key: a key others can read is not a secret any more.
  if ((status.permissions() & (fs::perms::group_all | fs::perms::others_all)) !=
      fs::perms::none) {
    return std::unexpected("key file " + path.string() +
                           " is accessible by other users; run: chmod 600 " + path.string());
  }

  std::ifstream in(path);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::string hex;
  for (const char c : text) {
    if (std::isspace(static_cast<unsigned char>(c)) == 0) {
      hex.push_back(c);
    }
  }
  if (hex.size() != 2 * kKeySize) {
    return std::unexpected("key file " + path.string() + " must hold exactly " +
                           std::to_string(2 * kKeySize) + " hex digits (openssl rand -hex 32)");
  }
  std::array<std::byte, kKeySize> bytes{};
  for (std::size_t i = 0; i < kKeySize; ++i) {
    const int hi = hex_digit(hex[2 * i]);
    const int lo = hex_digit(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      return std::unexpected("key file " + path.string() + " contains a non-hex character");
    }
    bytes[i] = static_cast<std::byte>((hi << 4) | lo);
  }
  return Key(bytes);
}

// ------------------------------------------------------------------ HmacSha256

struct HmacSha256::Context {
  EVP_MAC* mac = nullptr;
  EVP_MAC_CTX* ctx = nullptr;

  Context() = default;
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;
  ~Context() {
    EVP_MAC_CTX_free(ctx);
    EVP_MAC_free(mac);
  }
};

HmacSha256::HmacSha256(const Key& key) : ctx_(std::make_unique<Context>()) {
  ctx_->mac = EVP_MAC_fetch(nullptr, OSSL_MAC_NAME_HMAC, nullptr);
  ctx_->ctx = ctx_->mac != nullptr ? EVP_MAC_CTX_new(ctx_->mac) : nullptr;
  std::array<char, 7> digest{"SHA256"};
  const std::array<OSSL_PARAM, 2> params{
      OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest.data(), 0),
      OSSL_PARAM_construct_end()};
  if (ctx_->ctx == nullptr ||
      EVP_MAC_init(ctx_->ctx, reinterpret_cast<const unsigned char*>(key.bytes().data()),
                   key.bytes().size(), params.data()) != 1) {
    throw std::runtime_error("cannot set up HMAC-SHA256");
  }
}

HmacSha256::~HmacSha256() = default;
HmacSha256::HmacSha256(HmacSha256&&) noexcept = default;
HmacSha256& HmacSha256::operator=(HmacSha256&&) noexcept = default;

std::array<std::byte, HmacSha256::kSize> HmacSha256::operator()(
    std::span<const std::byte> data) {
  std::array<std::byte, kSize> out{};
  std::size_t length = 0;
  // A null key restarts the MAC with the key it already holds.
  if (EVP_MAC_init(ctx_->ctx, nullptr, 0, nullptr) != 1 ||
      EVP_MAC_update(ctx_->ctx, reinterpret_cast<const unsigned char*>(data.data()),
                     data.size()) != 1 ||
      EVP_MAC_final(ctx_->ctx, reinterpret_cast<unsigned char*>(out.data()), &length,
                    out.size()) != 1 ||
      length != kSize) {
    throw std::runtime_error("HMAC-SHA256 failed");
  }
  return out;
}

// ------------------------------------------------------------------ Sealer

Sealer::Sealer(Key key, std::uint64_t epoch) : key_(key), hmac_(key_), epoch_(epoch) {}

std::vector<std::byte> Sealer::seal(std::span<const std::byte> payload) {
  std::vector<std::byte> out;
  out.reserve(kOverhead + payload.size());
  put_le(out, kMagic);
  put_le(out, kVersion);
  put_le(out, key_.id());
  put_le(out, epoch_);
  put_le(out, counter_++);
  out.insert(out.end(), payload.begin(), payload.end());
  const auto mac = hmac_(out);
  out.insert(out.end(), mac.begin(), mac.begin() + kTagSize);
  return out;
}

std::uint64_t epoch_now() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count());
}

std::string_view to_string(AuthError error) {
  switch (error) {
    case AuthError::kTooShort: return "too short for an authenticated envelope";
    case AuthError::kNotAuthenticated: return "not authenticated";
    case AuthError::kUnsupportedVersion: return "unsupported envelope version";
    case AuthError::kWrongKey: return "sealed with a different key";
    case AuthError::kStaleEpoch: return "from an earlier run of the sender";
    case AuthError::kTooOld: return "older than the replay window";
    case AuthError::kReplayed: return "replayed";
    case AuthError::kBadTag: return "authentication tag does not match";
  }
  return "unknown authentication error";
}

// ------------------------------------------------------------------ ReplayWindow

std::expected<void, AuthError> ReplayWindow::check(std::uint64_t epoch,
                                                   std::uint64_t counter) const {
  if (!started_ || epoch > epoch_ || (epoch == epoch_ && counter > top_)) {
    return {};
  }
  if (epoch < epoch_) {
    return std::unexpected(AuthError::kStaleEpoch);
  }
  const std::uint64_t behind = top_ - counter;
  if (behind >= kWidth) {
    return std::unexpected(AuthError::kTooOld);
  }
  if (((seen_ >> behind) & 1U) != 0) {
    return std::unexpected(AuthError::kReplayed);
  }
  return {};
}

void ReplayWindow::accept(std::uint64_t epoch, std::uint64_t counter) {
  if (!started_ || epoch > epoch_) {
    started_ = true;
    epoch_ = epoch;
    top_ = counter;
    seen_ = 1;
  } else if (counter > top_) {
    const std::uint64_t ahead = counter - top_;
    seen_ = ahead >= kWidth ? 1 : (seen_ << ahead) | 1U;
    top_ = counter;
  } else {
    seen_ |= std::uint64_t{1} << (top_ - counter);
  }
}

// ------------------------------------------------------------------ Opener

Opener::Opener(Key key) : key_(key), hmac_(key_) {}

std::expected<std::span<const std::byte>, AuthError> Opener::open(
    std::span<const std::byte> datagram) {
  if (datagram.size() >= 4 && get_le<std::uint32_t>(datagram, 0) != kMagic) {
    return std::unexpected(AuthError::kNotAuthenticated);
  }
  if (datagram.size() < kOverhead) {
    return std::unexpected(AuthError::kTooShort);
  }
  if (get_le<std::uint16_t>(datagram, 4) != kVersion) {
    return std::unexpected(AuthError::kUnsupportedVersion);
  }
  if (get_le<std::uint16_t>(datagram, 6) != key_.id()) {
    return std::unexpected(AuthError::kWrongKey);
  }
  const auto epoch = get_le<std::uint64_t>(datagram, 8);
  const auto counter = get_le<std::uint64_t>(datagram, 16);
  // The window check is cheap, so it runs before the HMAC: replays cost almost nothing.
  if (const auto fresh = window_.check(epoch, counter); !fresh) {
    return std::unexpected(fresh.error());
  }

  const std::size_t signed_size = datagram.size() - kTagSize;
  const auto mac = hmac_(datagram.first(signed_size));
  // Constant-time comparison: how long it takes doesn't depend on where the first
  // mismatching byte is, so timing can't be used to guess a valid tag byte by byte.
  if (CRYPTO_memcmp(mac.data(), datagram.subspan(signed_size).data(), kTagSize) != 0) {
    return std::unexpected(AuthError::kBadTag);
  }

  window_.accept(epoch, counter);
  return datagram.subspan(kHeaderSize, signed_size - kHeaderSize);
}

}  // namespace vigil::auth
