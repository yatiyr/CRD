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
// log(m) = 2·atanh(t) = 2t + 2t·s·R(s), t = (m − 1)/(m + 1), s = t², |t| ≤ 3 − 2√2: R(s) = (atanh t − t)/t³ as a
// degree-7 Chebyshev fit on [0, (3 − 2√2)²] (tests/foundation/math/gen_log_coeffs.py), relative error 3e-18.
inline constexpr double kLogR0 = 3.33333333333333315e-01, kLogR1 = 2.00000000000004396e-01;
inline constexpr double kLogR2 = 1.42857142854018071e-01, kLogR3 = 1.11111111958569583e-01;
inline constexpr double kLogR4 = 9.09089782006645281e-02, kLogR5 = 7.69311985140732901e-02;
inline constexpr double kLogR6 = 6.63438656932844317e-02, kLogR7 = 6.54331307392005124e-02;

// Shared log reduction, x positive and finite: log(x) = ed·ln2 + hi + lo with ed the EXACT base-2 exponent and
// hi + lo = log(m), m ∈ [√½, √2), to 2^-58 relative (|lo| ≤ 0.011·|hi|; the pair is not normalised). t's rounding is
// carried: m − 1 is exact, m + 1 rounds by ul (exact), tl = (m − 1 − t·(m + 1))/(m + 1) through the division's exact
// remainder, and it enters as 2·tl·(1 + s), the first two terms of log's derivative 2/(1 − s). A subnormal x is
// scaled by 2^54 first, exactly.
[[nodiscard]] inline double crd_log_reduce(double x, double& ed, double& lo) noexcept
{
    int bias = 1023;
    if (x < kMinNormal)
    {
        x *= 18014398509481984.0; // 2^54
        bias += 54;
    }
    const crd::u64 bits = __builtin_bit_cast(crd::u64, x);
    int e = static_cast<int>((bits >> 52) & 0x7FFULL) - bias;
    const crd::u64 mbits = (bits & 0x000FFFFFFFFFFFFFULL) | 0x3FF0000000000000ULL;
    double m = __builtin_bit_cast(double, mbits); // m ∈ [1,2)
    if (m > kSqrt2)
    {
        m *= 0.5;
        e += 1;
    }
    ed = static_cast<double>(e);
    const double f = m - 1.0;
    const double u = m + 1.0;
    const double ul = m - (u - 1.0); // m + 1 = u + ul exactly
    const double t = f / u;
    const double tl = libm::fma(-t, ul, libm::fma(-t, u, f)) / u;
    const double s = t * t;
    double q = kLogR7;
    q = libm::fma(q, s, kLogR6);
    q = libm::fma(q, s, kLogR5);
    q = libm::fma(q, s, kLogR4);
    q = libm::fma(q, s, kLogR3);
    q = libm::fma(q, s, kLogR2);
    q = libm::fma(q, s, kLogR1);
    q = libm::fma(q, s, kLogR0);
    const double tt = t + t;
    const double tl2 = tl + tl;
    lo = libm::fma(tt * s, q, libm::fma(tl2, s, tl2));
    return tt;
}

// Scalar f64 log — the canonical bit pattern the SIMD twins reproduce; ≤0.52 ulp. ed·kLn2Hi is exact (an 11-bit
// integer times a 9-bit constant) and so is its sum with hi as a pair (Fast2Sum: |ed·kLn2Hi| ≥ |hi| or ed = 0); the
// result is one rounding of the pair's head plus the small terms.
[[nodiscard]] inline double crd_log1(double x) noexcept
{
    if (!(x > 0.0) || !libm::is_finite(x))
    {
        return libm::log(x); // x≤0 / NaN / +Inf → exact library result
    }
    double ed = 0.0;
    double lo = 0.0;
    const double hi = crd_log_reduce(x, ed, lo);
    const double a = ed * kLn2Hi;
    const double h = a + hi;
    const double he = hi - (h - a);
    return h + libm::fma(ed, kLn2Lo, he + lo);
}

// ---- exp ----
inline constexpr double kLog2e = 1.44269504088896340735992468100189214; // 1/ln2
inline constexpr double kLn2HiE = 6.93145751953125e-1;                  // ln2 high (2^-12 grid, exact)
inline constexpr double kLn2LoE = 1.42860682030941723212e-6;            // ln2 low (ln2 = hi + lo)
inline constexpr double kExpOverflow = 709.782712893383996732;
inline constexpr double kExpUnderflow = -745.133219101941108420;
// exp(r) ≈ 1 + r + Σ_{n=2}^{11} kE_n·r^n on |r| ≤ ln2/2: relative-error minimax (tests/foundation/math/
// gen_minimax_coeffs.py), 0.034·2^-53. c0 = c1 = 1 fixed (exp(0) = exp'(0) = 1 exact); the SIMD twins share them.
inline constexpr double kE2 = 5.00000000000001110e-01, kE3 = 1.66666666666664381e-01;
inline constexpr double kE4 = 4.16666666665257771e-02, kE5 = 8.33333333347370184e-03;
inline constexpr double kE6 = 1.38888889448713737e-03, kE7 = 1.98412695607172825e-04;
inline constexpr double kE8 = 2.48014917293364775e-05, kE9 = 2.75575299956451592e-06;
inline constexpr double kE10 = 2.76307639280123512e-07, kE11 = 2.50203947940207565e-08;

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
