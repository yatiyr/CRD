#pragma once

// The integer seed generators: SplitMix64 (Steele, Lea and Flood, "Fast splittable pseudorandom number generators",
// OOPSLA 2014, with Stafford's variant-13 finalizer) and the 64-bit linear congruential step of Knuth's MMIX. They are
// the engine's one implementation (capability random.seed-generators, cmake/capability-owners.json) and live in
// crd-core so every module can reach them: the workload probe, fault injection and the CEIR input generator sit below
// hesap-stats, whose engines (SplitMix64, xoshiro, PCG64, ChaCha, ...) and distributions build on these steps.
//
// SplitMix64 has three forms: `splitmix64_mix`, the bijective finalizer (a well-distributed hash of one word);
// `splitmix64_next`, the stateful generator (add the gamma, then mix); and `splitmix64_at`, output `n` (from 0) of the
// stream seeded with `seed`, computed directly. `mmix_lcg_next` is one step of state * 6364136223846793005 +
// 1442695040888963407 (mod 2^64): cheap and fully reproducible, for sketches and starting vectors whose quality
// needs no more.

#include <crd/core/types.hpp>

namespace crd
{

// 2^64 divided by the golden ratio, rounded to odd: SplitMix64's gamma and the hash combiner's constant.
inline constexpr u64 kGoldenRatio64 = 0x9E3779B97F4A7C15ULL;
inline constexpr u64 kMmixLcgMultiplier = 6364136223846793005ULL;
inline constexpr u64 kMmixLcgIncrement = 1442695040888963407ULL;

[[nodiscard]] constexpr u64 splitmix64_mix(u64 z) noexcept
{
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

[[nodiscard]] constexpr u64 splitmix64_next(u64& state) noexcept
{
    state += kGoldenRatio64;
    return splitmix64_mix(state);
}

[[nodiscard]] constexpr u64 splitmix64_at(u64 seed, u64 n) noexcept
{
    return splitmix64_mix(seed + (n + 1U) * kGoldenRatio64);
}

[[nodiscard]] constexpr u64 mmix_lcg_next(u64& state) noexcept
{
    state = state * kMmixLcgMultiplier + kMmixLcgIncrement;
    return state;
}

} // namespace crd
