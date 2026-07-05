// Vec4d — 4-lane f64 SIMD wrapper. Phase 3.1.6 v0d-perf-f64-avx2.
//
// AVX2: one 256-bit register (`__m256d`). Scalar fallback (SSE2 / NEON /
// pure scalar backends): 4-element f64 array. Same determinism contract as
// Vec4f / Vec8f (no hardware FMA in `mul_add`; pairwise reduction; hardware
// `sqrt` for `sqrt()` per ADR-0063).
//
// Used by `crd-hesap-dense` BLAS L3 GEMM f64 microkernel (v0d-perf
// follow-on) and reserved for future f64 SIMD consumers (spMV, FFT, AD).

#pragma once

#include <crd/core/types.hpp>
#include <crd/math/simd/backend.hpp>

#include <cmath>
#include <cstring>

namespace crd::math::simd
{
struct alignas(32) Vec4d
{
#if CRD_SIMD_HAS_AVX2
    using Native = __m256d;
    Native v;
#else
    f64 lanes[4];
#endif

    Vec4d() noexcept = default;

    CRD_FORCEINLINE explicit Vec4d(f64 broadcast) noexcept
    {
#if CRD_SIMD_HAS_AVX2
        v = _mm256_set1_pd(broadcast);
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
#else
        return Vec4d(p[0], p[1], p[2], p[3]);
#endif
    }

    CRD_FORCEINLINE void store(f64* p) const noexcept
    {
#if CRD_SIMD_HAS_AVX2
        _mm256_storeu_pd(p, v);
#else
        p[0] = lanes[0];
        p[1] = lanes[1];
        p[2] = lanes[2];
        p[3] = lanes[3];
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

// ---- arithmetic ------------------------------------------------------------

CRD_FORCEINLINE Vec4d operator+(Vec4d a, Vec4d b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec4d r;
    r.v = _mm256_add_pd(a.v, b.v);
    return r;
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
#else
    return Vec4d(a.lanes[0] / b.lanes[0], a.lanes[1] / b.lanes[1],
                 a.lanes[2] / b.lanes[2], a.lanes[3] / b.lanes[3]);
#endif
}

CRD_FORCEINLINE Vec4d operator-(Vec4d a) noexcept { return Vec4d::zero() - a; }
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
#else
    Vec4d r;
    for (usize i = 0; i < 4; ++i)
    {
        r.lanes[i] = std::fma(a.lanes[i], b.lanes[i], c.lanes[i]);
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
#else
    Vec4d r;
    for (usize i = 0; i < 4; ++i)
    {
        r.lanes[i] = std::fma(-a.lanes[i], b.lanes[i], c.lanes[i]);
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
#else
    Vec4d r;
    for (usize i = 0; i < 4; ++i)
    {
        r.lanes[i] = a.lanes[i] < 0.0 ? -a.lanes[i] : a.lanes[i];
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
#else
    Vec4d r;
    for (usize i = 0; i < 4; ++i)
    {
        r.lanes[i] = std::sqrt(a.lanes[i]);
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
#else
    Vec4d r;
    for (usize i = 0; i < 4; ++i)
    {
        // True mask = all-ones bit pattern as f64 (the canonical SIMD form).
        // Encode by reinterpreting an all-ones u64 as f64 (NaN bit pattern).
        const u64 all_ones = 0xFFFFFFFFFFFFFFFFULL;
        f64 true_mask;
        std::memcpy(&true_mask, &all_ones, sizeof(true_mask));
        r.lanes[i] = (a.lanes[i] < b.lanes[i]) ? true_mask : 0.0;
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
#else
    Vec4d r;
    for (usize i = 0; i < 4; ++i)
    {
        const u64 all_ones = 0xFFFFFFFFFFFFFFFFULL;
        f64 true_mask;
        std::memcpy(&true_mask, &all_ones, sizeof(true_mask));
        r.lanes[i] = (a.lanes[i] <= b.lanes[i]) ? true_mask : 0.0;
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
#else
    Vec4d r;
    for (usize i = 0; i < 4; ++i)
    {
        const u64 all_ones = 0xFFFFFFFFFFFFFFFFULL;
        f64 true_mask;
        std::memcpy(&true_mask, &all_ones, sizeof(true_mask));
        r.lanes[i] = (a.lanes[i] == b.lanes[i]) ? true_mask : 0.0;
    }
    return r;
#endif
}

CRD_FORCEINLINE Vec4d cmp_gt(Vec4d a, Vec4d b) noexcept { return cmp_lt(b, a); }
CRD_FORCEINLINE Vec4d cmp_ge(Vec4d a, Vec4d b) noexcept { return cmp_le(b, a); }

CRD_FORCEINLINE Vec4d select(Vec4d mask, Vec4d true_v, Vec4d false_v) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec4d r;
    r.v = _mm256_or_pd(_mm256_and_pd(mask.v, true_v.v),
                       _mm256_andnot_pd(mask.v, false_v.v));
    return r;
#else
    Vec4d r;
    // Mask lane is either all-1s or all-0s as f64-encoded bits; test by
    // round-tripping through u64.
    for (usize i = 0; i < 4; ++i)
    {
        u64 mask_bits;
        std::memcpy(&mask_bits, &mask.lanes[i], sizeof(mask_bits));
        r.lanes[i] = (mask_bits != 0) ? true_v.lanes[i] : false_v.lanes[i];
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
#else
    Vec4d r;
    for (usize i = 0; i < 4; ++i)
    {
        r.lanes[i] = std::fma(a.lanes[i], b.lanes[i], (i & 1U) ? c.lanes[i] : -c.lanes[i]);
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
#else
    return Vec4d(lo[0], lo[1], hi[0], hi[1]);
#endif
}

// [p[0] p[0] p[1] p[1]] — load two doubles, duplicating each within its 128-bit lane (the
// interleaved-complex twiddle expander: halves twiddle-table bytes vs pre-duplicated storage).
CRD_FORCEINLINE Vec4d load_dup_pairs(const f64* p) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec4d r;
    r.v = _mm256_permute4x64_pd(_mm256_castpd128_pd256(_mm_loadu_pd(p)), 0x50);
    return r;
#else
    return Vec4d(p[0], p[0], p[1], p[1]);
#endif
}

// 128-bit-lane concatenations (pure shuffles, bit-exact): [a.lo|b.lo], [a.hi|b.hi], [a.lo|b.hi].
CRD_FORCEINLINE Vec4d concat_lo(Vec4d a, Vec4d b) noexcept
{
#if CRD_SIMD_HAS_AVX2
    Vec4d r;
    r.v = _mm256_permute2f128_pd(a.v, b.v, 0x20);
    return r;
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

} // namespace crd::math::simd
