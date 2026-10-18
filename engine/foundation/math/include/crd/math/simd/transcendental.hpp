#pragma once

// crd-math — f64 SIMD transcendentals (natural log + exp), scalar + AVX2 twins that are BIT-IDENTICAL to each other.
// The reusable home for these primitives (the f32 log/exp live in crd/math/deterministic.hpp; Vec4d in simd/vec4d.hpp;
// these fill the f64 gap). Consumers: FFT, AD, and the hesap-special distribution/special-function hot paths.
//
//   log: atanh range reduction x=m·2^e, m∈[√½,√2) ⇒ |t|≤0.172, log(m)=2·atanh(t); ln2 split hi/lo.
//   exp: Cody-Waite x=k·ln2+r (|r|≤ln2/2) + degree-12 MINIMAX exp(r) (gen_exp_coeffs.py), ×2^k by a TWO-step exponent
//        injection (one half each) ⇒ correct subnormals (k≲−1022) AND near-overflow (k≈1024). Accuracy: exp ≤1 ulp.
// All combines use the single-rounded FMA in the SAME order; the build pins -ffp-contract=off (crd-simd-flags) ⇒ the
// scalar and SIMD kernels emit IDENTICAL bits. That bit-identity is what lets a parallel batch's SIMD body and its
// scalar tail agree byte-for-byte ⇒ a partition-independent {1,4,16}-thread determinism moat. Accuracy: log ≤2 ulp.

#include <crd/math/exp_log_core.hpp> // crd_log1 / crd_exp1, the scalar cores these twins reproduce
#include <crd/math/simd/backend.hpp>

#if CRD_SIMD_BACKEND == CRD_SIMD_BACKEND_AVX2
#include <immintrin.h>
#endif

namespace crd::math
{
#if CRD_SIMD_BACKEND == CRD_SIMD_BACKEND_AVX2
// AVX2/FMA log twin — bit-identical to crd_log1 on normal-positive lanes; abnormal lanes fall back to crd_log1.
[[nodiscard]] inline __m256d crd_log4(__m256d x) noexcept
{
    const __m256i bits = _mm256_castpd_si256(x);
    __m256i ei = _mm256_sub_epi64(_mm256_and_si256(_mm256_srli_epi64(bits, 52), _mm256_set1_epi64x(0x7FF)),
                                  _mm256_set1_epi64x(1023));
    const __m256i mbits = _mm256_or_si256(_mm256_and_si256(bits, _mm256_set1_epi64x(0x000FFFFFFFFFFFFF)),
                                          _mm256_set1_epi64x(0x3FF0000000000000));
    __m256d m = _mm256_castsi256_pd(mbits);
    const __m256d gt = _mm256_cmp_pd(m, _mm256_set1_pd(kSqrt2), _CMP_GT_OQ);
    m = _mm256_blendv_pd(m, _mm256_mul_pd(m, _mm256_set1_pd(0.5)), gt);
    ei = _mm256_add_epi64(ei, _mm256_and_si256(_mm256_castpd_si256(gt), _mm256_set1_epi64x(1)));
    const __m256d magic = _mm256_set1_pd(6755399441055744.0); // 0x4338000000000000
    const __m256d ed = _mm256_sub_pd(_mm256_castsi256_pd(_mm256_add_epi64(ei, _mm256_castpd_si256(magic))), magic);
    const __m256d one = _mm256_set1_pd(1.0);
    const __m256d t = _mm256_div_pd(_mm256_sub_pd(m, one), _mm256_add_pd(m, one));
    const __m256d s = _mm256_mul_pd(t, t);
    __m256d q = _mm256_set1_pd(kC17);
    q = _mm256_fmadd_pd(q, s, _mm256_set1_pd(kC15));
    q = _mm256_fmadd_pd(q, s, _mm256_set1_pd(kC13));
    q = _mm256_fmadd_pd(q, s, _mm256_set1_pd(kC11));
    q = _mm256_fmadd_pd(q, s, _mm256_set1_pd(kC9));
    q = _mm256_fmadd_pd(q, s, _mm256_set1_pd(kC7));
    q = _mm256_fmadd_pd(q, s, _mm256_set1_pd(kC5));
    q = _mm256_fmadd_pd(q, s, _mm256_set1_pd(kC3));
    const __m256d tt = _mm256_add_pd(t, t);
    const __m256d logm = _mm256_fmadd_pd(_mm256_mul_pd(tt, s), q, tt);
    __m256d r = _mm256_fmadd_pd(ed, _mm256_set1_pd(kLn2Hi), logm);
    r = _mm256_fmadd_pd(ed, _mm256_set1_pd(kLn2Lo), r);
    const __m256d le0 = _mm256_cmp_pd(x, _mm256_setzero_pd(), _CMP_LE_OQ);
    const __m256d unord = _mm256_cmp_pd(x, x, _CMP_UNORD_Q);
    const __m256d big = _mm256_cmp_pd(x, _mm256_set1_pd(1.7976931348623157e308), _CMP_GT_OQ);
    const __m256d sub = _mm256_cmp_pd(x, _mm256_set1_pd(kMinNormal), _CMP_LT_OQ);
    const __m256d abn = _mm256_or_pd(_mm256_or_pd(le0, unord), _mm256_or_pd(big, sub));
    if (_mm256_movemask_pd(abn) != 0)
    {
        alignas(32) double xa[4];
        alignas(32) double ra[4];
        _mm256_store_pd(xa, x);
        _mm256_store_pd(ra, r);
        const int mask = _mm256_movemask_pd(abn);
        for (int i = 0; i < 4; ++i)
        {
            if ((mask >> i) & 1)
            {
                ra[i] = crd_log1(xa[i]);
            }
        }
        r = _mm256_load_pd(ra);
    }
    return r;
}

// AVX2/FMA exp twin — bit-identical to crd_exp1 on the in-range fast path; over/underflow/NaN lanes via crd_exp1.
[[nodiscard]] inline __m256d crd_exp4(__m256d x) noexcept
{
    const __m256d kf = _mm256_floor_pd(_mm256_fmadd_pd(x, _mm256_set1_pd(kLog2e), _mm256_set1_pd(0.5)));
    __m256d r = _mm256_fmadd_pd(kf, _mm256_set1_pd(-kLn2HiE), x);
    r = _mm256_fmadd_pd(kf, _mm256_set1_pd(-kLn2LoE), r);
    __m256d p = _mm256_set1_pd(kE11);
    p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(kE10));
    p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(kE9));
    p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(kE8));
    p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(kE7));
    p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(kE6));
    p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(kE5));
    p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(kE4));
    p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(kE3));
    p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(kE2));
    p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(1.0));
    p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(1.0));
    // fast single injection — valid for k∈[−1022,1023] ⇔ |x|≲708 (all normal results); the rare subnormal /
    // near-overflow band (|x|≳708) + over/under/NaN fall back to crd_exp1 (its two-step path), bit-identical here.
    const __m128i klo = _mm256_cvtpd_epi32(kf);
    const __m256i ki = _mm256_add_epi64(_mm256_cvtepi32_epi64(klo), _mm256_set1_epi64x(1023));
    __m256d res = _mm256_mul_pd(p, _mm256_castsi256_pd(_mm256_slli_epi64(ki, 52)));
    const __m256d hi = _mm256_cmp_pd(x, _mm256_set1_pd(709.0), _CMP_GT_OQ);
    const __m256d lo = _mm256_cmp_pd(x, _mm256_set1_pd(-708.0), _CMP_LT_OQ);
    const __m256d unord = _mm256_cmp_pd(x, x, _CMP_UNORD_Q);
    const __m256d edge = _mm256_or_pd(_mm256_or_pd(hi, lo), unord);
    if (_mm256_movemask_pd(edge) != 0)
    {
        alignas(32) double xa[4];
        alignas(32) double ra[4];
        _mm256_store_pd(xa, x);
        _mm256_store_pd(ra, res);
        const int mask = _mm256_movemask_pd(edge);
        for (int i = 0; i < 4; ++i)
        {
            if ((mask >> i) & 1)
            {
                ra[i] = crd_exp1(xa[i]);
            }
        }
        res = _mm256_load_pd(ra);
    }
    return res;
}
#endif // AVX2

} // namespace crd::math
