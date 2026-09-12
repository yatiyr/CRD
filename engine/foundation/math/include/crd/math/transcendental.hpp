#pragma once

// crd-math — THE unified deterministic transcendental API (the "Cerid Math Mandate", docs/PRINCIPLES.md).
// All engine + tool code calls crd::math::{exp,log,exp2,exp10,expm1,log2,log10,log1p,...} HERE; std:: math is
// forbidden in engine code (guard crd-no-std-transcendental). These wrap the bit-identical scalar+SIMD cores in
// simd/transcendental.hpp (crd_exp1/crd_log1) and complete the exp/log family. Contract (docs/phases/
// crd-math-transcendental.md §1): <=1 ulp vs mpmath, -ffp-contract=off ⇒ identical bits on every platform/width
// (the certification moat). Edge cases (NaN/±Inf/subnormal/huge) fall back to the correctly-rounded std:: result —
// std:: is permitted HERE (the kernel + its fallbacks) and ONLY here. f32 overloads route through the f64 core
// (correctly-rounded into f32, deterministic). The family slices (trig/inverse-trig/power/hyperbolic) add sibling
// files following this exact pattern: fast common path + std:: edge fallback + a gen→mpmath-gate→golden-determinism
// test entry.

#include <crd/math/simd/transcendental.hpp> // crd_exp1 / crd_log1 cores (+ AVX2 twins)
#include <crd/math/trig.hpp>                 // crd::math::sin/cos/sincos/tan (the trig family)

#include <cmath> // edge fallbacks ONLY (NaN/Inf/subnormal/huge); the guard exempts crd-math's own kernel

namespace crd::math
{
// High-precision constants (ln2 carried by the cores; these are the family base changes).
inline constexpr double kLn2Const = 0.69314718055994530941723212145818;   // ln 2
inline constexpr double kLn10Const = 2.30258509299404568401799145468436;  // ln 10
inline constexpr double kLog2eConst = 1.44269504088896340735992468100189; // 1/ln2
inline constexpr double kLog10eConst = 0.43429448190325182765112891891661; // 1/ln10
// hi/lo splits (hi+lo = 1/ln(base) to ~106 bits) ⇒ r·const via fma in extra precision ⇒ ≤1 ulp log2/log10
// (the single rounded-constant multiply otherwise left them at ~2 ulp).
inline constexpr double kLog2eHi = 1.44269504088896339e+00, kLog2eLo = 2.03552737409310331e-17;
inline constexpr double kLog10eHi = 4.34294481903251817e-01, kLog10eLo = 1.09831965021676507e-17;
inline constexpr double kLog10_2Hi = 3.01029996015131473541e-01;  // log10(2) split (hi has 30 low zero bits ⇒ k·hi
inline constexpr double kLog10_2Lo = -3.51150278327520860677e-10; // exact) for the exp10 Cody-Waite reduction
// expm1 small-x: Q(x) = (e^x−1)/x ≈ 1 + Σ kQ_n·x^n on |x|<0.5 — MINIMAX (gen_expm1.py) ⇒ ≤1 ulp (Taylor was ~180).
inline constexpr double kQ1 = 5.00000000000000000e-01, kQ2 = 1.66666666666669461e-01;
inline constexpr double kQ3 = 4.16666666666644092e-02, kQ4 = 8.33333333316670435e-03;
inline constexpr double kQ5 = 1.38888888891446766e-03, kQ6 = 1.98412701744263634e-04;
inline constexpr double kQ7 = 2.48015873465096126e-05, kQ8 = 2.75570317237591386e-06;
inline constexpr double kQ9 = 2.75571220408857460e-07, kQ10 = 2.51638769228966323e-08;
inline constexpr double kQ11 = 2.09664440778585537e-09;

// ───────────────────────── exp family ─────────────────────────
[[nodiscard]] inline double exp(double x) noexcept { return crd_exp1(x); }

// 2^x — base-2 range reduction (integer part exact via ldexp; fractional via the exp core on f·ln2, |f|<=0.5).
[[nodiscard]] inline double exp2(double x) noexcept
{
    if (!std::isfinite(x) || x > 1024.0 || x < -1075.0)
    {
        return std::exp2(x); // overflow/underflow/NaN → correctly-rounded library edge
    }
    const double k = std::floor(x + 0.5);          // nearest integer
    const double f = x - k;                         // |f| <= 0.5, exact
    return std::ldexp(crd_exp1(f * kLn2Const), static_cast<int>(k)); // 2^k · e^{f·ln2}, ldexp exact
}

// 10^x — analogous base-10 reduction through base-2 (k = round(x·log2 10); exact 2^k · 10^{frac}).
[[nodiscard]] inline double exp10(double x) noexcept
{
    if (!std::isfinite(x) || x > 308.5 || x < -324.0)
    {
        return std::pow(10.0, x);
    }
    const double k = std::floor(x * 3.32192809488736234787 + 0.5);      // round(x·log2 10)
    const double f = std::fma(k, -kLog10_2Lo, std::fma(k, -kLog10_2Hi, x)); // x − k·log10(2), two-part (no precision loss)
    return std::ldexp(crd_exp1(f * kLn10Const), static_cast<int>(k));
}

// exp(x) − 1 — anti-cancellation. |x|<0.5: x·Q(x), Q=(e^x−1)/x minimax. |x|>=0.5: fdlibm reduction
// expm1(x) = 2^k·expm1(r) + (2^k − 1), r = x − k·ln2 (|r| ≤ ln2/2) — the (2^k − 1) integer part removes the
// exp(x)−1 cancellation that left the [0.5,3] mid-band at ~2 ulp. ≤1 ulp across the whole finite range.
[[nodiscard]] inline double expm1(double x) noexcept
{
    if (!std::isfinite(x))
    {
        return std::expm1(x);
    }
    if (x > -0.34657359027997264 && x < 0.34657359027997264) // |x| < ½·ln2: direct (Q peaks past this — reduce beyond)
    {
        // Q(x) = (e^x−1)/x ≈ 1 + Σ kQ_n·x^n (minimax); expm1 = x·Q(x). No 1+… cancellation.
        double q = kQ11;
        q = std::fma(q, x, kQ10);
        q = std::fma(q, x, kQ9);
        q = std::fma(q, x, kQ8);
        q = std::fma(q, x, kQ7);
        q = std::fma(q, x, kQ6);
        q = std::fma(q, x, kQ5);
        q = std::fma(q, x, kQ4);
        q = std::fma(q, x, kQ3);
        q = std::fma(q, x, kQ2);
        q = std::fma(q, x, kQ1);
        q = std::fma(q, x, 1.0);
        return x * q;
    }
    // |x| >= 0.5: reduce x = k·ln2 + r (same Cody-Waite split as crd_exp1), expm1(r) = r·Q(r) on |r| ≤ ln2/2,
    // then 2^k·expm1(r) + (2^k − 1). k>52 ⇒ (2^k−1)==2^k and the term vanishes (exp huge, no cancellation);
    // k very negative ⇒ 2^k→0 and the result → −1 exactly; k≥1024 ⇒ 2^k=Inf ⇒ expm1=Inf (overflow). All exact.
    const double kf = std::floor(std::fma(x, kLog2e, 0.5));
    const double r = std::fma(kf, -kLn2LoE, std::fma(kf, -kLn2HiE, x));
    double qr = kQ11;
    qr = std::fma(qr, r, kQ10);
    qr = std::fma(qr, r, kQ9);
    qr = std::fma(qr, r, kQ8);
    qr = std::fma(qr, r, kQ7);
    qr = std::fma(qr, r, kQ6);
    qr = std::fma(qr, r, kQ5);
    qr = std::fma(qr, r, kQ4);
    qr = std::fma(qr, r, kQ3);
    qr = std::fma(qr, r, kQ2);
    qr = std::fma(qr, r, kQ1);
    qr = std::fma(qr, r, 1.0);
    const double er = r * qr;                                  // expm1(r)
    const double twok = std::ldexp(1.0, static_cast<int>(kf)); // 2^k
    return std::fma(twok, er, twok - 1.0);                     // 2^k·expm1(r) + (2^k − 1)
}

// ───────────────────────── log family ─────────────────────────
[[nodiscard]] inline double log(double x) noexcept { return crd_log1(x); }

[[nodiscard]] inline double log2(double x) noexcept
{
    if (!(x > 0.0) || !std::isfinite(x) || x < kMinNormal)
    {
        return std::log2(x);
    }
    double ed = 0.0;
    const double logm = crd_log_reduce(x, ed);           // ed exact, logm small
    return ed + std::fma(logm, kLog2eHi, logm * kLog2eLo); // ed + logm·log2e (extra precision) ⇒ ≤1 ulp
}

[[nodiscard]] inline double log10(double x) noexcept
{
    if (!(x > 0.0) || !std::isfinite(x) || x < kMinNormal)
    {
        return std::log10(x);
    }
    double ed = 0.0;
    const double logm = crd_log_reduce(x, ed); // log10(x) = ed·log10(2) + logm·log10(e), ed exact (hi/lo split)
    const double lm = std::fma(logm, kLog10eHi, logm * kLog10eLo);
    return std::fma(ed, kLog10_2Hi, std::fma(ed, kLog10_2Lo, lm));
}

// log(1+x) — Goldberg correction so the low bits of x survive the 1+x rounding (<=1 ulp near 0).
[[nodiscard]] inline double log1p(double x) noexcept
{
    if (!(x > -1.0) || !std::isfinite(x))
    {
        return std::log1p(x);
    }
    const double u = 1.0 + x;
    if (u == 1.0)
    {
        return x; // x tiny → log(1+x) ≈ x to full precision
    }
    return crd_log1(u) * (x / (u - 1.0)); // correction factor for the rounding of (1+x)
}

// ───────────────────────── f32 overloads (route through the f64 core: correctly-rounded into f32) ─────────────────
[[nodiscard]] inline float exp(float x) noexcept { return static_cast<float>(crd_exp1(static_cast<double>(x))); }
[[nodiscard]] inline float exp2(float x) noexcept { return static_cast<float>(exp2(static_cast<double>(x))); }
[[nodiscard]] inline float exp10(float x) noexcept { return static_cast<float>(exp10(static_cast<double>(x))); }
[[nodiscard]] inline float expm1(float x) noexcept { return static_cast<float>(expm1(static_cast<double>(x))); }
[[nodiscard]] inline float log(float x) noexcept { return static_cast<float>(crd_log1(static_cast<double>(x))); }
[[nodiscard]] inline float log2(float x) noexcept { return static_cast<float>(log2(static_cast<double>(x))); }
[[nodiscard]] inline float log10(float x) noexcept { return static_cast<float>(log10(static_cast<double>(x))); }
[[nodiscard]] inline float log1p(float x) noexcept { return static_cast<float>(log1p(static_cast<double>(x))); }

} // namespace crd::math
