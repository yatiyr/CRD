#pragma once

#include "binned_core.hpp"
#include "dtypes.hpp"
#include "tensor.hpp"
#include "tensor_dispatch.hpp"

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/jobs/jobs.hpp>

#include <type_traits>

// ---------------------------------------------------------------------------
// crd-hesap-tensor reduce — reductions + the reproducible tier
// (Phase 3.1.6 v14-c; ADR-0096 §4).
//
// THE TWO NAMED TIERS (never conflated — the honest-scoreboard scar):
//
//   TIER D (default): fixed-order reduction trees, serial ≡ parallel.
//   The tensor is cut into kReduceBlock-element blocks — a function of the
//   SHAPE ONLY, never of num_workers — each block produces one partial with a
//   fixed internal order (four vertical accumulators of kEwLanes lanes, the
//   tensor kernels at the CPU's level; lanes folded in a pinned order, tail
//   appended last), and the partials are
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

// ---- Tier-D op functors: the scalar fold and the kernels of the op (kReduceBlock, the block: tensor_kernels.hpp) ----
// Min and Max are minps and maxps (the second operand on a NaN or a signed-zero tie), as in the kernels' vector lanes.
template <typename T> struct RAdd
{
    static constexpr ReduceOp kOp = ReduceOp::Sum;
    static constexpr T kInit = T{0};
    static T s(T a, T b) noexcept { return a + b; }
};
template <typename T> struct RMul
{
    static constexpr ReduceOp kOp = ReduceOp::Prod;
    static constexpr T kInit = T{1};
    static T s(T a, T b) noexcept { return a * b; }
};
template <typename T> struct RMin
{
    static constexpr ReduceOp kOp = ReduceOp::Min;
    static T s(T a, T b) noexcept { return a < b ? a : b; }
};
template <typename T> struct RMax
{
    static constexpr ReduceOp kOp = ReduceOp::Max;
    static T s(T a, T b) noexcept { return a > b ? a : b; }
};

// The Tier D kernels of Op at the CPU's level.
template <typename T, typename Op> [[nodiscard]] inline const ReduceKernels<T>& reduce_kernels() noexcept
{
    static_assert(std::is_same_v<T, crd::f32> || std::is_same_v<T, crd::f64>, "Tier D computes f32 and f64");
    const TensorKernels& k = *tensor_kernels().table;
    if constexpr (std::is_same_v<T, crd::f32>)
    {
        return k.reduce_f32[static_cast<crd::u32>(Op::kOp)];
    }
    else
    {
        return k.reduce_f64[static_cast<crd::u32>(Op::kOp)];
    }
}

// One contiguous block's partial with the PINNED internal order (ReduceKernels::block).
template <typename T, typename Op> [[nodiscard]] inline T block_partial(const T* p, crd::u64 n, T init) noexcept
{
    return reduce_kernels<T, Op>().block(p, n, init);
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

// TIER R: the binned accumulator (binned_core.hpp, the ReproBLAS v2.1.0 fold-3 transcription). Its contiguous sum
// runs the per-level tensor kernels (the twelve-accumulator AVX2 body or the scalar deposits), identical bits on every
// level and in any partition.
struct BinnedF64
{
    BinnedRungs rungs{};

    // binned_dmdmadd: this += other.
    void add(const BinnedF64& other) noexcept { binned_add(rungs, other.rungs); }

    // binned_ddmconv: the represented value, rounded once.
    [[nodiscard]] crd::f64 convert() const noexcept { return binned_convert(rungs); }
};

inline void binned_sum_run(BinnedF64& acc, const crd::f64* p, crd::u64 n) noexcept
{
    tensor_kernels().table->binned_sum_f64(acc.rungs, p, n);
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
