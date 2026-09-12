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

    // Masked partial load/store of the FIRST `count` lanes (1..7; count==8 is
    // `load`/`store`). Unloaded lanes are ZERO; unstored lanes untouched (the
    // vector-tail primitive — see Vec4d::load_partial).
    [[nodiscard]] CRD_FORCEINLINE static Vec8f load_partial(const f32* p, usize count) noexcept
    {
#if CRD_SIMD_HAS_AVX2
        alignas(32) static constexpr i32 kMaskLut[16] = {-1, -1, -1, -1, -1, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0};
        const __m256i m = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(kMaskLut + (8U - count)));
        Vec8f r;
        r.v = _mm256_maskload_ps(p, m);
        return r;
#else
        f32 tmp[8] = {};
        for (usize i = 0; i < count; ++i)
        {
            tmp[i] = p[i];
        }
        Vec8f r;
        r.lo = Vec4f::load(tmp);
        r.hi = Vec4f::load(tmp + 4);
        return r;
#endif
    }

    CRD_FORCEINLINE void store_partial(f32* p, usize count) const noexcept
    {
#if CRD_SIMD_HAS_AVX2
        alignas(32) static constexpr i32 kMaskLut[16] = {-1, -1, -1, -1, -1, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0};
        const __m256i m = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(kMaskLut + (8U - count)));
        _mm256_maskstore_ps(p, m, v);
#else
        f32 tmp[8];
        lo.store(tmp);
        hi.store(tmp + 4);
        for (usize i = 0; i < count; ++i)
        {
            p[i] = tmp[i];
        }
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

// ---- interleaved-complex idiom primitives (the FFT ip4-AoS f32 edition, 2026-07-04) -------------
// A Vec8f as FOUR interleaved complex f32 [z0r z0i z1r z1i z2r z2i z3r z3i]. Each op is exact per
// lane (shuffle / IEEE add-sub / single-rounded fused) ⇒ deterministic. The "c"-granularity ops
// work on 64-bit complex units — the f32 twins of vec4d's 128-bit concat_lo/concat_hi/mix_lo_hi.

// swap re/im within each complex pair.
CRD_FORCEINLINE Vec8f swap_pairs(Vec8f a) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r;
    r.v = _mm256_permute_ps(a.v, 0xB1); // [1,0,3,2] per 128
    return r;
#else
    alignas(32) f32 t[8];
    a.store(t);
    return Vec8f(t[1], t[0], t[3], t[2], t[5], t[4], t[7], t[6]);
#endif
}

// [a0-b0, a1+b1, ...] — subtract even lanes, add odd (the complex-mul combiner).
CRD_FORCEINLINE Vec8f addsub(Vec8f a, Vec8f b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r;
    r.v = _mm256_addsub_ps(a.v, b.v);
    return r;
#else
    alignas(32) f32 x[8];
    alignas(32) f32 y[8];
    a.store(x);
    b.store(y);
    return Vec8f(x[0] - y[0], x[1] + y[1], x[2] - y[2], x[3] + y[3], x[4] - y[4], x[5] + y[5],
                 x[6] - y[6], x[7] + y[7]);
#endif
}

// [a*b -/+ c] fused, subtract on even lanes, add on odd.
CRD_FORCEINLINE Vec8f fmaddsub(Vec8f a, Vec8f b, Vec8f c) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r;
    r.v = _mm256_fmaddsub_ps(a.v, b.v, c.v);
    return r;
#else
    alignas(32) f32 x[8];
    alignas(32) f32 y[8];
    alignas(32) f32 z[8];
    a.store(x);
    b.store(y);
    c.store(z);
    alignas(32) f32 o[8];
    for (usize i = 0; i < 8; ++i)
    {
        o[i] = std::fma(x[i], y[i], (i & 1U) ? z[i] : -z[i]);
    }
    return Vec8f::load(o);
#endif
}

// [p0 p0 p1 p1 p2 p2 p3 p3] — four table entries duplicated per complex lane (twiddle expander).
CRD_FORCEINLINE Vec8f load_dup_pairs(const f32* p) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r;
    const __m256i idx = _mm256_setr_epi32(0, 0, 1, 1, 2, 2, 3, 3);
    r.v = _mm256_permutevar8x32_ps(_mm256_castps128_ps256(_mm_loadu_ps(p)), idx);
    return r;
#else
    return Vec8f(p[0], p[0], p[1], p[1], p[2], p[2], p[3], p[3]);
#endif
}

// complex-granularity interleaves: [a.c0 b.c0 a.c2 b.c2] / [a.c1 b.c1 a.c3 b.c3] (per-128 unpack).
CRD_FORCEINLINE Vec8f unpack_c_lo(Vec8f a, Vec8f b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r;
    r.v = _mm256_castpd_ps(_mm256_unpacklo_pd(_mm256_castps_pd(a.v), _mm256_castps_pd(b.v)));
    return r;
#else
    alignas(32) f32 x[8];
    alignas(32) f32 y[8];
    a.store(x);
    b.store(y);
    return Vec8f(x[0], x[1], y[0], y[1], x[4], x[5], y[4], y[5]);
#endif
}
CRD_FORCEINLINE Vec8f unpack_c_hi(Vec8f a, Vec8f b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r;
    r.v = _mm256_castpd_ps(_mm256_unpackhi_pd(_mm256_castps_pd(a.v), _mm256_castps_pd(b.v)));
    return r;
#else
    alignas(32) f32 x[8];
    alignas(32) f32 y[8];
    a.store(x);
    b.store(y);
    return Vec8f(x[2], x[3], y[2], y[3], x[6], x[7], y[6], y[7]);
#endif
}

// [a.c0 b.c1 a.c2 b.c3] — odd complex units from b (the f32 twin of vec4d mix_lo_hi).
CRD_FORCEINLINE Vec8f blend_c_odd(Vec8f a, Vec8f b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r;
    r.v = _mm256_blend_ps(a.v, b.v, 0xCC); // floats 2,3,6,7 from b
    return r;
#else
    alignas(32) f32 x[8];
    alignas(32) f32 y[8];
    a.store(x);
    b.store(y);
    return Vec8f(x[0], x[1], y[2], y[3], x[4], x[5], y[6], y[7]);
#endif
}

// [lo.c0 hi.c0 lo.c1 hi.c1] — complex-interleave of two 2-complex (128-bit) loads: the twin-unit
// gather builder (units v, v+1 share a vector; roles a/b at l0 / l0+ms).
CRD_FORCEINLINE Vec8f load_c_quad(const f32* lo, const f32* hi) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec8f r;
    const __m256d both = _mm256_castps_pd(
        _mm256_set_m128(_mm_loadu_ps(hi), _mm_loadu_ps(lo))); // [lo.c0 lo.c1 | hi.c0 hi.c1]
    r.v = _mm256_castpd_ps(_mm256_permute4x64_pd(both, 0xD8)); // (0,2,1,3)
    return r;
#else
    return Vec8f(lo[0], lo[1], hi[0], hi[1], lo[2], lo[3], hi[2], hi[3]);
#endif
}

// 128-bit half stores (the twin-unit scatter: low half → unit v, high half → unit v+1).
CRD_FORCEINLINE void store_c_lo(f32* p, Vec8f a) noexcept
{
#if CRD_SIMD_HAS_AVX2
    _mm_storeu_ps(p, _mm256_castps256_ps128(a.v));
#else
    alignas(32) f32 x[8];
    a.store(x);
    p[0] = x[0];
    p[1] = x[1];
    p[2] = x[2];
    p[3] = x[3];
#endif
}
CRD_FORCEINLINE void store_c_hi(f32* p, Vec8f a) noexcept
{
#if CRD_SIMD_HAS_AVX2
    _mm_storeu_ps(p, _mm256_extractf128_ps(a.v, 1));
#else
    alignas(32) f32 x[8];
    a.store(x);
    p[0] = x[4];
    p[1] = x[5];
    p[2] = x[6];
    p[3] = x[7];
#endif
}

// In-register 8x8 transpose of f32 (rows r0..r7 ↔ columns) — the Vec8f mirror of the Vec4d transpose4x4.
// AVX2 unpack+shuffle+permute (24 ops, no memory). Pure shuffles ⇒ bit-exact vs the scalar path.
CRD_FORCEINLINE void transpose8x8(Vec8f& r0, Vec8f& r1, Vec8f& r2, Vec8f& r3, Vec8f& r4, Vec8f& r5, Vec8f& r6,
                                  Vec8f& r7) noexcept
{
#if CRD_SIMD_HAS_AVX2
    const __m256 t0 = _mm256_unpacklo_ps(r0.v, r1.v); // a0 b0 a1 b1 | a4 b4 a5 b5
    const __m256 t1 = _mm256_unpackhi_ps(r0.v, r1.v); // a2 b2 a3 b3 | a6 b6 a7 b7
    const __m256 t2 = _mm256_unpacklo_ps(r2.v, r3.v);
    const __m256 t3 = _mm256_unpackhi_ps(r2.v, r3.v);
    const __m256 t4 = _mm256_unpacklo_ps(r4.v, r5.v);
    const __m256 t5 = _mm256_unpackhi_ps(r4.v, r5.v);
    const __m256 t6 = _mm256_unpacklo_ps(r6.v, r7.v);
    const __m256 t7 = _mm256_unpackhi_ps(r6.v, r7.v);
    const __m256 u0 = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(1, 0, 1, 0)); // a0 b0 c0 d0 | a4 b4 c4 d4
    const __m256 u1 = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(3, 2, 3, 2)); // a1 b1 c1 d1 | a5 b5 c5 d5
    const __m256 u2 = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(1, 0, 1, 0));
    const __m256 u3 = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(3, 2, 3, 2));
    const __m256 u4 = _mm256_shuffle_ps(t4, t6, _MM_SHUFFLE(1, 0, 1, 0));
    const __m256 u5 = _mm256_shuffle_ps(t4, t6, _MM_SHUFFLE(3, 2, 3, 2));
    const __m256 u6 = _mm256_shuffle_ps(t5, t7, _MM_SHUFFLE(1, 0, 1, 0));
    const __m256 u7 = _mm256_shuffle_ps(t5, t7, _MM_SHUFFLE(3, 2, 3, 2));
    r0.v = _mm256_permute2f128_ps(u0, u4, 0x20);
    r1.v = _mm256_permute2f128_ps(u1, u5, 0x20);
    r2.v = _mm256_permute2f128_ps(u2, u6, 0x20);
    r3.v = _mm256_permute2f128_ps(u3, u7, 0x20);
    r4.v = _mm256_permute2f128_ps(u0, u4, 0x31);
    r5.v = _mm256_permute2f128_ps(u1, u5, 0x31);
    r6.v = _mm256_permute2f128_ps(u2, u6, 0x31);
    r7.v = _mm256_permute2f128_ps(u3, u7, 0x31);
#else
    Vec8f* rows[8] = {&r0, &r1, &r2, &r3, &r4, &r5, &r6, &r7};
    f32 m[8][8];
    for (usize i = 0; i < 8; ++i)
    {
        for (usize j = 0; j < 8; ++j)
        {
            m[j][i] = rows[i]->lane(j);
        }
    }
    for (usize i = 0; i < 8; ++i)
    {
        *rows[i] = Vec8f(m[i][0], m[i][1], m[i][2], m[i][3], m[i][4], m[i][5], m[i][6], m[i][7]);
    }
#endif
}

} // namespace crd::math::simd
