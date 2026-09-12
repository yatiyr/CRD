// Vec4f/Vec8f ↔ Vec4i/Vec8i conversions + bitwise ops on float types.
// Phase 3.1 v0c-debt-A part 5.
//
// Two op classes:
//   - bitcast: bit-pattern preserving (no value change; just type punning)
//   - convert: numerical conversion (rounds float→int per mode chosen)
//   - bitwise: AND/OR/XOR/AND-NOT operating on the float bit pattern
//   - truncate / round_nearest: float-to-float rounding (returns same type)

#pragma once

#include <crd/math/simd/backend.hpp>
#include <crd/math/simd/vec4f.hpp>
#include <crd/math/simd/vec8f.hpp>
#include <crd/math/simd/vec4i.hpp>
#include <crd/math/simd/vec8i.hpp>

#include <bit>
#include <cmath>

namespace crd::math::simd
{
// ===========================================================================
// Bitcast — bit-pattern preserving reinterpret. No value change.
// ===========================================================================

CRD_FORCEINLINE Vec4i bitcast_to_int(Vec4f a) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4i r; r.v = _mm_castps_si128(a.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4i r; r.v = vreinterpretq_s32_f32(a.v); return r;
#else
    Vec4i r;
    for (int i = 0; i < 4; ++i) r.v[i] = std::bit_cast<crd::i32>(a.v[i]);
    return r;
#endif
}

CRD_FORCEINLINE Vec4f bitcast_to_float(Vec4i a) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4f r; r.v = _mm_castsi128_ps(a.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4f r; r.v = vreinterpretq_f32_s32(a.v); return r;
#else
    Vec4f r;
    for (int i = 0; i < 4; ++i) r.v[i] = std::bit_cast<f32>(a.v[i]);
    return r;
#endif
}

CRD_FORCEINLINE Vec8i bitcast_to_int(Vec8f a) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8i r; r.v = _mm256_castps_si256(a.v); return r;
#else
    return Vec8i(bitcast_to_int(a.lo), bitcast_to_int(a.hi));
#endif
}

CRD_FORCEINLINE Vec8f bitcast_to_float(Vec8i a) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r; r.v = _mm256_castsi256_ps(a.v); return r;
#else
    return Vec8f(bitcast_to_float(a.lo), bitcast_to_float(a.hi));
#endif
}

// ===========================================================================
// Numerical conversion — float ↔ int (rounding/truncating semantics).
// ===========================================================================

// Truncate float toward zero, return as Vec*i (e.g., 3.7 → 3, -2.3 → -2).
CRD_FORCEINLINE Vec4i convert_truncate(Vec4f a) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4i r; r.v = _mm_cvttps_epi32(a.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4i r; r.v = vcvtq_s32_f32(a.v); return r;  // NEON cvtq rounds toward zero
#else
    Vec4i r;
    for (int i = 0; i < 4; ++i) r.v[i] = static_cast<crd::i32>(a.v[i]);  // C cast → trunc
    return r;
#endif
}

CRD_FORCEINLINE Vec8i convert_truncate(Vec8f a) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8i r; r.v = _mm256_cvttps_epi32(a.v); return r;
#else
    return Vec8i(convert_truncate(a.lo), convert_truncate(a.hi));
#endif
}

// Convert int back to float (lossless for |i| < 2^24).
CRD_FORCEINLINE Vec4f convert_to_float(Vec4i a) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4f r; r.v = _mm_cvtepi32_ps(a.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4f r; r.v = vcvtq_f32_s32(a.v); return r;
#else
    Vec4f r;
    for (int i = 0; i < 4; ++i) r.v[i] = static_cast<f32>(a.v[i]);
    return r;
#endif
}

CRD_FORCEINLINE Vec8f convert_to_float(Vec8i a) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r; r.v = _mm256_cvtepi32_ps(a.v); return r;
#else
    return Vec8f(convert_to_float(a.lo), convert_to_float(a.hi));
#endif
}

// ===========================================================================
// Float-rounding ops (return same type, no int round-trip).
// ===========================================================================

// Truncate float toward zero (e.g., 3.7 → 3.0, -2.3 → -2.0).
CRD_FORCEINLINE Vec4f truncate(Vec4f a) noexcept
{
    return convert_to_float(convert_truncate(a));
}

CRD_FORCEINLINE Vec8f truncate(Vec8f a) noexcept
{
    return convert_to_float(convert_truncate(a));
}

// Round-to-nearest (banker's rounding under default FPU mode), return as float.
CRD_FORCEINLINE Vec4f round_nearest(Vec4f a) noexcept
{
#if CRD_SIMD_HAS_SSE2
    // cvtps2dq uses current rounding mode (default = nearest-even);
    // cvtdq2ps converts back to float. Bit-exact across runs.
    Vec4f r; r.v = _mm_cvtepi32_ps(_mm_cvtps_epi32(a.v)); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4f r; r.v = vcvtq_f32_s32(vcvtnq_s32_f32(a.v)); return r;
#else
    Vec4f r;
    for (int i = 0; i < 4; ++i)
    {
        // Round half to even (banker's), matching SSE2 cvtps2dq default.
        const f32 x = a.v[i];
        const f32 fl = std::floor(x);
        const f32 frac = x - fl;
        if (frac > 0.5F) r.v[i] = fl + 1.0F;
        else if (frac < 0.5F) r.v[i] = fl;
        else r.v[i] = (static_cast<crd::i32>(fl) & 1) == 0 ? fl : fl + 1.0F;
    }
    return r;
#endif
}

CRD_FORCEINLINE Vec8f round_nearest(Vec8f a) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r; r.v = _mm256_cvtepi32_ps(_mm256_cvtps_epi32(a.v)); return r;
#else
    return Vec8f(round_nearest(a.lo), round_nearest(a.hi));
#endif
}

// ===========================================================================
// Bitwise ops on Vec4f / Vec8f (operate on the float bit pattern).
// ===========================================================================

CRD_FORCEINLINE Vec4f bit_and(Vec4f a, Vec4f b) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4f r; r.v = _mm_and_ps(a.v, b.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4f r; r.v = vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(a.v),
                                                    vreinterpretq_u32_f32(b.v))); return r;
#else
    return bitcast_to_float(bitcast_to_int(a) & bitcast_to_int(b));
#endif
}

CRD_FORCEINLINE Vec4f bit_or(Vec4f a, Vec4f b) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4f r; r.v = _mm_or_ps(a.v, b.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4f r; r.v = vreinterpretq_f32_u32(vorrq_u32(vreinterpretq_u32_f32(a.v),
                                                    vreinterpretq_u32_f32(b.v))); return r;
#else
    return bitcast_to_float(bitcast_to_int(a) | bitcast_to_int(b));
#endif
}

CRD_FORCEINLINE Vec4f bit_xor(Vec4f a, Vec4f b) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4f r; r.v = _mm_xor_ps(a.v, b.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4f r; r.v = vreinterpretq_f32_u32(veorq_u32(vreinterpretq_u32_f32(a.v),
                                                    vreinterpretq_u32_f32(b.v))); return r;
#else
    return bitcast_to_float(bitcast_to_int(a) ^ bitcast_to_int(b));
#endif
}

CRD_FORCEINLINE Vec4f bit_andnot(Vec4f a, Vec4f b) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4f r; r.v = _mm_andnot_ps(a.v, b.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4f r; r.v = vreinterpretq_f32_u32(vbicq_u32(vreinterpretq_u32_f32(b.v),
                                                    vreinterpretq_u32_f32(a.v))); return r;
#else
    return bitcast_to_float(and_not(bitcast_to_int(a), bitcast_to_int(b)));
#endif
}

CRD_FORCEINLINE Vec8f bit_and(Vec8f a, Vec8f b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r; r.v = _mm256_and_ps(a.v, b.v); return r;
#else
    return Vec8f(bit_and(a.lo, b.lo), bit_and(a.hi, b.hi));
#endif
}

CRD_FORCEINLINE Vec8f bit_or(Vec8f a, Vec8f b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r; r.v = _mm256_or_ps(a.v, b.v); return r;
#else
    return Vec8f(bit_or(a.lo, b.lo), bit_or(a.hi, b.hi));
#endif
}

CRD_FORCEINLINE Vec8f bit_xor(Vec8f a, Vec8f b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r; r.v = _mm256_xor_ps(a.v, b.v); return r;
#else
    return Vec8f(bit_xor(a.lo, b.lo), bit_xor(a.hi, b.hi));
#endif
}

CRD_FORCEINLINE Vec8f bit_andnot(Vec8f a, Vec8f b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r; r.v = _mm256_andnot_ps(a.v, b.v); return r;
#else
    return Vec8f(bit_andnot(a.lo, b.lo), bit_andnot(a.hi, b.hi));
#endif
}

}  // namespace crd::math::simd
