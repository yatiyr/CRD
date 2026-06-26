#pragma once

// crd-hesap-special — recurrence-free Lanczos lgamma (g=7, n=9), scalar + AVX2 twins, built on the crd_log
// primitive. Unlike the Stirling+product-recurrence form (which can't vectorize — its shift count is per-lane),
// the Lanczos sum is a FIXED 8-term rational ⇒ branch-free ⇒ it vectorizes cleanly and the SIMD log win carries
// through. This is the lgamma the batch uses to win MATLAB-MT (paired with a P-core-sized pool, ADR-0094).
//
//   lgamma(x), x ≥ 0.5:  z=x−1;  a = c0 + Σ_{k=1..8} c_k/(z+k);  t = z + g + 0.5;
//                        lgamma = ½ln(2π) + (z+0.5)·ln(t) − t + ln(a)
//   x < 0.5:  reflection  lgamma(x) = ln(π) − ln|sin(πx)| − lgamma(1−x)   (1−x ≥ 0.5 ⇒ Lanczos branch)
//
// Bit-identical scalar↔SIMD: same accumulation order, same crd_log, -ffp-contract=off ⇒ the batch's SIMD body and
// its scalar tail/reflection fallback agree byte-for-byte ⇒ the {1,4,16}-thread determinism moat holds.
// Coefficients are the canonical g=7 set (accurate ~1e-15 in the right half-plane); gated vs scipy at 1e-13.

#include <crd/math/simd/transcendental.hpp> // crd::math::crd_log{1,4} / crd_exp{1,4} (reusable f64 SIMD primitive)

#include <crd/math/cmath.hpp>

namespace crd::hesap::special::detail
{
using crd::math::crd_exp1;
using crd::math::crd_log1;
#if CRD_SIMD_BACKEND == CRD_SIMD_BACKEND_AVX2
using crd::math::crd_exp4;
using crd::math::crd_log4;
#endif

inline constexpr double kLanczosG = 7.0;
inline constexpr double kLanczosGph = kLanczosG + 0.5; // g+0.5 = 7.5 (one add ⇒ scalar/SIMD bit-identical t)
inline constexpr double kHalfLn2Pi = 0.918938533204672741780329736405617639; // ½·ln(2π)
inline constexpr double kLnPi = 1.144729885849400174143427351353058712;      // ln(π)
inline constexpr double kPiLz = 3.14159265358979323846264338327950288;
inline constexpr double kLanczosC[9] = {0.99999999999980993,     676.5203681218851,      -1259.1392167224028,
                                        771.32342877765313,      -176.61502916214059,    12.507343278686905,
                                        -0.13857109526572012,    9.9843695780195716e-6,  1.5056327351493116e-7};

// Scalar Lanczos lgamma for x ≥ 0.5 (no reflection).
[[nodiscard]] inline double crd_lgamma_lz_half(double x) noexcept
{
    const double z = x - 1.0;
    double a = kLanczosC[0];
    a += kLanczosC[1] / (z + 1.0);
    a += kLanczosC[2] / (z + 2.0);
    a += kLanczosC[3] / (z + 3.0);
    a += kLanczosC[4] / (z + 4.0);
    a += kLanczosC[5] / (z + 5.0);
    a += kLanczosC[6] / (z + 6.0);
    a += kLanczosC[7] / (z + 7.0);
    a += kLanczosC[8] / (z + 8.0);
    const double t = z + kLanczosGph;
    return kHalfLn2Pi + (z + 0.5) * crd_log1(t) - t + crd_log1(a);
}

// Full scalar Lanczos lgamma (reflection / pole / NaN tail).
[[nodiscard]] inline double crd_lgamma_lz1(double x) noexcept
{
    if (std::isnan(x))
    {
        return x;
    }
    if (x >= 0.5)
    {
        return crd_lgamma_lz_half(x);
    }
    if (x <= 0.0 && x == crd::math::floor(x))
    {
        return std::numeric_limits<double>::infinity(); // pole
    }
    const double s = std::abs(crd::math::sin(kPiLz * x));
    return kLnPi - crd_log1(s) - crd_lgamma_lz_half(1.0 - x);
}

// Scalar Lanczos tgamma. Γ(x)=exp(lnΓ(x)) for x>0 (Γ>0); reflection Γ(x)=π/(sin(πx)·Γ(1−x)) for x<0.
[[nodiscard]] inline double crd_tgamma_lz1(double x) noexcept
{
    if (std::isnan(x))
    {
        return x;
    }
    if (x > 0.0)
    {
        return crd_exp1(crd_lgamma_lz1(x));
    }
    if (x == crd::math::floor(x))
    {
        return std::numeric_limits<double>::quiet_NaN(); // pole at 0, −1, −2, …
    }
    const double s = crd::math::sin(kPiLz * x);
    return kPiLz / (s * crd_exp1(crd_lgamma_lz_half(1.0 - x)));
}

#if CRD_SIMD_BACKEND == CRD_SIMD_BACKEND_AVX2
// AVX2 Lanczos lgamma for x ≥ 0.5 — bit-identical to crd_lgamma_lz_half (same order, crd_log4 ≡ crd_log1).
[[nodiscard]] inline __m256d crd_lgamma_lz_half4(__m256d x) noexcept
{
    const __m256d z = _mm256_sub_pd(x, _mm256_set1_pd(1.0));
    __m256d a = _mm256_set1_pd(kLanczosC[0]);
    for (int k = 1; k <= 8; ++k)
    {
        const __m256d denom = _mm256_add_pd(z, _mm256_set1_pd(static_cast<double>(k)));
        a = _mm256_add_pd(a, _mm256_div_pd(_mm256_set1_pd(kLanczosC[k]), denom));
    }
    const __m256d t = _mm256_add_pd(z, _mm256_set1_pd(kLanczosGph));
    __m256d r = _mm256_mul_pd(_mm256_add_pd(z, _mm256_set1_pd(0.5)), crd_log4(t)); // (z+0.5)·log(t)
    r = _mm256_add_pd(_mm256_set1_pd(kHalfLn2Pi), r);                              // + ½ln(2π)
    r = _mm256_sub_pd(r, t);                                                       // − t
    r = _mm256_add_pd(r, crd_log4(a));                                            // + log(a)
    return r;
}

// Full AVX2 Lanczos lgamma — fast path for x ≥ 0.5; lanes with x < 0.5 (reflection / pole / NaN) → scalar.
[[nodiscard]] inline __m256d crd_lgamma_lz4(__m256d x) noexcept
{
    __m256d r = crd_lgamma_lz_half4(x);
    const __m256d lt = _mm256_cmp_pd(x, _mm256_set1_pd(0.5), _CMP_LT_OQ);
    const __m256d unord = _mm256_cmp_pd(x, x, _CMP_UNORD_Q);
    const __m256d need = _mm256_or_pd(lt, unord);
    if (_mm256_movemask_pd(need) != 0)
    {
        alignas(32) double xa[4];
        alignas(32) double ra[4];
        _mm256_store_pd(xa, x);
        _mm256_store_pd(ra, r);
        const int mask = _mm256_movemask_pd(need);
        for (int i = 0; i < 4; ++i)
        {
            if ((mask >> i) & 1)
            {
                ra[i] = crd_lgamma_lz1(xa[i]);
            }
        }
        r = _mm256_load_pd(ra);
    }
    return r;
}

// AVX2 Lanczos tgamma — fast path x ≥ 0.5 = exp(lgamma); lanes x < 0.5 (reflection / pole / NaN) → scalar.
[[nodiscard]] inline __m256d crd_tgamma_lz4(__m256d x) noexcept
{
    __m256d r = crd_exp4(crd_lgamma_lz_half4(x));
    const __m256d lt = _mm256_cmp_pd(x, _mm256_set1_pd(0.5), _CMP_LT_OQ);
    const __m256d unord = _mm256_cmp_pd(x, x, _CMP_UNORD_Q);
    const __m256d need = _mm256_or_pd(lt, unord);
    if (_mm256_movemask_pd(need) != 0)
    {
        alignas(32) double xa[4];
        alignas(32) double ra[4];
        _mm256_store_pd(xa, x);
        _mm256_store_pd(ra, r);
        const int mask = _mm256_movemask_pd(need);
        for (int i = 0; i < 4; ++i)
        {
            if ((mask >> i) & 1)
            {
                ra[i] = crd_tgamma_lz1(xa[i]);
            }
        }
        r = _mm256_load_pd(ra);
    }
    return r;
}
#endif // AVX2

} // namespace crd::hesap::special::detail
