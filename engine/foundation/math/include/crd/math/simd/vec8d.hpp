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

// Eight interleaved complex {r0,i0,...,r7,i7} (16 contiguous f64) into split re and im, and the inverse store, as
// Vec4d's helpers on each half: pure data movement, so every level gives the same bits.
CRD_FORCEINLINE void load_complex_deinterleaved(const f64* p, Vec8d& re, Vec8d& im) noexcept
{
#if CRD_SIMD_HAS_AVX512
    const __m512d a = _mm512_loadu_pd(p);     // r0 i0 r1 i1 r2 i2 r3 i3
    const __m512d b = _mm512_loadu_pd(p + 8); // r4 i4 r5 i5 r6 i6 r7 i7
    re.v = _mm512_permutex2var_pd(a, _mm512_setr_epi64(0, 2, 4, 6, 8, 10, 12, 14), b);
    im.v = _mm512_permutex2var_pd(a, _mm512_setr_epi64(1, 3, 5, 7, 9, 11, 13, 15), b);
#else
    load_complex_deinterleaved(p, re.lo, im.lo);
    load_complex_deinterleaved(p + 8, re.hi, im.hi);
#endif
}

CRD_FORCEINLINE void store_complex_interleaved(f64* p, Vec8d re, Vec8d im) noexcept
{
#if CRD_SIMD_HAS_AVX512
    _mm512_storeu_pd(p, _mm512_permutex2var_pd(re.v, _mm512_setr_epi64(0, 8, 1, 9, 2, 10, 3, 11), im.v));
    _mm512_storeu_pd(p + 8, _mm512_permutex2var_pd(re.v, _mm512_setr_epi64(4, 12, 5, 13, 6, 14, 7, 15), im.v));
#else
    store_complex_interleaved(p, re.lo, im.lo);
    store_complex_interleaved(p + 8, re.hi, im.hi);
#endif
}

// In-register 8x8 transpose (lane j of row i becomes lane i of row j): unpacks and two 128-bit lane shuffle stages at
// avx512 (24 ops), Vec4d's transpose4x4 on the four quadrants and a swap of the off-diagonal ones below it. Pure data
// movement, so every level gives the same bits.
CRD_FORCEINLINE void transpose8x8(Vec8d& r0, Vec8d& r1, Vec8d& r2, Vec8d& r3, Vec8d& r4, Vec8d& r5, Vec8d& r6,
                                  Vec8d& r7) noexcept
{
#if CRD_SIMD_HAS_AVX512
    // 128-bit lane k of t0 holds column 2k of rows 0-1, of t1 column 2k + 1; t2..t7 the same for rows 2-7.
    const __m512d t0 = _mm512_unpacklo_pd(r0.v, r1.v);
    const __m512d t1 = _mm512_unpackhi_pd(r0.v, r1.v);
    const __m512d t2 = _mm512_unpacklo_pd(r2.v, r3.v);
    const __m512d t3 = _mm512_unpackhi_pd(r2.v, r3.v);
    const __m512d t4 = _mm512_unpacklo_pd(r4.v, r5.v);
    const __m512d t5 = _mm512_unpackhi_pd(r4.v, r5.v);
    const __m512d t6 = _mm512_unpacklo_pd(r6.v, r7.v);
    const __m512d t7 = _mm512_unpackhi_pd(r6.v, r7.v);
    // Row 2k + h is 128-bit lane k of t(h), t(2 + h), t(4 + h), t(6 + h): a 4x4 transpose of 128-bit lanes.
    const auto lanes4x4 =
        [](__m512d a, __m512d b, __m512d c, __m512d d, Vec8d& o0, Vec8d& o1, Vec8d& o2, Vec8d& o3) noexcept
    {
        const __m512d s0 = _mm512_shuffle_f64x2(a, b, 0x44); // a0 a1 b0 b1
        const __m512d s1 = _mm512_shuffle_f64x2(a, b, 0xEE); // a2 a3 b2 b3
        const __m512d s2 = _mm512_shuffle_f64x2(c, d, 0x44); // c0 c1 d0 d1
        const __m512d s3 = _mm512_shuffle_f64x2(c, d, 0xEE); // c2 c3 d2 d3
        o0.v = _mm512_shuffle_f64x2(s0, s2, 0x88);           // a0 b0 c0 d0
        o1.v = _mm512_shuffle_f64x2(s0, s2, 0xDD);           // a1 b1 c1 d1
        o2.v = _mm512_shuffle_f64x2(s1, s3, 0x88);           // a2 b2 c2 d2
        o3.v = _mm512_shuffle_f64x2(s1, s3, 0xDD);           // a3 b3 c3 d3
    };
    lanes4x4(t0, t2, t4, t6, r0, r2, r4, r6);
    lanes4x4(t1, t3, t5, t7, r1, r3, r5, r7);
#else
    transpose4x4(r0.lo, r1.lo, r2.lo, r3.lo);
    transpose4x4(r0.hi, r1.hi, r2.hi, r3.hi);
    transpose4x4(r4.lo, r5.lo, r6.lo, r7.lo);
    transpose4x4(r4.hi, r5.hi, r6.hi, r7.hi);
    Vec8d* const top[4] = {&r0, &r1, &r2, &r3};
    Vec8d* const bottom[4] = {&r4, &r5, &r6, &r7};
    for (usize i = 0; i < 4; ++i)
    {
        const Vec4d upper_right = top[i]->hi;
        top[i]->hi = bottom[i]->lo;
        bottom[i]->lo = upper_right;
    }
#endif
}

} // namespace crd::math::simd::inline CRD_MATH_LEVEL_NS
