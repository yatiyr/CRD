#pragma once

// The short contiguous dot and axpy of the split-complex sweeps (the Hermitian tridiagonalization in eig_sym.cpp
// and detail/dot_simd_complex.hpp), which call them once per panel row: two Vec4d (f64) or one Vec8f (f32)
// accumulators and single-rounded fma. Every real sweep runs on the per-level kernels of
// crd/hesap/dense/dense_kernels.hpp; these follow them as per-level units (SIMD-DISPATCH).

#include <crd/core/types.hpp>
#include <crd/math/simd/vec4d.hpp>
#include <crd/math/simd/vec8f.hpp>

#include <type_traits>

namespace crd::hesap::dense::detail
{

// f32 and f64; other T run the scalar loop.
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
