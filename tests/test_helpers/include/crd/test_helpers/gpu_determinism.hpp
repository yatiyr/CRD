#pragma once

// ---------------------------------------------------------------------------
// crd::test::gpu_determinism_check — run a GPU dispatch N times, assert
// every round produces byte-identical output. Phase 3.1.7
// v9-prereq-test-harness (2026-05-18).
//
// USE CASE: GPU compute is throughput-tier (atomic ordering varies),
// but many compute workloads ARE deterministic in practice (single-
// pass non-atomic kernels, scan-free reductions, fixed-iteration loops).
// The discriminating test is "run 3× with same input, get the same
// bytes". If a v9 slice ships claiming determinism, this helper proves
// it; if it doesn't, this helper exposes the variance.
//
// Pattern:
//
//   const bool deterministic = crd::test::gpu_determinism_check(
//       /*dispatch  */ [&]{ run_lbvh_build(); fence->wait(); fence->reset(); },
//       /*read_bytes*/ [&]() -> crd::containers::ConstSpan<crd::u8> {
//           return { static_cast<crd::u8*>(out_buf->map()), out_size };
//       },
//       /*rounds    */ 3);
//   REQUIRE(deterministic);
//
// CALLER OBLIGATIONS:
// - `dispatch` must complete + sync (fence wait); helper assumes
//   `read_bytes()` returns valid data after it returns.
// - `dispatch` must be idempotent w.r.t. inputs (same input each call).
// - `read_bytes()` returns a fresh view per call; the helper snapshots
//   the bytes into an owned buffer before the next round overwrites.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

#include <cstring>

namespace crd::test
{

template <typename DispatchFn, typename ReadFn>
[[nodiscard]] inline bool gpu_determinism_check(DispatchFn&& dispatch,
                                                ReadFn&&     read_bytes,
                                                int          rounds = 3)
{
    if (rounds < 2)
    {
        return true; // Trivially deterministic across 0 or 1 rounds.
    }

    // Round 0: dispatch + snapshot.
    dispatch();
    auto first_bytes = read_bytes();
    crd::containers::Array<crd::u8> reference;
    reference.resize(first_bytes.size());
    if (!first_bytes.empty())
    {
        std::memcpy(reference.data(), first_bytes.data(), first_bytes.size());
    }

    // Rounds 1..N-1: dispatch + memcmp against snapshot.
    for (int r = 1; r < rounds; ++r)
    {
        dispatch();
        auto round_bytes = read_bytes();
        if (round_bytes.size() != reference.size())
        {
            return false;
        }
        if (!round_bytes.empty() &&
            std::memcmp(reference.data(), round_bytes.data(), reference.size()) != 0)
        {
            return false;
        }
    }
    return true;
}

} // namespace crd::test
