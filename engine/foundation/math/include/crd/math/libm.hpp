// The scalar math a header shared by general code and kernel units calls (SIMD-DISPATCH, ADR-0134 §5).
//
// <cmath>'s float overloads and classification functions are inline functions of the standard library: a kernel unit
// that instantiates one compiles a body for its own ISA under the same symbol as general code's, and the linker keeps
// one of them for every caller. These wrappers live in the unit's level namespace and call only compiler builtins
// (GCC, Clang, clang-cl) or the C library's external functions (MSVC), so every level keeps its own copy.

#pragma once

#include <crd/core/types.hpp>
#include <crd/math/kernel_level.hpp>

#if !defined(__clang__) && !defined(__GNUC__)
#include <math.h>
#endif

namespace crd::math::inline CRD_MATH_LEVEL_NS::libm
{

#if defined(__clang__) || defined(__GNUC__)
[[nodiscard]] inline crd::f32 sqrt(crd::f32 x) noexcept
{
    return __builtin_sqrtf(x);
}
[[nodiscard]] inline crd::f64 sqrt(crd::f64 x) noexcept
{
    return __builtin_sqrt(x);
}
[[nodiscard]] inline crd::f32 fma(crd::f32 a, crd::f32 b, crd::f32 c) noexcept
{
    return __builtin_fmaf(a, b, c);
}
[[nodiscard]] inline crd::f64 fma(crd::f64 a, crd::f64 b, crd::f64 c) noexcept
{
    return __builtin_fma(a, b, c);
}
[[nodiscard]] inline crd::f64 floor(crd::f64 x) noexcept
{
    return __builtin_floor(x);
}
[[nodiscard]] inline crd::f64 log(crd::f64 x) noexcept
{
    return __builtin_log(x);
}
#else
[[nodiscard]] inline crd::f32 sqrt(crd::f32 x) noexcept
{
    return ::sqrtf(x);
}
[[nodiscard]] inline crd::f64 sqrt(crd::f64 x) noexcept
{
    return ::sqrt(x);
}
[[nodiscard]] inline crd::f32 fma(crd::f32 a, crd::f32 b, crd::f32 c) noexcept
{
    return ::fmaf(a, b, c);
}
[[nodiscard]] inline crd::f64 fma(crd::f64 a, crd::f64 b, crd::f64 c) noexcept
{
    return ::fma(a, b, c);
}
[[nodiscard]] inline crd::f64 floor(crd::f64 x) noexcept
{
    return ::floor(x);
}
[[nodiscard]] inline crd::f64 log(crd::f64 x) noexcept
{
    return ::log(x);
}
#endif

// IEEE classification by bit pattern, the same on every compiler and level.
[[nodiscard]] constexpr bool is_nan(crd::f32 x) noexcept
{
    return (__builtin_bit_cast(crd::u32, x) & 0x7FFFFFFFU) > 0x7F800000U;
}
[[nodiscard]] constexpr bool is_nan(crd::f64 x) noexcept
{
    return (__builtin_bit_cast(crd::u64, x) & 0x7FFFFFFFFFFFFFFFULL) > 0x7FF0000000000000ULL;
}
[[nodiscard]] constexpr bool is_finite(crd::f64 x) noexcept
{
    return (__builtin_bit_cast(crd::u64, x) & 0x7FF0000000000000ULL) != 0x7FF0000000000000ULL;
}

} // namespace crd::math::inline CRD_MATH_LEVEL_NS::libm
