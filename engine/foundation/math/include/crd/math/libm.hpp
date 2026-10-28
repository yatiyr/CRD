// The scalar math a header shared by general code and kernel units calls (SIMD-DISPATCH, ADR-0134 §5).
//
// <cmath>'s float overloads and classification functions are inline functions of the standard library: a kernel unit
// that instantiates one compiles a body for its own ISA under the same symbol as general code's, and the linker keeps
// one of them for every caller. These wrappers live in the unit's level namespace and call only compiler builtins
// (GCC, Clang, clang-cl) or the C library's external functions (MSVC), so every level keeps its own copy. The sign
// and class operations are bit operations, the same on every compiler and level.

#pragma once

#include <crd/core/types.hpp>
#include <crd/math/kernel_level.hpp>

#if !defined(__clang__) && !defined(__GNUC__)
#include <math.h>
#endif

#if defined(__clang__) || defined(__GNUC__)
#define CRD_LIBM_FN(c_name) __builtin_##c_name
#else
#define CRD_LIBM_FN(c_name) ::c_name
#endif

namespace crd::math::inline CRD_MATH_LEVEL_NS::libm
{

// The exact operations, in both precisions.
[[nodiscard]] inline crd::f32 sqrt(crd::f32 x) noexcept
{
    return CRD_LIBM_FN(sqrtf)(x);
}
[[nodiscard]] inline crd::f64 sqrt(crd::f64 x) noexcept
{
    return CRD_LIBM_FN(sqrt)(x);
}
[[nodiscard]] inline crd::f32 floor(crd::f32 x) noexcept
{
    return CRD_LIBM_FN(floorf)(x);
}
[[nodiscard]] inline crd::f64 floor(crd::f64 x) noexcept
{
    return CRD_LIBM_FN(floor)(x);
}
[[nodiscard]] inline crd::f32 ceil(crd::f32 x) noexcept
{
    return CRD_LIBM_FN(ceilf)(x);
}
[[nodiscard]] inline crd::f64 ceil(crd::f64 x) noexcept
{
    return CRD_LIBM_FN(ceil)(x);
}
[[nodiscard]] inline crd::f32 round(crd::f32 x) noexcept
{
    return CRD_LIBM_FN(roundf)(x);
}
[[nodiscard]] inline crd::f64 round(crd::f64 x) noexcept
{
    return CRD_LIBM_FN(round)(x);
}
[[nodiscard]] inline crd::f32 trunc(crd::f32 x) noexcept
{
    return CRD_LIBM_FN(truncf)(x);
}
[[nodiscard]] inline crd::f64 trunc(crd::f64 x) noexcept
{
    return CRD_LIBM_FN(trunc)(x);
}
[[nodiscard]] inline crd::f32 rint(crd::f32 x) noexcept
{
    return CRD_LIBM_FN(rintf)(x);
}
[[nodiscard]] inline long lround(crd::f32 x) noexcept
{
    return CRD_LIBM_FN(lroundf)(x);
}
[[nodiscard]] inline long lround(crd::f64 x) noexcept
{
    return CRD_LIBM_FN(lround)(x);
}
[[nodiscard]] inline crd::f32 fmod(crd::f32 x, crd::f32 y) noexcept
{
    return CRD_LIBM_FN(fmodf)(x, y);
}
[[nodiscard]] inline crd::f64 fmod(crd::f64 x, crd::f64 y) noexcept
{
    return CRD_LIBM_FN(fmod)(x, y);
}
[[nodiscard]] inline crd::f32 fma(crd::f32 x, crd::f32 y, crd::f32 z) noexcept
{
    return CRD_LIBM_FN(fmaf)(x, y, z);
}
[[nodiscard]] inline crd::f64 fma(crd::f64 x, crd::f64 y, crd::f64 z) noexcept
{
    return CRD_LIBM_FN(fma)(x, y, z);
}

// The correctly rounded library results crd-math's own functions return at their edges (NaN, infinities, overflow,
// subnormal ranges), and the natural log the vector cores' oracles use.
[[nodiscard]] inline crd::f64 exp2(crd::f64 x) noexcept
{
    return CRD_LIBM_FN(exp2)(x);
}
[[nodiscard]] inline crd::f64 expm1(crd::f64 x) noexcept
{
    return CRD_LIBM_FN(expm1)(x);
}
[[nodiscard]] inline crd::f64 log(crd::f64 x) noexcept
{
    return CRD_LIBM_FN(log)(x);
}
[[nodiscard]] inline crd::f64 log2(crd::f64 x) noexcept
{
    return CRD_LIBM_FN(log2)(x);
}
[[nodiscard]] inline crd::f64 log10(crd::f64 x) noexcept
{
    return CRD_LIBM_FN(log10)(x);
}
[[nodiscard]] inline crd::f64 log1p(crd::f64 x) noexcept
{
    return CRD_LIBM_FN(log1p)(x);
}
[[nodiscard]] inline crd::f64 cbrt(crd::f64 x) noexcept
{
    return CRD_LIBM_FN(cbrt)(x);
}
[[nodiscard]] inline crd::f64 sin(crd::f64 x) noexcept
{
    return CRD_LIBM_FN(sin)(x);
}
[[nodiscard]] inline crd::f64 cos(crd::f64 x) noexcept
{
    return CRD_LIBM_FN(cos)(x);
}
[[nodiscard]] inline crd::f64 tan(crd::f64 x) noexcept
{
    return CRD_LIBM_FN(tan)(x);
}
[[nodiscard]] inline crd::f64 asin(crd::f64 x) noexcept
{
    return CRD_LIBM_FN(asin)(x);
}
[[nodiscard]] inline crd::f64 acos(crd::f64 x) noexcept
{
    return CRD_LIBM_FN(acos)(x);
}
[[nodiscard]] inline crd::f64 sinh(crd::f64 x) noexcept
{
    return CRD_LIBM_FN(sinh)(x);
}
[[nodiscard]] inline crd::f64 cosh(crd::f64 x) noexcept
{
    return CRD_LIBM_FN(cosh)(x);
}
[[nodiscard]] inline crd::f64 tanh(crd::f64 x) noexcept
{
    return CRD_LIBM_FN(tanh)(x);
}
[[nodiscard]] inline crd::f64 asinh(crd::f64 x) noexcept
{
    return CRD_LIBM_FN(asinh)(x);
}
[[nodiscard]] inline crd::f64 acosh(crd::f64 x) noexcept
{
    return CRD_LIBM_FN(acosh)(x);
}
[[nodiscard]] inline crd::f64 atanh(crd::f64 x) noexcept
{
    return CRD_LIBM_FN(atanh)(x);
}
[[nodiscard]] inline crd::f64 atan2(crd::f64 x, crd::f64 y) noexcept
{
    return CRD_LIBM_FN(atan2)(x, y);
}
[[nodiscard]] inline crd::f64 pow(crd::f64 x, crd::f64 y) noexcept
{
    return CRD_LIBM_FN(pow)(x, y);
}
[[nodiscard]] inline crd::f64 hypot(crd::f64 x, crd::f64 y) noexcept
{
    return CRD_LIBM_FN(hypot)(x, y);
}

[[nodiscard]] inline crd::f64 ldexp(crd::f64 x, int e) noexcept
{
    return CRD_LIBM_FN(ldexp)(x, e);
}

// IEEE sign and classification by bit pattern.
[[nodiscard]] constexpr crd::f32 fabs(crd::f32 x) noexcept
{
    return __builtin_bit_cast(crd::f32, __builtin_bit_cast(crd::u32, x) & 0x7FFFFFFFU);
}
[[nodiscard]] constexpr crd::f64 fabs(crd::f64 x) noexcept
{
    return __builtin_bit_cast(crd::f64, __builtin_bit_cast(crd::u64, x) & 0x7FFFFFFFFFFFFFFFULL);
}
[[nodiscard]] constexpr crd::f32 copysign(crd::f32 m, crd::f32 s) noexcept
{
    return __builtin_bit_cast(crd::f32, (__builtin_bit_cast(crd::u32, m) & 0x7FFFFFFFU) |
                                            (__builtin_bit_cast(crd::u32, s) & 0x80000000U));
}
[[nodiscard]] constexpr crd::f64 copysign(crd::f64 m, crd::f64 s) noexcept
{
    return __builtin_bit_cast(crd::f64, (__builtin_bit_cast(crd::u64, m) & 0x7FFFFFFFFFFFFFFFULL) |
                                            (__builtin_bit_cast(crd::u64, s) & 0x8000000000000000ULL));
}
[[nodiscard]] constexpr bool is_nan(crd::f32 x) noexcept
{
    return (__builtin_bit_cast(crd::u32, x) & 0x7FFFFFFFU) > 0x7F800000U;
}
[[nodiscard]] constexpr bool is_nan(crd::f64 x) noexcept
{
    return (__builtin_bit_cast(crd::u64, x) & 0x7FFFFFFFFFFFFFFFULL) > 0x7FF0000000000000ULL;
}
[[nodiscard]] constexpr bool is_inf(crd::f64 x) noexcept
{
    return (__builtin_bit_cast(crd::u64, x) & 0x7FFFFFFFFFFFFFFFULL) == 0x7FF0000000000000ULL;
}
[[nodiscard]] constexpr bool is_finite(crd::f32 x) noexcept
{
    return (__builtin_bit_cast(crd::u32, x) & 0x7F800000U) != 0x7F800000U;
}
[[nodiscard]] constexpr bool is_finite(crd::f64 x) noexcept
{
    return (__builtin_bit_cast(crd::u64, x) & 0x7FF0000000000000ULL) != 0x7FF0000000000000ULL;
}

} // namespace crd::math::inline CRD_MATH_LEVEL_NS::libm

#undef CRD_LIBM_FN
