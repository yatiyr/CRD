#pragma once

// crd-math — f64 SIMD transcendentals (natural log + exp): AVX-512 eight-lane, AVX2 four-lane and SSE2/SSE4.2
// two-lane twins of the scalar cores (crd/math/exp_log_core.hpp), BIT-IDENTICAL to them; the two-lane twins round each
// FMA once through crd/math/simd/fma_exact.hpp.
// The reusable home for these primitives (the f32 log/exp live in crd/math/deterministic.hpp; Vec4d in simd/vec4d.hpp;
// these fill the f64 gap). Consumers: FFT, AD, and the hesap-special distribution/special-function hot paths.
//
//   log: atanh range reduction x=m·2^e, m∈[√½,√2) ⇒ |t|≤0.172, log(m)=2·atanh(t) with t's rounding carried (the
//        pair of crd_log_reduce); ln2 split hi/lo, the head summed exactly. Accuracy: log ≤0.52 ulp.
//   exp: Cody-Waite x=k·ln2+r (|r|≤ln2/2) + degree-11 minimax exp(r) (gen_minimax_coeffs), ×2^k by a TWO-step exponent
//        injection (one half each) ⇒ correct subnormals (k≲−1022) AND near-overflow (k≈1024). Accuracy: exp ≤1 ulp.
// All combines use the single-rounded FMA in the SAME order; the build pins -ffp-contract=off (crd-simd-flags) ⇒ the
// scalar and SIMD kernels emit IDENTICAL bits. That bit-identity is what lets a parallel batch's SIMD body and its
// scalar tail agree byte-for-byte ⇒ a partition-independent {1,4,16}-thread determinism moat.

#include <crd/math/exp_log_core.hpp> // crd_log1 / crd_exp1, the scalar cores these twins reproduce
#include <crd/math/simd/backend.hpp>
#include <crd/math/simd/fma_exact.hpp>

namespace crd::math::inline CRD_MATH_LEVEL_NS
{
#if CRD_SIMD_HAS_AVX2
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
    const __m256d f = _mm256_sub_pd(m, one);
    const __m256d u = _mm256_add_pd(m, one);
    const __m256d ul = _mm256_sub_pd(m, _mm256_sub_pd(u, one));
    const __m256d t = _mm256_div_pd(f, u);
    const __m256d tl = _mm256_div_pd(_mm256_fnmadd_pd(t, ul, _mm256_fnmadd_pd(t, u, f)), u);
    const __m256d s = _mm256_mul_pd(t, t);
    __m256d q = _mm256_set1_pd(kLogR7);
    q = _mm256_fmadd_pd(q, s, _mm256_set1_pd(kLogR6));
    q = _mm256_fmadd_pd(q, s, _mm256_set1_pd(kLogR5));
    q = _mm256_fmadd_pd(q, s, _mm256_set1_pd(kLogR4));
    q = _mm256_fmadd_pd(q, s, _mm256_set1_pd(kLogR3));
    q = _mm256_fmadd_pd(q, s, _mm256_set1_pd(kLogR2));
    q = _mm256_fmadd_pd(q, s, _mm256_set1_pd(kLogR1));
    q = _mm256_fmadd_pd(q, s, _mm256_set1_pd(kLogR0));
    const __m256d tt = _mm256_add_pd(t, t);
    const __m256d tl2 = _mm256_add_pd(tl, tl);
    const __m256d lo = _mm256_fmadd_pd(_mm256_mul_pd(tt, s), q, _mm256_fmadd_pd(tl2, s, tl2));
    const __m256d a = _mm256_mul_pd(ed, _mm256_set1_pd(kLn2Hi));
    const __m256d h = _mm256_add_pd(a, tt);
    const __m256d he = _mm256_sub_pd(tt, _mm256_sub_pd(h, a));
    __m256d r = _mm256_add_pd(h, _mm256_fmadd_pd(ed, _mm256_set1_pd(kLn2Lo), _mm256_add_pd(he, lo)));
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

#if CRD_SIMD_HAS_AVX512
// The eight-lane twins: crd_log4 and crd_exp4's operations in the same order on a zmm register, the blends as masked
// operations, so each lane gives crd_log1's and crd_exp1's bits; abnormal lanes take the scalar core.
template <typename Core>
[[nodiscard]] inline __m512d crd_scalar_lanes8(__m512d x, __m512d r, __mmask8 lanes, Core core) noexcept
{
    alignas(64) double xa[8];
    alignas(64) double ra[8];
    _mm512_store_pd(xa, x);
    _mm512_store_pd(ra, r);
    for (int i = 0; i < 8; ++i)
    {
        if ((lanes >> i) & 1)
        {
            ra[i] = core(xa[i]);
        }
    }
    return _mm512_load_pd(ra);
}

[[nodiscard]] inline __m512d crd_log8(__m512d x) noexcept
{
    const __m512i bits = _mm512_castpd_si512(x);
    __m512i ei = _mm512_sub_epi64(_mm512_and_si512(_mm512_srli_epi64(bits, 52), _mm512_set1_epi64(0x7FF)),
                                  _mm512_set1_epi64(1023));
    const __m512i mbits = _mm512_or_si512(_mm512_and_si512(bits, _mm512_set1_epi64(0x000FFFFFFFFFFFFF)),
                                          _mm512_set1_epi64(0x3FF0000000000000));
    __m512d m = _mm512_castsi512_pd(mbits);
    const __mmask8 gt = _mm512_cmp_pd_mask(m, _mm512_set1_pd(kSqrt2), _CMP_GT_OQ);
    m = _mm512_mask_mul_pd(m, gt, m, _mm512_set1_pd(0.5));
    ei = _mm512_mask_add_epi64(ei, gt, ei, _mm512_set1_epi64(1));
    const __m512d magic = _mm512_set1_pd(6755399441055744.0); // 0x4338000000000000
    const __m512d ed = _mm512_sub_pd(_mm512_castsi512_pd(_mm512_add_epi64(ei, _mm512_castpd_si512(magic))), magic);
    const __m512d one = _mm512_set1_pd(1.0);
    const __m512d f = _mm512_sub_pd(m, one);
    const __m512d u = _mm512_add_pd(m, one);
    const __m512d ul = _mm512_sub_pd(m, _mm512_sub_pd(u, one));
    const __m512d t = _mm512_div_pd(f, u);
    const __m512d tl = _mm512_div_pd(_mm512_fnmadd_pd(t, ul, _mm512_fnmadd_pd(t, u, f)), u);
    const __m512d s = _mm512_mul_pd(t, t);
    __m512d q = _mm512_set1_pd(kLogR7);
    q = _mm512_fmadd_pd(q, s, _mm512_set1_pd(kLogR6));
    q = _mm512_fmadd_pd(q, s, _mm512_set1_pd(kLogR5));
    q = _mm512_fmadd_pd(q, s, _mm512_set1_pd(kLogR4));
    q = _mm512_fmadd_pd(q, s, _mm512_set1_pd(kLogR3));
    q = _mm512_fmadd_pd(q, s, _mm512_set1_pd(kLogR2));
    q = _mm512_fmadd_pd(q, s, _mm512_set1_pd(kLogR1));
    q = _mm512_fmadd_pd(q, s, _mm512_set1_pd(kLogR0));
    const __m512d tt = _mm512_add_pd(t, t);
    const __m512d tl2 = _mm512_add_pd(tl, tl);
    const __m512d lo = _mm512_fmadd_pd(_mm512_mul_pd(tt, s), q, _mm512_fmadd_pd(tl2, s, tl2));
    const __m512d a = _mm512_mul_pd(ed, _mm512_set1_pd(kLn2Hi));
    const __m512d h = _mm512_add_pd(a, tt);
    const __m512d he = _mm512_sub_pd(tt, _mm512_sub_pd(h, a));
    __m512d r = _mm512_add_pd(h, _mm512_fmadd_pd(ed, _mm512_set1_pd(kLn2Lo), _mm512_add_pd(he, lo)));
    const __mmask8 abn = _mm512_cmp_pd_mask(x, _mm512_setzero_pd(), _CMP_LE_OQ) |
                         _mm512_cmp_pd_mask(x, x, _CMP_UNORD_Q) |
                         _mm512_cmp_pd_mask(x, _mm512_set1_pd(1.7976931348623157e308), _CMP_GT_OQ) |
                         _mm512_cmp_pd_mask(x, _mm512_set1_pd(kMinNormal), _CMP_LT_OQ);
    if (abn != 0)
    {
        r = crd_scalar_lanes8(x, r, abn, [](double v) noexcept { return crd_log1(v); });
    }
    return r;
}

[[nodiscard]] inline __m512d crd_exp8(__m512d x) noexcept
{
    const __m512d kf = _mm512_roundscale_pd(_mm512_fmadd_pd(x, _mm512_set1_pd(kLog2e), _mm512_set1_pd(0.5)),
                                            _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC);
    __m512d r = _mm512_fmadd_pd(kf, _mm512_set1_pd(-kLn2HiE), x);
    r = _mm512_fmadd_pd(kf, _mm512_set1_pd(-kLn2LoE), r);
    __m512d p = _mm512_set1_pd(kE11);
    p = _mm512_fmadd_pd(p, r, _mm512_set1_pd(kE10));
    p = _mm512_fmadd_pd(p, r, _mm512_set1_pd(kE9));
    p = _mm512_fmadd_pd(p, r, _mm512_set1_pd(kE8));
    p = _mm512_fmadd_pd(p, r, _mm512_set1_pd(kE7));
    p = _mm512_fmadd_pd(p, r, _mm512_set1_pd(kE6));
    p = _mm512_fmadd_pd(p, r, _mm512_set1_pd(kE5));
    p = _mm512_fmadd_pd(p, r, _mm512_set1_pd(kE4));
    p = _mm512_fmadd_pd(p, r, _mm512_set1_pd(kE3));
    p = _mm512_fmadd_pd(p, r, _mm512_set1_pd(kE2));
    p = _mm512_fmadd_pd(p, r, _mm512_set1_pd(1.0));
    p = _mm512_fmadd_pd(p, r, _mm512_set1_pd(1.0));
    // The single exponent injection of crd_exp4, valid for |x| up to about 708; the edge band takes crd_exp1.
    const __m512i ki = _mm512_add_epi64(_mm512_cvtepi32_epi64(_mm512_cvtpd_epi32(kf)), _mm512_set1_epi64(1023));
    __m512d res = _mm512_mul_pd(p, _mm512_castsi512_pd(_mm512_slli_epi64(ki, 52)));
    const __mmask8 edge = _mm512_cmp_pd_mask(x, _mm512_set1_pd(709.0), _CMP_GT_OQ) |
                          _mm512_cmp_pd_mask(x, _mm512_set1_pd(-708.0), _CMP_LT_OQ) |
                          _mm512_cmp_pd_mask(x, x, _CMP_UNORD_Q);
    if (edge != 0)
    {
        res = crd_scalar_lanes8(x, res, edge, [](double v) noexcept { return crd_exp1(v); });
    }
    return res;
}
#endif
#elif CRD_SIMD_HAS_SSE2
// SSE2/SSE4.2 log twin — bit-identical to crd_log1 on normal-positive lanes; abnormal lanes fall back to crd_log1.
[[nodiscard]] inline __m128d crd_log_x2(__m128d x) noexcept
{
    using simd::detail::fma_pd;
    const __m128i bits = _mm_castpd_si128(x);
    __m128i ei = _mm_sub_epi64(_mm_and_si128(_mm_srli_epi64(bits, 52), _mm_set1_epi64x(0x7FF)), _mm_set1_epi64x(1023));
    const __m128i mbits =
        _mm_or_si128(_mm_and_si128(bits, _mm_set1_epi64x(0x000FFFFFFFFFFFFF)), _mm_set1_epi64x(0x3FF0000000000000));
    __m128d m = _mm_castsi128_pd(mbits);
    const __m128d gt = _mm_cmpgt_pd(m, _mm_set1_pd(kSqrt2));
    m = _mm_or_pd(_mm_and_pd(gt, _mm_mul_pd(m, _mm_set1_pd(0.5))), _mm_andnot_pd(gt, m));
    ei = _mm_add_epi64(ei, _mm_and_si128(_mm_castpd_si128(gt), _mm_set1_epi64x(1)));
    const __m128d magic = _mm_set1_pd(6755399441055744.0); // 0x4338000000000000
    const __m128d ed = _mm_sub_pd(_mm_castsi128_pd(_mm_add_epi64(ei, _mm_castpd_si128(magic))), magic);
    const __m128d one = _mm_set1_pd(1.0);
    const __m128d f = _mm_sub_pd(m, one);
    const __m128d u = _mm_add_pd(m, one);
    const __m128d ul = _mm_sub_pd(m, _mm_sub_pd(u, one));
    const __m128d t = _mm_div_pd(f, u);
    const __m128d nt = _mm_xor_pd(t, _mm_set1_pd(-0.0)); // −t, exact
    const __m128d tl = _mm_div_pd(fma_pd(nt, ul, fma_pd(nt, u, f)), u);
    const __m128d s = _mm_mul_pd(t, t);
    __m128d q = _mm_set1_pd(kLogR7);
    q = fma_pd(q, s, _mm_set1_pd(kLogR6));
    q = fma_pd(q, s, _mm_set1_pd(kLogR5));
    q = fma_pd(q, s, _mm_set1_pd(kLogR4));
    q = fma_pd(q, s, _mm_set1_pd(kLogR3));
    q = fma_pd(q, s, _mm_set1_pd(kLogR2));
    q = fma_pd(q, s, _mm_set1_pd(kLogR1));
    q = fma_pd(q, s, _mm_set1_pd(kLogR0));
    const __m128d tt = _mm_add_pd(t, t);
    const __m128d tl2 = _mm_add_pd(tl, tl);
    const __m128d lo = fma_pd(_mm_mul_pd(tt, s), q, fma_pd(tl2, s, tl2));
    const __m128d a = _mm_mul_pd(ed, _mm_set1_pd(kLn2Hi));
    const __m128d h = _mm_add_pd(a, tt);
    const __m128d he = _mm_sub_pd(tt, _mm_sub_pd(h, a));
    __m128d r = _mm_add_pd(h, fma_pd(ed, _mm_set1_pd(kLn2Lo), _mm_add_pd(he, lo)));
    const __m128d le0 = _mm_cmple_pd(x, _mm_setzero_pd());
    const __m128d unord = _mm_cmpunord_pd(x, x);
    const __m128d big = _mm_cmpgt_pd(x, _mm_set1_pd(1.7976931348623157e308));
    const __m128d sub = _mm_cmplt_pd(x, _mm_set1_pd(kMinNormal));
    const int mask = _mm_movemask_pd(_mm_or_pd(_mm_or_pd(le0, unord), _mm_or_pd(big, sub)));
    if (mask != 0)
    {
        alignas(16) double xa[2];
        alignas(16) double ra[2];
        _mm_store_pd(xa, x);
        _mm_store_pd(ra, r);
        for (int i = 0; i < 2; ++i)
        {
            if (((mask >> i) & 1) != 0)
            {
                ra[i] = crd_log1(xa[i]);
            }
        }
        r = _mm_load_pd(ra);
    }
    return r;
}

// SSE2/SSE4.2 exp twin — bit-identical to crd_exp1 on the in-range fast path; over/underflow/NaN lanes via crd_exp1.
[[nodiscard]] inline __m128d crd_exp_x2(__m128d x) noexcept
{
    using simd::detail::fma_pd;
    const __m128d y = fma_pd(x, _mm_set1_pd(kLog2e), _mm_set1_pd(0.5));
#if CRD_SIMD_HAS_SSE42
    const __m128d kf = _mm_floor_pd(y);
#else
    // floor by truncation, one less where that rounded up: exact on the in-range lanes (|y| < 1100).
    const __m128d truncated = _mm_cvtepi32_pd(_mm_cvttpd_epi32(y));
    const __m128d kf = _mm_sub_pd(truncated, _mm_and_pd(_mm_cmpgt_pd(truncated, y), _mm_set1_pd(1.0)));
#endif
    __m128d r = fma_pd(kf, _mm_set1_pd(-kLn2HiE), x);
    r = fma_pd(kf, _mm_set1_pd(-kLn2LoE), r);
    __m128d p = _mm_set1_pd(kE11);
    p = fma_pd(p, r, _mm_set1_pd(kE10));
    p = fma_pd(p, r, _mm_set1_pd(kE9));
    p = fma_pd(p, r, _mm_set1_pd(kE8));
    p = fma_pd(p, r, _mm_set1_pd(kE7));
    p = fma_pd(p, r, _mm_set1_pd(kE6));
    p = fma_pd(p, r, _mm_set1_pd(kE5));
    p = fma_pd(p, r, _mm_set1_pd(kE4));
    p = fma_pd(p, r, _mm_set1_pd(kE3));
    p = fma_pd(p, r, _mm_set1_pd(kE2));
    p = fma_pd(p, r, _mm_set1_pd(1.0));
    p = fma_pd(p, r, _mm_set1_pd(1.0));
    // 2^k by one exponent injection, valid for the normal results (|x| <= 708): k + 1023 sits in the low bits of
    // kf + 2^52 + 1023, and the shift drops that sum's exponent field.
    const __m128i biased = _mm_castpd_si128(_mm_add_pd(kf, _mm_set1_pd(4503599627371519.0))); // 2^52 + 1023
    __m128d res = _mm_mul_pd(p, _mm_castsi128_pd(_mm_slli_epi64(biased, 52)));
    const __m128d hi = _mm_cmpgt_pd(x, _mm_set1_pd(709.0));
    const __m128d lo = _mm_cmplt_pd(x, _mm_set1_pd(-708.0));
    const __m128d unord = _mm_cmpunord_pd(x, x);
    const int mask = _mm_movemask_pd(_mm_or_pd(_mm_or_pd(hi, lo), unord));
    if (mask != 0)
    {
        alignas(16) double xa[2];
        alignas(16) double ra[2];
        _mm_store_pd(xa, x);
        _mm_store_pd(ra, res);
        for (int i = 0; i < 2; ++i)
        {
            if (((mask >> i) & 1) != 0)
            {
                ra[i] = crd_exp1(xa[i]);
            }
        }
        res = _mm_load_pd(ra);
    }
    return res;
}
#endif

} // namespace crd::math::inline CRD_MATH_LEVEL_NS
