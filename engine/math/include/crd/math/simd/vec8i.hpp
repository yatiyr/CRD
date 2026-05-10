// Vec8i — 8-lane i32 SIMD integer type. Phase 3.1 v0c-debt-A part 5.
//
// Companion to Vec8f. AVX2 native (__m256i); composed Vec4i lo/hi on
// SSE2/NEON; scalar fallback otherwise. Used by branchless SIMD trig/exp/log
// for octant + bit manipulation.

#pragma once

#include <crd/math/simd/backend.hpp>
#include <crd/math/simd/vec4i.hpp>

namespace crd::math::simd
{
struct alignas(32) Vec8i
{
#if CRD_SIMD_HAS_AVX2
    using Native = __m256i;
    Native v;
#else
    Vec4i lo, hi;
#endif

    Vec8i() noexcept = default;

    CRD_FORCEINLINE explicit Vec8i(crd::i32 broadcast) noexcept
    {
#if CRD_SIMD_HAS_AVX2
        v = _mm256_set1_epi32(broadcast);
#else
        lo = Vec4i(broadcast);
        hi = Vec4i(broadcast);
#endif
    }

    CRD_FORCEINLINE Vec8i(crd::i32 e0, crd::i32 e1, crd::i32 e2, crd::i32 e3,
                          crd::i32 e4, crd::i32 e5, crd::i32 e6, crd::i32 e7) noexcept
    {
#if CRD_SIMD_HAS_AVX2
        v = _mm256_set_epi32(e7, e6, e5, e4, e3, e2, e1, e0);
#else
        lo = Vec4i(e0, e1, e2, e3);
        hi = Vec4i(e4, e5, e6, e7);
#endif
    }

    CRD_FORCEINLINE Vec8i(Vec4i lo_in, Vec4i hi_in) noexcept
    {
#if CRD_SIMD_HAS_AVX2
        v = _mm256_set_m128i(hi_in.v, lo_in.v);
#else
        lo = lo_in; hi = hi_in;
#endif
    }

    [[nodiscard]] CRD_FORCEINLINE static Vec8i zero() noexcept { return Vec8i(0); }

    [[nodiscard]] CRD_FORCEINLINE crd::i32 lane(usize i) const noexcept
    {
        crd::i32 tmp[8];
#if CRD_SIMD_HAS_AVX2
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(tmp), v);
#else
        crd::i32 lo_buf[4]; crd::i32 hi_buf[4];
    #if CRD_SIMD_HAS_SSE2
        _mm_storeu_si128(reinterpret_cast<__m128i*>(lo_buf), lo.v);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(hi_buf), hi.v);
    #elif CRD_SIMD_HAS_NEON
        vst1q_s32(lo_buf, lo.v);
        vst1q_s32(hi_buf, hi.v);
    #else
        for (int j = 0; j < 4; ++j) { lo_buf[j] = lo.v[j]; hi_buf[j] = hi.v[j]; }
    #endif
        for (int j = 0; j < 4; ++j) { tmp[j] = lo_buf[j]; tmp[j + 4] = hi_buf[j]; }
#endif
        return tmp[i];
    }
};

// ---- arithmetic ------------------------------------------------------------

CRD_FORCEINLINE Vec8i operator+(Vec8i a, Vec8i b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8i r; r.v = _mm256_add_epi32(a.v, b.v); return r;
#else
    return Vec8i(a.lo + b.lo, a.hi + b.hi);
#endif
}

CRD_FORCEINLINE Vec8i operator-(Vec8i a, Vec8i b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8i r; r.v = _mm256_sub_epi32(a.v, b.v); return r;
#else
    return Vec8i(a.lo - b.lo, a.hi - b.hi);
#endif
}

// ---- bitwise ---------------------------------------------------------------

CRD_FORCEINLINE Vec8i operator&(Vec8i a, Vec8i b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8i r; r.v = _mm256_and_si256(a.v, b.v); return r;
#else
    return Vec8i(a.lo & b.lo, a.hi & b.hi);
#endif
}

CRD_FORCEINLINE Vec8i operator|(Vec8i a, Vec8i b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8i r; r.v = _mm256_or_si256(a.v, b.v); return r;
#else
    return Vec8i(a.lo | b.lo, a.hi | b.hi);
#endif
}

CRD_FORCEINLINE Vec8i operator^(Vec8i a, Vec8i b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8i r; r.v = _mm256_xor_si256(a.v, b.v); return r;
#else
    return Vec8i(a.lo ^ b.lo, a.hi ^ b.hi);
#endif
}

CRD_FORCEINLINE Vec8i and_not(Vec8i a, Vec8i b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8i r; r.v = _mm256_andnot_si256(a.v, b.v); return r;
#else
    return Vec8i(and_not(a.lo, b.lo), and_not(a.hi, b.hi));
#endif
}

template <int N>
CRD_FORCEINLINE Vec8i shift_left(Vec8i a) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8i r; r.v = _mm256_slli_epi32(a.v, N); return r;
#else
    return Vec8i(shift_left<N>(a.lo), shift_left<N>(a.hi));
#endif
}

template <int N>
CRD_FORCEINLINE Vec8i shift_right_arith(Vec8i a) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8i r; r.v = _mm256_srai_epi32(a.v, N); return r;
#else
    return Vec8i(shift_right_arith<N>(a.lo), shift_right_arith<N>(a.hi));
#endif
}

template <int N>
CRD_FORCEINLINE Vec8i shift_right_logical(Vec8i a) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8i r; r.v = _mm256_srli_epi32(a.v, N); return r;
#else
    return Vec8i(shift_right_logical<N>(a.lo), shift_right_logical<N>(a.hi));
#endif
}

// ---- comparisons -----------------------------------------------------------

CRD_FORCEINLINE Vec8i cmp_eq(Vec8i a, Vec8i b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8i r; r.v = _mm256_cmpeq_epi32(a.v, b.v); return r;
#else
    return Vec8i(cmp_eq(a.lo, b.lo), cmp_eq(a.hi, b.hi));
#endif
}

CRD_FORCEINLINE Vec8i cmp_gt(Vec8i a, Vec8i b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8i r; r.v = _mm256_cmpgt_epi32(a.v, b.v); return r;
#else
    return Vec8i(cmp_gt(a.lo, b.lo), cmp_gt(a.hi, b.hi));
#endif
}

}  // namespace crd::math::simd
