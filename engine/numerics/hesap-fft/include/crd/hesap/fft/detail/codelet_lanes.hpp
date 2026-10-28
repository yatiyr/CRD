// The lanes of the generated FFT codelet bodies (SIMD-DISPATCH, ADR-0134 §5, ADR-0063).
//
// A generated body is a DAG of lane-wise IEEE operations over a tile of four f64 or eight f32 columns. Optimized
// builds above the portable level run a tile as one Vec4d or Vec8f. Unoptimized builds and the portable level run the
// same body one column at a time on scalars: the same operations on every column, so the same bits, with a quarter or
// an eighth of the code. The portable vectors are arrays (a hierarchical codelet unit took 10.9 GB at /O2, 2026-10-28),
// and unoptimized builds give every temporary its own stack slot (whole SIMD bodies needed 1.2-1.4 MB frames, MSVC
// /Od /RTC1, 2026-07-03). __OPTIMIZE__ covers optimized GCC/Clang builds without NDEBUG; MSVC's optimized
// configurations all define NDEBUG here. Only codelet units include this header.

#pragma once

#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>
#include <crd/math/kernel_level.hpp>
#include <crd/math/libm.hpp>
#include <crd/math/simd/backend.hpp>

#if (defined(NDEBUG) || defined(__OPTIMIZE__)) && !CRD_SIMD_IS_SCALAR
#define CRD_FFT_GEN_SIMD_BODY
#include <crd/math/simd/vec4d.hpp>
#include <crd/math/simd/vec8f.hpp>
#endif

namespace crd::hesap::fft::lanes::inline CRD_MATH_LEVEL_NS
{

// Lane<T> holds the columns one step of a tile computes; kWidth<V> counts them.
template <typename T> struct LaneOf
{
    using Type = T;
};
template <typename V> inline constexpr crd::usize kWidth = 1;

#ifdef CRD_FFT_GEN_SIMD_BODY

template <> struct LaneOf<crd::f64>
{
    using Type = crd::math::simd::Vec4d;
};
template <> struct LaneOf<crd::f32>
{
    using Type = crd::math::simd::Vec8f;
};
template <> inline constexpr crd::usize kWidth<crd::math::simd::Vec4d> = 4;
template <> inline constexpr crd::usize kWidth<crd::math::simd::Vec8f> = 8;

using crd::math::simd::fma;
using crd::math::simd::fnmadd;
using crd::math::simd::load_complex_deinterleaved;
using crd::math::simd::store_complex_interleaved;

[[nodiscard]] CRD_FORCEINLINE crd::math::simd::Vec4d load(const crd::f64* p) noexcept
{
    return crd::math::simd::Vec4d::load(p);
}
[[nodiscard]] CRD_FORCEINLINE crd::math::simd::Vec8f load(const crd::f32* p) noexcept
{
    return crd::math::simd::Vec8f::load(p);
}
template <typename V, typename T> CRD_FORCEINLINE void store(T* p, V v) noexcept
{
    v.store(p);
}

#else

// The vector operations' column semantics: fma and fnmadd round once, as the instructions and libm::fma do.
template <typename T> [[nodiscard]] CRD_FORCEINLINE T fma(T a, T b, T c) noexcept
{
    return crd::math::libm::fma(a, b, c);
}
template <typename T> [[nodiscard]] CRD_FORCEINLINE T fnmadd(T a, T b, T c) noexcept
{
    return crd::math::libm::fma(-a, b, c);
}
template <typename T> [[nodiscard]] CRD_FORCEINLINE T load(const T* p) noexcept
{
    return *p;
}
template <typename T> CRD_FORCEINLINE void store(T* p, T v) noexcept
{
    *p = v;
}
template <typename T> CRD_FORCEINLINE void load_complex_deinterleaved(const T* p, T& re, T& im) noexcept
{
    re = p[0];
    im = p[1];
}
template <typename T> CRD_FORCEINLINE void store_complex_interleaved(T* p, T re, T im) noexcept
{
    p[0] = re;
    p[1] = im;
}

#endif

template <typename T> using Lane = typename LaneOf<T>::Type;

} // namespace crd::hesap::fft::lanes::inline CRD_MATH_LEVEL_NS
