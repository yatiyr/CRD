// Vec4d — 4-lane f64 SIMD wrapper. Phase 3.1.6 v0d-perf-f64-avx2.
//
// AVX2: one 256-bit register (`__m256d`). SSE2 and SSE4.2: two 128-bit registers (`lo` holds lanes 0 and 1, `hi`
// lanes 2 and 3). NEON and scalar: a 4-element f64 array. Every backend gives the scalar array's bits: IEEE lane
// arithmetic, hardware `sqrt`, the fixed pairwise horizontal sum, and a single-rounded `fma` (exact emulation on SSE2
// and SSE4.2, fma_exact.hpp; the C library's fma per lane on NEON and scalar). `mul_add` rounds twice on every backend
// (ADR-0063).

#pragma once

#include <crd/core/types.hpp>
#include <crd/math/libm.hpp>
#include <crd/math/simd/backend.hpp>
#include <crd/math/simd/fma_exact.hpp>

namespace crd::math::simd::inline CRD_MATH_LEVEL_NS
{
struct Vec4d
{
#if CRD_SIMD_HAS_AVX2
    using Native = __m256d;
    Native v;
#elif CRD_SIMD_HAS_SSE2
    __m128d lo;
    __m128d hi;
#else
    alignas(16) f64 lanes[4];
#endif

    Vec4d() noexcept = default;

#if CRD_SIMD_HAS_SSE2 && !CRD_SIMD_HAS_AVX2
    CRD_FORCEINLINE Vec4d(__m128d lo_in, __m128d hi_in) noexcept : lo(lo_in), hi(hi_in) {}
#endif

    CRD_FORCEINLINE explicit Vec4d(f64 broadcast) noexcept
    {
#if CRD_SIMD_HAS_AVX2
        v = _mm256_set1_pd(broadcast);
#elif CRD_SIMD_HAS_SSE2
        lo = _mm_set1_pd(broadcast);
        hi = lo;
#else
        lanes[0] = broadcast;
        lanes[1] = broadcast;
        lanes[2] = broadcast;
        lanes[3] = broadcast;
#endif
    }

    CRD_FORCEINLINE Vec4d(f64 e0, f64 e1, f64 e2, f64 e3) noexcept
    {
#if CRD_SIMD_HAS_AVX2
        v = _mm256_set_pd(e3, e2, e1, e0);
#elif CRD_SIMD_HAS_SSE2
        lo = _mm_set_pd(e1, e0);
        hi = _mm_set_pd(e3, e2);
#else
        lanes[0] = e0;
        lanes[1] = e1;
        lanes[2] = e2;
        lanes[3] = e3;
#endif
    }

    [[nodiscard]] CRD_FORCEINLINE static Vec4d zero() noexcept { return Vec4d(0.0); }
    [[nodiscard]] CRD_FORCEINLINE static Vec4d one() noexcept { return Vec4d(1.0); }

    [[nodiscard]] CRD_FORCEINLINE static Vec4d load(const f64* p) noexcept
    {
#if CRD_SIMD_HAS_AVX2
        Vec4d r;
        r.v = _mm256_loadu_pd(p);
        return r;
#elif CRD_SIMD_HAS_SSE2
        return Vec4d(_mm_loadu_pd(p), _mm_loadu_pd(p + 2));
#else
        return Vec4d(p[0], p[1], p[2], p[3]);
#endif
    }

    [[nodiscard]] CRD_FORCEINLINE static Vec4d load_aligned(const f64* p) noexcept
    {
#if CRD_SIMD_HAS_AVX2
        Vec4d r;
        r.v = _mm256_load_pd(p);
        return r;
#elif CRD_SIMD_HAS_SSE2
        return Vec4d(_mm_load_pd(p), _mm_load_pd(p + 2));
#else
        return Vec4d(p[0], p[1], p[2], p[3]);
#endif
    }

    CRD_FORCEINLINE void store(f64* p) const noexcept
    {
#if CRD_SIMD_HAS_AVX2
        _mm256_storeu_pd(p, v);
#elif CRD_SIMD_HAS_SSE2
        _mm_storeu_pd(p, lo);
        _mm_storeu_pd(p + 2, hi);
#else
        p[0] = lanes[0];
        p[1] = lanes[1];
        p[2] = lanes[2];
        p[3] = lanes[3];
#endif
    }

    // [p[0] p[0] p[1] p[1]] — load two doubles, duplicating each within its 128-bit lane (the
    // interleaved-complex twiddle expander: halves twiddle-table bytes vs pre-duplicated storage).
    [[nodiscard]] CRD_FORCEINLINE static Vec4d load_dup_pairs(const f64* p) noexcept
    {
#if CRD_SIMD_HAS_AVX2
        Vec4d r;
        r.v = _mm256_permute4x64_pd(_mm256_castpd128_pd256(_mm_loadu_pd(p)), 0x50);
        return r;
#elif CRD_SIMD_HAS_SSE2
        return Vec4d(_mm_set1_pd(p[0]), _mm_set1_pd(p[1]));
#else
        return Vec4d(p[0], p[0], p[1], p[1]);
#endif
    }

    // Masked partial load/store of the FIRST `count` lanes (1..3; count==4 is
    // `load`/`store`). Unloaded lanes are ZERO; unstored lanes untouched. The
    // vector-tail primitive for kernels whose column count is not a multiple
    // of the width (v14-h batched tiny-GEMM tails — scalar tails lose the
    // single-rounded-fma bit contract's throughput, masked lanes keep it).
    [[nodiscard]] CRD_FORCEINLINE static Vec4d load_partial(const f64* p, usize count) noexcept
    {
#if CRD_SIMD_HAS_AVX2
        alignas(32) static constexpr i64 kMaskLut[8] = {-1, -1, -1, -1, 0, 0, 0, 0};
        const __m256i m = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(kMaskLut + (4U - count)));
        Vec4d r;
        r.v = _mm256_maskload_pd(p, m);
        return r;
#elif CRD_SIMD_HAS_SSE2
        // Reads only the first count lanes, as the mask load does.
        if (count == 1U)
        {
            return Vec4d(_mm_load_sd(p), _mm_setzero_pd());
        }
        if (count == 2U)
        {
            return Vec4d(_mm_loadu_pd(p), _mm_setzero_pd());
        }
        return Vec4d(_mm_loadu_pd(p), _mm_load_sd(p + 2));
#else
        Vec4d r(0.0, 0.0, 0.0, 0.0);
        for (usize i = 0; i < count; ++i)
        {
            r.lanes[i] = p[i];
        }
        return r;
#endif
    }

    CRD_FORCEINLINE void store_partial(f64* p, usize count) const noexcept
    {
#if CRD_SIMD_HAS_AVX2
        alignas(32) static constexpr i64 kMaskLut[8] = {-1, -1, -1, -1, 0, 0, 0, 0};
        const __m256i m = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(kMaskLut + (4U - count)));
        _mm256_maskstore_pd(p, m, v);
#elif CRD_SIMD_HAS_SSE2
        if (count == 1U)
        {
            _mm_store_sd(p, lo);
        }
        else
        {
            _mm_storeu_pd(p, lo);
            if (count == 3U)
            {
                _mm_store_sd(p + 2, hi);
            }
        }
#else
        for (usize i = 0; i < count; ++i)
        {
            p[i] = lanes[i];
        }
#endif
    }

    CRD_FORCEINLINE void store_aligned(f64* p) const noexcept
    {
#if CRD_SIMD_HAS_AVX2
        _mm256_store_pd(p, v);
#elif CRD_SIMD_HAS_SSE2
        _mm_store_pd(p, lo);
        _mm_store_pd(p + 2, hi);
#else
        p[0] = lanes[0];
        p[1] = lanes[1];
        p[2] = lanes[2];
        p[3] = lanes[3];
#endif
    }

    [[nodiscard]] CRD_FORCEINLINE f64 lane(usize i) const noexcept
    {
        f64 tmp[4];
        store(tmp);
        return tmp[i];
    }
};
static_assert(sizeof(Vec4d) == 32 && alignof(Vec4d) == simd_alignment(32),
              "Vec4d is 256 bits, aligned no higher than the level's widest register");

// ---- arithmetic ------------------------------------------------------------

CRD_FORCEINLINE Vec4d operator+(Vec4d a, Vec4d b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec4d r;
    r.v = _mm256_add_pd(a.v, b.v);
    return r;
#elif CRD_SIMD_HAS_SSE2
    return Vec4d(_mm_add_pd(a.lo, b.lo), _mm_add_pd(a.hi, b.hi));
#else
    return Vec4d(a.lanes[0] + b.lanes[0], a.lanes[1] + b.lanes[1],
                 a.lanes[2] + b.lanes[2], a.lanes[3] + b.lanes[3]);
#endif
}

CRD_FORCEINLINE Vec4d operator-(Vec4d a, Vec4d b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec4d r;
    r.v = _mm256_sub_pd(a.v, b.v);
    return r;
#elif CRD_SIMD_HAS_SSE2
    return Vec4d(_mm_sub_pd(a.lo, b.lo), _mm_sub_pd(a.hi, b.hi));
#else
    return Vec4d(a.lanes[0] - b.lanes[0], a.lanes[1] - b.lanes[1],
                 a.lanes[2] - b.lanes[2], a.lanes[3] - b.lanes[3]);
#endif
}

CRD_FORCEINLINE Vec4d operator*(Vec4d a, Vec4d b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec4d r;
    r.v = _mm256_mul_pd(a.v, b.v);
    return r;
#elif CRD_SIMD_HAS_SSE2
    return Vec4d(_mm_mul_pd(a.lo, b.lo), _mm_mul_pd(a.hi, b.hi));
#else
    return Vec4d(a.lanes[0] * b.lanes[0], a.lanes[1] * b.lanes[1],
                 a.lanes[2] * b.lanes[2], a.lanes[3] * b.lanes[3]);
#endif
}

CRD_FORCEINLINE Vec4d operator/(Vec4d a, Vec4d b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec4d r;
    r.v = _mm256_div_pd(a.v, b.v);
    return r;
#elif CRD_SIMD_HAS_SSE2
    return Vec4d(_mm_div_pd(a.lo, b.lo), _mm_div_pd(a.hi, b.hi));
#else
    return Vec4d(a.lanes[0] / b.lanes[0], a.lanes[1] / b.lanes[1],
                 a.lanes[2] / b.lanes[2], a.lanes[3] / b.lanes[3]);
#endif
}

// IEEE negation (the sign bit flips), like Vec4f's.
CRD_FORCEINLINE Vec4d operator-(Vec4d a) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec4d r;
    r.v = _mm256_xor_pd(a.v, _mm256_set1_pd(-0.0));
    return r;
#elif CRD_SIMD_HAS_SSE2
    const __m128d sign = _mm_set1_pd(-0.0);
    return Vec4d(_mm_xor_pd(a.lo, sign), _mm_xor_pd(a.hi, sign));
#else
    return Vec4d(-a.lanes[0], -a.lanes[1], -a.lanes[2], -a.lanes[3]);
#endif
}
CRD_FORCEINLINE Vec4d operator*(Vec4d a, f64 s) noexcept { return a * Vec4d(s); }
CRD_FORCEINLINE Vec4d operator*(f64 s, Vec4d a) noexcept { return Vec4d(s) * a; }
CRD_FORCEINLINE Vec4d operator/(Vec4d a, f64 s) noexcept { return a / Vec4d(s); }

// Determinism contract (ADR-0063): NO hardware FMA. Use (a*b)+c with two
// roundings so SIMD/scalar parity holds bit-exactly. Same call as Vec8f.
CRD_FORCEINLINE Vec4d mul_add(Vec4d a, Vec4d b, Vec4d c) noexcept { return (a * b) + c; }
CRD_FORCEINLINE Vec4d mul_sub(Vec4d a, Vec4d b, Vec4d c) noexcept { return (a * b) - c; }

// Single-rounded IEEE 754 FMA (a*b + c with one rounding step). See vec8f.hpp
// for the contract; same rules apply (use ALL fma or ALL mul_add per call site).
CRD_FORCEINLINE Vec4d fma(Vec4d a, Vec4d b, Vec4d c) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec4d r;
    r.v = _mm256_fmadd_pd(a.v, b.v, c.v);
    return r;
#elif CRD_SIMD_HAS_SSE2
    return Vec4d(detail::fma_pd(a.lo, b.lo, c.lo), detail::fma_pd(a.hi, b.hi, c.hi));
#else
    Vec4d r;
    for (usize i = 0; i < 4; ++i)
    {
        r.lanes[i] = libm::fma(a.lanes[i], b.lanes[i], c.lanes[i]);
    }
    return r;
#endif
}

// Single-rounded negated FMA: c - a*b (one rounding). The complex-twiddle workhorse
// (re = wr*ar - wi*ai). Same all-fma-or-all-mul_add contract per call site.
CRD_FORCEINLINE Vec4d fnmadd(Vec4d a, Vec4d b, Vec4d c) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec4d r;
    r.v = _mm256_fnmadd_pd(a.v, b.v, c.v);
    return r;
#elif CRD_SIMD_HAS_SSE2
    return fma(-a, b, c); // the negation is exact, so this rounds once like the instruction
#else
    Vec4d r;
    for (usize i = 0; i < 4; ++i)
    {
        r.lanes[i] = libm::fma(-a.lanes[i], b.lanes[i], c.lanes[i]);
    }
    return r;
#endif
}

// ---- min / max / abs / sqrt -----------------------------------------------

CRD_FORCEINLINE Vec4d min(Vec4d a, Vec4d b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec4d r;
    r.v = _mm256_min_pd(a.v, b.v);
    return r;
#elif CRD_SIMD_HAS_SSE2
    return Vec4d(_mm_min_pd(a.lo, b.lo), _mm_min_pd(a.hi, b.hi));
#else
    Vec4d r;
    for (usize i = 0; i < 4; ++i)
    {
        r.lanes[i] = a.lanes[i] < b.lanes[i] ? a.lanes[i] : b.lanes[i];
    }
    return r;
#endif
}

CRD_FORCEINLINE Vec4d max(Vec4d a, Vec4d b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec4d r;
    r.v = _mm256_max_pd(a.v, b.v);
    return r;
#elif CRD_SIMD_HAS_SSE2
    return Vec4d(_mm_max_pd(a.lo, b.lo), _mm_max_pd(a.hi, b.hi));
#else
    Vec4d r;
    for (usize i = 0; i < 4; ++i)
    {
        r.lanes[i] = a.lanes[i] > b.lanes[i] ? a.lanes[i] : b.lanes[i];
    }
    return r;
#endif
}

CRD_FORCEINLINE Vec4d abs(Vec4d a) noexcept
{
#if CRD_SIMD_HAS_AVX2
    // Clear the sign bit via AND with 0x7FFFFFFFFFFFFFFF (f64 mask).
    const __m256d sign_mask = _mm256_castsi256_pd(_mm256_set1_epi64x(0x7FFFFFFFFFFFFFFFLL));
    Vec4d r;
    r.v = _mm256_and_pd(a.v, sign_mask);
    return r;
#elif CRD_SIMD_HAS_SSE2
    const __m128d sign_mask = _mm_castsi128_pd(_mm_set1_epi64x(0x7FFFFFFFFFFFFFFFLL));
    return Vec4d(_mm_and_pd(a.lo, sign_mask), _mm_and_pd(a.hi, sign_mask));
#else
    Vec4d r;
    for (usize i = 0; i < 4; ++i)
    {
        // Clear the sign bit, as the AVX2 backend does (NaN and -0.0 included).
        r.lanes[i] = __builtin_bit_cast(f64, __builtin_bit_cast(u64, a.lanes[i]) & 0x7FFFFFFFFFFFFFFFULL);
    }
    return r;
#endif
}

CRD_FORCEINLINE Vec4d clamp(Vec4d a, Vec4d lo, Vec4d hi) noexcept { return min(max(a, lo), hi); }

CRD_FORCEINLINE Vec4d sqrt(Vec4d a) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec4d r;
    r.v = _mm256_sqrt_pd(a.v);
    return r;
#elif CRD_SIMD_HAS_SSE2
    return Vec4d(_mm_sqrt_pd(a.lo), _mm_sqrt_pd(a.hi));
#else
    Vec4d r;
    for (usize i = 0; i < 4; ++i)
    {
        r.lanes[i] = libm::sqrt(a.lanes[i]);
    }
    return r;
#endif
}

// ---- reductions (deterministic pairwise order) ----------------------------

CRD_FORCEINLINE f64 horizontal_sum(Vec4d a) noexcept
{
    f64 l[4];
    a.store(l);
    const f64 s01 = l[0] + l[1];
    const f64 s23 = l[2] + l[3];
    return s01 + s23;
}

CRD_FORCEINLINE f64 dot(Vec4d a, Vec4d b) noexcept { return horizontal_sum(a * b); }

// ---- comparisons (mask-producing) -----------------------------------------

CRD_FORCEINLINE Vec4d cmp_lt(Vec4d a, Vec4d b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec4d r;
    r.v = _mm256_cmp_pd(a.v, b.v, _CMP_LT_OQ);
    return r;
#elif CRD_SIMD_HAS_SSE2
    return Vec4d(_mm_cmplt_pd(a.lo, b.lo), _mm_cmplt_pd(a.hi, b.hi));
#else
    Vec4d r;
    for (usize i = 0; i < 4; ++i)
    {
        // True mask = all-ones bit pattern as f64 (the canonical SIMD form, a NaN pattern).
        r.lanes[i] = (a.lanes[i] < b.lanes[i]) ? __builtin_bit_cast(f64, ~u64{0}) : 0.0;
    }
    return r;
#endif
}

CRD_FORCEINLINE Vec4d cmp_le(Vec4d a, Vec4d b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec4d r;
    r.v = _mm256_cmp_pd(a.v, b.v, _CMP_LE_OQ);
    return r;
#elif CRD_SIMD_HAS_SSE2
    return Vec4d(_mm_cmple_pd(a.lo, b.lo), _mm_cmple_pd(a.hi, b.hi));
#else
    Vec4d r;
    for (usize i = 0; i < 4; ++i)
    {
        r.lanes[i] = (a.lanes[i] <= b.lanes[i]) ? __builtin_bit_cast(f64, ~u64{0}) : 0.0;
    }
    return r;
#endif
}

CRD_FORCEINLINE Vec4d cmp_eq(Vec4d a, Vec4d b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec4d r;
    r.v = _mm256_cmp_pd(a.v, b.v, _CMP_EQ_OQ);
    return r;
#elif CRD_SIMD_HAS_SSE2
    return Vec4d(_mm_cmpeq_pd(a.lo, b.lo), _mm_cmpeq_pd(a.hi, b.hi));
#else
    Vec4d r;
    for (usize i = 0; i < 4; ++i)
    {
        r.lanes[i] = (a.lanes[i] == b.lanes[i]) ? __builtin_bit_cast(f64, ~u64{0}) : 0.0;
    }
    return r;
#endif
}

CRD_FORCEINLINE Vec4d cmp_gt(Vec4d a, Vec4d b) noexcept { return cmp_lt(b, a); }
CRD_FORCEINLINE Vec4d cmp_ge(Vec4d a, Vec4d b) noexcept { return cmp_le(b, a); }

// The mask is a comparison result (each lane all-bits-set or zero): the blend reads its sign bits.
CRD_FORCEINLINE Vec4d select(Vec4d mask, Vec4d true_v, Vec4d false_v) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec4d r;
    r.v = _mm256_blendv_pd(false_v.v, true_v.v, mask.v);
    return r;
#elif CRD_SIMD_HAS_SSE2
    return Vec4d(_mm_or_pd(_mm_and_pd(mask.lo, true_v.lo), _mm_andnot_pd(mask.lo, false_v.lo)),
                 _mm_or_pd(_mm_and_pd(mask.hi, true_v.hi), _mm_andnot_pd(mask.hi, false_v.hi)));
#else
    Vec4d r;
    // Mask lane is either all-1s or all-0s as f64-encoded bits; test by
    // round-tripping through u64.
    for (usize i = 0; i < 4; ++i)
    {
        r.lanes[i] = __builtin_bit_cast(u64, mask.lanes[i]) != 0 ? true_v.lanes[i] : false_v.lanes[i];
    }
    return r;
#endif
}

// AoS<->SoA for complex f64: load 4 consecutive complex {r0,i0,r1,i1,r2,i2,r3,i3} from `p` (8 contiguous
// f64) into split re={r0,r1,r2,r3}, im={i0,i1,i2,i3}; and the inverse store. Pure data shuffles (no
// arithmetic) ⇒ bit-identical to the scalar fallback on every backend (no determinism caveat, ADR-0063).
// Lets SoA-kernel consumers (FFT first/last pass) read/write interleaved complex without a separate pass.
CRD_FORCEINLINE void load_complex_deinterleaved(const f64* p, Vec4d& re, Vec4d& im) noexcept
{
#if CRD_SIMD_HAS_AVX2
    const __m256d lo = _mm256_loadu_pd(p);         // r0 i0 r1 i1
    const __m256d hi = _mm256_loadu_pd(p + 4);     // r2 i2 r3 i3
    const __m256d ev = _mm256_unpacklo_pd(lo, hi); // r0 r2 r1 r3
    const __m256d od = _mm256_unpackhi_pd(lo, hi); // i0 i2 i1 i3
    re.v = _mm256_permute4x64_pd(ev, 0xD8);        // r0 r1 r2 r3
    im.v = _mm256_permute4x64_pd(od, 0xD8);        // i0 i1 i2 i3
#elif CRD_SIMD_HAS_SSE2
    const __m128d z0 = _mm_loadu_pd(p);     // r0 i0
    const __m128d z1 = _mm_loadu_pd(p + 2); // r1 i1
    const __m128d z2 = _mm_loadu_pd(p + 4); // r2 i2
    const __m128d z3 = _mm_loadu_pd(p + 6); // r3 i3
    re = Vec4d(_mm_unpacklo_pd(z0, z1), _mm_unpacklo_pd(z2, z3));
    im = Vec4d(_mm_unpackhi_pd(z0, z1), _mm_unpackhi_pd(z2, z3));
#else
    re = Vec4d(p[0], p[2], p[4], p[6]);
    im = Vec4d(p[1], p[3], p[5], p[7]);
#endif
}

CRD_FORCEINLINE void store_complex_interleaved(f64* p, Vec4d re, Vec4d im) noexcept
{
#if CRD_SIMD_HAS_AVX2
    const __m256d ul = _mm256_unpacklo_pd(re.v, im.v); // r0 i0 r2 i2
    const __m256d uh = _mm256_unpackhi_pd(re.v, im.v); // r1 i1 r3 i3
    _mm256_storeu_pd(p, _mm256_permute2f128_pd(ul, uh, 0x20));     // r0 i0 r1 i1
    _mm256_storeu_pd(p + 4, _mm256_permute2f128_pd(ul, uh, 0x31)); // r2 i2 r3 i3
#elif CRD_SIMD_HAS_SSE2
    _mm_storeu_pd(p, _mm_unpacklo_pd(re.lo, im.lo));     // r0 i0
    _mm_storeu_pd(p + 2, _mm_unpackhi_pd(re.lo, im.lo)); // r1 i1
    _mm_storeu_pd(p + 4, _mm_unpacklo_pd(re.hi, im.hi)); // r2 i2
    _mm_storeu_pd(p + 6, _mm_unpackhi_pd(re.hi, im.hi)); // r3 i3
#else
    p[0] = re.lanes[0];
    p[1] = im.lanes[0];
    p[2] = re.lanes[1];
    p[3] = im.lanes[1];
    p[4] = re.lanes[2];
    p[5] = im.lanes[2];
    p[6] = re.lanes[3];
    p[7] = im.lanes[3];
#endif
}

// ---- interleaved-complex idiom primitives (the FFT ip4-AoS engine, 2026-07-04) -------------------
// A Vec4d as TWO interleaved complex f64 [z0.re z0.im z1.re z1.im]. All three are exact per lane
// (shuffle / IEEE add+sub / single-rounded fused multiply-add) ⇒ deterministic like fma()/sqrt.

// [a1 a0 a3 a2] — swap re/im within each complex pair. Pure shuffle, bit-exact vs scalar.
CRD_FORCEINLINE Vec4d swap_pairs(Vec4d a) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec4d r;
    r.v = _mm256_permute_pd(a.v, 0x5);
    return r;
#elif CRD_SIMD_HAS_SSE2
    return Vec4d(_mm_shuffle_pd(a.lo, a.lo, 1), _mm_shuffle_pd(a.hi, a.hi, 1));
#else
    return Vec4d(a.lanes[1], a.lanes[0], a.lanes[3], a.lanes[2]);
#endif
}

// [a0-b0, a1+b1, a2-b2, a3+b3] — subtract on even lanes, add on odd (the complex-mul combiner).
CRD_FORCEINLINE Vec4d addsub(Vec4d a, Vec4d b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec4d r;
    r.v = _mm256_addsub_pd(a.v, b.v);
    return r;
#elif CRD_SIMD_HAS_SSE2
    // SSE2 has no addsub (SSE3): lane 0 of each half from the difference, lane 1 from the sum.
    return Vec4d(_mm_move_sd(_mm_add_pd(a.lo, b.lo), _mm_sub_pd(a.lo, b.lo)),
                 _mm_move_sd(_mm_add_pd(a.hi, b.hi), _mm_sub_pd(a.hi, b.hi)));
#else
    return Vec4d(a.lanes[0] - b.lanes[0], a.lanes[1] + b.lanes[1], a.lanes[2] - b.lanes[2],
                 a.lanes[3] + b.lanes[3]);
#endif
}

// [a0*b0-c0, a1*b1+c1, a2*b2-c2, a3*b3+c3] — fused single-rounded multiply-addsub.
CRD_FORCEINLINE Vec4d fmaddsub(Vec4d a, Vec4d b, Vec4d c) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec4d r;
    r.v = _mm256_fmaddsub_pd(a.v, b.v, c.v);
    return r;
#elif CRD_SIMD_HAS_SSE2
    // c negated (exactly) on the even lanes.
    const __m128d even = _mm_set_pd(0.0, -0.0);
    return Vec4d(detail::fma_pd(a.lo, b.lo, _mm_xor_pd(c.lo, even)),
                 detail::fma_pd(a.hi, b.hi, _mm_xor_pd(c.hi, even)));
#else
    Vec4d r;
    for (usize i = 0; i < 4; ++i)
    {
        r.lanes[i] = libm::fma(a.lanes[i], b.lanes[i], (i & 1U) != 0U ? c.lanes[i] : -c.lanes[i]);
    }
    return r;
#endif
}

// [lo[0] lo[1] hi[0] hi[1]] — two 128-bit loads concatenated (one interleaved complex per lane;
// the gather's pair-builder: 3 µops vs 4-5 for scalar lane assembly).
CRD_FORCEINLINE Vec4d load_pair128(const f64* lo, const f64* hi) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec4d r;
    r.v = _mm256_insertf128_pd(_mm256_castpd128_pd256(_mm_loadu_pd(lo)), _mm_loadu_pd(hi), 1);
    return r;
#elif CRD_SIMD_HAS_SSE2
    return Vec4d(_mm_loadu_pd(lo), _mm_loadu_pd(hi));
#else
    return Vec4d(lo[0], lo[1], hi[0], hi[1]);
#endif
}

// 128-bit-lane concatenations (pure shuffles, bit-exact): [a.lo|b.lo], [a.hi|b.hi], [a.lo|b.hi].
CRD_FORCEINLINE Vec4d concat_lo(Vec4d a, Vec4d b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec4d r;
    r.v = _mm256_permute2f128_pd(a.v, b.v, 0x20);
    return r;
#elif CRD_SIMD_HAS_SSE2
    return Vec4d(a.lo, b.lo);
#else
    return Vec4d(a.lanes[0], a.lanes[1], b.lanes[0], b.lanes[1]);
#endif
}
CRD_FORCEINLINE Vec4d concat_hi(Vec4d a, Vec4d b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec4d r;
    r.v = _mm256_permute2f128_pd(a.v, b.v, 0x31);
    return r;
#elif CRD_SIMD_HAS_SSE2
    return Vec4d(a.hi, b.hi);
#else
    return Vec4d(a.lanes[2], a.lanes[3], b.lanes[2], b.lanes[3]);
#endif
}
CRD_FORCEINLINE Vec4d mix_lo_hi(Vec4d a, Vec4d b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec4d r;
    r.v = _mm256_permute2f128_pd(a.v, b.v, 0x30);
    return r;
#elif CRD_SIMD_HAS_SSE2
    return Vec4d(a.lo, b.hi);
#else
    return Vec4d(a.lanes[0], a.lanes[1], b.lanes[2], b.lanes[3]);
#endif
}

// In-register 4x4 transpose of f64 (rows r0..r3 ↔ columns). AVX2 unpack+permute (8 ops, no memory); the
// building block for a bandwidth-efficient matrix transpose. Pure shuffles ⇒ bit-exact vs the scalar path.
CRD_FORCEINLINE void transpose4x4(Vec4d& r0, Vec4d& r1, Vec4d& r2, Vec4d& r3) noexcept
{
#if CRD_SIMD_HAS_AVX2
    const __m256d t0 = _mm256_unpacklo_pd(r0.v, r1.v);
    const __m256d t1 = _mm256_unpackhi_pd(r0.v, r1.v);
    const __m256d t2 = _mm256_unpacklo_pd(r2.v, r3.v);
    const __m256d t3 = _mm256_unpackhi_pd(r2.v, r3.v);
    r0.v = _mm256_permute2f128_pd(t0, t2, 0x20);
    r1.v = _mm256_permute2f128_pd(t1, t3, 0x20);
    r2.v = _mm256_permute2f128_pd(t0, t2, 0x31);
    r3.v = _mm256_permute2f128_pd(t1, t3, 0x31);
#elif CRD_SIMD_HAS_SSE2
    const Vec4d o0(_mm_unpacklo_pd(r0.lo, r1.lo), _mm_unpacklo_pd(r2.lo, r3.lo));
    const Vec4d o1(_mm_unpackhi_pd(r0.lo, r1.lo), _mm_unpackhi_pd(r2.lo, r3.lo));
    const Vec4d o2(_mm_unpacklo_pd(r0.hi, r1.hi), _mm_unpacklo_pd(r2.hi, r3.hi));
    const Vec4d o3(_mm_unpackhi_pd(r0.hi, r1.hi), _mm_unpackhi_pd(r2.hi, r3.hi));
    r0 = o0;
    r1 = o1;
    r2 = o2;
    r3 = o3;
#else
    const Vec4d o0(r0.lanes[0], r1.lanes[0], r2.lanes[0], r3.lanes[0]);
    const Vec4d o1(r0.lanes[1], r1.lanes[1], r2.lanes[1], r3.lanes[1]);
    const Vec4d o2(r0.lanes[2], r1.lanes[2], r2.lanes[2], r3.lanes[2]);
    const Vec4d o3(r0.lanes[3], r1.lanes[3], r2.lanes[3], r3.lanes[3]);
    r0 = o0;
    r1 = o1;
    r2 = o2;
    r3 = o3;
#endif
}

} // namespace crd::math::simd::inline CRD_MATH_LEVEL_NS
