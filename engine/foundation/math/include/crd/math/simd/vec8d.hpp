// Vec8d — 8-lane f64 SIMD type (SIMD-DISPATCH, ADR-0134 §5, §7): one 512-bit register on the avx512 level, two
// Vec4d (lanes 0-3 and 4-7) below it. Every operation gives the bits of the two Vec4d halves on every level: IEEE lane
// arithmetic, mul_add rounding twice and fma once, min/max returning the second operand on a NaN or two zeros, the
// all-ones lane masks of the comparisons, and the horizontal sum of the low half plus that of the high half (each the
// Vec4d pairwise tree). A partial load reads only its `count` lanes and zeroes the rest; a partial store writes only
// its `count` lanes. Kernel units include it directly; it is not part of the simd.hpp umbrella that general code
// reaches.

#pragma once

#include <crd/math/simd/backend.hpp>
#include <crd/math/simd/vec4d.hpp>

namespace crd::math::simd::inline CRD_MATH_LEVEL_NS
{
struct Vec8d
{
#if CRD_SIMD_HAS_AVX512
    using Native = __m512d;
    Native v;
#else
    Vec4d lo, hi;
#endif

    Vec8d() noexcept = default;

    CRD_FORCEINLINE explicit Vec8d(f64 broadcast) noexcept
    {
#if CRD_SIMD_HAS_AVX512
        v = _mm512_set1_pd(broadcast);
#else
        lo = Vec4d(broadcast);
        hi = Vec4d(broadcast);
#endif
    }

    CRD_FORCEINLINE Vec8d(Vec4d lo_in, Vec4d hi_in) noexcept
    {
#if CRD_SIMD_HAS_AVX512
        v = _mm512_insertf64x4(_mm512_castpd256_pd512(lo_in.v), hi_in.v, 1);
#else
        lo = lo_in;
        hi = hi_in;
#endif
    }

    [[nodiscard]] CRD_FORCEINLINE static Vec8d zero() noexcept { return Vec8d(0.0); }
    [[nodiscard]] CRD_FORCEINLINE static Vec8d one() noexcept { return Vec8d(1.0); }

    [[nodiscard]] CRD_FORCEINLINE static Vec8d load(const f64* p) noexcept
    {
#if CRD_SIMD_HAS_AVX512
        Vec8d r;
        r.v = _mm512_loadu_pd(p);
        return r;
#else
        return Vec8d(Vec4d::load(p), Vec4d::load(p + 4));
#endif
    }

    [[nodiscard]] CRD_FORCEINLINE static Vec8d load_aligned(const f64* p) noexcept
    {
#if CRD_SIMD_HAS_AVX512
        Vec8d r;
        r.v = _mm512_load_pd(p);
        return r;
#else
        return Vec8d(Vec4d::load_aligned(p), Vec4d::load_aligned(p + 4));
#endif
    }

    CRD_FORCEINLINE void store(f64* p) const noexcept
    {
#if CRD_SIMD_HAS_AVX512
        _mm512_storeu_pd(p, v);
#else
        lo.store(p);
        hi.store(p + 4);
#endif
    }

    CRD_FORCEINLINE void store_aligned(f64* p) const noexcept
    {
#if CRD_SIMD_HAS_AVX512
        _mm512_store_pd(p, v);
#else
        lo.store_aligned(p);
        hi.store_aligned(p + 4);
#endif
    }

    // The first `count` lanes (1..7; 8 is `load`/`store`): the other lanes load as +0 and are never read or written.
    [[nodiscard]] CRD_FORCEINLINE static Vec8d load_partial(const f64* p, usize count) noexcept
    {
#if CRD_SIMD_HAS_AVX512
        Vec8d r;
        r.v = _mm512_maskz_loadu_pd(static_cast<__mmask8>((1U << count) - 1U), p);
        return r;
#else
        if (count < 4U)
        {
            return Vec8d(Vec4d::load_partial(p, count), Vec4d::zero());
        }
        return Vec8d(Vec4d::load(p), count == 4U ? Vec4d::zero() : Vec4d::load_partial(p + 4, count - 4U));
#endif
    }

    CRD_FORCEINLINE void store_partial(f64* p, usize count) const noexcept
    {
#if CRD_SIMD_HAS_AVX512
        _mm512_mask_storeu_pd(p, static_cast<__mmask8>((1U << count) - 1U), v);
#else
        if (count < 4U)
        {
            lo.store_partial(p, count);
            return;
        }
        lo.store(p);
        if (count > 4U)
        {
            hi.store_partial(p + 4, count - 4U);
        }
#endif
    }

    [[nodiscard]] CRD_FORCEINLINE f64 lane(usize i) const noexcept
    {
        alignas(64) f64 tmp[8];
        store_aligned(tmp);
        return tmp[i];
    }
};
static_assert(sizeof(Vec8d) == 64 && alignof(Vec8d) == simd_alignment(64),
              "Vec8d is 512 bits, aligned no higher than the level's widest register");

// Lanes 0-3 and 4-7.
CRD_FORCEINLINE Vec4d low_half(Vec8d a) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec4d r;
    r.v = _mm512_castpd512_pd256(a.v);
    return r;
#else
    return a.lo;
#endif
}

CRD_FORCEINLINE Vec4d high_half(Vec8d a) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec4d r;
    r.v = _mm512_extractf64x4_pd(a.v, 1);
    return r;
#else
    return a.hi;
#endif
}

// ---- arithmetic -------------------------------------------------------------

CRD_FORCEINLINE Vec8d operator+(Vec8d a, Vec8d b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec8d r;
    r.v = _mm512_add_pd(a.v, b.v);
    return r;
#else
    return Vec8d(a.lo + b.lo, a.hi + b.hi);
#endif
}

CRD_FORCEINLINE Vec8d operator-(Vec8d a, Vec8d b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec8d r;
    r.v = _mm512_sub_pd(a.v, b.v);
    return r;
#else
    return Vec8d(a.lo - b.lo, a.hi - b.hi);
#endif
}

CRD_FORCEINLINE Vec8d operator*(Vec8d a, Vec8d b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec8d r;
    r.v = _mm512_mul_pd(a.v, b.v);
    return r;
#else
    return Vec8d(a.lo * b.lo, a.hi * b.hi);
#endif
}

CRD_FORCEINLINE Vec8d operator/(Vec8d a, Vec8d b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec8d r;
    r.v = _mm512_div_pd(a.v, b.v);
    return r;
#else
    return Vec8d(a.lo / b.lo, a.hi / b.hi);
#endif
}

// IEEE negation (the sign bit flips).
CRD_FORCEINLINE Vec8d operator-(Vec8d a) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec8d r;
    r.v = _mm512_xor_pd(a.v, _mm512_set1_pd(-0.0));
    return r;
#else
    return Vec8d(-a.lo, -a.hi);
#endif
}

CRD_FORCEINLINE Vec8d operator*(Vec8d a, f64 s) noexcept
{
    return a * Vec8d(s);
}
CRD_FORCEINLINE Vec8d operator*(f64 s, Vec8d a) noexcept
{
    return Vec8d(s) * a;
}
CRD_FORCEINLINE Vec8d operator/(Vec8d a, f64 s) noexcept
{
    return a / Vec8d(s);
}

// Two roundings (ADR-0063), as Vec4d's.
CRD_FORCEINLINE Vec8d mul_add(Vec8d a, Vec8d b, Vec8d c) noexcept
{
    return (a * b) + c;
}
CRD_FORCEINLINE Vec8d mul_sub(Vec8d a, Vec8d b, Vec8d c) noexcept
{
    return (a * b) - c;
}

// One rounding: a * b + c, and c - a * b.
CRD_FORCEINLINE Vec8d fma(Vec8d a, Vec8d b, Vec8d c) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec8d r;
    r.v = _mm512_fmadd_pd(a.v, b.v, c.v);
    return r;
#else
    return Vec8d(fma(a.lo, b.lo, c.lo), fma(a.hi, b.hi, c.hi));
#endif
}

CRD_FORCEINLINE Vec8d fnmadd(Vec8d a, Vec8d b, Vec8d c) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec8d r;
    r.v = _mm512_fnmadd_pd(a.v, b.v, c.v);
    return r;
#else
    return Vec8d(fnmadd(a.lo, b.lo, c.lo), fnmadd(a.hi, b.hi, c.hi));
#endif
}

// ---- min / max / abs / sqrt -------------------------------------------------

CRD_FORCEINLINE Vec8d min(Vec8d a, Vec8d b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec8d r;
    r.v = _mm512_min_pd(a.v, b.v);
    return r;
#else
    return Vec8d(min(a.lo, b.lo), min(a.hi, b.hi));
#endif
}

CRD_FORCEINLINE Vec8d max(Vec8d a, Vec8d b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec8d r;
    r.v = _mm512_max_pd(a.v, b.v);
    return r;
#else
    return Vec8d(max(a.lo, b.lo), max(a.hi, b.hi));
#endif
}

CRD_FORCEINLINE Vec8d abs(Vec8d a) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec8d r;
    r.v = _mm512_and_pd(a.v, _mm512_castsi512_pd(_mm512_set1_epi64(0x7FFFFFFFFFFFFFFFLL)));
    return r;
#else
    return Vec8d(abs(a.lo), abs(a.hi));
#endif
}

CRD_FORCEINLINE Vec8d clamp(Vec8d a, Vec8d lo, Vec8d hi) noexcept
{
    return min(max(a, lo), hi);
}

CRD_FORCEINLINE Vec8d sqrt(Vec8d a) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec8d r;
    r.v = _mm512_sqrt_pd(a.v);
    return r;
#else
    return Vec8d(sqrt(a.lo), sqrt(a.hi));
#endif
}

// ---- reductions: the low half's pairwise sum plus the high half's -----------

CRD_FORCEINLINE f64 horizontal_sum(Vec8d a) noexcept
{
    return horizontal_sum(low_half(a)) + horizontal_sum(high_half(a));
}

CRD_FORCEINLINE f64 dot(Vec8d a, Vec8d b) noexcept
{
    return horizontal_sum(a * b);
}

// ---- comparisons: an all-ones lane where true (ordered, so a NaN compares false) -----------------------------------

#if CRD_SIMD_HAS_AVX512
namespace detail
{
CRD_FORCEINLINE __m512d lane_mask_pd(__mmask8 m) noexcept
{
    return _mm512_castsi512_pd(_mm512_movm_epi64(m));
}
} // namespace detail
#endif

CRD_FORCEINLINE Vec8d cmp_lt(Vec8d a, Vec8d b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec8d r;
    r.v = detail::lane_mask_pd(_mm512_cmp_pd_mask(a.v, b.v, _CMP_LT_OQ));
    return r;
#else
    return Vec8d(cmp_lt(a.lo, b.lo), cmp_lt(a.hi, b.hi));
#endif
}

CRD_FORCEINLINE Vec8d cmp_le(Vec8d a, Vec8d b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec8d r;
    r.v = detail::lane_mask_pd(_mm512_cmp_pd_mask(a.v, b.v, _CMP_LE_OQ));
    return r;
#else
    return Vec8d(cmp_le(a.lo, b.lo), cmp_le(a.hi, b.hi));
#endif
}

CRD_FORCEINLINE Vec8d cmp_eq(Vec8d a, Vec8d b) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec8d r;
    r.v = detail::lane_mask_pd(_mm512_cmp_pd_mask(a.v, b.v, _CMP_EQ_OQ));
    return r;
#else
    return Vec8d(cmp_eq(a.lo, b.lo), cmp_eq(a.hi, b.hi));
#endif
}

CRD_FORCEINLINE Vec8d cmp_gt(Vec8d a, Vec8d b) noexcept
{
    return cmp_lt(b, a);
}
CRD_FORCEINLINE Vec8d cmp_ge(Vec8d a, Vec8d b) noexcept
{
    return cmp_le(b, a);
}

// The mask is a comparison result; the blend reads each lane's sign bit, as Vec4d's does.
CRD_FORCEINLINE Vec8d select(Vec8d mask, Vec8d true_v, Vec8d false_v) noexcept
{
#if CRD_SIMD_HAS_AVX512
    Vec8d r;
    r.v = _mm512_mask_blend_pd(_mm512_movepi64_mask(_mm512_castpd_si512(mask.v)), false_v.v, true_v.v);
    return r;
#else
    return Vec8d(select(mask.lo, true_v.lo, false_v.lo), select(mask.hi, true_v.hi, false_v.hi));
#endif
}

} // namespace crd::math::simd::inline CRD_MATH_LEVEL_NS
