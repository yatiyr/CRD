#pragma once

// crd-math — the scalar f64 natural log and exp cores (crd_log1, crd_exp1) and their constants: the canonical bit
// patterns that the AVX2 twins in crd/math/simd/transcendental.hpp reproduce lane for lane (the method and the
// determinism contract are described there). Scalar code (crd/math/transcendental.hpp: exp, log and their family)
// includes only this, so it needs no SIMD backend or intrinsics header (2026-10-18 compile-time profile: through
// crd/math/cmath.hpp those cost about 0.34 s in each of 818 Clang units). Kernel units include it too, so it lives
// in the unit's level namespace and calls only crd/math/libm.hpp (ADR-0134 §5).

#include <crd/core/types.hpp>
#include <crd/math/libm.hpp>

namespace crd::math::inline CRD_MATH_LEVEL_NS
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
    const crd::u64 bits = __builtin_bit_cast(crd::u64, x);
    int e = static_cast<int>((bits >> 52) & 0x7FFULL) - 1023;
    const crd::u64 mbits = (bits & 0x000FFFFFFFFFFFFFULL) | 0x3FF0000000000000ULL;
    double m = __builtin_bit_cast(double, mbits); // m ∈ [1,2)
    if (m > kSqrt2)
    {
        m *= 0.5;
        e += 1;
    }
    ed = static_cast<double>(e);
    const double t = (m - 1.0) / (m + 1.0);
    const double s = t * t;
    double q = kC17;
    q = libm::fma(q, s, kC15);
    q = libm::fma(q, s, kC13);
    q = libm::fma(q, s, kC11);
    q = libm::fma(q, s, kC9);
    q = libm::fma(q, s, kC7);
    q = libm::fma(q, s, kC5);
    q = libm::fma(q, s, kC3);
    const double tt = t + t;         // 2t
    return libm::fma(tt * s, q, tt); // logm = 2t·(1 + s·q)
}

// Scalar f64 log — the canonical bit pattern the SIMD twin reproduces.
[[nodiscard]] inline double crd_log1(double x) noexcept
{
    if (!(x > 0.0) || !libm::is_finite(x) || x < kMinNormal)
    {
        return libm::log(x); // x≤0 / NaN / +Inf / subnormal → exact library result
    }
    double ed = 0.0;
    const double logm = crd_log_reduce(x, ed);
    double r = libm::fma(ed, kLn2Hi, logm);
    r = libm::fma(ed, kLn2Lo, r);
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
    if (libm::is_nan(x))
    {
        return x;
    }
    if (x > kExpOverflow)
    {
        return __builtin_bit_cast(double, 0x7FF0000000000000ULL); // +infinity
    }
    if (x < kExpUnderflow)
    {
        return 0.0;
    }
    const double kf = libm::floor(libm::fma(x, kLog2e, 0.5));             // k = round(x·log2e)
    const double r = libm::fma(kf, -kLn2LoE, libm::fma(kf, -kLn2HiE, x)); // r = x − k·ln2 (Cody-Waite)
    double p = kE11;                                                      // exp(r) minimax, Horner from the top
    p = libm::fma(p, r, kE10);
    p = libm::fma(p, r, kE9);
    p = libm::fma(p, r, kE8);
    p = libm::fma(p, r, kE7);
    p = libm::fma(p, r, kE6);
    p = libm::fma(p, r, kE5);
    p = libm::fma(p, r, kE4);
    p = libm::fma(p, r, kE3);
    p = libm::fma(p, r, kE2);
    p = libm::fma(p, r, 1.0); // + r (c1)
    p = libm::fma(p, r, 1.0); // + 1 (c0)
    const int ki = static_cast<int>(kf);
    if (ki >= -1022 && ki <= 1023) // common path: single exponent injection — exact + fast (the normal-result range)
    {
        const crd::u64 bits = static_cast<crd::u64>(static_cast<long long>(ki) + 1023) << 52;
        return p * __builtin_bit_cast(double, bits);
    }
    // boundary (subnormal k≲−1022 / near-overflow k≈1024): TWO half-steps so neither exponent field overflows — gives
    // correct subnormals + the right finite near max-double, where a single injection produced garbage. Power-of-2
    // multiplies ⇒ bit-identical to the single-injection path on the overlap, so the classifier need not match SIMD's.
    const int k1 = ki >> 1; // floor(k/2), C++20 arithmetic shift
    const int k2 = ki - k1;
    const crd::u64 b1 = static_cast<crd::u64>(static_cast<long long>(k1) + 1023) << 52;
    const crd::u64 b2 = static_cast<crd::u64>(static_cast<long long>(k2) + 1023) << 52;
    return (p * __builtin_bit_cast(double, b1)) * __builtin_bit_cast(double, b2);
}
} // namespace crd::math::inline CRD_MATH_LEVEL_NS
