#pragma once

// SIMD-vectorized dot / nrm2-squared kernels for f32 + f64 (v0b-simd-followon).
//
// Trade-off vs the scalar KBN-pairwise path in pairwise_sum.hpp:
//   - Scalar KBN: ~1 GFLOPS, bit-exact across compilers + numerically stable
//     for ill-conditioned inputs. Used by physics replay (ADR-0063 strict).
//   - SIMD direct: 30-50 GFLOPS f32 / 15-25 GFLOPS f64, no KBN compensation,
//     matches BLAS conventional accuracy (Eigen / OpenBLAS / MKL parity).
//     Reduction tree (4 independent accumulators + pairwise combine +
//     Vec8f/Vec4d horizontal_sum) is deterministic AND bit-identical across
//     SIMD widths within hesap.
//
// Per ADR-0082 §determinism-relaxation (FMA path established 2026-05-20):
// hesap is numerical computing, not physics replay. Users needing KBN call
// `crd::hesap::dense::detail::kbn_sum` / `pairwise_sum` directly.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/simd/vec4d.hpp>
#include <crd/math/simd/vec8f.hpp>

#include <type_traits>

namespace crd::hesap::dense::detail
{

// Inner product (x . y) using 8 independent Vec8f accumulators + FMA.
// 8 accumulators give 2 FMA ports × 4-cycle latency = 8 in-flight = peak ILP.
[[nodiscard]] inline crd::f32 simd_dot_f32(const crd::f32* x, const crd::f32* y,
                                           crd::usize n) noexcept
{
    namespace simd = crd::math::simd;
    simd::Vec8f a0 = simd::Vec8f::zero();
    simd::Vec8f a1 = simd::Vec8f::zero();
    simd::Vec8f a2 = simd::Vec8f::zero();
    simd::Vec8f a3 = simd::Vec8f::zero();
    simd::Vec8f a4 = simd::Vec8f::zero();
    simd::Vec8f a5 = simd::Vec8f::zero();
    simd::Vec8f a6 = simd::Vec8f::zero();
    simd::Vec8f a7 = simd::Vec8f::zero();
    crd::usize i = 0;
    for (; i + 64 <= n; i += 64)
    {
        a0 = simd::fma(simd::Vec8f::load(x + i + 0),  simd::Vec8f::load(y + i + 0),  a0);
        a1 = simd::fma(simd::Vec8f::load(x + i + 8),  simd::Vec8f::load(y + i + 8),  a1);
        a2 = simd::fma(simd::Vec8f::load(x + i + 16), simd::Vec8f::load(y + i + 16), a2);
        a3 = simd::fma(simd::Vec8f::load(x + i + 24), simd::Vec8f::load(y + i + 24), a3);
        a4 = simd::fma(simd::Vec8f::load(x + i + 32), simd::Vec8f::load(y + i + 32), a4);
        a5 = simd::fma(simd::Vec8f::load(x + i + 40), simd::Vec8f::load(y + i + 40), a5);
        a6 = simd::fma(simd::Vec8f::load(x + i + 48), simd::Vec8f::load(y + i + 48), a6);
        a7 = simd::fma(simd::Vec8f::load(x + i + 56), simd::Vec8f::load(y + i + 56), a7);
    }
    for (; i + 8 <= n; i += 8)
    {
        a0 = simd::fma(simd::Vec8f::load(x + i), simd::Vec8f::load(y + i), a0);
    }
    // Balanced reduction tree.
    const simd::Vec8f s01 = a0 + a1;
    const simd::Vec8f s23 = a2 + a3;
    const simd::Vec8f s45 = a4 + a5;
    const simd::Vec8f s67 = a6 + a7;
    const simd::Vec8f total = (s01 + s23) + (s45 + s67);
    crd::f32 result = simd::horizontal_sum(total);
    for (; i < n; ++i)
    {
        result += x[i] * y[i];
    }
    return result;
}

[[nodiscard]] inline crd::f64 simd_dot_f64(const crd::f64* x, const crd::f64* y,
                                           crd::usize n) noexcept
{
    namespace simd = crd::math::simd;
    simd::Vec4d a0 = simd::Vec4d::zero();
    simd::Vec4d a1 = simd::Vec4d::zero();
    simd::Vec4d a2 = simd::Vec4d::zero();
    simd::Vec4d a3 = simd::Vec4d::zero();
    simd::Vec4d a4 = simd::Vec4d::zero();
    simd::Vec4d a5 = simd::Vec4d::zero();
    simd::Vec4d a6 = simd::Vec4d::zero();
    simd::Vec4d a7 = simd::Vec4d::zero();
    crd::usize i = 0;
    for (; i + 32 <= n; i += 32)
    {
        a0 = simd::fma(simd::Vec4d::load(x + i + 0),  simd::Vec4d::load(y + i + 0),  a0);
        a1 = simd::fma(simd::Vec4d::load(x + i + 4),  simd::Vec4d::load(y + i + 4),  a1);
        a2 = simd::fma(simd::Vec4d::load(x + i + 8),  simd::Vec4d::load(y + i + 8),  a2);
        a3 = simd::fma(simd::Vec4d::load(x + i + 12), simd::Vec4d::load(y + i + 12), a3);
        a4 = simd::fma(simd::Vec4d::load(x + i + 16), simd::Vec4d::load(y + i + 16), a4);
        a5 = simd::fma(simd::Vec4d::load(x + i + 20), simd::Vec4d::load(y + i + 20), a5);
        a6 = simd::fma(simd::Vec4d::load(x + i + 24), simd::Vec4d::load(y + i + 24), a6);
        a7 = simd::fma(simd::Vec4d::load(x + i + 28), simd::Vec4d::load(y + i + 28), a7);
    }
    for (; i + 4 <= n; i += 4)
    {
        a0 = simd::fma(simd::Vec4d::load(x + i), simd::Vec4d::load(y + i), a0);
    }
    const simd::Vec4d s01 = a0 + a1;
    const simd::Vec4d s23 = a2 + a3;
    const simd::Vec4d s45 = a4 + a5;
    const simd::Vec4d s67 = a6 + a7;
    const simd::Vec4d total = (s01 + s23) + (s45 + s67);
    crd::f64 result = simd::horizontal_sum(total);
    for (; i < n; ++i)
    {
        result += x[i] * y[i];
    }
    return result;
}

// Sum of squares (||x||^2). Reused by nrm2 (which sqrts the result).
// 8 accumulators for max ILP.
[[nodiscard]] inline crd::f32 simd_sumsq_f32(const crd::f32* x, crd::usize n) noexcept
{
    namespace simd = crd::math::simd;
    simd::Vec8f a0 = simd::Vec8f::zero();
    simd::Vec8f a1 = simd::Vec8f::zero();
    simd::Vec8f a2 = simd::Vec8f::zero();
    simd::Vec8f a3 = simd::Vec8f::zero();
    simd::Vec8f a4 = simd::Vec8f::zero();
    simd::Vec8f a5 = simd::Vec8f::zero();
    simd::Vec8f a6 = simd::Vec8f::zero();
    simd::Vec8f a7 = simd::Vec8f::zero();
    crd::usize i = 0;
    for (; i + 64 <= n; i += 64)
    {
        const simd::Vec8f v0 = simd::Vec8f::load(x + i + 0);
        const simd::Vec8f v1 = simd::Vec8f::load(x + i + 8);
        const simd::Vec8f v2 = simd::Vec8f::load(x + i + 16);
        const simd::Vec8f v3 = simd::Vec8f::load(x + i + 24);
        const simd::Vec8f v4 = simd::Vec8f::load(x + i + 32);
        const simd::Vec8f v5 = simd::Vec8f::load(x + i + 40);
        const simd::Vec8f v6 = simd::Vec8f::load(x + i + 48);
        const simd::Vec8f v7 = simd::Vec8f::load(x + i + 56);
        a0 = simd::fma(v0, v0, a0);
        a1 = simd::fma(v1, v1, a1);
        a2 = simd::fma(v2, v2, a2);
        a3 = simd::fma(v3, v3, a3);
        a4 = simd::fma(v4, v4, a4);
        a5 = simd::fma(v5, v5, a5);
        a6 = simd::fma(v6, v6, a6);
        a7 = simd::fma(v7, v7, a7);
    }
    for (; i + 8 <= n; i += 8)
    {
        const simd::Vec8f v = simd::Vec8f::load(x + i);
        a0 = simd::fma(v, v, a0);
    }
    const simd::Vec8f s01 = a0 + a1;
    const simd::Vec8f s23 = a2 + a3;
    const simd::Vec8f s45 = a4 + a5;
    const simd::Vec8f s67 = a6 + a7;
    const simd::Vec8f total = (s01 + s23) + (s45 + s67);
    crd::f32 result = simd::horizontal_sum(total);
    for (; i < n; ++i)
    {
        result += x[i] * x[i];
    }
    return result;
}

[[nodiscard]] inline crd::f64 simd_sumsq_f64(const crd::f64* x, crd::usize n) noexcept
{
    namespace simd = crd::math::simd;
    simd::Vec4d a0 = simd::Vec4d::zero();
    simd::Vec4d a1 = simd::Vec4d::zero();
    simd::Vec4d a2 = simd::Vec4d::zero();
    simd::Vec4d a3 = simd::Vec4d::zero();
    simd::Vec4d a4 = simd::Vec4d::zero();
    simd::Vec4d a5 = simd::Vec4d::zero();
    simd::Vec4d a6 = simd::Vec4d::zero();
    simd::Vec4d a7 = simd::Vec4d::zero();
    crd::usize i = 0;
    for (; i + 32 <= n; i += 32)
    {
        const simd::Vec4d v0 = simd::Vec4d::load(x + i + 0);
        const simd::Vec4d v1 = simd::Vec4d::load(x + i + 4);
        const simd::Vec4d v2 = simd::Vec4d::load(x + i + 8);
        const simd::Vec4d v3 = simd::Vec4d::load(x + i + 12);
        const simd::Vec4d v4 = simd::Vec4d::load(x + i + 16);
        const simd::Vec4d v5 = simd::Vec4d::load(x + i + 20);
        const simd::Vec4d v6 = simd::Vec4d::load(x + i + 24);
        const simd::Vec4d v7 = simd::Vec4d::load(x + i + 28);
        a0 = simd::fma(v0, v0, a0);
        a1 = simd::fma(v1, v1, a1);
        a2 = simd::fma(v2, v2, a2);
        a3 = simd::fma(v3, v3, a3);
        a4 = simd::fma(v4, v4, a4);
        a5 = simd::fma(v5, v5, a5);
        a6 = simd::fma(v6, v6, a6);
        a7 = simd::fma(v7, v7, a7);
    }
    for (; i + 4 <= n; i += 4)
    {
        const simd::Vec4d v = simd::Vec4d::load(x + i);
        a0 = simd::fma(v, v, a0);
    }
    const simd::Vec4d s01 = a0 + a1;
    const simd::Vec4d s23 = a2 + a3;
    const simd::Vec4d s45 = a4 + a5;
    const simd::Vec4d s67 = a6 + a7;
    const simd::Vec4d total = (s01 + s23) + (s45 + s67);
    crd::f64 result = simd::horizontal_sum(total);
    for (; i < n; ++i)
    {
        result += x[i] * x[i];
    }
    return result;
}

// Templated contiguous SIMD dot / axpy over [0, n) (f32 + f64; other T fall to
// scalar). Lighter 2-accumulator (f64) / 1-accumulator (f32) shape than the
// 8-way simd_dot_* above — the right balance for the short panel rows in the
// blocked dsytrd / dgebrd reductions, which call these per matrix row. FMA
// (single-rounded), deterministic + bit-identical across widths within hesap.
template <typename T>
[[nodiscard]] inline T simd_dot(const T* x, const T* y, crd::usize n) noexcept
{
    namespace simd = crd::math::simd;
    crd::usize t = 0;
    T acc{};
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        simd::Vec4d a0 = simd::Vec4d::zero();
        simd::Vec4d a1 = simd::Vec4d::zero();
        for (; t + 8 <= n; t += 8)
        {
            a0 = simd::fma(simd::Vec4d::load(x + t), simd::Vec4d::load(y + t), a0);
            a1 = simd::fma(simd::Vec4d::load(x + t + 4), simd::Vec4d::load(y + t + 4), a1);
        }
        acc = simd::horizontal_sum(a0 + a1);
    }
    else if constexpr (std::is_same_v<T, crd::f32>)
    {
        simd::Vec8f a0 = simd::Vec8f::zero();
        for (; t + 8 <= n; t += 8)
        {
            a0 = simd::fma(simd::Vec8f::load(x + t), simd::Vec8f::load(y + t), a0);
        }
        acc = simd::horizontal_sum(a0);
    }
    for (; t < n; ++t)
    {
        acc += x[t] * y[t];
    }
    return acc;
}

template <typename T>
inline void simd_axpy(T* y, const T* x, T a, crd::usize n) noexcept
{
    namespace simd = crd::math::simd;
    crd::usize t = 0;
    if constexpr (std::is_same_v<T, crd::f64>)
    {
        const simd::Vec4d av(a);
        for (; t + 8 <= n; t += 8)
        {
            simd::fma(av, simd::Vec4d::load(x + t), simd::Vec4d::load(y + t)).store(y + t);
            simd::fma(av, simd::Vec4d::load(x + t + 4), simd::Vec4d::load(y + t + 4)).store(y + t + 4);
        }
    }
    else if constexpr (std::is_same_v<T, crd::f32>)
    {
        const simd::Vec8f av(a);
        for (; t + 8 <= n; t += 8)
        {
            simd::fma(av, simd::Vec8f::load(x + t), simd::Vec8f::load(y + t)).store(y + t);
        }
    }
    for (; t < n; ++t)
    {
        y[t] += a * x[t];
    }
}

} // namespace crd::hesap::dense::detail
