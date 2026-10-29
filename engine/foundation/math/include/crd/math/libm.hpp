// The scalar math a header shared by general code and kernel units calls (SIMD-DISPATCH, ADR-0134 §5).
//
// <cmath>'s float overloads and classification functions are inline functions of the standard library: a kernel unit
// that instantiates one compiles a body for its own ISA under the same symbol as general code's, and the linker keeps
// one of them for every caller. These wrappers live in the unit's level namespace and call only compiler builtins
// (GCC, Clang, clang-cl) or the C library's external functions (MSVC), so every level keeps its own copy. The sign
// and class operations are bit operations, the same on every compiler and level.
//
// fma rounds once on every level. On x64 Windows the UCRT's fma runs the FMA3 instruction only while its FMA3 bodies
// are enabled (the CPU has FMA3 and no CRD_CPU_LEVEL below avx2 turned them off); its software body otherwise does not
// round once (it loses the sign of an underflowing product, and rounds some sums twice). A unit of the avx2 and
// avx512 levels issues the instruction itself; every other unit there calls the UCRT only while FMA3 is active and
// else computes the f64 result by integer arithmetic, and f32 always rounds an exact f64 sum to odd first.

#pragma once

#include <crd/core/types.hpp>
#include <crd/math/kernel_level.hpp>

#if !defined(__clang__) && !defined(__GNUC__)
#include <math.h>
#endif

// NOLINTBEGIN(cppcoreguidelines-macro-usage): the choice reaches the preprocessor.
#if defined(_MSC_VER) && defined(_M_X64) && !defined(_M_ARM64EC)
#if defined(__AVX2__)
#define CRD_LIBM_FMA_INSTRUCTION 1
#include <immintrin.h>
#else
#define CRD_LIBM_FMA_SOFT 1
#include <crd/core/cpu_features.hpp>

#include <intrin.h>
#endif
#endif
// NOLINTEND(cppcoreguidelines-macro-usage)

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
#if defined(CRD_LIBM_FMA_SOFT)
namespace detail
{

struct U128
{
    crd::u64 hi;
    crd::u64 lo;
};

[[nodiscard]] inline int top_bit(crd::u64 x) noexcept // x != 0
{
    unsigned long index = 0;
    (void)_BitScanReverse64(&index, x);
    return static_cast<int>(index);
}

// x >> s for s >= 0, every bit shifted out ORed into bit 0.
[[nodiscard]] inline U128 shift_right_sticky(U128 x, int s) noexcept
{
    if (s == 0)
    {
        return x;
    }
    if (s < 64)
    {
        const crd::u64 out = x.lo << (64 - s);
        return {x.hi >> s, (x.lo >> s) | (x.hi << (64 - s)) | (out != 0U ? 1U : 0U)};
    }
    if (s < 128)
    {
        const crd::u64 out = x.lo | (s > 64 ? x.hi << (128 - s) : 0U);
        return {0U, (x.hi >> (s - 64)) | (out != 0U ? 1U : 0U)};
    }
    return {0U, (x.hi | x.lo) != 0U ? 1U : 0U};
}

// A finite nonzero |v| as m * 2^e with m in [2^52, 2^53).
inline void unpack(crd::f64 v, crd::u64& m, int& e) noexcept
{
    const crd::u64 bits = __builtin_bit_cast(crd::u64, v) & 0x7FFFFFFFFFFFFFFFULL;
    int biased = static_cast<int>(bits >> 52U);
    m = bits & 0x000FFFFFFFFFFFFFULL;
    if (biased == 0)
    {
        const int up = 52 - top_bit(m);
        m <<= up;
        biased = 1 - up;
    }
    else
    {
        m |= 1ULL << 52U;
    }
    e = biased - 1075;
}

// RN(x*y + z) in f64 by integer arithmetic: the 106-bit product and z's significand are placed with their top bit at
// bit 124 or 125 of 128, the operand of the smaller exponent is shifted right with its lost bits kept as a sticky bit
// (which can only happen when the exponents differ by more than 20, so a cancellation removes at most one bit and the
// sticky bit lies far below the rounding position), and the exact sum is rounded to nearest even, subnormals and
// overflow included.
[[nodiscard]] inline crd::f64 fma_f64(crd::f64 x, crd::f64 y, crd::f64 z) noexcept
{
    constexpr crd::u64 abs_mask = 0x7FFFFFFFFFFFFFFFULL;
    constexpr crd::u64 inf_bits = 0x7FF0000000000000ULL;
    const crd::u64 bx = __builtin_bit_cast(crd::u64, x);
    const crd::u64 by = __builtin_bit_cast(crd::u64, y);
    const crd::u64 bz = __builtin_bit_cast(crd::u64, z);
    if ((bx & abs_mask) >= inf_bits || (by & abs_mask) >= inf_bits)
    {
        return x * y + z; // an infinite or NaN product: IEEE's sum
    }
    if ((bz & abs_mask) >= inf_bits)
    {
        return z + z; // a finite product: the infinity, or the NaN quieted
    }
    if ((bx & abs_mask) == 0U || (by & abs_mask) == 0U)
    {
        return x * y + z; // an exact signed zero product
    }
    if ((bz & abs_mask) == 0U)
    {
        return x * y; // a nonzero exact product rounded once keeps its sign
    }
    crd::u64 mx = 0U;
    crd::u64 my = 0U;
    crd::u64 mz = 0U;
    int ex = 0;
    int ey = 0;
    int ez = 0;
    unpack(x, mx, ex);
    unpack(y, my, ey);
    unpack(z, mz, ez);
    U128 p{};
    p.lo = _umul128(mx, my, &p.hi);
    p = {(p.hi << 20U) | (p.lo >> 44U), p.lo << 20U};
    const int ep = ex + ey - 20;
    U128 q{mz << 9U, 0U}; // mz * 2^73
    const int eq = ez - 73;
    int e = ep;
    if (ep >= eq)
    {
        q = shift_right_sticky(q, ep - eq);
    }
    else
    {
        p = shift_right_sticky(p, eq - ep);
        e = eq;
    }
    const bool product_negative = ((bx ^ by) >> 63U) != 0U;
    const bool z_negative = (bz >> 63U) != 0U;
    bool negative = z_negative;
    U128 s{};
    if (product_negative == z_negative)
    {
        s.lo = p.lo + q.lo;
        s.hi = p.hi + q.hi + (s.lo < p.lo ? 1U : 0U);
    }
    else
    {
        const bool p_larger = p.hi != q.hi ? p.hi > q.hi : p.lo >= q.lo;
        const U128 a = p_larger ? p : q;
        const U128 b = p_larger ? q : p;
        s.lo = a.lo - b.lo;
        s.hi = a.hi - b.hi - (a.lo < b.lo ? 1U : 0U);
        negative = p_larger ? product_negative : z_negative;
        if ((s.hi | s.lo) == 0U)
        {
            return 0.0; // an exact cancellation is +0 when rounding to nearest
        }
    }
    const int top = s.hi != 0U ? 64 + top_bit(s.hi) : top_bit(s.lo);
    int exponent = e + top - 52; // the result is m * 2^exponent, m in [2^52, 2^53) when normal
    int shift = top - 52;
    if (exponent < -1074)
    {
        shift += -1074 - exponent;
        exponent = -1074;
    }
    crd::u64 m = 0U;
    if (shift <= 0)
    {
        m = s.lo << -shift; // exact: s < 2^53
    }
    else if (shift < 128)
    {
        U128 rem{};
        U128 half{};
        if (shift < 64)
        {
            m = (s.lo >> shift) | (s.hi << (64 - shift));
            rem = {0U, s.lo & ((1ULL << shift) - 1U)};
            half = {0U, 1ULL << (shift - 1)};
        }
        else
        {
            m = s.hi >> (shift - 64);
            rem = {shift == 64 ? 0U : s.hi & ((1ULL << (shift - 64)) - 1U), s.lo};
            half = shift == 64 ? U128{0U, 1ULL << 63U} : U128{1ULL << (shift - 65), 0U};
        }
        const bool above = rem.hi != half.hi ? rem.hi > half.hi : rem.lo > half.lo;
        const bool tie = rem.hi == half.hi && rem.lo == half.lo;
        if (above || (tie && (m & 1U) != 0U))
        {
            ++m;
        }
    } // else s < 2^127, below half a unit of the smallest subnormal: m stays 0
    const crd::u64 sign = negative ? 0x8000000000000000ULL : 0U;
    if (exponent > 971)
    {
        return __builtin_bit_cast(crd::f64, sign | inf_bits);
    }
    // m's bit 52 (a normal or a rounded-up subnormal) adds one to the exponent field, and a significand rounded up to
    // 2^53 carries into the next binade, up to infinity.
    const crd::u64 bits = (static_cast<crd::u64>(exponent + 1074) << 52U) + m;
    return __builtin_bit_cast(crd::f64, sign | (bits < inf_bits ? bits : inf_bits));
}

// RN_24(x*y + z): the f64 product is exact, TwoSum gives the f64 sum's error exactly, and the sum rounded to odd
// (truncated toward zero with bit 0 set when inexact) rounds to f32 once, because 53 >= 24 + 2.
[[nodiscard]] inline crd::f32 fma_f32(crd::f32 x, crd::f32 y, crd::f32 z) noexcept
{
    const crd::f64 p = static_cast<crd::f64>(x) * static_cast<crd::f64>(y);
    const crd::f64 c = z;
    const crd::f64 s = p + c;
    const crd::f64 v = s - p;
    const crd::f64 err = (p - (s - v)) + (c - v);
    crd::u64 bits = __builtin_bit_cast(crd::u64, s);
    if (err < 0.0 || err > 0.0) // a non-finite sum's error is NaN: exact
    {
        if ((bits >> 63U) != (__builtin_bit_cast(crd::u64, err) >> 63U))
        {
            --bits; // s rounded away from zero
        }
        bits |= 1U;
    }
    return static_cast<crd::f32>(__builtin_bit_cast(crd::f64, bits));
}

} // namespace detail
#endif

[[nodiscard]] inline crd::f32 fma(crd::f32 x, crd::f32 y, crd::f32 z) noexcept
{
#if defined(CRD_LIBM_FMA_INSTRUCTION)
    return _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(x), _mm_set_ss(y), _mm_set_ss(z)));
#elif defined(CRD_LIBM_FMA_SOFT)
    return detail::fma_f32(x, y, z);
#else
    return CRD_LIBM_FN(fmaf)(x, y, z);
#endif
}
[[nodiscard]] inline crd::f64 fma(crd::f64 x, crd::f64 y, crd::f64 z) noexcept
{
#if defined(CRD_LIBM_FMA_INSTRUCTION)
    return _mm_cvtsd_f64(_mm_fmadd_sd(_mm_set_sd(x), _mm_set_sd(y), _mm_set_sd(z)));
#elif defined(CRD_LIBM_FMA_SOFT)
    return crd::detail::ucrt_fma3_active ? CRD_LIBM_FN(fma)(x, y, z) : detail::fma_f64(x, y, z);
#else
    return CRD_LIBM_FN(fma)(x, y, z);
#endif
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
#undef CRD_LIBM_FMA_INSTRUCTION
#undef CRD_LIBM_FMA_SOFT
