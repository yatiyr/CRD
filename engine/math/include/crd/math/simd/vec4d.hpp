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

} // namespace crd::math::simd
