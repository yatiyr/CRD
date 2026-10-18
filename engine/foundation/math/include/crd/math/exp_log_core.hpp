#pragma once

// crd-math — the scalar f64 natural log and exp cores (crd_log1, crd_exp1) and their constants: the canonical bit
// patterns that the AVX2 twins in crd/math/simd/transcendental.hpp reproduce lane for lane (the method and the
// determinism contract are described there). Scalar code (crd/math/transcendental.hpp: exp, log and their family)
// includes only this, so it needs no SIMD backend or intrinsics header (2026-10-18 compile-time profile: through
// crd/math/cmath.hpp those cost about 0.34 s in each of 818 Clang units).

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

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
    const double tt = t + t;        // 2t
    return std::fma(tt * s, q, tt); // logm = 2t·(1 + s·q)
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
inline constexpr double kLn2LoE = 1.42860682030941723212e-6;            // ln2 low (ln2 = hi + lo)
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
} // namespace crd::math
