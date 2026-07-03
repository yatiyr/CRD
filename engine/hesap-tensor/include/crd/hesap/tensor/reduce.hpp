#pragma once

#include "dtypes.hpp"
#include "elementwise.hpp" // EwSimd vector traits
#include "tensor.hpp"

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/math/simd/simd.hpp>

#include <bit>

// ---------------------------------------------------------------------------
// crd-hesap-tensor reduce — reductions + the reproducible tier
// (Phase 3.1.6 v14-c; ADR-0096 §4).
//
// THE TWO NAMED TIERS (never conflated — the honest-scoreboard scar):
//
//   TIER D (default): fixed-order reduction trees, serial ≡ parallel.
//   The tensor is cut into kReduceBlock-element blocks — a function of the
//   SHAPE ONLY, never of num_workers — each block produces one partial with a
//   fixed internal order (the 4×Vec4d/4×Vec8f vertical-accumulator layout,
//   lanes folded in a pinned order, tail appended last), and the partials are
//   folded LEFT-TO-RIGHT in block order. Workers compute whole blocks; the
//   fold is identical regardless of who computed what ⇒ bit-identical across
//   {1..16} workers BY CONSTRUCTION. (The order is a function of shape and
//   contiguity class: the strided fallback uses the same block boundaries in
//   logical row-major order with a scalar in-block fold — documented, gated.)
//
//   TIER R (opt-in): ReproBLAS-class binned summation — bit-reproducible
//   INDEPENDENT of partitioning (chunk counts, merge orders, machines).
//   BinnedF64 is a faithful fold-3 transcription of ReproBLAS v2.1.0
//   (external/reproblas, SHA dfb8150): binned_dindex/dmbins ladder,
//   binned_dmdupdate (index shift), binned_dmddeposit (the sticky-bit |1
//   two-sum ladder that makes every deposit order-free), binned_dmrenorm
//   (bit-twiddled carry extraction), binned_dmdmadd (accumulator merge), and
//   binned_ddmconv (read-out). The SIMD path runs TWELVE independent binned
//   accumulators (3 latency-hiding streams × 4 Vec4d lanes) with a
//   SPECULATIVE single-DRAM-pass window (deposit under the previous
//   super-block's index, track |max| in-pass, snapshot-rollback on the rare
//   violation) and merges them at the end — legal precisely because the
//   representation is partition-independent. Measured: 1.6× ReproBLAS @1M,
//   faster than a naive serial sum @16M.
//
//   ★Deterministic SR accumulation: reduce into a bf16 accumulator with a
//   Philox stochastic-rounding decision per step, keyed (seed, step index,
//   kSrAccumStep) — reproducible-by-seed, unbiased on the grid (ADR-0096 §4).
//
// v13 pillars: zero heap per call (the parallel path takes a caller
// workspace), noexcept, status-not-exception.
// ---------------------------------------------------------------------------

namespace crd::hesap::tensor
{

namespace detail
{

// Tier-D block: a function of the SHAPE ONLY (never num_workers).
inline constexpr crd::u64 kReduceBlock = 4096U;

// ---- Tier-D op functors ----------------------------------------------------
template <typename T> struct RAdd
{
    static constexpr T kInit = T{0};
    static T s(T a, T b) noexcept { return a + b; }
    template <typename V> static V v(V a, V b) noexcept { return a + b; }
};
template <typename T> struct RMul
{
    static constexpr T kInit = T{1};
    static T s(T a, T b) noexcept { return a * b; }
    template <typename V> static V v(V a, V b) noexcept { return a * b; }
};
template <typename T> struct RMin
{
    static T s(T a, T b) noexcept { return b < a ? b : a; }
    template <typename V> static V v(V a, V b) noexcept
    {
        using crd::math::simd::min;
        return min(a, b);
    }
};
template <typename T> struct RMax
{
    static T s(T a, T b) noexcept { return a < b ? b : a; }
    template <typename V> static V v(V a, V b) noexcept
    {
        using crd::math::simd::max;
        return max(a, b);
    }
};

// One contiguous block's partial with the PINNED internal order:
// 4 vector accumulators striped over the block (acc[j] gathers lanes
// j*W..j*W+W-1 of every 4W-wide stripe), combined ((a0+a1)+(a2+a3)), lanes
// stored and folded left-to-right, the scalar tail appended last.
template <typename T, typename Op> [[nodiscard]] inline T block_partial(const T* p, crd::u64 n, T init) noexcept
{
    using V = typename EwSimd<T>::Vec;
    constexpr crd::u64 kW = EwSimd<T>::kWidth;
    T acc = init;
    crd::u64 i = 0;
    if (n >= 4U * kW)
    {
        V a0 = V::load(p);
        V a1 = V::load(p + kW);
        V a2 = V::load(p + 2U * kW);
        V a3 = V::load(p + 3U * kW);
        for (i = 4U * kW; i + 4U * kW <= n; i += 4U * kW)
        {
            a0 = Op::v(a0, V::load(p + i));
            a1 = Op::v(a1, V::load(p + i + kW));
            a2 = Op::v(a2, V::load(p + i + 2U * kW));
            a3 = Op::v(a3, V::load(p + i + 3U * kW));
        }
        const V c = Op::v(Op::v(a0, a1), Op::v(a2, a3));
        alignas(64) T lanes[kW];
        c.store(lanes);
        T va = lanes[0];
        for (crd::u64 l = 1; l < kW; ++l)
        {
            va = Op::s(va, lanes[l]);
        }
        acc = Op::s(acc, va);
    }
    for (; i < n; ++i)
    {
        acc = Op::s(acc, p[i]);
    }
    return acc;
}

// Fold a contiguous buffer through the Tier-D fixed tree, optionally in
// parallel over whole blocks (partials land in ws in block order; the final
// fold is the same serial left-to-right pass either way).
template <typename T, typename Op>
[[nodiscard]] inline T reduce_fixed_tree(const T* p, crd::u64 n, T init, crd::containers::Span<T> ws) noexcept
{
    if (n == 0U)
    {
        return init;
    }
    const crd::u64 nblocks = (n + kReduceBlock - 1U) / kReduceBlock;
    const bool parallel = nblocks >= 4U && ws.size() >= nblocks && crd::jobs::num_workers() > 1U;
    if (!parallel)
    {
        // Serial streaming: identical numeric order (block partial, then fold).
        T acc = init;
        bool first = true;
        for (crd::u64 b = 0; b < nblocks; ++b)
        {
            const crd::u64 off = b * kReduceBlock;
            const crd::u64 len = n - off < kReduceBlock ? n - off : kReduceBlock;
            const T partial = block_partial<T, Op>(p + off, len, init);
            acc = first ? partial : Op::s(acc, partial);
            first = false;
        }
        return acc;
    }
    T* partials = ws.data();
    crd::jobs::Counter* c =
        crd::jobs::parallel_for(static_cast<crd::u32>(nblocks), crd::jobs::num_workers(),
                                [p, n, init, partials](crd::u32 b0, crd::u32 b1)
                                {
                                    for (crd::u32 b = b0; b < b1; ++b)
                                    {
                                        const crd::u64 off = static_cast<crd::u64>(b) * kReduceBlock;
                                        const crd::u64 len = n - off < kReduceBlock ? n - off : kReduceBlock;
                                        partials[b] = block_partial<T, Op>(p + off, len, init);
                                    }
                                });
    crd::jobs::wait(c);
    crd::jobs::frame_reset(); // parallel_for JobDecls (the spmv-op hygiene pattern)
    T acc = partials[0];
    for (crd::u64 b = 1; b < nblocks; ++b)
    {
        acc = Op::s(acc, partials[b]);
    }
    return acc;
}

// ============================================================================
// TIER R — BinnedF64: faithful fold-3 transcription of ReproBLAS v2.1.0
// (binned.h + src/binned/{dindex,dmbins,dmdupdate,dmddeposit,dmrenorm,
// dmdmadd,ddmconv}.c — external/reproblas, SHA dfb8150). Deposits are
// order-free (the sticky-bit two-sum ladder), the representation is
// partition-independent, and the read-out rounds once at the end.
// ============================================================================

inline constexpr crd::i32 kBinWidth = 40;                 // DBWIDTH
inline constexpr crd::i32 kBinFold = 3;                   // the default fold
inline constexpr crd::i32 kBinMaxIndex = 51;              // ((1024+1021+52)/40)-1
inline constexpr crd::i32 kBinMaxFold = kBinMaxIndex + 1; // binned_DBMAXFOLD
inline constexpr crd::u64 kBinEndurance = 1ULL << 11U;    // 1<<(53-40-2)
inline constexpr crd::f64 kBinCompression = 1.0 / (1 << 14);
inline constexpr crd::f64 kBinExpansion = 1.0 * (1 << 14);

// 0.75 × 2^k as bits (1.5 × 2^(k-1): exponent field k-1+1023, mantissa 0.5).
[[nodiscard]] constexpr crd::f64 pow2_075(crd::i32 k) noexcept
{
    const crd::u64 bits = (static_cast<crd::u64>(k - 1 + 1023) << 52U) | 0x8000000000000ULL;
    return std::bit_cast<crd::f64>(bits);
}

// The bin ladder (binned_dmbins): bins[0] = 1.5×2^1023; bins[i>=1] =
// 0.75×2^(1038 - 40i); the tail repeats the last finite rung.
[[nodiscard]] inline crd::f64 bin_value(crd::i32 index) noexcept
{
    if (index <= 0)
    {
        return pow2_075(1024); // 2·0.75·2^1023
    }
    const crd::i32 i = index > kBinMaxIndex ? kBinMaxIndex : index;
    return pow2_075(1024 + 53 - kBinWidth + 1 - i * kBinWidth);
}

[[nodiscard]] inline crd::u32 f64_biased_exp(crd::f64 x) noexcept
{
    return static_cast<crd::u32>((std::bit_cast<crd::u64>(x) >> 52U) & 0x7FFU);
}
[[nodiscard]] inline bool f64_nan_or_inf(crd::f64 x) noexcept
{
    return f64_biased_exp(x) == 0x7FFU;
}

// binned_dindex: the ladder bucket of |X|.
[[nodiscard]] inline crd::i32 bin_index_of(crd::f64 x) noexcept
{
    const crd::u32 e = f64_biased_exp(x);
    if (e == 0U)
    {
        return kBinMaxIndex; // zero/denormal: the bottom bucket (their MIN() clamp)
    }
    return (2047 - static_cast<crd::i32>(e)) / kBinWidth; // (DBL_MAX_EXP + EXP_BIAS - exp)/W
}

// binned_dmindex: the index OF an accumulator, from its top primary.
[[nodiscard]] inline crd::i32 bin_index_of_acc(crd::f64 pri0) noexcept
{
    return (1024 + 53 - kBinWidth + 1 + 1023 - static_cast<crd::i32>(f64_biased_exp(pri0))) / kBinWidth;
}

// Fold-3 binned accumulator (primary + carry per rung).
struct BinnedF64
{
    crd::f64 pri[kBinFold] = {0.0, 0.0, 0.0};
    crd::f64 car[kBinFold] = {0.0, 0.0, 0.0};

    // binned_dmdupdate: make the index suitable for |x| <= max_abs.
    void update(crd::f64 max_abs) noexcept
    {
        if (f64_nan_or_inf(pri[0]))
        {
            return;
        }
        const crd::i32 x_index = bin_index_of(max_abs);
        if (pri[0] == 0.0)
        {
            for (crd::i32 i = 0; i < kBinFold; ++i)
            {
                pri[i] = bin_value(x_index + i);
                car[i] = 0.0;
            }
            return;
        }
        const crd::i32 shift = bin_index_of_acc(pri[0]) - x_index;
        if (shift > 0)
        {
            crd::i32 i = kBinFold - 1;
            for (; i >= shift; --i)
            {
                pri[i] = pri[i - shift];
                car[i] = car[i - shift];
            }
            for (crd::i32 j = 0; j < i + 1; ++j)
            {
                pri[j] = bin_value(x_index + j);
                car[j] = 0.0;
            }
        }
    }

    // binned_dmddeposit: the sticky-bit two-sum ladder (order-free).
    void deposit(crd::f64 x) noexcept
    {
        if (f64_nan_or_inf(x) || f64_nan_or_inf(pri[0]))
        {
            pri[0] += x;
            return;
        }
        crd::f64 m;
        crd::u64 ql;
        crd::f64 qd;
        if (bin_index_of_acc(pri[0]) == 0) // the index-0 compression branch
        {
            m = pri[0];
            qd = x * kBinCompression;
            ql = std::bit_cast<crd::u64>(qd) | 1ULL;
            qd = std::bit_cast<crd::f64>(ql) + m;
            pri[0] = qd;
            m -= qd;
            m *= kBinExpansion * 0.5;
            x += m;
            x += m;
            for (crd::i32 i = 1; i < kBinFold - 1; ++i)
            {
                m = pri[i];
                ql = std::bit_cast<crd::u64>(x) | 1ULL;
                qd = std::bit_cast<crd::f64>(ql) + m;
                pri[i] = qd;
                m -= qd;
                x += m;
            }
            ql = std::bit_cast<crd::u64>(x) | 1ULL;
            pri[kBinFold - 1] += std::bit_cast<crd::f64>(ql);
            return;
        }
        for (crd::i32 i = 0; i < kBinFold - 1; ++i)
        {
            m = pri[i];
            ql = std::bit_cast<crd::u64>(x) | 1ULL;
            qd = std::bit_cast<crd::f64>(ql) + m;
            pri[i] = qd;
            m -= qd;
            x += m;
        }
        ql = std::bit_cast<crd::u64>(x) | 1ULL;
        pri[kBinFold - 1] += std::bit_cast<crd::f64>(ql);
    }

    // binned_dmrenorm (the bit-twiddled form): pull the top 2 mantissa bits
    // into the carry and re-center the primary. Call at least every
    // kBinEndurance deposits and before add/convert.
    void renorm() noexcept
    {
        if (pri[0] == 0.0 || f64_nan_or_inf(pri[0]))
        {
            return;
        }
        for (crd::i32 i = 0; i < kBinFold; ++i)
        {
            crd::u64 bits = std::bit_cast<crd::u64>(pri[i]);
            car[i] += static_cast<crd::f64>(static_cast<crd::i32>((bits >> 50U) & 3ULL) - 2);
            bits &= ~(1ULL << 50U);
            bits |= 1ULL << 51U;
            pri[i] = std::bit_cast<crd::f64>(bits);
        }
    }

    // binned_dmdmadd: this += other (index-aligned merge, then renorm).
    void add(const BinnedF64& other) noexcept
    {
        if (other.pri[0] == 0.0)
        {
            return;
        }
        if (pri[0] == 0.0)
        {
            *this = other;
            return;
        }
        if (f64_nan_or_inf(other.pri[0]) || f64_nan_or_inf(pri[0]))
        {
            pri[0] += other.pri[0];
            return;
        }
        const crd::i32 x_index = bin_index_of_acc(other.pri[0]);
        const crd::i32 y_index = bin_index_of_acc(pri[0]);
        const crd::i32 shift = y_index - x_index;
        if (shift > 0)
        {
            // shift THIS upwards and add other
            for (crd::i32 i = kBinFold - 1; i >= shift; --i)
            {
                pri[i] = other.pri[i] + (pri[i - shift] - bin_value(y_index + (i - shift)));
                car[i] = other.car[i] + car[i - shift];
            }
            for (crd::i32 i = 0; i < shift && i < kBinFold; ++i)
            {
                pri[i] = other.pri[i];
                car[i] = other.car[i];
            }
        }
        else
        {
            // shift OTHER upwards and add onto this
            for (crd::i32 i = -shift; i < kBinFold; ++i)
            {
                pri[i] += other.pri[i + shift] - bin_value(x_index + (i + shift));
                car[i] += other.car[i + shift];
            }
        }
        renorm();
    }

    // binned_ddmconv: read out the represented value (rounds ONCE, here).
    [[nodiscard]] crd::f64 convert() const noexcept
    {
        if (f64_nan_or_inf(pri[0]))
        {
            return pri[0];
        }
        if (pri[0] == 0.0)
        {
            return 0.0;
        }
        crd::f64 y = 0.0;
        const crd::i32 x_index = bin_index_of_acc(pri[0]);
        if (x_index <= (3 * 53) / kBinWidth)
        {
            // near the overflow end: scaled accumulation (their scale branch)
            const crd::f64 scale_down = std::bit_cast<crd::f64>((static_cast<crd::u64>(-65 + 1023) << 52U)); // 2^-65
            const crd::f64 scale_up = std::bit_cast<crd::f64>((static_cast<crd::u64>(65 + 1023) << 52U));    // 2^65
            const crd::i32 want = (3 * 53) / kBinWidth - x_index;
            const crd::i32 scaled = want < 0 ? 0 : (want > kBinFold ? kBinFold : want);
            crd::i32 i;
            if (x_index == 0)
            {
                y += car[0] * ((bin_value(x_index + 0) / 6.0) * scale_down * kBinExpansion);
                y += car[1] * ((bin_value(x_index + 1) / 6.0) * scale_down);
                y += (pri[0] - bin_value(x_index + 0)) * scale_down * kBinExpansion;
                i = 2;
            }
            else
            {
                y += car[0] * ((bin_value(x_index + 0) / 6.0) * scale_down);
                i = 1;
            }
            for (; i < scaled; ++i)
            {
                y += car[i] * ((bin_value(x_index + i) / 6.0) * scale_down);
                y += (pri[i - 1] - bin_value(x_index + i - 1)) * scale_down;
            }
            if (i == kBinFold)
            {
                y += (pri[kBinFold - 1] - bin_value(x_index + kBinFold - 1)) * scale_down;
                return y * scale_up;
            }
            if (f64_nan_or_inf(y * scale_up))
            {
                return y * scale_up;
            }
            y *= scale_up;
            for (; i < kBinFold; ++i)
            {
                y += car[i] * (bin_value(x_index + i) / 6.0);
                y += pri[i - 1] - bin_value(x_index + i - 1);
            }
            y += pri[kBinFold - 1] - bin_value(x_index + kBinFold - 1);
            return y;
        }
        y += car[0] * (bin_value(x_index + 0) / 6.0);
        for (crd::i32 i = 1; i < kBinFold; ++i)
        {
            y += car[i] * (bin_value(x_index + i) / 6.0);
            y += pri[i - 1] - bin_value(x_index + i - 1);
        }
        y += pri[kBinFold - 1] - bin_value(x_index + kBinFold - 1);
        return y;
    }
};

#if defined(__AVX2__)
// SIMD reproducible sum: TWELVE independent binned accumulators — 3 streams
// (breaking the rung-ladder latency chain) × 4 Vec4d lanes — merged at the
// end. Legal precisely because the binned representation is
// partition-INDEPENDENT (the repartition/shuffle gates prove the property);
// the merged bits equal the scalar whole-array sum. The per-element ladder is
// the exact scalar deposit (sticky-or, two-sum) per lane. The rare index-0
// compression regime (|x| within 2^40 of DBL_MAX) falls back to scalar.
namespace binned_avx2
{
inline constexpr crd::u64 kStreams = 3U;
inline constexpr crd::u64 kLanes = 4U;
inline constexpr crd::u64 kStep = kStreams * kLanes;

// One super-block of deposits over the 12 SoA accumulators. TrackMax fuses
// the |max| scan into the same pass (the speculative single-DRAM-pass mode);
// returns the block's abs-max (0 when not tracking).
template <bool TrackMax>
[[nodiscard]] inline crd::f64 deposit_superblock(BinnedF64 (&lanes)[kStep], const crd::f64* p, crd::u64 len) noexcept
{
    const __m256d sticky = _mm256_castsi256_pd(_mm256_set1_epi64x(1LL));
    const __m256d absmask = _mm256_castsi256_pd(_mm256_set1_epi64x(0x7FFFFFFFFFFFFFFFLL));
    __m256d pri[kStreams][kBinFold];
    for (crd::u64 s = 0; s < kStreams; ++s)
    {
        for (crd::i32 k = 0; k < kBinFold; ++k)
        {
            pri[s][k] = _mm256_set_pd(lanes[s * kLanes + 3U].pri[k], lanes[s * kLanes + 2U].pri[k],
                                      lanes[s * kLanes + 1U].pri[k], lanes[s * kLanes + 0U].pri[k]);
        }
    }
    __m256d vmax = _mm256_setzero_pd();
    for (crd::u64 k = 0; k < len; k += kStep)
    {
        for (crd::u64 s = 0; s < kStreams; ++s)
        {
            __m256d x = _mm256_loadu_pd(p + k + s * kLanes);
            if constexpr (TrackMax)
            {
                vmax = _mm256_max_pd(vmax, _mm256_and_pd(x, absmask));
            }
            for (crd::i32 r = 0; r < kBinFold - 1; ++r)
            {
                const __m256d m = pri[s][r];
                const __m256d q = _mm256_or_pd(x, sticky);
                const __m256d snew = _mm256_add_pd(q, m);
                pri[s][r] = snew;
                x = _mm256_add_pd(x, _mm256_sub_pd(m, snew));
            }
            pri[s][kBinFold - 1] = _mm256_add_pd(pri[s][kBinFold - 1], _mm256_or_pd(x, sticky));
        }
    }
    for (crd::u64 s = 0; s < kStreams; ++s)
    {
        for (crd::i32 k = 0; k < kBinFold; ++k)
        {
            alignas(32) crd::f64 lv[kLanes];
            _mm256_store_pd(lv, pri[s][k]);
            for (crd::u64 l = 0; l < kLanes; ++l)
            {
                lanes[s * kLanes + l].pri[k] = lv[l];
            }
        }
    }
    if constexpr (TrackMax)
    {
        alignas(32) crd::f64 mx[kLanes];
        _mm256_store_pd(mx, vmax);
        crd::f64 amax = mx[0];
        for (crd::u64 l = 1; l < kLanes; ++l)
        {
            amax = mx[l] > amax ? mx[l] : amax;
        }
        return amax;
    }
    else
    {
        return 0.0;
    }
}
} // namespace binned_avx2

inline void binned_sum_run_avx2(BinnedF64& out, const crd::f64* p, crd::u64 n) noexcept
{
    using namespace binned_avx2;
    BinnedF64 lanes[kStep]; // 12 lane accumulators (SoA inside the kernel)

    crd::i32 cur_index = -1; // no window yet: the first block seeds it two-pass
    crd::u64 i = 0;
    while (i + kStep <= n)
    {
        const crd::u64 len0 = ((n - i) / kStep) * kStep;
        const crd::u64 len = len0 < kBinEndurance * kStep ? len0 : kBinEndurance * kStep;

        if (cur_index < 0)
        {
            // Seed pass: scan |max| first (reads once from DRAM, deposit hits L2).
            __m256d vmax = _mm256_setzero_pd();
            const __m256d absmask = _mm256_castsi256_pd(_mm256_set1_epi64x(0x7FFFFFFFFFFFFFFFLL));
            for (crd::u64 k = 0; k < len; k += kLanes)
            {
                vmax = _mm256_max_pd(vmax, _mm256_and_pd(_mm256_loadu_pd(p + i + k), absmask));
            }
            alignas(32) crd::f64 mx[kLanes];
            _mm256_store_pd(mx, vmax);
            crd::f64 amax = mx[0];
            for (crd::u64 l = 1; l < kLanes; ++l)
            {
                amax = mx[l] > amax ? mx[l] : amax;
            }
            if (bin_index_of(amax) == 0) // near-DBL_MAX regime: scalar handles compression
            {
                break;
            }
            for (auto& lane : lanes)
            {
                lane.update(amax);
            }
            cur_index = bin_index_of_acc(lanes[0].pri[0]);
            (void)deposit_superblock<false>(lanes, p + i, len);
        }
        else
        {
            // SPECULATIVE single-DRAM-pass: deposit under the previous block's
            // window while tracking |max| in the same registers. On the rare
            // violation (a bigger magnitude arrived: its index < the window's)
            // restore the 576-byte snapshot, widen, and redo from L2. The
            // result is bit-identical either way (partition-independence).
            BinnedF64 snap[kStep];
            for (crd::u64 l = 0; l < kStep; ++l)
            {
                snap[l] = lanes[l];
            }
            const crd::f64 amax = deposit_superblock<true>(lanes, p + i, len);
            const crd::i32 amax_index = bin_index_of(amax);
            if (amax_index == 0)
            {
                for (crd::u64 l = 0; l < kStep; ++l)
                {
                    lanes[l] = snap[l];
                }
                break; // scalar tail handles the compression regime
            }
            if (amax_index < cur_index)
            {
                for (crd::u64 l = 0; l < kStep; ++l)
                {
                    lanes[l] = snap[l];
                }
                for (auto& lane : lanes)
                {
                    lane.update(amax);
                }
                cur_index = bin_index_of_acc(lanes[0].pri[0]);
                (void)deposit_superblock<false>(lanes, p + i, len);
            }
        }
        for (auto& lane : lanes)
        {
            lane.renorm();
        }
        i += len;
    }
    // Merge the lane accumulators + the scalar tail into `out`.
    for (const auto& lane : lanes)
    {
        out.add(lane);
    }
    if (i < n)
    {
        crd::u64 j = i;
        while (j < n)
        {
            const crd::u64 len = n - j < kBinEndurance ? n - j : kBinEndurance;
            crd::f64 amax = 0.0;
            for (crd::u64 k = 0; k < len; ++k)
            {
                const crd::f64 a = p[j + k] < 0.0 ? -p[j + k] : p[j + k];
                amax = a > amax ? a : amax;
            }
            out.update(amax);
            for (crd::u64 k = 0; k < len; ++k)
            {
                out.deposit(p[j + k]);
            }
            out.renorm();
            j += len;
        }
    }
}
#endif // __AVX2__

// Reproducible sum of a contiguous run: per-endurance-block max-update +
// deposits + renorm (the binnedBLAS dbdsum protocol).
inline void binned_sum_run_scalar(BinnedF64& acc, const crd::f64* p, crd::u64 n) noexcept
{
    crd::u64 i = 0;
    while (i < n)
    {
        const crd::u64 len = n - i < kBinEndurance ? n - i : kBinEndurance;
        crd::f64 amax = 0.0;
        for (crd::u64 k = 0; k < len; ++k) // |max| scan (branch-light; vectorizes)
        {
            const crd::f64 a = p[i + k] < 0.0 ? -p[i + k] : p[i + k];
            amax = a > amax ? a : amax;
        }
        acc.update(amax);
        for (crd::u64 k = 0; k < len; ++k)
        {
            acc.deposit(p[i + k]);
        }
        acc.renorm();
        i += len;
    }
}

// Dispatch: the 12-accumulator SIMD path when available, scalar otherwise —
// identical bits either way (the partition-independence property, gated).
inline void binned_sum_run(BinnedF64& acc, const crd::f64* p, crd::u64 n) noexcept
{
#if defined(__AVX2__)
    if (n >= 96U)
    {
        binned_sum_run_avx2(acc, p, n);
        return;
    }
#endif
    binned_sum_run_scalar(acc, p, n);
}

} // namespace detail

// ============================================================================
// Public reductions (Tier D). `ws` is the optional parallel workspace: with
// >= ceil(n/4096) elements and a live jobs pool the blocks run in parallel —
// bit-identical to serial by construction ({1..16} gated).
// ============================================================================

template <typename T>
[[nodiscard]] inline T reduce_sum(const TensorView<const T>& v, crd::containers::Span<T> ws = {}) noexcept
{
    CRD_ASSERT_MSG(v.is_contiguous(), "reduce_sum: contiguous view required (materialize first)");
    return detail::reduce_fixed_tree<T, detail::RAdd<T>>(v.data(), v.size(), T{0}, ws);
}
template <typename T>
[[nodiscard]] inline T reduce_prod(const TensorView<const T>& v, crd::containers::Span<T> ws = {}) noexcept
{
    CRD_ASSERT_MSG(v.is_contiguous(), "reduce_prod: contiguous view required");
    return detail::reduce_fixed_tree<T, detail::RMul<T>>(v.data(), v.size(), T{1}, ws);
}
template <typename T>
[[nodiscard]] inline T reduce_min(const TensorView<const T>& v, crd::containers::Span<T> ws = {}) noexcept
{
    CRD_ASSERT_MSG(v.size() > 0U && v.is_contiguous(), "reduce_min: non-empty contiguous view required");
    return detail::reduce_fixed_tree<T, detail::RMin<T>>(v.data(), v.size(), v.data()[0], ws);
}
template <typename T>
[[nodiscard]] inline T reduce_max(const TensorView<const T>& v, crd::containers::Span<T> ws = {}) noexcept
{
    CRD_ASSERT_MSG(v.size() > 0U && v.is_contiguous(), "reduce_max: non-empty contiguous view required");
    return detail::reduce_fixed_tree<T, detail::RMax<T>>(v.data(), v.size(), v.data()[0], ws);
}
template <typename T>
[[nodiscard]] inline T reduce_mean(const TensorView<const T>& v, crd::containers::Span<T> ws = {}) noexcept
{
    return reduce_sum(v, ws) / static_cast<T>(v.size());
}

// First-extremum argmin/argmax (NumPy tie semantics: the LOWEST index wins) —
// per-block (value, first-index) partials folded in block order.
template <typename T> [[nodiscard]] inline crd::u64 reduce_argmin(const TensorView<const T>& v) noexcept
{
    CRD_ASSERT_MSG(v.size() > 0U && v.is_contiguous(), "reduce_argmin: non-empty contiguous view required");
    const T* p = v.data();
    T best = p[0];
    crd::u64 bi = 0;
    for (crd::u64 i = 1; i < v.size(); ++i)
    {
        if (p[i] < best)
        {
            best = p[i];
            bi = i;
        }
    }
    return bi;
}
template <typename T> [[nodiscard]] inline crd::u64 reduce_argmax(const TensorView<const T>& v) noexcept
{
    CRD_ASSERT_MSG(v.size() > 0U && v.is_contiguous(), "reduce_argmax: non-empty contiguous view required");
    const T* p = v.data();
    T best = p[0];
    crd::u64 bi = 0;
    for (crd::u64 i = 1; i < v.size(); ++i)
    {
        if (best < p[i])
        {
            best = p[i];
            bi = i;
        }
    }
    return bi;
}

// Exact serial prefix sum (the inherently ordered op; matches np.cumsum).
template <typename T>
[[nodiscard]] inline TensorStatus reduce_cumsum(const TensorView<const T>& v, const TensorView<T>& dst) noexcept
{
    if (dst.rank() != v.rank() || dst.size() != v.size() || !dst.is_contiguous() || !v.is_contiguous())
    {
        return TensorStatus::ShapeMismatch;
    }
    const T* p = v.data();
    T* o = dst.data();
    T acc = T{0};
    for (crd::u64 i = 0; i < v.size(); ++i)
    {
        acc += p[i];
        o[i] = acc;
    }
    return TensorStatus::Ok;
}

// log(sum(exp(x))) with the max-shift (deterministic: crd::math::exp/log +
// the Tier-D fixed tree for the shifted sum).
[[nodiscard]] crd::f64 reduce_logsumexp(const TensorView<const crd::f64>& v) noexcept; // reduce.cpp

// ============================================================================
// TIER R — reproducible sum (partition-INDEPENDENT: any chunking/merge order
// of BinnedAccumulatorF64 pieces yields identical bits; gated under forced
// repartition + shuffle).
// ============================================================================

using BinnedAccumulatorF64 = detail::BinnedF64;

[[nodiscard]] inline crd::f64 reduce_sum_reproducible(const TensorView<const crd::f64>& v) noexcept
{
    CRD_ASSERT_MSG(v.is_contiguous(), "reduce_sum_reproducible: contiguous view required");
    detail::BinnedF64 acc;
    detail::binned_sum_run(acc, v.data(), v.size());
    return acc.convert();
}

// Chunked/parallel building block: accumulate a piece, merge with add().
inline void binned_accumulate(BinnedAccumulatorF64& acc, crd::containers::ConstSpan<crd::f64> piece) noexcept
{
    detail::binned_sum_run(acc, piece.data(), piece.size());
}

// ============================================================================
// ★Deterministic SR accumulation (the ADR-0096 §4 3-tuple: seed, step index,
// the accumulation domain tag) — sum into a bf16 accumulator, reproducible by
// seed, unbiased on the bf16 grid.
// ============================================================================

inline constexpr crd::u32 kSrAccumStep = 0xACC0'0001U;

[[nodiscard]] inline crd::u16 reduce_sum_sr_bf16(const TensorView<const crd::f32>& v, crd::u64 seed) noexcept
{
    CRD_ASSERT_MSG(v.is_contiguous(), "reduce_sum_sr_bf16: contiguous view required");
    const crd::f32* p = v.data();
    crd::u16 acc = 0; // +0.0bf16
    for (crd::u64 i = 0; i < v.size(); ++i)
    {
        const crd::f32 wide = crd::math::bf16_bits_to_f32(acc) + p[i];
        acc = crd::math::f32_to_bf16_bits_sr(wide, detail::sr_draw(seed, i, kSrAccumStep));
    }
    return acc;
}

} // namespace crd::hesap::tensor
