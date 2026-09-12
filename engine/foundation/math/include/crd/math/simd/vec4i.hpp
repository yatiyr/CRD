// Vec4i — 4-lane i32 SIMD integer type. Phase 3.1 v0c-debt-A part 5.
//
// Companion to Vec4f. Used by the branchless SIMD trig/exp/log implementations
// for octant arithmetic, mantissa/exponent bit manipulation, and mask
// generation. Same backend selection + alignment posture as Vec4f.

#pragma once

#include <crd/math/simd/backend.hpp>

namespace crd::math::simd
{
struct alignas(16) Vec4i
{
#if CRD_SIMD_HAS_SSE2
    using Native = __m128i;
#elif CRD_SIMD_HAS_NEON
    using Native = int32x4_t;
#else
    using Native = crd::i32[4];
#endif

    Native v;

    Vec4i() noexcept = default;

    CRD_FORCEINLINE explicit Vec4i(crd::i32 broadcast) noexcept
    {
#if CRD_SIMD_HAS_SSE2
        v = _mm_set1_epi32(broadcast);
#elif CRD_SIMD_HAS_NEON
        v = vdupq_n_s32(broadcast);
#else
        v[0] = broadcast; v[1] = broadcast; v[2] = broadcast; v[3] = broadcast;
#endif
    }

    CRD_FORCEINLINE Vec4i(crd::i32 a, crd::i32 b, crd::i32 c, crd::i32 d) noexcept
    {
#if CRD_SIMD_HAS_SSE2
        v = _mm_set_epi32(d, c, b, a);
#elif CRD_SIMD_HAS_NEON
        const crd::i32 lanes[4] = { a, b, c, d };
        v = vld1q_s32(lanes);
#else
        v[0] = a; v[1] = b; v[2] = c; v[3] = d;
#endif
    }

    [[nodiscard]] CRD_FORCEINLINE static Vec4i zero() noexcept { return Vec4i(0); }

    [[nodiscard]] CRD_FORCEINLINE crd::i32 lane(usize i) const noexcept
    {
        crd::i32 tmp[4];
#if CRD_SIMD_HAS_SSE2
        _mm_storeu_si128(reinterpret_cast<__m128i*>(tmp), v);
#elif CRD_SIMD_HAS_NEON
        vst1q_s32(tmp, v);
#else
        tmp[0] = v[0]; tmp[1] = v[1]; tmp[2] = v[2]; tmp[3] = v[3];
#endif
        return tmp[i];
    }
};

// ---- arithmetic ------------------------------------------------------------

CRD_FORCEINLINE Vec4i operator+(Vec4i a, Vec4i b) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4i r; r.v = _mm_add_epi32(a.v, b.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4i r; r.v = vaddq_s32(a.v, b.v); return r;
#else
    return Vec4i(a.v[0] + b.v[0], a.v[1] + b.v[1], a.v[2] + b.v[2], a.v[3] + b.v[3]);
#endif
}

CRD_FORCEINLINE Vec4i operator-(Vec4i a, Vec4i b) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4i r; r.v = _mm_sub_epi32(a.v, b.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4i r; r.v = vsubq_s32(a.v, b.v); return r;
#else
    return Vec4i(a.v[0] - b.v[0], a.v[1] - b.v[1], a.v[2] - b.v[2], a.v[3] - b.v[3]);
#endif
}

// ---- bitwise ---------------------------------------------------------------

CRD_FORCEINLINE Vec4i operator&(Vec4i a, Vec4i b) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4i r; r.v = _mm_and_si128(a.v, b.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4i r; r.v = vandq_s32(a.v, b.v); return r;
#else
    return Vec4i(a.v[0] & b.v[0], a.v[1] & b.v[1], a.v[2] & b.v[2], a.v[3] & b.v[3]);
#endif
}

CRD_FORCEINLINE Vec4i operator|(Vec4i a, Vec4i b) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4i r; r.v = _mm_or_si128(a.v, b.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4i r; r.v = vorrq_s32(a.v, b.v); return r;
#else
    return Vec4i(a.v[0] | b.v[0], a.v[1] | b.v[1], a.v[2] | b.v[2], a.v[3] | b.v[3]);
#endif
}

CRD_FORCEINLINE Vec4i operator^(Vec4i a, Vec4i b) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4i r; r.v = _mm_xor_si128(a.v, b.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4i r; r.v = veorq_s32(a.v, b.v); return r;
#else
    return Vec4i(a.v[0] ^ b.v[0], a.v[1] ^ b.v[1], a.v[2] ^ b.v[2], a.v[3] ^ b.v[3]);
#endif
}

// AND-NOT: ~a & b (matches SSE2 semantics).
CRD_FORCEINLINE Vec4i and_not(Vec4i a, Vec4i b) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4i r; r.v = _mm_andnot_si128(a.v, b.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4i r; r.v = vbicq_s32(b.v, a.v); return r;  // NEON: bic = a & ~b, so flip args
#else
    return Vec4i(~a.v[0] & b.v[0], ~a.v[1] & b.v[1], ~a.v[2] & b.v[2], ~a.v[3] & b.v[3]);
#endif
}

// Logical shift left by compile-time imm.
template <int N>
CRD_FORCEINLINE Vec4i shift_left(Vec4i a) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4i r; r.v = _mm_slli_epi32(a.v, N); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4i r; r.v = vshlq_n_s32(a.v, N); return r;
#else
    return Vec4i(a.v[0] << N, a.v[1] << N, a.v[2] << N, a.v[3] << N);
#endif
}

// Arithmetic shift right (sign-extending).
template <int N>
CRD_FORCEINLINE Vec4i shift_right_arith(Vec4i a) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4i r; r.v = _mm_srai_epi32(a.v, N); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4i r; r.v = vshrq_n_s32(a.v, N); return r;
#else
    return Vec4i(a.v[0] >> N, a.v[1] >> N, a.v[2] >> N, a.v[3] >> N);
#endif
}

// Logical shift right (zero-extending). Cast to unsigned then back.
template <int N>
CRD_FORCEINLINE Vec4i shift_right_logical(Vec4i a) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4i r; r.v = _mm_srli_epi32(a.v, N); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4i r; r.v = vreinterpretq_s32_u32(vshrq_n_u32(vreinterpretq_u32_s32(a.v), N)); return r;
#else
    return Vec4i(static_cast<crd::i32>(static_cast<crd::u32>(a.v[0]) >> N),
                 static_cast<crd::i32>(static_cast<crd::u32>(a.v[1]) >> N),
                 static_cast<crd::i32>(static_cast<crd::u32>(a.v[2]) >> N),
                 static_cast<crd::i32>(static_cast<crd::u32>(a.v[3]) >> N));
#endif
}

// ---- comparisons (all-bits-set on true, zero on false) ---------------------

CRD_FORCEINLINE Vec4i cmp_eq(Vec4i a, Vec4i b) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4i r; r.v = _mm_cmpeq_epi32(a.v, b.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4i r; r.v = vreinterpretq_s32_u32(vceqq_s32(a.v, b.v)); return r;
#else
    return Vec4i(a.v[0] == b.v[0] ? -1 : 0, a.v[1] == b.v[1] ? -1 : 0,
                 a.v[2] == b.v[2] ? -1 : 0, a.v[3] == b.v[3] ? -1 : 0);
#endif
}

CRD_FORCEINLINE Vec4i cmp_gt(Vec4i a, Vec4i b) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4i r; r.v = _mm_cmpgt_epi32(a.v, b.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4i r; r.v = vreinterpretq_s32_u32(vcgtq_s32(a.v, b.v)); return r;
#else
    return Vec4i(a.v[0] > b.v[0] ? -1 : 0, a.v[1] > b.v[1] ? -1 : 0,
                 a.v[2] > b.v[2] ? -1 : 0, a.v[3] > b.v[3] ? -1 : 0);
#endif
}

}  // namespace crd::math::simd
