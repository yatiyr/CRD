// ---------------------------------------------------------------------------
// CPU stable LSD radix sort for Morton-code pairs. Phase 3.1.7 v9a-b1 +
// v9a-b1-simd close (2026-05-18, scalar + destination prefetch).
//
// THE ALGORITHM DEFINITION. v9a-b2 GPU radix sort (4-bit digit, 8 passes)
// is conformance-tested against this path via `bit_compare<MortonPair<KeyT>>`.
// Bit-identical output across compilers, platforms, runs.
//
// Body identical for u32 and u64 -- only `num_passes = sizeof(KeyT) * 8 /
// kRadixBits` changes. Even pass count guarantees the ping-pong ends in
// the output buffer (static_assert).
//
// =========================================================================
// v9a-b1-simd CLOSE (2026-05-18). Three "clever" optimizations were
// investigated via SOTA literature; measured median-of-5 on win-shipping
// /O2 + /LTCG + /OPT:ICF + AVX2 enabled, 1 M u32 random Morton codes:
//
//   - AVX2 8-wide SoA sub-histograms (single-thread, no scatter SIMD):
//     11.81 ms (1.55x SLOWER than scalar 7.6 ms single-shot baseline)
//   - Software Write Combining + 1-cache-line bucket buffers + AVX2 burst
//     flush (Wassenberg 2010 / Satish 2010 / RADULS Kokot 2017):
//     8.00 ms (1.61x SLOWER than scalar+prefetch 4.99 ms median)
//   - Destination cache-line prefetch in the scatter inner loop (Hennessy
//     & Patterson §2.6, applied to LSD radix scatter): **4.99 ms**
//     (1.07x FASTER than scalar baseline 5.32 ms median).
//
// Diagnosis: at 1 M u32 pairs = 8 MB working set, the destination buffer
// fits in L2 cache (16-32 MB on the dev box). Wassenberg's 89%-of-peak-
// memory-bandwidth result holds at GB-scale RAM-bound workloads; at L2
// resident scale the SWWC bookkeeping overhead (buffer table, slot count,
// branch-on-full) exceeds the destination-cache-pollution saving it
// eliminates. The AVX2 sub-histogram approach saves no work because
// vector scatter (vpscatterdd) is AVX-512-only -- AVX2 cannot vectorise
// the bucket-write at the bottleneck.
//
// The winning optimization is the smallest: a single `_mm_prefetch` hint
// `prefetch_distance = 8` iterations ahead of the actual scatter, telling
// the cache hierarchy to stage the destination cache line into L1d before
// the store. Net 6.2% throughput improvement at zero code complexity cost.
// Output remains byte-identical to the un-prefetched reference (prefetch
// is a HINT; it never changes program state). Final 1 M u32 budget
// headroom: 5.0 ms measured vs 20 ms asserted NDEBUG budget = **4x**.
//
// Future paths to >5 ms (not pursued; filed for crd-jobs era):
//   - Parallel radix via `crd-jobs::parallel_for` across `num_threads`
//     workers. Bandwidth-bound problem ⇒ N-core scaling caps below N
//     because all cores share the L2/RAM bus, but 2-4x speedup is real.
//     Needs deterministic merge logic for stability across workers.
//   - AVX-512 vpscatterdd hardware scatter. Not portable to dev box's
//     consumer CPU lineage; revisit when the CI matrix adds AVX-512.
//
// Full negative-finding write-up: docs/sessions/2026-05-18-geometry-v9a-b1-simd-close.md.
// ---------------------------------------------------------------------------

#include <crd/geometry/bvh_gpu/morton_sort.hpp>

#include <crd/core/assert.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/math/simd/backend.hpp>

#include <climits>          // CHAR_BIT for the radix-pass-count arithmetic
#include <cstdint>          // UINT32_MAX for the index-ceiling assertion

namespace crd::geometry::bvh_gpu
{

namespace
{

// 8-bit digit chosen for CPU cache locality (256-bin histogram = 1 KiB at
// usize, fits trivially in L1) and minimum pass count for fixed-size keys.
// Digit width is purely an implementation detail; output is byte-identical
// regardless of digit width because LSD radix is stable by construction.
constexpr crd::usize kRadixBits = 8U;
constexpr crd::usize kRadixBins = crd::usize{1} << kRadixBits;          // 256

// One stable LSD radix pass over [src, src+n) writing to [dst, dst+n).
// `shift` selects the 8-bit lane of `code` that drives bucket assignment
// for this pass. Stability: within a bucket, src-order is preserved,
// because the offset table is bumped only on actual scatter and we visit
// src left-to-right.
//
// The scatter loop issues an `_mm_prefetch` hint to stage the destination
// cache line into L1d `prefetch_distance` iterations before the actual
// store -- converts the random-bucket store from a demand miss into a hit.
// Determinism is unaffected: prefetch is a HINT, never a write.
template <typename KeyT>
void radix_pass(const MortonPair<KeyT>* src,
                MortonPair<KeyT>*       dst,
                crd::usize              n,
                crd::usize              shift) noexcept
{
    crd::usize histogram[kRadixBins] = {0U};

    // Pass A -- count occurrences of each byte at the current shift.
    for (crd::usize i = 0U; i < n; ++i)
    {
        const crd::usize bucket =
            static_cast<crd::usize>((src[i].code >> shift) & KeyT{0xFFU});
        ++histogram[bucket];
    }

    // Exclusive prefix sum -- histogram[k] becomes the starting offset of
    // bucket k in `dst`.
    crd::usize sum = 0U;
    for (crd::usize k = 0U; k < kRadixBins; ++k)
    {
        const crd::usize count = histogram[k];
        histogram[k] = sum;
        sum += count;
    }

    // Pass B -- scatter each src pair into its bucket's next slot.
#if CRD_SIMD_HAS_SSE2
    // Prefetch distance: 8 iterations ahead. Tuned to cover the ~40-cycle
    // L1 miss latency on modern x64 at the dispatch IPC of this inner loop.
    constexpr crd::usize prefetch_distance = 8U;
    const crd::usize n_prefetch = (n > prefetch_distance) ? (n - prefetch_distance) : 0U;
    for (crd::usize i = 0U; i < n_prefetch; ++i)
    {
        // Stage the destination cache line for the future scatter at i + 8.
        // The histogram value is approximate (we have not yet performed the
        // i..i+7 scatters that bump it), but approximate is fine for a hint
        // -- the actual store lands within ~64 bytes of the prefetched line.
        const crd::usize fut_bucket =
            static_cast<crd::usize>((src[i + prefetch_distance].code >> shift) & KeyT{0xFFU});
        _mm_prefetch(reinterpret_cast<const char*>(dst + histogram[fut_bucket]),
                     _MM_HINT_T0);

        const crd::usize bucket =
            static_cast<crd::usize>((src[i].code >> shift) & KeyT{0xFFU});
        const crd::usize pos = histogram[bucket]++;
        dst[pos] = src[i];
    }
    for (crd::usize i = n_prefetch; i < n; ++i)
    {
        const crd::usize bucket =
            static_cast<crd::usize>((src[i].code >> shift) & KeyT{0xFFU});
        const crd::usize pos = histogram[bucket]++;
        dst[pos] = src[i];
    }
#else
    for (crd::usize i = 0U; i < n; ++i)
    {
        const crd::usize bucket =
            static_cast<crd::usize>((src[i].code >> shift) & KeyT{0xFFU});
        const crd::usize pos = histogram[bucket]++;
        dst[pos] = src[i];
    }
#endif
}

} // namespace

template <typename KeyT>
crd::containers::Array<MortonPair<KeyT>>
sort_morton_pairs(crd::containers::ConstSpan<KeyT> codes,
                  crd::memory::IAllocator* alloc) noexcept
{
    crd::containers::Array<MortonPair<KeyT>> out(alloc);

    const crd::usize n = codes.size();
    CRD_ASSERT_MSG(n <= static_cast<crd::usize>(UINT32_MAX),
                   "sort_morton_pairs: input exceeds the u32 index ceiling (D144)");

    if (n == 0U)
    {
        return out;
    }

    out.resize(n);

    // Build (code, monotonic-ascending index) pairs. The monotonic index
    // is what gives LSD-radix stability its phase-contract meaning:
    // "equal Morton codes => lower input index wins" emerges automatically.
    for (crd::usize i = 0U; i < n; ++i)
    {
        out[i].code  = codes[i];
        out[i].index = static_cast<crd::u32>(i);
    }

    if (n < 2U)
    {
        return out;
    }

    // Pass count is even for both u32 (4) and u64 (8) -- so after the
    // ping-pong, the final result lands back in `out` without an extra
    // copy. The static_assert below is the load-bearing invariant.
    constexpr crd::usize num_passes = (sizeof(KeyT) * CHAR_BIT) / kRadixBits;
    static_assert(num_passes % 2U == 0U,
                  "Radix pass count must be even so the ping-pong terminates in `out`.");

    crd::containers::Array<MortonPair<KeyT>> aux(alloc);
    aux.resize(n);

    MortonPair<KeyT>* a = out.data();
    MortonPair<KeyT>* b = aux.data();

    for (crd::usize pass = 0U; pass < num_passes; ++pass)
    {
        const crd::usize shift = pass * kRadixBits;
        radix_pass<KeyT>(a, b, n, shift);
        MortonPair<KeyT>* tmp = a;
        a = b;
        b = tmp;
    }

    // Even pass count => `a == out.data()`. The `aux` array drops here and
    // releases its backing memory through the allocator.
    return out;
}

// Explicit instantiations -- the only two valid `KeyT`s in the v9 GPU LBVH
// pipeline. Adding a third (e.g. u128 someday) is a one-line addition + a
// matching `extern template` in the header.
template crd::containers::Array<MortonPair<crd::u32>>
sort_morton_pairs<crd::u32>(crd::containers::ConstSpan<crd::u32>,
                              crd::memory::IAllocator*) noexcept;

template crd::containers::Array<MortonPair<std::uint64_t>>
sort_morton_pairs<std::uint64_t>(crd::containers::ConstSpan<std::uint64_t>,
                                   crd::memory::IAllocator*) noexcept;

// =========================================================================
// v9a-b1-parallel — 3-phase parallel radix via crd-jobs (2026-05-18).
//
// Public function `sort_morton_pairs_parallel<KeyT>` falls back to the
// serial `sort_morton_pairs<KeyT>` when jobs is not initialised, when
// num_jobs <= 1, or when codes.size() < parallel_threshold. Otherwise it
// dispatches the 3-phase parallel radix described in the header doc-block.
//
// Stability across worker boundaries is the load-bearing invariant. The
// per-(chunk, bucket) scatter offset table is computed serially in Phase 2:
// `scatter_off[chunk][bucket]` = global bucket start + sum of prior chunks'
// counts in that bucket. Each chunk's scatter write region is strictly
// less than the next chunk's region within the same bucket, so equal-key
// items from earlier chunks land before later chunks. Combined with the
// within-chunk left-to-right visit order, this preserves global monotonic
// input-index stability.
// =========================================================================

namespace
{

// One pass of parallel radix. Returns nothing; advances `a` -> `b`.
// `hist_tiles` is the [num_jobs * kRadixBins] working table; `scatter_off`
// is the [num_jobs * kRadixBins] per-(chunk, bucket) offset table. Both
// are caller-allocated so they can be reused across all passes.
template <typename KeyT>
void parallel_radix_pass(const MortonPair<KeyT>* src,
                         MortonPair<KeyT>*       dst,
                         crd::usize              n,
                         crd::usize              shift,
                         crd::u32                num_jobs,
                         crd::usize*             hist_tiles,
                         crd::usize*             scatter_off) noexcept
{
    // ---- Phase 1: parallel histogram per chunk ------------------------------
    struct HistParams
    {
        const MortonPair<KeyT>* src;
        crd::usize              n;
        crd::usize              shift;
        crd::u32                num_jobs;
        crd::usize*             hist_tiles;
    };
    HistParams hp{src, n, shift, num_jobs, hist_tiles};
    const HistParams* hpp = &hp;

    crd::jobs::Counter* c1 = crd::jobs::parallel_for(
        num_jobs, num_jobs,
        [hpp](crd::u32 i, crd::u32 /*unused*/) noexcept
        {
            const crd::usize begin = static_cast<crd::usize>(i)        * hpp->n / hpp->num_jobs;
            const crd::usize end   = static_cast<crd::usize>(i + 1U)   * hpp->n / hpp->num_jobs;
            crd::usize* hist = hpp->hist_tiles + static_cast<crd::usize>(i) * kRadixBins;
            for (crd::usize k = 0U; k < kRadixBins; ++k) { hist[k] = 0U; }
            for (crd::usize j = begin; j < end; ++j)
            {
                const crd::usize bucket =
                    static_cast<crd::usize>((hpp->src[j].code >> hpp->shift) & KeyT{0xFFU});
                ++hist[bucket];
            }
        });
    crd::jobs::wait(c1);

    // ---- Phase 2: serial offset table (the stability-critical step) --------
    // For each bucket B, total[B] = sum over chunks of hist_tiles[chunk * kBins + B].
    // bucket_start[B] = exclusive prefix over total[]. scatter_off[chunk * kBins + B]
    // = bucket_start[B] + sum over c < chunk of hist_tiles[c * kBins + B].
    crd::usize bucket_total[kRadixBins] = {0U};
    for (crd::u32 c = 0U; c < num_jobs; ++c)
    {
        const crd::usize* tile = hist_tiles + static_cast<crd::usize>(c) * kRadixBins;
        for (crd::usize k = 0U; k < kRadixBins; ++k)
        {
            bucket_total[k] += tile[k];
        }
    }
    // Build scatter_off in place using a running per-bucket cursor seeded with
    // the global exclusive-prefix bucket-start. After this loop, for each
    // chunk c and bucket k, scatter_off[c * kBins + k] is the absolute index
    // in `dst` where chunk c's first item with bucket k will be written.
    crd::usize running[kRadixBins];
    {
        crd::usize sum = 0U;
        for (crd::usize k = 0U; k < kRadixBins; ++k)
        {
            running[k] = sum;
            sum += bucket_total[k];
        }
    }
    for (crd::u32 c = 0U; c < num_jobs; ++c)
    {
        crd::usize*       off  = scatter_off + static_cast<crd::usize>(c) * kRadixBins;
        const crd::usize* tile = hist_tiles  + static_cast<crd::usize>(c) * kRadixBins;
        for (crd::usize k = 0U; k < kRadixBins; ++k)
        {
            off[k] = running[k];
            running[k] += tile[k];
        }
    }

    // ---- Phase 3: parallel scatter (each chunk writes a disjoint range) ----
    struct ScatterParams
    {
        const MortonPair<KeyT>* src;
        MortonPair<KeyT>*       dst;
        crd::usize              n;
        crd::usize              shift;
        crd::u32                num_jobs;
        crd::usize*             scatter_off;
    };
    ScatterParams sp{src, dst, n, shift, num_jobs, scatter_off};
    const ScatterParams* spp = &sp;

    crd::jobs::Counter* c2 = crd::jobs::parallel_for(
        num_jobs, num_jobs,
        [spp](crd::u32 i, crd::u32 /*unused*/) noexcept
        {
            const crd::usize begin = static_cast<crd::usize>(i)        * spp->n / spp->num_jobs;
            const crd::usize end   = static_cast<crd::usize>(i + 1U)   * spp->n / spp->num_jobs;
            crd::usize* off = spp->scatter_off + static_cast<crd::usize>(i) * kRadixBins;
            for (crd::usize j = begin; j < end; ++j)
            {
                const MortonPair<KeyT> pair = spp->src[j];
                const crd::usize bucket =
                    static_cast<crd::usize>((pair.code >> spp->shift) & KeyT{0xFFU});
                const crd::usize pos = off[bucket]++;
                spp->dst[pos] = pair;
            }
        });
    crd::jobs::wait(c2);
}

} // namespace

template <typename KeyT>
crd::containers::Array<MortonPair<KeyT>>
sort_morton_pairs_parallel(crd::containers::ConstSpan<KeyT> codes,
                           crd::memory::IAllocator* alloc,
                           crd::u32 num_jobs,
                           crd::u32 parallel_threshold) noexcept
{
    if (num_jobs == 0U)
    {
        num_jobs = crd::jobs::num_workers();
    }
    const crd::usize n = codes.size();

    // Fall back to the canonical serial path when:
    //   - jobs not initialised (num_workers() == 0)
    //   - single-worker pool (no parallelism to extract)
    //   - input below the threshold where per-pass parallel overhead
    //     (phase-1 + phase-2 + phase-3 + 2 barriers) exceeds the saving
    if (num_jobs <= 1U || n < parallel_threshold)
    {
        return sort_morton_pairs<KeyT>(codes, alloc);
    }

    crd::containers::Array<MortonPair<KeyT>> out(alloc);

    CRD_ASSERT_MSG(n <= static_cast<crd::usize>(UINT32_MAX),
                   "sort_morton_pairs_parallel: input exceeds the u32 index ceiling (D144)");

    if (n == 0U) { return out; }
    out.resize(n);
    for (crd::usize i = 0U; i < n; ++i)
    {
        out[i].code  = codes[i];
        out[i].index = static_cast<crd::u32>(i);
    }
    if (n < 2U) { return out; }

    constexpr crd::usize num_passes = (sizeof(KeyT) * CHAR_BIT) / kRadixBits;
    static_assert(num_passes % 2U == 0U,
                  "Radix pass count must be even so the ping-pong terminates in `out`.");

    crd::containers::Array<MortonPair<KeyT>> aux(alloc);
    aux.resize(n);

    // Working tables — shared across all passes. Each is `num_jobs * 256`
    // usize entries (~16 KB at num_jobs=8 / sizeof(usize)=8). Fits L1d on
    // every thread.
    crd::containers::Array<crd::usize> hist_tiles(alloc);
    hist_tiles.resize(static_cast<crd::usize>(num_jobs) * kRadixBins);
    crd::containers::Array<crd::usize> scatter_off(alloc);
    scatter_off.resize(static_cast<crd::usize>(num_jobs) * kRadixBins);

    MortonPair<KeyT>* a = out.data();
    MortonPair<KeyT>* b = aux.data();

    for (crd::usize pass = 0U; pass < num_passes; ++pass)
    {
        parallel_radix_pass<KeyT>(a, b, n, pass * kRadixBits, num_jobs,
                                   hist_tiles.data(), scatter_off.data());
        MortonPair<KeyT>* tmp = a; a = b; b = tmp;
    }

    return out;
}

template crd::containers::Array<MortonPair<crd::u32>>
sort_morton_pairs_parallel<crd::u32>(crd::containers::ConstSpan<crd::u32>,
                                     crd::memory::IAllocator*,
                                     crd::u32, crd::u32) noexcept;

template crd::containers::Array<MortonPair<std::uint64_t>>
sort_morton_pairs_parallel<std::uint64_t>(crd::containers::ConstSpan<std::uint64_t>,
                                          crd::memory::IAllocator*,
                                          crd::u32, crd::u32) noexcept;

} // namespace crd::geometry::bvh_gpu
