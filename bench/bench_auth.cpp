// Cost of authenticating datagrams: sealing on the sender, and the full receive-side check
// (parse, replay window, HMAC, constant-time compare) on the tracker.

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <vector>

#include "vigil/auth.hpp"

namespace {

vigil::auth::Key bench_key() {
  std::array<std::byte, vigil::auth::kKeySize> bytes{};
  bytes.fill(std::byte{0x5A});
  return vigil::auth::Key(bytes);
}

// Argument: payload bytes. 1,424 is a full authenticated detection report (88 detections).
void BM_Seal(benchmark::State& state) {
  vigil::auth::Sealer sealer(bench_key(), 1);
  const std::vector<std::byte> payload(static_cast<std::size_t>(state.range(0)));
  for (auto _ : state) {
    auto sealed = sealer.seal(payload);
    benchmark::DoNotOptimize(sealed);
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Seal)->Arg(64)->Arg(1424);

void BM_Open(benchmark::State& state) {
  vigil::auth::Sealer sealer(bench_key(), 1);
  vigil::auth::Opener opener(bench_key());
  const std::vector<std::byte> payload(static_cast<std::size_t>(state.range(0)));
  for (auto _ : state) {
    state.PauseTiming();  // each datagram can only be opened once
    const auto sealed = sealer.seal(payload);
    state.ResumeTiming();
    auto opened = opener.open(sealed);
    benchmark::DoNotOptimize(opened);
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Open)->Arg(64)->Arg(1424);

// A forged datagram: rejected at the tag, after a full HMAC.
void BM_RejectForgery(benchmark::State& state) {
  vigil::auth::Sealer sealer(bench_key(), 1);
  vigil::auth::Opener opener(bench_key());
  auto forged = sealer.seal(std::vector<std::byte>(1424));
  forged.back() ^= std::byte{1};
  for (auto _ : state) {
    auto opened = opener.open(forged);
    benchmark::DoNotOptimize(opened);
  }
}
BENCHMARK(BM_RejectForgery);

}  // namespace
