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

namespace crd::math::simd::inline CRD_MATH_LEVEL_NS
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
    for (int i = 0; i < 4; ++i)
    {
        r.v[i] = __builtin_bit_cast(crd::i32, a.v[i]);
    }
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
    for (int i = 0; i < 4; ++i)
    {
        r.v[i] = __builtin_bit_cast(f32, a.v[i]);
    }
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
// Truncate toward zero to i32. A NaN or a value outside the i32 range gives INT32_MIN on every backend (the x86
// "integer indefinite" result).
CRD_FORCEINLINE Vec4i convert_truncate(Vec4f a) noexcept
{
#if CRD_SIMD_HAS_SSE2
    Vec4i r; r.v = _mm_cvttps_epi32(a.v); return r;
#elif CRD_SIMD_HAS_NEON
    Vec4i r; r.v = vcvtq_s32_f32(a.v); return r;  // NEON cvtq rounds toward zero
#else
    Vec4i r;
    for (int i = 0; i < 4; ++i)
    {
        const f32 x = a.v[i];
        r.v[i] =
            x >= -2147483648.0F && x < 2147483648.0F ? static_cast<crd::i32>(x) : static_cast<crd::i32>(0x80000000U);
    }
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
    for (int i = 0; i < 4; ++i)
    {
        r.v[i] = static_cast<f32>(a.v[i]);
    }
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
// Float-rounding ops: IEEE 754 roundToIntegral, returning the same type.
// ===========================================================================
//
// Every backend gives the same bits for every input: the sign of a zero result is the input's (-0.3 gives -0.0), a
// value of magnitude 2^23 or more is already integral and comes back unchanged, infinities stay, and a NaN comes
// back quiet. SSE4.1 rounds in one instruction; SSE2 and the scalar fallback convert through i32 below 2^23 and add
// zero above it (which quiets a NaN like the instruction does). round_nearest is ties-to-even in the default
// rounding mode, which the ADR-0063 contract keeps.

namespace detail
{
// The scalar fallback of truncate (ToZero) and round_nearest: one lane.
template <bool Nearest> [[nodiscard]] CRD_FORCEINLINE f32 round_integral_lane(f32 x) noexcept
{
    const crd::u32 bits = __builtin_bit_cast(crd::u32, x);
    const f32 magnitude = __builtin_bit_cast(f32, bits & 0x7FFFFFFFU);
    if (!(magnitude < 8388608.0F))
    {
        return x + 0.0F;
    }
    f32 integral = 0.0F;
    if constexpr (Nearest)
    {
        integral = (magnitude + 8388608.0F) - 8388608.0F; // the f32 addition rounds to an integer, ties to even
    }
    else
    {
        integral = static_cast<f32>(static_cast<crd::i32>(magnitude));
    }
    return __builtin_bit_cast(f32, __builtin_bit_cast(crd::u32, integral) | (bits & 0x80000000U));
}

#if CRD_SIMD_HAS_SSE2 && !CRD_SIMD_HAS_SSE42
// The SSE2 form: the i32 round trip `integral` below 2^23 with the input's sign, x + 0 above it.
[[nodiscard]] CRD_FORCEINLINE __m128 round_integral_sse2(__m128 x, __m128 integral) noexcept
{
    const __m128 sign = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0x80000000U)));
    const __m128 small = _mm_cmplt_ps(_mm_andnot_ps(sign, x), _mm_set1_ps(8388608.0F));
    const __m128 kept = _mm_or_ps(integral, _mm_and_ps(x, sign));
    return _mm_or_ps(_mm_and_ps(small, kept), _mm_andnot_ps(small, _mm_add_ps(x, _mm_setzero_ps())));
}
#endif
} // namespace detail

// Round toward zero (3.7 gives 3.0, -2.3 gives -2.0).
CRD_FORCEINLINE Vec4f truncate(Vec4f a) noexcept
{
#if CRD_SIMD_HAS_SSE42
    Vec4f r;
    r.v = _mm_round_ps(a.v, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC);
    return r;
#elif CRD_SIMD_HAS_SSE2
    Vec4f r;
    r.v = detail::round_integral_sse2(a.v, _mm_cvtepi32_ps(_mm_cvttps_epi32(a.v)));
    return r;
#elif CRD_SIMD_HAS_NEON
    Vec4f r;
    r.v = vrndq_f32(a.v);
    return r;
#else
    return Vec4f(detail::round_integral_lane<false>(a.v[0]), detail::round_integral_lane<false>(a.v[1]),
                 detail::round_integral_lane<false>(a.v[2]), detail::round_integral_lane<false>(a.v[3]));
#endif
}

CRD_FORCEINLINE Vec8f truncate(Vec8f a) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r;
    r.v = _mm256_round_ps(a.v, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC);
    return r;
#else
    return Vec8f(truncate(a.lo), truncate(a.hi));
#endif
}

// Round to the nearest integer, ties to even (2.5 gives 2.0, -0.4 gives -0.0).
CRD_FORCEINLINE Vec4f round_nearest(Vec4f a) noexcept
{
#if CRD_SIMD_HAS_SSE42
    Vec4f r;
    r.v = _mm_round_ps(a.v, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    return r;
#elif CRD_SIMD_HAS_SSE2
    Vec4f r;
    r.v = detail::round_integral_sse2(a.v, _mm_cvtepi32_ps(_mm_cvtps_epi32(a.v)));
    return r;
#elif CRD_SIMD_HAS_NEON
    Vec4f r;
    r.v = vrndnq_f32(a.v);
    return r;
#else
    return Vec4f(detail::round_integral_lane<true>(a.v[0]), detail::round_integral_lane<true>(a.v[1]),
                 detail::round_integral_lane<true>(a.v[2]), detail::round_integral_lane<true>(a.v[3]));
#endif
}

CRD_FORCEINLINE Vec8f round_nearest(Vec8f a) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r;
    r.v = _mm256_round_ps(a.v, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    return r;
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

} // namespace crd::math::simd::inline CRD_MATH_LEVEL_NS
