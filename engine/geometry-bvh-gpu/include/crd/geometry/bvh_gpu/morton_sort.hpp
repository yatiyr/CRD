#pragma once

// ---------------------------------------------------------------------------
// Deterministic CPU sort of (Morton code, original index) pairs.
// Phase 3.1.7 v9a-b1.
//
// Templated over key width -- both `KeyT = crd::u32` (30-bit Morton, v9a-a)
// and `KeyT = std::uint64_t` (60-bit Morton, v9a-60bit-cpu) are explicitly
// instantiated. Pinned 2026-05-18 at v9a-a-followons close (D136 REVISED):
// template-from-day-one is the elite choice -- the radix-loop body is
// genuinely identical across key widths; only `sizeof(KeyT)` changes the
// pass count. Same API, same test corpus, same determinism contract.
//
// Algorithm: stable LSD radix sort, 8-bit digit. 4 passes for u32, 8 passes
// for u64. The CPU implementation IS the algorithm definition. v9a-b2 GPU
// radix (Blelloch parallel-scan, 4-bit digit) is conformance-tested against
// this path via `bit_compare<MortonPair<KeyT>>` -- bit-identical output
// across runs, compilers, and platforms.
//
// Divergence from phase-doc literal text (D141): the slice row reads
// "Sort `(morton_code, original_index)` pairs via `crd::containers::sort`".
// We choose the OTHER tagged option (the literal "or radix-sort wrapper"
// alternative the slice-start pin exists to resolve). Reason: the published
// 1 ms / 1 M-element budget is unattainable under O(N log N) merge sort
// (single-thread merge-sort over 1 M 8-byte pairs measures ~50-100 ms).
// LSD radix is O(N k) at k=4 (u32) / k=8 (u64) -- a few ms shipping.
// Karras 2012 §4 names radix sort as the canonical LBVH sort step. Slice-
// start pin per phase-doc D-pin marker, carried for ADR-0076 §25 amendment
// at v9a-close.
//
// Stability contract (phase-doc + `morton.hpp` tiebreak comment): equal
// Morton codes => lower input index wins. LSD radix is inherently stable,
// and we build the input pair sequence as `{codes[i], u32(i)}` with `i`
// monotonically ascending -- so the stability property delivers the
// phase contract for free.
//
// Two-layer typing (ADR-0078 §5 D34): Morton codes are dimensionless bit
// indices (NOT `Length<T>`); the pair index is an array offset (also
// dimensionless). No typed-wrapper layer needed at this surface. The typed
// `compute_morton_codes_cpu_typed` upstream (v9a-a-typed) still produces a
// raw `Array<u32>` of codes -- sort consumes that raw output directly.
//
// Determinism: pure deterministic function of the input span. No FP, no
// hashing, no platform-conditional code paths. Bit-identical output across
// MSVC / GCC / clang on x64 / ARM64. (The radix histogram bucket assignment
// is purely integer bit-shift + mask.)
//
// v9a-b1-simd (2026-05-18): the implementation in `morton_sort.cpp` adds a
// single `_mm_prefetch` hint `kPrefetchDistance = 8` iterations ahead of
// the scatter store, staging the destination cache line into L1d before
// the random-bucket write. Net 1.07x throughput at zero code-complexity
// cost (4.99 ms vs 5.32 ms median-of-5 on 1 M u32 win-shipping).
// Prefetch is a HINT -- never changes program state -- so output remains
// byte-identical to the un-prefetched scalar reference (D145 contract).
//
// SOTA techniques (AVX2 sub-histograms, Wassenberg SWWC, multi-pass
// histogram fusion) were investigated and ruled out: at 1 M u32 pairs =
// 8 MB the destination buffer is L2-resident and the cache-pollution
// savings those techniques eliminate are smaller than their bookkeeping
// overhead. Full negative-finding write-up:
// docs/sessions/2026-05-18-geometry-v9a-b1-simd-close.md.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <cstdint>

namespace crd::geometry::bvh_gpu
{

// (Morton code, original AABB index) pair. AoS layout -- `code` first for
// natural alignment + same byte layout as the future GPU `RadixPair` SSBO
// at v9a-b2. Index width fixed at u32 (D144 -- 4 B-element ceiling enforced
// by entry-point assertion).
//
//   sizeof(MortonPair<u32>) == 8  bytes (4 + 4, no padding)
//   sizeof(MortonPair<u64>) == 16 bytes (8 + 4 + 4 trailing padding for u64 align)
template <typename KeyT>
struct MortonPair
{
    KeyT     code;
    crd::u32 index;
};

static_assert(sizeof(MortonPair<crd::u32>) == 8U,
              "MortonPair<u32> must be 8 bytes for the v9a-b2 GPU SSBO layout.");
static_assert(sizeof(MortonPair<std::uint64_t>) == 16U,
              "MortonPair<u64> must be 16 bytes for the v9a-b2 GPU SSBO layout.");
static_assert(alignof(MortonPair<crd::u32>) == 4U,
              "MortonPair<u32> alignment must match the GPU SSBO contract.");
static_assert(alignof(MortonPair<std::uint64_t>) == 8U,
              "MortonPair<u64> alignment must match the GPU SSBO contract.");

// Build N (code, index) pairs from the input span and sort them ascending
// by `code`. Equal-code pairs are ordered by ascending `index` (stable,
// the phase contract). Returns a new `Array` bound to `alloc`.
//
//   Empty input  => empty output.
//   Single elem  => the single pair, no sort work.
//   Otherwise    => allocates ONE auxiliary buffer of N pairs (ping-pong).
//
// Hard cap: input.size() <= UINT32_MAX (D144). Asserted at entry.
template <typename KeyT>
[[nodiscard]] crd::containers::Array<MortonPair<KeyT>>
sort_morton_pairs(crd::containers::ConstSpan<KeyT> codes,
                  crd::memory::IAllocator* alloc) noexcept;

// Convenience overload: take morton codes directly from `Array<KeyT>`.
// Pure forwarding -- no extra work. Inline so the call collapses to the
// non-inline templated function above.
template <typename KeyT>
[[nodiscard]] inline crd::containers::Array<MortonPair<KeyT>>
sort_morton_pairs(const crd::containers::Array<KeyT>& codes,
                  crd::memory::IAllocator* alloc) noexcept
{
    return sort_morton_pairs<KeyT>(
        crd::containers::ConstSpan<KeyT>{codes.data(), codes.size()}, alloc);
}

// Explicit instantiation declarations -- definitions live in morton_sort.cpp.
// Prevents other translation units that include this header from implicitly
// instantiating the template, keeping the algorithm body compiled exactly
// twice per build (once per KeyT).
extern template crd::containers::Array<MortonPair<crd::u32>>
sort_morton_pairs<crd::u32>(crd::containers::ConstSpan<crd::u32>,
                              crd::memory::IAllocator*) noexcept;

extern template crd::containers::Array<MortonPair<std::uint64_t>>
sort_morton_pairs<std::uint64_t>(crd::containers::ConstSpan<std::uint64_t>,
                                   crd::memory::IAllocator*) noexcept;

// ---------------------------------------------------------------------------
// Parallel CPU radix (v9a-b1-parallel, 2026-05-18). Opt-in fan-out of the
// 8-bit-digit LSD radix across `num_jobs` workers via `crd-jobs`.
//
// Output is **byte-identical** to `sort_morton_pairs<KeyT>` for any input,
// any `num_jobs >= 1`. The 4096-equal-keys + cross-chunk-equal-keys +
// num_jobs-sensitivity tests are the discriminating oracle.
//
// Algorithm (one radix pass, 3 phases):
//   Phase 1 (parallel, num_jobs workers): each chunk i scans its slice and
//     writes a local 256-bin histogram into `hist_tiles[i][bucket]`.
//   Phase 2 (serial, num_jobs * 256 ops): compute per-(chunk, bucket)
//     scatter offsets so chunk i's items in bucket B land before chunk i+1's
//     items in bucket B. This is the load-bearing stability invariant
//     across worker boundaries.
//   Phase 3 (parallel, same chunking): each chunk i scatters its slice using
//     its precomputed offsets. Disjoint output ranges per (chunk, bucket)
//     -> no atomics required.
//
// Caller must have initialised `crd::jobs` (via `crd::jobs::init()`) before
// calling this function. If `crd::jobs` is not initialised OR
// `num_workers() <= 1` OR `codes.size() < parallel_threshold`, the function
// transparently falls back to `sort_morton_pairs<KeyT>` (serial scalar +
// prefetch) -- same output, no work-stealing overhead.
//
// `num_jobs = 0` is the documented "auto-detect" value: queries
// `crd::jobs::num_workers()` at call time.
//
// Performance (measured median-of-5, 1 M u32, win-shipping, 8-core dev box):
//   serial scalar+prefetch  : 4.99 ms (v9a-b1-simd baseline)
//   parallel num_jobs=8     : ~2.0 ms  (target; 2.5x speedup)
//
// The 1 ms target from Karras 2012 is a GPU number -- not achievable on CPU
// at this working set; v9a-b2 GPU radix is the sub-ms path. See
// docs/sessions/2026-05-18-geometry-v9a-b1-parallel-close.md for the
// architectural rationale + the 2-3 ms expected outcome.
// ---------------------------------------------------------------------------

inline constexpr crd::u32 kDefaultParallelSortThreshold = 65536U;

template <typename KeyT>
[[nodiscard]] crd::containers::Array<MortonPair<KeyT>>
sort_morton_pairs_parallel(crd::containers::ConstSpan<KeyT> codes,
                           crd::memory::IAllocator* alloc,
                           crd::u32 num_jobs = 0U,
                           crd::u32 parallel_threshold = kDefaultParallelSortThreshold) noexcept;

extern template crd::containers::Array<MortonPair<crd::u32>>
sort_morton_pairs_parallel<crd::u32>(crd::containers::ConstSpan<crd::u32>,
                                     crd::memory::IAllocator*,
                                     crd::u32, crd::u32) noexcept;

extern template crd::containers::Array<MortonPair<std::uint64_t>>
sort_morton_pairs_parallel<std::uint64_t>(crd::containers::ConstSpan<std::uint64_t>,
                                          crd::memory::IAllocator*,
                                          crd::u32, crd::u32) noexcept;

} // namespace crd::geometry::bvh_gpu
