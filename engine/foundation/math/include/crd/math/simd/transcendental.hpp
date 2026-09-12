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

#include <crd/core/types.hpp>
#include <crd/math/simd/backend.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#if CRD_SIMD_BACKEND == CRD_SIMD_BACKEND_AVX2
#include <immintrin.h>
#endif

namespace crd::math
{
// ---- natural log ----
inline constexpr double kLn2Hi = 0.693359375; // exact (355/512); Cephes ln2 split
inline constexpr double kLn2Lo = -2.121944400546905827679e-4;
inline constexpr double kSqrt2 = 1.41421356237309504880168872420969808;
inline constexpr double kMinNormal = 2.2250738585072014e-308;
inline constexpr double kC3 = 1.0 / 3.0, kC5 = 1.0 / 5.0, kC7 = 1.0 / 7.0, kC9 = 1.0 / 9.0;
inline constexpr double kC11 = 1.0 / 11.0, kC13 = 1.0 / 13.0, kC15 = 1.0 / 15.0, kC17 = 1.0 / 17.0;

// Shared log reduction: x normal>0 → returns logm = log(mantissa m∈[√½,√2)), sets ed = the EXACT base-2 exponent.
// log(x)=ed·ln2+logm; log2(x)=ed+logm·log2e; log10(x)=ed·log10 2+logm·log10e — ed carries zero error, so the
// derived log2/log10 stay ≤1 ulp (a single rounded multiply of the natural log left them at ~2 ulp).
[[nodiscard]] inline double crd_log_reduce(double x, double& ed) noexcept
{
    std::uint64_t bits;
    std::memcpy(&bits, &x, sizeof(bits));
    int e = static_cast<int>((bits >> 52) & 0x7FFULL) - 1023;
    const std::uint64_t mbits = (bits & 0x000FFFFFFFFFFFFFULL) | 0x3FF0000000000000ULL;
    double m;
    std::memcpy(&m, &mbits, sizeof(m)); // m ∈ [1,2)
    if (m > kSqrt2)
    {
        m *= 0.5;
        e += 1;
    }
    ed = static_cast<double>(e);
    const double t = (m - 1.0) / (m + 1.0);
    const double s = t * t;
    double q = kC17;
    q = std::fma(q, s, kC15);
    q = std::fma(q, s, kC13);
    q = std::fma(q, s, kC11);
    q = std::fma(q, s, kC9);
    q = std::fma(q, s, kC7);
    q = std::fma(q, s, kC5);
    q = std::fma(q, s, kC3);
    const double tt = t + t;             // 2t
    return std::fma(tt * s, q, tt);      // logm = 2t·(1 + s·q)
}

// Scalar f64 log — the canonical bit pattern the SIMD twin reproduces.
[[nodiscard]] inline double crd_log1(double x) noexcept
{
    if (!(x > 0.0) || !std::isfinite(x) || x < kMinNormal)
    {
        return std::log(x); // x≤0 / NaN / +Inf / subnormal → exact library result
    }
    double ed = 0.0;
    const double logm = crd_log_reduce(x, ed);
    double r = std::fma(ed, kLn2Hi, logm);
    r = std::fma(ed, kLn2Lo, r);
    return r;
}

// ---- exp ----
inline constexpr double kLog2e = 1.44269504088896340735992468100189214; // 1/ln2
inline constexpr double kLn2HiE = 6.93145751953125e-1;                  // ln2 high (2^-12 grid, exact)
inline constexpr double kLn2LoE = 1.42860682030941723212e-6;           // ln2 low (ln2 = hi + lo)
inline constexpr double kExpOverflow = 709.782712893383996732;
inline constexpr double kExpUnderflow = -745.133219101941108420;
// exp(r) ≈ 1 + r + Σ_{n=2}^{12} kE_n·r^n on |r| ≤ ln2/2 — MINIMAX (gen_exp_coeffs.py, Lawson-reweighted) ⇒ ≤1 ulp,
// vs the old degree-11 Taylor's ~30 ulp. c0=c1=1 fixed (exp(0)=exp'(0)=1 exact). Determinism: same coeffs scalar↔SIMD.
inline constexpr double kE2 = 5.00000000000000555e-01, kE3 = 1.66666666666665603e-01;
inline constexpr double kE4 = 4.16666666665814339e-02, kE5 = 8.33333333346961136e-03;
inline constexpr double kE6 = 1.38888889278951204e-03, kE7 = 1.98412694900482666e-04;
inline constexpr double kE8 = 2.48015108506657736e-05, kE9 = 2.75576332857440428e-06;
inline constexpr double kE10 = 2.76235551234085071e-07, kE11 = 2.49817193679824969e-08;

// Scalar f64 exp — the canonical bit pattern.
[[nodiscard]] inline double crd_exp1(double x) noexcept
{
    if (std::isnan(x))
    {
        return x;
    }
    if (x > kExpOverflow)
    {
        return std::numeric_limits<double>::infinity();
    }
    if (x < kExpUnderflow)
    {
        return 0.0;
    }
    const double kf = std::floor(std::fma(x, kLog2e, 0.5));             // k = round(x·log2e)
    const double r = std::fma(kf, -kLn2LoE, std::fma(kf, -kLn2HiE, x)); // r = x − k·ln2 (Cody-Waite)
    double p = kE11;                                                    // exp(r) minimax, Horner from the top
    p = std::fma(p, r, kE10);
    p = std::fma(p, r, kE9);
    p = std::fma(p, r, kE8);
    p = std::fma(p, r, kE7);
    p = std::fma(p, r, kE6);
    p = std::fma(p, r, kE5);
    p = std::fma(p, r, kE4);
    p = std::fma(p, r, kE3);
    p = std::fma(p, r, kE2);
    p = std::fma(p, r, 1.0); // + r (c1)
    p = std::fma(p, r, 1.0); // + 1 (c0)
    const int ki = static_cast<int>(kf);
    if (ki >= -1022 && ki <= 1023) // common path: single exponent injection — exact + fast (the normal-result range)
    {
        const std::uint64_t bits = static_cast<std::uint64_t>(static_cast<long long>(ki) + 1023) << 52;
        double two_k;
        std::memcpy(&two_k, &bits, sizeof(two_k));
        return p * two_k;
    }
    // boundary (subnormal k≲−1022 / near-overflow k≈1024): TWO half-steps so neither exponent field overflows — gives
    // correct subnormals + the right finite near max-double, where a single injection produced garbage. Power-of-2
    // multiplies ⇒ bit-identical to the single-injection path on the overlap, so the classifier need not match SIMD's.
    const int k1 = ki >> 1; // floor(k/2), C++20 arithmetic shift
    const int k2 = ki - k1;
    const std::uint64_t b1 = static_cast<std::uint64_t>(static_cast<long long>(k1) + 1023) << 52;
    const std::uint64_t b2 = static_cast<std::uint64_t>(static_cast<long long>(k2) + 1023) << 52;
    double s1;
    double s2;
    std::memcpy(&s1, &b1, sizeof(s1));
    std::memcpy(&s2, &b2, sizeof(s2));
    return (p * s1) * s2;
}

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
