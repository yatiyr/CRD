// Vec8f — 8-lane f32 SIMD wrapper. Phase 3.1 v0a.
//
// AVX2: one 256-bit register. SSE2/NEON: two Vec4f composed (lo + hi).
// Scalar: eight-element array. Same determinism contract as Vec4f
// (no hardware FMA; fixed pairwise reduction; hardware sqrt).

#pragma once

#include <crd/math/simd/backend.hpp>
#include <crd/math/simd/vec4f.hpp>

#include <cmath>

namespace crd::math::simd
{
struct alignas(32) Vec8f
{
#if CRD_SIMD_HAS_AVX2
    using Native = __m256;
    Native v;
#else
    Vec4f lo, hi;
#endif

    Vec8f() noexcept = default;

    CRD_FORCEINLINE explicit Vec8f(f32 broadcast) noexcept
    {
#if CRD_SIMD_HAS_AVX2
        v = _mm256_set1_ps(broadcast);
#else
        lo = Vec4f(broadcast);
        hi = Vec4f(broadcast);
#endif
    }

    CRD_FORCEINLINE Vec8f(f32 e0, f32 e1, f32 e2, f32 e3, f32 e4, f32 e5, f32 e6, f32 e7) noexcept
    {
#if CRD_SIMD_HAS_AVX2
        v = _mm256_set_ps(e7, e6, e5, e4, e3, e2, e1, e0);
#else
        lo = Vec4f(e0, e1, e2, e3);
        hi = Vec4f(e4, e5, e6, e7);
#endif
    }

    CRD_FORCEINLINE Vec8f(Vec4f lo_in, Vec4f hi_in) noexcept
    {
#if CRD_SIMD_HAS_AVX2
        v = _mm256_set_m128(hi_in.v, lo_in.v);
#else
        lo = lo_in;
        hi = hi_in;
#endif
    }

    [[nodiscard]] CRD_FORCEINLINE static Vec8f zero() noexcept { return Vec8f(0.0F); }
    [[nodiscard]] CRD_FORCEINLINE static Vec8f one() noexcept { return Vec8f(1.0F); }

    [[nodiscard]] CRD_FORCEINLINE static Vec8f load(const f32* p) noexcept
    {
#if CRD_SIMD_HAS_AVX2
        Vec8f r;
        r.v = _mm256_loadu_ps(p);
        return r;
#else
        return Vec8f(Vec4f::load(p), Vec4f::load(p + 4));
#endif
    }

    [[nodiscard]] CRD_FORCEINLINE static Vec8f load_aligned(const f32* p) noexcept
    {
#if CRD_SIMD_HAS_AVX2
        Vec8f r;
        r.v = _mm256_load_ps(p);
        return r;
#else
        return Vec8f(Vec4f::load_aligned(p), Vec4f::load_aligned(p + 4));
#endif
    }

    CRD_FORCEINLINE void store(f32* p) const noexcept
    {
#if CRD_SIMD_HAS_AVX2
        _mm256_storeu_ps(p, v);
#else
        lo.store(p);
        hi.store(p + 4);
#endif
    }

    CRD_FORCEINLINE void store_aligned(f32* p) const noexcept
    {
#if CRD_SIMD_HAS_AVX2
        _mm256_store_ps(p, v);
#else
        lo.store_aligned(p);
        hi.store_aligned(p + 4);
#endif
    }

    [[nodiscard]] CRD_FORCEINLINE f32 lane(usize i) const noexcept
    {
        f32 tmp[8];
        store(tmp);
        return tmp[i];
    }
};

#if CRD_SIMD_HAS_AVX2
#define CRD_SIMD_VEC8_BIN_AVX2(OP, INTRIN)                                                                             \
    Vec8f r;                                                                                                           \
    r.v = INTRIN(a.v, b.v);                                                                                            \
    return r;
#else
#define CRD_SIMD_VEC8_BIN_FALLBACK(OP) return Vec8f(a.lo OP b.lo, a.hi OP b.hi);
#endif

// ---- arithmetic ------------------------------------------------------------

CRD_FORCEINLINE Vec8f operator+(Vec8f a, Vec8f b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    CRD_SIMD_VEC8_BIN_AVX2(+, _mm256_add_ps)
#else
    return Vec8f(a.lo + b.lo, a.hi + b.hi);
#endif
}

CRD_FORCEINLINE Vec8f operator-(Vec8f a, Vec8f b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    CRD_SIMD_VEC8_BIN_AVX2(-, _mm256_sub_ps)
#else
    return Vec8f(a.lo - b.lo, a.hi - b.hi);
#endif
}

CRD_FORCEINLINE Vec8f operator*(Vec8f a, Vec8f b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    CRD_SIMD_VEC8_BIN_AVX2(*, _mm256_mul_ps)
#else
    return Vec8f(a.lo * b.lo, a.hi * b.hi);
#endif
}

CRD_FORCEINLINE Vec8f operator/(Vec8f a, Vec8f b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    CRD_SIMD_VEC8_BIN_AVX2(/, _mm256_div_ps)
#else
    return Vec8f(a.lo / b.lo, a.hi / b.hi);
#endif
}

CRD_FORCEINLINE Vec8f operator-(Vec8f a) noexcept
{
    return Vec8f::zero() - a;
}
CRD_FORCEINLINE Vec8f operator*(Vec8f a, f32 s) noexcept
{
    return a * Vec8f(s);
}
CRD_FORCEINLINE Vec8f operator*(f32 s, Vec8f a) noexcept
{
    return Vec8f(s) * a;
}
CRD_FORCEINLINE Vec8f operator/(Vec8f a, f32 s) noexcept
{
    return a / Vec8f(s);
}

CRD_FORCEINLINE Vec8f mul_add(Vec8f a, Vec8f b, Vec8f c) noexcept
{
    return (a * b) + c;
}
CRD_FORCEINLINE Vec8f mul_sub(Vec8f a, Vec8f b, Vec8f c) noexcept
{
    return (a * b) - c;
}

// Single-rounded IEEE 754 FMA (a*b + c with one rounding step).
// Distinct from mul_add (two roundings, ADR-0063 default). Use when the
// numerical-computing consumer (crd-hesap microkernels per ADR-0082)
// wants ~2x throughput on AVX2-FMA hardware AND accepts the single-rounded
// arithmetic. Bit-exact across SIMD widths only when ALL lanes/widths use
// fma() (NOT mixed with mul_add). Scalar fallback uses std::fma which is
// IEEE 754-2008 mandated to match the hardware FMA bit-for-bit.
CRD_FORCEINLINE Vec8f fma(Vec8f a, Vec8f b, Vec8f c) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r;
    r.v = _mm256_fmadd_ps(a.v, b.v, c.v);
    return r;
#else
    return Vec8f(fma(a.lo, b.lo, c.lo), fma(a.hi, b.hi, c.hi));
#endif
}

// Single-rounded negated FMA: c - a*b. The complex-twiddle workhorse.
CRD_FORCEINLINE Vec8f fnmadd(Vec8f a, Vec8f b, Vec8f c) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r;
    r.v = _mm256_fnmadd_ps(a.v, b.v, c.v);
    return r;
#else
    return c - (a * b); // scalar fallback (non-AVX2); the AVX2 build uses the fused op
#endif
}

// ---- min / max / abs / sqrt -----------------------------------------------

CRD_FORCEINLINE Vec8f min(Vec8f a, Vec8f b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r;
    r.v = _mm256_min_ps(a.v, b.v);
    return r;
#else
    return Vec8f(min(a.lo, b.lo), min(a.hi, b.hi));
#endif
}

CRD_FORCEINLINE Vec8f max(Vec8f a, Vec8f b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r;
    r.v = _mm256_max_ps(a.v, b.v);
    return r;
#else
    return Vec8f(max(a.lo, b.lo), max(a.hi, b.hi));
#endif
}

CRD_FORCEINLINE Vec8f abs(Vec8f a) noexcept
{
#if CRD_SIMD_HAS_AVX2
    const __m256 sign_mask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
    Vec8f r;
    r.v = _mm256_and_ps(a.v, sign_mask);
    return r;
#else
    return Vec8f(abs(a.lo), abs(a.hi));
#endif
}

CRD_FORCEINLINE Vec8f clamp(Vec8f a, Vec8f lo, Vec8f hi) noexcept
{
    return min(max(a, lo), hi);
}

CRD_FORCEINLINE Vec8f sqrt(Vec8f a) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r;
    r.v = _mm256_sqrt_ps(a.v);
    return r;
#else
    return Vec8f(sqrt(a.lo), sqrt(a.hi));
#endif
}

// ---- reductions (deterministic pairwise order) ----------------------------

CRD_FORCEINLINE f32 horizontal_sum(Vec8f a) noexcept
{
    f32 lanes[8];
    a.store(lanes);
    const f32 s01 = lanes[0] + lanes[1];
    const f32 s23 = lanes[2] + lanes[3];
    const f32 s45 = lanes[4] + lanes[5];
    const f32 s67 = lanes[6] + lanes[7];
    const f32 s0123 = s01 + s23;
    const f32 s4567 = s45 + s67;
    return s0123 + s4567;
}

CRD_FORCEINLINE f32 dot(Vec8f a, Vec8f b) noexcept
{
    return horizontal_sum(a * b);
}

// ---- comparisons (mask-producing) -----------------------------------------

CRD_FORCEINLINE Vec8f cmp_lt(Vec8f a, Vec8f b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r;
    r.v = _mm256_cmp_ps(a.v, b.v, _CMP_LT_OQ);
    return r;
#else
    return Vec8f(cmp_lt(a.lo, b.lo), cmp_lt(a.hi, b.hi));
#endif
}

CRD_FORCEINLINE Vec8f cmp_le(Vec8f a, Vec8f b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r;
    r.v = _mm256_cmp_ps(a.v, b.v, _CMP_LE_OQ);
    return r;
#else
    return Vec8f(cmp_le(a.lo, b.lo), cmp_le(a.hi, b.hi));
#endif
}

CRD_FORCEINLINE Vec8f cmp_eq(Vec8f a, Vec8f b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r;
    r.v = _mm256_cmp_ps(a.v, b.v, _CMP_EQ_OQ);
    return r;
#else
    return Vec8f(cmp_eq(a.lo, b.lo), cmp_eq(a.hi, b.hi));
#endif
}

CRD_FORCEINLINE Vec8f cmp_gt(Vec8f a, Vec8f b) noexcept
{
    return cmp_lt(b, a);
}
CRD_FORCEINLINE Vec8f cmp_ge(Vec8f a, Vec8f b) noexcept
{
    return cmp_le(b, a);
}

CRD_FORCEINLINE Vec8f select(Vec8f mask, Vec8f true_v, Vec8f false_v) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r;
    r.v = _mm256_or_ps(_mm256_and_ps(mask.v, true_v.v), _mm256_andnot_ps(mask.v, false_v.v));
    return r;
#else
    return Vec8f(select(mask.lo, true_v.lo, false_v.lo), select(mask.hi, true_v.hi, false_v.hi));
#endif
}

// Complex deinterleave/interleave for f32 (mirror of the Vec4d helpers): load 8 interleaved complex
// {r0,i0,...,r7,i7} into split re={r0..r7}, im={i0..i7}; and the inverse store. Pure data shuffles (no
// arithmetic) ⇒ bit-identical to the scalar fallback on every backend. Lets the batched FFT butterflies run a
// real Vec8f SoA path for f32 (8 transforms/vector) instead of the scalar tail.
CRD_FORCEINLINE void load_complex_deinterleaved(const f32* p, Vec8f& re, Vec8f& im) noexcept
{
#if CRD_SIMD_HAS_AVX2
    const __m256 a = _mm256_loadu_ps(p);                                // r0 i0 r1 i1 | r2 i2 r3 i3
    const __m256 b = _mm256_loadu_ps(p + 8);                            // r4 i4 r5 i5 | r6 i6 r7 i7
    const __m256 ev = _mm256_shuffle_ps(a, b, _MM_SHUFFLE(2, 0, 2, 0)); // r0 r1 r4 r5 | r2 r3 r6 r7
    const __m256 od = _mm256_shuffle_ps(a, b, _MM_SHUFFLE(3, 1, 3, 1)); // i0 i1 i4 i5 | i2 i3 i6 i7
    const __m256i idx = _mm256_setr_epi32(0, 1, 4, 5, 2, 3, 6, 7);
    re.v = _mm256_permutevar8x32_ps(ev, idx); // r0 r1 r2 r3 r4 r5 r6 r7
    im.v = _mm256_permutevar8x32_ps(od, idx); // i0 i1 i2 i3 i4 i5 i6 i7
#else
    re = Vec8f(p[0], p[2], p[4], p[6], p[8], p[10], p[12], p[14]);
    im = Vec8f(p[1], p[3], p[5], p[7], p[9], p[11], p[13], p[15]);
#endif
}

CRD_FORCEINLINE void store_complex_interleaved(f32* p, Vec8f re, Vec8f im) noexcept
{
#if CRD_SIMD_HAS_AVX2
    const __m256 ul = _mm256_unpacklo_ps(re.v, im.v);              // r0 i0 r1 i1 | r4 i4 r5 i5
    const __m256 uh = _mm256_unpackhi_ps(re.v, im.v);              // r2 i2 r3 i3 | r6 i6 r7 i7
    _mm256_storeu_ps(p, _mm256_permute2f128_ps(ul, uh, 0x20));     // r0 i0 r1 i1 r2 i2 r3 i3
    _mm256_storeu_ps(p + 8, _mm256_permute2f128_ps(ul, uh, 0x31)); // r4 i4 r5 i5 r6 i6 r7 i7
#else
    for (usize k = 0; k < 8; ++k)
    {
        p[2 * k] = re.lane(k);
        p[2 * k + 1] = im.lane(k);
    }
#endif
}

} // namespace crd::math::simd
