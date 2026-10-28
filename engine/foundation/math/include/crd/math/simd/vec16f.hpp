// Vec16f — 16-lane f32 SIMD type (SIMD-DISPATCH, ADR-0134 §5, §7): one 512-bit register on the avx512 level, two
// Vec8f (lanes 0-7 and 8-15) below it. Every operation gives the bits of the two Vec8f halves on every level: IEEE lane
// arithmetic, mul_add rounding twice and fma once, min/max returning the second operand on a NaN or two zeros, the
// all-ones lane masks of the comparisons, and the horizontal sum of the low half plus that of the high half (each the
// Vec8f pairwise tree). A partial load reads only its `count` lanes and zeroes the rest; a partial store writes only
// its `count` lanes. Kernel units include it directly; it is not part of the simd.hpp umbrella that general code
// reaches.

#pragma once

#include <crd/math/simd/backend.hpp>
#include <crd/math/simd/convert.hpp>
#include <crd/math/simd/vec16i.hpp>
#include <crd/math/simd/vec8f.hpp>

namespace crd::math::simd::inline CRD_MATH_LEVEL_NS
{
struct alignas(64) Vec16f
{
#if CRD_SIMD_HAS_AVX512
    using Native = __m512;
    Native v;
#else
    Vec8f lo, hi;
#endif

    Vec16f() noexcept = default;

    CRD_FORCEINLINE explicit Vec16f(f32 broadcast) noexcept
    {
#if CRD_SIMD_HAS_AVX512
        v = _mm512_set1_ps(broadcast);
#else
        lo = Vec8f(broadcast);
        hi = Vec8f(broadcast);
#endif
    }

    CRD_FORCEINLINE Vec16f(Vec8f lo_in, Vec8f hi_in) noexcept
    {
#if CRD_SIMD_HAS_AVX512
        v = _mm512_insertf32x8(_mm512_castps256_ps512(lo_in.v), hi_in.v, 1);
#else
        lo = lo_in;
        hi = hi_in;
#endif
    }

    [[nodiscard]] CRD_FORCEINLINE static Vec16f zero() noexcept { return Vec16f(0.0F); }
    [[nodiscard]] CRD_FORCEINLINE static Vec16f one() noexcept { return Vec16f(1.0F); }

    [[nodiscard]] CRD_FORCEINLINE static Vec16f load(const f32* p) noexcept
    {
#if CRD_SIMD_HAS_AVX512
        Vec16f r;
        r.v = _mm512_loadu_ps(p);
        return r;
#else
        return Vec16f(Vec8f::load(p), Vec8f::load(p + 8));
#endif
    }

    [[nodiscard]] CRD_FORCEINLINE static Vec16f load_aligned(const f32* p) noexcept
    {
#if CRD_SIMD_HAS_AVX512
        Vec16f r;
        r.v = _mm512_load_ps(p);
        return r;
#else
        return Vec16f(Vec8f::load_aligned(p), Vec8f::load_aligned(p + 8));
#endif
    }

    CRD_FORCEINLINE void store(f32* p) const noexcept
    {
#if CRD_SIMD_HAS_AVX512
        _mm512_storeu_ps(p, v);
#else
        lo.store(p);
        hi.store(p + 8);
#endif
    }

    CRD_FORCEINLINE void store_aligned(f32* p) const noexcept
    {
#if CRD_SIMD_HAS_AVX512
        _mm512_store_ps(p, v);
#else
        lo.store_aligned(p);
        hi.store_aligned(p + 8);
#endif
    }

    // The first `count` lanes (1..15; 16 is `load`/`store`): the other lanes load as +0 and are never read or written.
    [[nodiscard]] CRD_FORCEINLINE static Vec16f load_partial(const f32* p, usize count) noexcept
    {
#if CRD_SIMD_HAS_AVX512
        Vec16f r;
        r.v = _mm512_maskz_loadu_ps(static_cast<__mmask16>((1U << count) - 1U), p);
        return r;
#else
        if (count < 8U)
        {
            return Vec16f(Vec8f::load_partial(p, count), Vec8f::zero());
        }
        return Vec16f(Vec8f::load(p), count == 8U ? Vec8f::zero() : Vec8f::load_partial(p + 8, count - 8U));
#endif
    }

    CRD_FORCEINLINE void store_partial(f32* p, usize count) const noexcept
    {
#if CRD_SIMD_HAS_AVX512
        _mm512_mask_storeu_ps(p, static_cast<__mmask16>((1U << count) - 1U), v);
#else
        if (count < 8U)
        {
            lo.store_partial(p, count);
            return;
        }
        lo.store(p);
        if (count > 8U)
        {
            hi.store_partial(p + 8, count - 8U);
        }
#endif
    }

    [[nodiscard]] CRD_FORCEINLINE f32 lane(usize i) const noexcept
    {
        alignas(64) f32 tmp[16];
        store_aligned(tmp);
        return tmp[i];
    }
};
static_assert(sizeof(Vec16f) == 64 && alignof(Vec16f) == 64, "Vec16f is one 512-bit vector on every level");

// Lanes 0-7 and 8-15.
CRD_FORCEINLINE Vec8f low_half(Vec16f a) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec8f r;
    r.v = _mm512_castps512_ps256(a.v);
    return r;
#else
    return a.lo;
#endif
}

CRD_FORCEINLINE Vec8f high_half(Vec16f a) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec8f r;
    r.v = _mm512_extractf32x8_ps(a.v, 1);
    return r;
#else
    return a.hi;
#endif
}

// ---- arithmetic -------------------------------------------------------------

CRD_FORCEINLINE Vec16f operator+(Vec16f a, Vec16f b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16f r;
    r.v = _mm512_add_ps(a.v, b.v);
    return r;
#else
    return Vec16f(a.lo + b.lo, a.hi + b.hi);
#endif
}

CRD_FORCEINLINE Vec16f operator-(Vec16f a, Vec16f b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16f r;
    r.v = _mm512_sub_ps(a.v, b.v);
    return r;
#else
    return Vec16f(a.lo - b.lo, a.hi - b.hi);
#endif
}

CRD_FORCEINLINE Vec16f operator*(Vec16f a, Vec16f b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16f r;
    r.v = _mm512_mul_ps(a.v, b.v);
    return r;
#else
    return Vec16f(a.lo * b.lo, a.hi * b.hi);
#endif
}

CRD_FORCEINLINE Vec16f operator/(Vec16f a, Vec16f b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16f r;
    r.v = _mm512_div_ps(a.v, b.v);
    return r;
#else
    return Vec16f(a.lo / b.lo, a.hi / b.hi);
#endif
}

// IEEE negation (the sign bit flips).
CRD_FORCEINLINE Vec16f operator-(Vec16f a) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16f r;
    r.v = _mm512_xor_ps(a.v, _mm512_set1_ps(-0.0F));
    return r;
#else
    return Vec16f(-a.lo, -a.hi);
#endif
}

CRD_FORCEINLINE Vec16f operator*(Vec16f a, f32 s) noexcept
{
    return a * Vec16f(s);
}
CRD_FORCEINLINE Vec16f operator*(f32 s, Vec16f a) noexcept
{
    return Vec16f(s) * a;
}
CRD_FORCEINLINE Vec16f operator/(Vec16f a, f32 s) noexcept
{
    return a / Vec16f(s);
}

// Two roundings (ADR-0063), as Vec8f's.
CRD_FORCEINLINE Vec16f mul_add(Vec16f a, Vec16f b, Vec16f c) noexcept
{
    return (a * b) + c;
}
CRD_FORCEINLINE Vec16f mul_sub(Vec16f a, Vec16f b, Vec16f c) noexcept
{
    return (a * b) - c;
}

// One rounding: a * b + c, and c - a * b.
CRD_FORCEINLINE Vec16f fma(Vec16f a, Vec16f b, Vec16f c) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16f r;
    r.v = _mm512_fmadd_ps(a.v, b.v, c.v);
    return r;
#else
    return Vec16f(fma(a.lo, b.lo, c.lo), fma(a.hi, b.hi, c.hi));
#endif
}

CRD_FORCEINLINE Vec16f fnmadd(Vec16f a, Vec16f b, Vec16f c) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16f r;
    r.v = _mm512_fnmadd_ps(a.v, b.v, c.v);
    return r;
#else
    return Vec16f(fnmadd(a.lo, b.lo, c.lo), fnmadd(a.hi, b.hi, c.hi));
#endif
}

// ---- min / max / abs / sqrt -------------------------------------------------

CRD_FORCEINLINE Vec16f min(Vec16f a, Vec16f b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16f r;
    r.v = _mm512_min_ps(a.v, b.v);
    return r;
#else
    return Vec16f(min(a.lo, b.lo), min(a.hi, b.hi));
#endif
}

CRD_FORCEINLINE Vec16f max(Vec16f a, Vec16f b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16f r;
    r.v = _mm512_max_ps(a.v, b.v);
    return r;
#else
    return Vec16f(max(a.lo, b.lo), max(a.hi, b.hi));
#endif
}

CRD_FORCEINLINE Vec16f abs(Vec16f a) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16f r;
    r.v = _mm512_and_ps(a.v, _mm512_castsi512_ps(_mm512_set1_epi32(0x7FFFFFFF)));
    return r;
#else
    return Vec16f(abs(a.lo), abs(a.hi));
#endif
}

CRD_FORCEINLINE Vec16f clamp(Vec16f a, Vec16f lo, Vec16f hi) noexcept
{
    return min(max(a, lo), hi);
}

CRD_FORCEINLINE Vec16f sqrt(Vec16f a) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16f r;
    r.v = _mm512_sqrt_ps(a.v);
    return r;
#else
    return Vec16f(sqrt(a.lo), sqrt(a.hi));
#endif
}

// ---- reductions: the low half's pairwise sum plus the high half's -----------

CRD_FORCEINLINE f32 horizontal_sum(Vec16f a) noexcept
{
    return horizontal_sum(low_half(a)) + horizontal_sum(high_half(a));
}

CRD_FORCEINLINE f32 dot(Vec16f a, Vec16f b) noexcept
{
    return horizontal_sum(a * b);
}

// ---- comparisons: an all-ones lane where true (ordered, so a NaN compares false) -----------------------------------

#if CRD_SIMD_HAS_AVX512
namespace detail
{
CRD_FORCEINLINE __m512 lane_mask_ps(__mmask16 m) noexcept
{
    return _mm512_castsi512_ps(_mm512_movm_epi32(m));
}
} // namespace detail
#endif

CRD_FORCEINLINE Vec16f cmp_lt(Vec16f a, Vec16f b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16f r;
    r.v = detail::lane_mask_ps(_mm512_cmp_ps_mask(a.v, b.v, _CMP_LT_OQ));
    return r;
#else
    return Vec16f(cmp_lt(a.lo, b.lo), cmp_lt(a.hi, b.hi));
#endif
}

CRD_FORCEINLINE Vec16f cmp_le(Vec16f a, Vec16f b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16f r;
    r.v = detail::lane_mask_ps(_mm512_cmp_ps_mask(a.v, b.v, _CMP_LE_OQ));
    return r;
#else
    return Vec16f(cmp_le(a.lo, b.lo), cmp_le(a.hi, b.hi));
#endif
}

CRD_FORCEINLINE Vec16f cmp_eq(Vec16f a, Vec16f b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16f r;
    r.v = detail::lane_mask_ps(_mm512_cmp_ps_mask(a.v, b.v, _CMP_EQ_OQ));
    return r;
#else
    return Vec16f(cmp_eq(a.lo, b.lo), cmp_eq(a.hi, b.hi));
#endif
}

CRD_FORCEINLINE Vec16f cmp_gt(Vec16f a, Vec16f b) noexcept
{
    return cmp_lt(b, a);
}
CRD_FORCEINLINE Vec16f cmp_ge(Vec16f a, Vec16f b) noexcept
{
    return cmp_le(b, a);
}

// The mask is a comparison result; the blend reads each lane's sign bit, as Vec8f's does.
CRD_FORCEINLINE Vec16f select(Vec16f mask, Vec16f true_v, Vec16f false_v) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16f r;
    r.v = _mm512_mask_blend_ps(_mm512_movepi32_mask(_mm512_castps_si512(mask.v)), false_v.v, true_v.v);
    return r;
#else
    return Vec16f(select(mask.lo, true_v.lo, false_v.lo), select(mask.hi, true_v.hi, false_v.hi));
#endif
}

// ---- bit patterns, conversions and rounding (convert.hpp's, sixteen lanes) ------------------------------------------

CRD_FORCEINLINE Vec16i bitcast_to_int(Vec16f a) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16i r;
    r.v = _mm512_castps_si512(a.v);
    return r;
#else
    return Vec16i(bitcast_to_int(a.lo), bitcast_to_int(a.hi));
#endif
}

CRD_FORCEINLINE Vec16f bitcast_to_float(Vec16i a) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16f r;
    r.v = _mm512_castsi512_ps(a.v);
    return r;
#else
    return Vec16f(bitcast_to_float(a.lo), bitcast_to_float(a.hi));
#endif
}

// Toward zero; a NaN or a value outside the i32 range gives INT32_MIN.
CRD_FORCEINLINE Vec16i convert_truncate(Vec16f a) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16i r;
    r.v = _mm512_cvttps_epi32(detail::fold_barrier(a.v));
    return r;
#else
    return Vec16i(convert_truncate(a.lo), convert_truncate(a.hi));
#endif
}

CRD_FORCEINLINE Vec16f convert_to_float(Vec16i a) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16f r;
    r.v = _mm512_cvtepi32_ps(a.v);
    return r;
#else
    return Vec16f(convert_to_float(a.lo), convert_to_float(a.hi));
#endif
}

// roundscale with scale 0 is roundps: the input's sign on a zero result, a quiet NaN.
CRD_FORCEINLINE Vec16f truncate(Vec16f a) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16f r;
    r.v = _mm512_roundscale_ps(a.v, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC);
    return r;
#else
    return Vec16f(truncate(a.lo), truncate(a.hi));
#endif
}

CRD_FORCEINLINE Vec16f round_nearest(Vec16f a) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16f r;
    r.v = _mm512_roundscale_ps(a.v, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    return r;
#else
    return Vec16f(round_nearest(a.lo), round_nearest(a.hi));
#endif
}

CRD_FORCEINLINE Vec16f bit_and(Vec16f a, Vec16f b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16f r;
    r.v = _mm512_and_ps(a.v, b.v);
    return r;
#else
    return Vec16f(bit_and(a.lo, b.lo), bit_and(a.hi, b.hi));
#endif
}

CRD_FORCEINLINE Vec16f bit_or(Vec16f a, Vec16f b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16f r;
    r.v = _mm512_or_ps(a.v, b.v);
    return r;
#else
    return Vec16f(bit_or(a.lo, b.lo), bit_or(a.hi, b.hi));
#endif
}

CRD_FORCEINLINE Vec16f bit_xor(Vec16f a, Vec16f b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16f r;
    r.v = _mm512_xor_ps(a.v, b.v);
    return r;
#else
    return Vec16f(bit_xor(a.lo, b.lo), bit_xor(a.hi, b.hi));
#endif
}

// ~a & b, as Vec8f's.
CRD_FORCEINLINE Vec16f bit_andnot(Vec16f a, Vec16f b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec16f r;
    r.v = _mm512_andnot_ps(a.v, b.v);
    return r;
#else
    return Vec16f(bit_andnot(a.lo, b.lo), bit_andnot(a.hi, b.hi));
#endif
}

} // namespace crd::math::simd::inline CRD_MATH_LEVEL_NS
