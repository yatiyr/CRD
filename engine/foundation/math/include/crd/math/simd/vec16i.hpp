// Vec16i — 16-lane i32 SIMD integer type (SIMD-DISPATCH, ADR-0134 §5): one 512-bit register on the avx512 level, two
// Vec8i (lanes 0-7 and 8-15) below it, with Vec8i's operations and bits on every level. Kernel units include it
// directly; it is not part of the simd.hpp umbrella that general code reaches.

#pragma once

#include <crd/math/simd/backend.hpp>
#include <crd/math/simd/vec8i.hpp>

namespace crd::math::simd::inline CRD_MATH_LEVEL_NS
{
struct alignas(64) Vec16i
{
#if CRD_SIMD_HAS_AVX512
    using Native = __m512i;
    Native v;
#else
    Vec8i lo, hi;
#endif

    Vec16i() noexcept = default;

    CRD_FORCEINLINE explicit Vec16i(crd::i32 broadcast) noexcept
    {
#if CRD_SIMD_HAS_AVX512
        v = _mm512_set1_epi32(broadcast);
#else
        lo = Vec8i(broadcast);
        hi = Vec8i(broadcast);
#endif
    }

    CRD_FORCEINLINE Vec16i(Vec8i lo_in, Vec8i hi_in) noexcept
    {
#if CRD_SIMD_HAS_AVX512
        v = _mm512_inserti64x4(_mm512_castsi256_si512(lo_in.v), hi_in.v, 1);
#else
        lo = lo_in;
        hi = hi_in;
#endif
    }

    // Lanes 0-15 from p[0..15].
    [[nodiscard]] CRD_FORCEINLINE static Vec16i from(const crd::i32* p) noexcept
    {
#if CRD_SIMD_HAS_AVX512
        Vec16i r;
        r.v = _mm512_loadu_si512(p);
        return r;
#else
        return Vec16i(Vec8i(p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]),
                      Vec8i(p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]));
#endif
    }

    [[nodiscard]] CRD_FORCEINLINE static Vec16i zero() noexcept { return Vec16i(0); }

    [[nodiscard]] CRD_FORCEINLINE crd::i32 lane(usize i) const noexcept
    {
#if CRD_SIMD_HAS_AVX512
        alignas(64) crd::i32 tmp[16];
        _mm512_store_si512(tmp, v);
        return tmp[i];
#else
        return i < 8U ? lo.lane(i) : hi.lane(i - 8U);
#endif
    }
};
static_assert(sizeof(Vec16i) == 64 && alignof(Vec16i) == 64, "Vec16i is one 512-bit vector on every level");

// ---- arithmetic (wrapping) and bitwise --------------------------------------

CRD_FORCEINLINE Vec16i operator+(Vec16i a, Vec16i b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16i r;
    r.v = _mm512_add_epi32(a.v, b.v);
    return r;
#else
    return Vec16i(a.lo + b.lo, a.hi + b.hi);
#endif
}

CRD_FORCEINLINE Vec16i operator-(Vec16i a, Vec16i b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16i r;
    r.v = _mm512_sub_epi32(a.v, b.v);
    return r;
#else
    return Vec16i(a.lo - b.lo, a.hi - b.hi);
#endif
}

CRD_FORCEINLINE Vec16i operator&(Vec16i a, Vec16i b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16i r;
    r.v = _mm512_and_si512(a.v, b.v);
    return r;
#else
    return Vec16i(a.lo & b.lo, a.hi & b.hi);
#endif
}

CRD_FORCEINLINE Vec16i operator|(Vec16i a, Vec16i b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16i r;
    r.v = _mm512_or_si512(a.v, b.v);
    return r;
#else
    return Vec16i(a.lo | b.lo, a.hi | b.hi);
#endif
}

CRD_FORCEINLINE Vec16i operator^(Vec16i a, Vec16i b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16i r;
    r.v = _mm512_xor_si512(a.v, b.v);
    return r;
#else
    return Vec16i(a.lo ^ b.lo, a.hi ^ b.hi);
#endif
}

// ~a & b, as Vec8i's.
CRD_FORCEINLINE Vec16i and_not(Vec16i a, Vec16i b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16i r;
    r.v = _mm512_andnot_si512(a.v, b.v);
    return r;
#else
    return Vec16i(and_not(a.lo, b.lo), and_not(a.hi, b.hi));
#endif
}

template <int N> CRD_FORCEINLINE Vec16i shift_left(Vec16i a) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16i r;
    r.v = _mm512_slli_epi32(a.v, N);
    return r;
#else
    return Vec16i(shift_left<N>(a.lo), shift_left<N>(a.hi));
#endif
}

template <int N> CRD_FORCEINLINE Vec16i shift_right_arith(Vec16i a) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16i r;
    r.v = _mm512_srai_epi32(a.v, N);
    return r;
#else
    return Vec16i(shift_right_arith<N>(a.lo), shift_right_arith<N>(a.hi));
#endif
}

template <int N> CRD_FORCEINLINE Vec16i shift_right_logical(Vec16i a) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16i r;
    r.v = _mm512_srli_epi32(a.v, N);
    return r;
#else
    return Vec16i(shift_right_logical<N>(a.lo), shift_right_logical<N>(a.hi));
#endif
}

// ---- comparisons: an all-ones lane where true, as Vec8i's ------------------

CRD_FORCEINLINE Vec16i cmp_eq(Vec16i a, Vec16i b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16i r;
    r.v = _mm512_movm_epi32(_mm512_cmpeq_epi32_mask(a.v, b.v));
    return r;
#else
    return Vec16i(cmp_eq(a.lo, b.lo), cmp_eq(a.hi, b.hi));
#endif
}

CRD_FORCEINLINE Vec16i cmp_gt(Vec16i a, Vec16i b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16i r;
    r.v = _mm512_movm_epi32(_mm512_cmpgt_epi32_mask(a.v, b.v));
    return r;
#else
    return Vec16i(cmp_gt(a.lo, b.lo), cmp_gt(a.hi, b.hi));
#endif
}

} // namespace crd::math::simd::inline CRD_MATH_LEVEL_NS
