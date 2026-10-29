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

// The log reduction's front end, x positive and finite: x = 2^ed·m with ed the EXACT base-2 exponent and m ∈ [√½, √2),
// and t + tl = (m − 1)/(m + 1) to ~2^-104. m − 1 is exact, m + 1 rounds by ul (exact), tl = (m − 1 − t·(m + 1))/(m + 1)
// through the division's exact remainder. A subnormal x is scaled by 2^54 first, exactly.
[[nodiscard]] inline double crd_log_t(double x, double& ed, double& tl) noexcept
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
    tl = libm::fma(-t, ul, libm::fma(-t, u, f)) / u;
    return t;
}

// Shared log reduction, x positive and finite: log(x) = ed·ln2 + hi + lo with hi + lo = log(m) to 2^-58 relative
// (|lo| ≤ 0.011·|hi|; the pair is not normalised). t's rounding tl enters as 2·tl·(1 + s), the first two terms of
// log's derivative 2/(1 − s).
[[nodiscard]] inline double crd_log_reduce(double x, double& ed, double& lo) noexcept
{
    double tl = 0.0;
    const double t = crd_log_t(x, ed, tl);
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

// ---- log2 as a pair (pow) ----
// log2(e) and ln2 as double-doubles (hi + lo to ~2^-107), and 1/3 and 1/5 as pairs.
inline constexpr double kLog2eHi = 1.44269504088896339e+00, kLog2eLo = 2.03552737409310331e-17;
inline constexpr double kLn2PairHi = 6.93147180559945286e-01, kLn2PairLo = 2.31904681384629956e-17;
inline constexpr double kThirdHi = 3.33333333333333315e-01, kThirdLo = 1.85037170770859413e-17;
inline constexpr double kFifthHi = 2.00000000000000011e-01, kFifthLo = -1.11022302462515654e-17;
// atanh(t)/t = 1 + s/3 + s²/5 + s³·R3(s) on s ∈ [0, (3 − 2√2)²]: relative-error minimax of the whole ratio
// (tests/foundation/math/gen_minimax_coeffs.py), 1.05e-6·2^-53.
inline constexpr double kLogPairR3 = 1.42857142857165720e-01, kLogPairR4 = 1.11111111100095403e-01;
inline constexpr double kLogPairR5 = 9.09090930221169852e-02, kLogPairR6 = 7.69228648876375959e-02;
inline constexpr double kLogPairR7 = 6.66787992783805517e-02, kLogPairR8 = 5.84255566348507935e-02;
inline constexpr double kLogPairR9 = 5.95518786700743319e-02;

// log2(x) as a normalised pair, x positive and finite: hi + lo to 2^-69 relative, so y·log2(x) keeps ~2^-59 of
// absolute error over pow's whole exponent range. log(m) = 2t·(1 + S) + 2·tl/(1 − s), S = s/3 + s²/5 + s³·R3: S is
// carried as a double-double from its s²/5 term up (s = t² as an exact pair), R3 in plain double (s³·R3 ≤ 2^-18).
[[nodiscard]] inline double crd_log2_pair(double x, double& lo) noexcept
{
    double ed = 0.0;
    double tl = 0.0;
    const double t = crd_log_t(x, ed, tl);
    const double s = t * t;
    const double sl = libm::fma(t, t, -s);
    double r = kLogPairR9;
    r = libm::fma(r, s, kLogPairR8);
    r = libm::fma(r, s, kLogPairR7);
    r = libm::fma(r, s, kLogPairR6);
    r = libm::fma(r, s, kLogPairR5);
    r = libm::fma(r, s, kLogPairR4);
    r = libm::fma(r, s, kLogPairR3);
    const double sr = s * r; // A = 1/5 + s·R3
    const double ah = kFifthHi + sr;
    const double al = ((kFifthHi - ah) + sr) + kFifthLo;
    const double p = s * ah; // B = 1/3 + s·A
    const double pl = libm::fma(s, ah, -p) + (s * al + sl * ah);
    const double bh = kThirdHi + p;
    const double bl = ((kThirdHi - bh) + p) + (kThirdLo + pl);
    const double sh = s * bh; // S = s·B
    const double sb = libm::fma(s, bh, -sh) + (s * bl + sl * bh);
    const double tt = t + t; // T = 2t·S
    const double th = tt * sh;
    const double tb = libm::fma(tt, sh, -th) + tt * sb;
    const double tl2 = tl + tl;
    const double d = tl2 * (1.0 + s * (1.0 + s * (1.0 + s))); // 2·tl/(1 − s)
    const double lh = tt + th;                                // log(m) = lh + ll (Fast2Sum: |th| < |tt|)
    const double ll = (th - (lh - tt)) + (tb + d);
    const double ph = lh * kLog2eHi; // log2(m) = ph + pl2
    const double pl2 = libm::fma(lh, kLog2eHi, -ph) + (lh * kLog2eLo + ll * kLog2eHi);
    const double hi = ed + ph; // Fast2Sum: |ed| ≥ 1 > |ph| or ed = 0
    lo = (ph - (hi - ed)) + pl2;
    return hi;
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

// p·2^k for k ∈ [−1076, 1025], p near 1: one exponent injection on the normal-result range (exact); outside it two
// half-steps, so neither exponent field overflows, giving correct subnormals and the right finite value near the
// largest double. Power-of-two multiplies ⇒ bit-identical to the single injection on the overlap.
[[nodiscard]] inline double crd_scale2(double p, int k) noexcept
{
    if (k >= -1022 && k <= 1023)
    {
        const crd::u64 bits = static_cast<crd::u64>(static_cast<long long>(k) + 1023) << 52;
        return p * __builtin_bit_cast(double, bits);
    }
    const int k1 = k >> 1; // floor(k/2), C++20 arithmetic shift
    const int k2 = k - k1;
    const crd::u64 b1 = static_cast<crd::u64>(static_cast<long long>(k1) + 1023) << 52;
    const crd::u64 b2 = static_cast<crd::u64>(static_cast<long long>(k2) + 1023) << 52;
    return (p * __builtin_bit_cast(double, b1)) * __builtin_bit_cast(double, b2);
}

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
    return crd_scale2(p, static_cast<int>(kf));
}

// 2^(hi + lo), the exponent a pair with |lo| ≤ 2^-40 and hi ∈ [−1076, 1025) (the caller decides overflow and
// underflow from hi): ≤0.58 ulp for a normal result. 2^f·ln2 as an exact pair r = rh + rl (with lo·ln2), then
// exp(r) = 1 + rh + rh²·P(rh) + rl·exp(rh) on the exp core's polynomial, 1 + rh exact as a pair, one rounding.
[[nodiscard]] inline double crd_exp2_pair(double hi, double lo) noexcept
{
    const double k = libm::floor(hi + 0.5);
    const double f = hi - k; // |f| ≤ ½, exact
    const double rh = f * kLn2PairHi;
    const double rl = libm::fma(f, kLn2PairHi, -rh) + (f * kLn2PairLo + lo * kLn2PairHi);
    double p = kE11;
    p = libm::fma(p, rh, kE10);
    p = libm::fma(p, rh, kE9);
    p = libm::fma(p, rh, kE8);
    p = libm::fma(p, rh, kE7);
    p = libm::fma(p, rh, kE6);
    p = libm::fma(p, rh, kE5);
    p = libm::fma(p, rh, kE4);
    p = libm::fma(p, rh, kE3);
    p = libm::fma(p, rh, kE2);
    const double rh2 = rh * rh;
    const double rh2l = libm::fma(rh, rh, -rh2);
    const double h = 1.0 + rh; // Fast2Sum: |rh| < 1
    const double hl = (1.0 - h) + rh;
    const double e0 = libm::fma(rh2, p, h); // exp(rh), for the rl term only
    const double tail = libm::fma(rh2, p, hl + libm::fma(rh2l, p, rl * e0));
    return crd_scale2(h + tail, static_cast<int>(k));
}
} // namespace crd::math::inline CRD_MATH_LEVEL_NS
