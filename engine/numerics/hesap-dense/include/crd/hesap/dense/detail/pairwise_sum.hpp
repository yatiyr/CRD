#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>

namespace crd::hesap::dense::detail
{
// -----------------------------------------------------------------------
// Kahan-Babuška-Neumaier (KBN) compensated summation + pairwise reduction
// tree. Phase 3.1.6 v0b — the deterministic reduction primitive every
// BLAS L1 sum / norm / abs-sum / dot consumes (per D10).
//
// Why both KBN AND pairwise:
//   - Pairwise alone: O(sqrt(log N) * eps * |sum|) round-off (Higham 2002).
//   - KBN alone (one accumulator):   O(eps * |sum|) but only when the
//     summation order doesn't cross-cancel the compensation register.
//   - KBN at the leaves of a pairwise tree: combines both — the leaves
//     get KBN's ~1-ulp accuracy on small blocks; the internal tree gets
//     pairwise's accuracy-vs-N profile. Stan-math / Eigen / xsimd
//     reductions all use this composite shape.
//
// Determinism contract (ADR-0063): the summation tree topology is fixed
// (8-wide leaves; tree depth = ceil(log2(N / 8))). Same input → same
// output across scalar / SIMD / parallel paths AS LONG AS each path
// walks the same tree. The scalar reference path here is the canonical
// algorithm definition; SIMD specialisations are mechanical translations.
//
// Block size locked at 8 per D11 — matches Vec8f lane count so a future
// SIMD path can vectorise the leaf 1:1.
// -----------------------------------------------------------------------

inline constexpr crd::usize kPairwiseLeafBlock = 8;

// KBN scalar sum of `xs.size()` elements (any size). Single-accumulator;
// runs in O(N) with one f64 (or T) of running compensation. Used for the
// leaf blocks of the pairwise tree.
template <typename T>
[[nodiscard]] T kbn_sum(crd::containers::ConstSpan<T> xs) noexcept
{
    T sum{};
    T comp{};
    for (crd::usize i = 0; i < xs.size(); ++i)
    {
        const T x = xs[i];
        const T t = sum + x;
        // Neumaier's variant: compare magnitudes; accumulate the lost low
        // bits of whichever was larger. Works for both real and complex T
        // because Complex's `<` is not defined; instead we compare each
        // component if T is complex. Branch below specialises.
        if constexpr (std::is_floating_point_v<T>)
        {
            if ((sum < T(0) ? -sum : sum) >= (x < T(0) ? -x : x))
            {
                comp += (sum - t) + x;
            }
            else
            {
                comp += (x - t) + sum;
            }
        }
        else
        {
            // Complex<U> — compensate per component.
            using U = decltype(T{}.re);
            const U abs_sum_re = sum.re < U(0) ? -sum.re : sum.re;
            const U abs_x_re = x.re < U(0) ? -x.re : x.re;
            const U abs_sum_im = sum.im < U(0) ? -sum.im : sum.im;
            const U abs_x_im = x.im < U(0) ? -x.im : x.im;

            if (abs_sum_re >= abs_x_re)
            {
                comp.re += (sum.re - t.re) + x.re;
            }
            else
            {
                comp.re += (x.re - t.re) + sum.re;
            }
            if (abs_sum_im >= abs_x_im)
            {
                comp.im += (sum.im - t.im) + x.im;
            }
            else
            {
                comp.im += (x.im - t.im) + sum.im;
            }
        }
        sum = t;
    }
    return sum + comp;
}

// Pairwise tree sum of any-size input. Leaves of width `kPairwiseLeafBlock`
// summed by KBN; internal nodes summed by KBN of two children. Tail block
// (last < kPairwiseLeafBlock elements) is just KBN'd directly.
//
// Algorithm:
//   1. Split `xs` into blocks of size kPairwiseLeafBlock (last may be short).
//   2. Compute each block's KBN sum (= leaf value).
//   3. Reduce the leaves with KBN, pairwise — recursive split-and-combine
//      to keep the round-off profile pairwise (not linear).
//
// For tiny N (≤ kPairwiseLeafBlock) skip the tree entirely; just KBN.
template <typename T>
[[nodiscard]] T pairwise_sum(crd::containers::ConstSpan<T> xs) noexcept
{
    if (xs.size() <= kPairwiseLeafBlock)
    {
        return kbn_sum(xs);
    }

    // Build leaves on the stack — kPairwiseLeafBlock = 8 leaves means up to
    // N/8 entries. For very large N we still allocate-on-stack (up to a cap)
    // and recurse if needed.
    // For simplicity in v0b: build leaves via two-pass split + pairwise combine.
    // Iterative version to avoid stack-depth concerns at huge N:
    //   - First pass: replace the input with leaf KBN sums (one per 8-block).
    //   - Loop: pairwise-combine adjacent pairs (sum[i] = KBN(sum[2i], sum[2i+1]))
    //     until size 1.
    //
    // To stay deterministic and avoid scratch alloc, we recurse via a
    // bounded-depth split: this function is called rarely with truly huge N
    // (BLAS L1 inputs typical for v0b: up to ~10^6), so a recursive split
    // with depth ceil(log2(N/8)) ≈ 17 frames at N=10^6 is fine.

    // Split point — round up to a multiple of leaf block so left half is a
    // power-of-2 multiple of the leaf size (canonical balanced tree).
    crd::usize mid = (xs.size() + 1) / 2;
    // Align mid up to the next leaf-block boundary so the left subtree is
    // always a complete sequence of leaves. The last leaf in the right
    // subtree absorbs the remainder.
    const crd::usize remainder = mid % kPairwiseLeafBlock;
    if (remainder != 0)
    {
        mid += kPairwiseLeafBlock - remainder;
    }
    if (mid > xs.size())
    {
        mid = xs.size();
    }

    const T left = pairwise_sum(crd::containers::ConstSpan<T>{xs.data(), mid});
    const T right = pairwise_sum(crd::containers::ConstSpan<T>{xs.data() + mid, xs.size() - mid});

    // Combine via 2-element KBN.
    T pair[2] = {left, right};
    return kbn_sum(crd::containers::ConstSpan<T>{pair, 2});
}

// Helper: pairwise-sum over the index range [start, end), produced lazily
// by `produce(i) -> T`. Same canonical tree topology as `pairwise_sum`
// (kPairwiseLeafBlock leaves; balanced split aligned to a leaf boundary).
//
// IMPORTANT: parameterised over the index range (not a sub-lambda) so
// `ProduceFn` is the SAME type across every recursion level. Capturing
// `produce` into a fresh `[&produce, mid]` lambda each level would give
// every recursion a distinct closure type, blowing template instantiation
// depth at compile time (C1060 on MSVC at large N).
template <typename T, typename ProduceFn>
[[nodiscard]] T pairwise_sum_in_range(crd::usize start, crd::usize end, ProduceFn produce) noexcept
{
    const crd::usize n = end - start;
    if (n <= kPairwiseLeafBlock)
    {
        T buf[kPairwiseLeafBlock]{};
        for (crd::usize i = 0; i < n; ++i)
        {
            buf[i] = produce(start + i);
        }
        return kbn_sum(crd::containers::ConstSpan<T>{buf, n});
    }
    crd::usize mid_offset = (n + 1) / 2;
    const crd::usize remainder = mid_offset % kPairwiseLeafBlock;
    if (remainder != 0)
    {
        mid_offset += kPairwiseLeafBlock - remainder;
    }
    if (mid_offset > n)
    {
        mid_offset = n;
    }
    const crd::usize mid = start + mid_offset;
    const T left = pairwise_sum_in_range<T>(start, mid, produce);
    const T right = pairwise_sum_in_range<T>(mid, end, produce);
    T pair[2] = {left, right};
    return kbn_sum(crd::containers::ConstSpan<T>{pair, 2});
}

// Convenience: pairwise-sum of `n` values produced by `produce(i) -> T`.
// Used by `dot` / `dotu` / `dotc` / `nrm2` / `asum` to avoid materialising
// an intermediate buffer.
template <typename T, typename ProduceFn>
[[nodiscard]] T pairwise_sum_produced(crd::usize n, ProduceFn produce) noexcept
{
    return pairwise_sum_in_range<T>(crd::usize{0}, n, produce);
}

} // namespace crd::hesap::dense::detail
