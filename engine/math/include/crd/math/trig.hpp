#pragma once

// crd-math trig family — deterministic sin/cos/sincos/tan, ≤1 ulp, faster-than-libm. Cody-Waite 3-part π/2
// argument reduction (≤1 ulp for |x| ≲ 1e6 — the practical angle range) + minimax sin/cos on |r| ≤ π/4 + a
// quadrant combine. -ffp-contract=off + explicit fma ⇒ bit-identical on every platform (the moat). Same contract
// as transcendental.hpp; std:: appears ONLY in the non-finite edge fallback. (Payne–Hanek for |x| ≫ 1e6 is the
// large-argument extension — tracked in docs/phases/crd-math-transcendental.md tx-gaps.)

#include <cmath> // edge fallback (NaN/Inf) + floor/nearbyint reduction primitives ONLY

namespace crd::math
{
// π/2 in three parts: kPio2_1+kPio2_2+kPio2_3 = π/2 to ~150 bits, so x − k·(π/2) keeps full precision (Cody-Waite).
inline constexpr double kPio2_1 = 1.57079632673412561417e+00;  // π/2 part 1 (low 33 bits zero ⇒ k·part exact)
inline constexpr double kPio2_2 = 6.07710050630396597660e-11;  // π/2 part 2 (low bits zero ⇒ k·part exact)
inline constexpr double kPio2_3 = 2.02226624871116645580e-21;  // π/2 part 3 (low bits zero ⇒ k·part exact)
inline constexpr double kPio2_4 = 8.47842766036889956997e-32;  // π/2 part 4 (tail) ⇒ π/2 to ~200 bits ⇒ ≤1 ulp at zeros
inline constexpr double k2OverPi = 6.36619772367581382433e-01; // 2/π

// sin(r) = r·(1 + Σ kSin_j s^j), s = r²  (minimax on |r|≤π/4 — gen build/_trig_gen.py, ≤1 ulp)
inline constexpr double kSin1 = -1.66666666666668850e-01, kSin2 = 8.33333333336602061e-03,
                        kSin3 = -1.98412698635934935e-04, kSin4 = 2.75573273852535577e-06,
                        kSin5 = -2.50537399183996951e-08, kSin6 = 1.62264344539045369e-10,
                        kSin7 = -1.45094678459523063e-12;
// cos(r) = 1 − ½s + s²·(Σ kCos_j s^{j-2}), s = r²  (minimax, ≤1 ulp)
inline constexpr double kCos2 = 4.16666666666578728e-02, kCos3 = -1.38888888866768741e-03,
                        kCos4 = 2.48015852908292254e-05, kCos5 = -2.75564380670121137e-07,
                        kCos6 = 2.06755116900856489e-09, kCos7 = 1.16006688779288041e-11,
                        kCos8 = -1.04371124949305481e-11;

// atan(t) = t·(1 + s·h(s)), h(s) = Σ kAtan_j s^{j-1}, s = t², |t| ≤ tan(π/8)  (Chebyshev, c0=1 exact — ≤2 ulp)
inline constexpr double kAtan1 = -3.33333333333332760e-01, kAtan2 = 1.99999999999732336e-01,
                        kAtan3 = -1.42857142815918048e-01, kAtan4 = 1.11111108061898403e-01,
                        kAtan5 = -9.09089704297670886e-02, kAtan6 = 7.69202588395859893e-02,
                        kAtan7 = -6.66251080531530399e-02, kAtan8 = 5.84242658563427081e-02,
                        kAtan9 = -5.01097915533663743e-02, kAtan10 = 3.72850640754660428e-02,
                        kAtan11 = -1.70224278550655320e-02;
inline constexpr double kHalfPi = 1.57079632679489661923;    // π/2
inline constexpr double kHalfPiLo = 6.12323399573676604e-17; // π/2 low (hi=kHalfPi)
inline constexpr double kQuarterPi = 0.78539816339744830961; // π/4
inline constexpr double kPi = 3.14159265358979311600;        // π
inline constexpr double kTanPio8 = 0.41421356237309503;      // tan(π/8) reduction breakpoint

namespace detail
{
// sin(r + rt) on |r| ≤ π/4, rt = the reduction tail (low part). sin(r+rt) ≈ sin(r) + rt·cos(r); the +rt restores
// the low bits the reduction cancellation drops near multiples of π (without it, sin near its zeros is ~7 ulp).
[[nodiscard]] inline double sin_poly(double r, double rt) noexcept
{
    const double s = r * r;
    double p = kSin7;
    p = std::fma(p, s, kSin6);
    p = std::fma(p, s, kSin5);
    p = std::fma(p, s, kSin4);
    p = std::fma(p, s, kSin3);
    p = std::fma(p, s, kSin2);
    p = std::fma(p, s, kSin1);
    // sin(r) = r + r·s·p; tail: + rt·cos(r) ≈ rt·(1 − ½s). r dominates ⇒ add the corrections then r.
    return r + std::fma(r * s, p, rt * (1.0 - 0.5 * s));
}
// cos(r + rt) on |r| ≤ π/4. cos(r+rt) ≈ cos(r) − rt·sin(r) ≈ cos(r) − rt·r. (1−½s carried for accuracy near 1.)
[[nodiscard]] inline double cos_poly(double r, double rt) noexcept
{
    const double s = r * r;
    double p = kCos8;
    p = std::fma(p, s, kCos7);
    p = std::fma(p, s, kCos6);
    p = std::fma(p, s, kCos5);
    p = std::fma(p, s, kCos4);
    p = std::fma(p, s, kCos3);
    p = std::fma(p, s, kCos2);
    const double hz = 0.5 * s;
    const double w = 1.0 - hz;
    return w + (((1.0 - w) - hz) + std::fma(s * s, p, -r * rt));
}
// reduce x to r + rt = x − k·(π/2), |r| ≤ π/4 (double-double via the low-bits-zero 3-part π/2 + fma tails);
// returns the quadrant k & 3. ≤1 ulp for |x| ≲ 1e6 (Cody-Waite); Payne-Hanek for |x| ≫ 1e6 is the tx-gaps item.
[[nodiscard]] inline int reduce_pio2(double x, double& r, double& rt) noexcept
{
    const double fn = std::floor(std::fma(x, k2OverPi, 0.5)); // k = round(x·2/π), deterministic
    const double t = x - fn * kPio2_1;                        // fn·pio2_1 exact (k≤2^20) ⇒ exact subtraction
    // two_sum(t, −fn·pio2_2): r = round, rt = exact tail (robust — no operand-order assumption)
    const double w2 = fn * kPio2_2;
    r = t - w2;
    const double b2 = r - t;
    rt = (t - (r - b2)) - (w2 + b2);
    // subtract fn·pio2_3, accumulating its tail
    const double w3 = fn * kPio2_3;
    const double r3 = r - w3;
    const double b3 = r3 - r;
    rt += (r - (r3 - b3)) - (w3 + b3);
    r = r3;
    rt -= fn * kPio2_4; // the tiny 4th part ⇒ reduced arg good to ~200 bits ⇒ ≤1 ulp even at sin's zeros
    return static_cast<int>(static_cast<long long>(fn)) & 3;
}
// atan(t), |t| ≤ tan(π/8), via t·(1 + s·P(s)), s = t²
[[nodiscard]] inline double atan_core(double t) noexcept
{
    const double s = t * t;
    double p = kAtan11;
    p = std::fma(p, s, kAtan10);
    p = std::fma(p, s, kAtan9);
    p = std::fma(p, s, kAtan8);
    p = std::fma(p, s, kAtan7);
    p = std::fma(p, s, kAtan6);
    p = std::fma(p, s, kAtan5);
    p = std::fma(p, s, kAtan4);
    p = std::fma(p, s, kAtan3);
    p = std::fma(p, s, kAtan2);
    p = std::fma(p, s, kAtan1);
    return std::fma(t * s, p, t); // t·(1 + s·h(s))
}
} // namespace detail

// atan(x): reduce to |t| ≤ tan(π/8) via x↦1/x (|x|>1, +π/2) and the π/4 half-angle (|t|>tan(π/8)).
[[nodiscard]] inline double atan(double x) noexcept
{
    if (std::isnan(x))
    {
        return x;
    }
    const double a = std::fabs(x);
    if (a > 0x1p100)
    {
        return std::copysign(kHalfPi, x); // atan(±huge) = ±π/2
    }
    double aa = a;
    const bool inv = a > 1.0;
    if (inv)
    {
        aa = 1.0 / a;
    }
    double base = 0.0;
    double t = aa;
    if (aa > kTanPio8)
    {
        t = (aa - 1.0) / (aa + 1.0); // atan(aa) = π/4 + atan(t), t ∈ [−tan(π/8), 0]
        base = kQuarterPi;
    }
    double r = base + detail::atan_core(t);
    if (inv)
    {
        r = (kHalfPi - r) + kHalfPiLo; // atan(a) = π/2 − atan(1/a); the lo term keeps it ≤1 ulp
    }
    return std::copysign(r, x);
}

// atan2(y, x): full quadrant. Edge cases (zeros, ±Inf) resolve to exact deterministic constants.
[[nodiscard]] inline double atan2(double y, double x) noexcept
{
    if (std::isnan(x) || std::isnan(y))
    {
        return x + y; // NaN
    }
    if (x == 0.0 || std::isinf(x) || std::isinf(y))
    {
        return std::atan2(y, x); // edges (zeros / ±Inf) → exact π/4·k constants, bit-identical on every platform
    }
    const double r = atan(y / x);
    if (x > 0.0)
    {
        return r; // quadrants I/IV
    }
    return (y >= 0.0) ? r + kPi : r - kPi; // x<0: quadrants II/III
}

// asin(x) = atan(x/√((1−x)(1+x))), |x| ≤ 1  (the product form keeps 1−x² accurate near ±1).
[[nodiscard]] inline double asin(double x) noexcept
{
    if (!(std::fabs(x) <= 1.0))
    {
        return std::asin(x); // |x|>1 / NaN → NaN
    }
    return atan(x / std::sqrt((1.0 - x) * (1.0 + x)));
}

// acos(x) = 2·atan(√((1−x)/(1+x))), |x| ≤ 1  (no π/2−asin cancellation near x=1; →0 at 1, →π at −1).
[[nodiscard]] inline double acos(double x) noexcept
{
    if (!(std::fabs(x) <= 1.0))
    {
        return std::acos(x);
    }
    return 2.0 * atan(std::sqrt((1.0 - x) / (1.0 + x)));
}

[[nodiscard]] inline double sin(double x) noexcept
{
    if (!std::isfinite(x))
    {
        return std::sin(x); // NaN/±Inf → NaN
    }
    double r = 0.0;
    double rt = 0.0;
    switch (detail::reduce_pio2(x, r, rt))
    {
    case 0:
        return detail::sin_poly(r, rt);
    case 1:
        return detail::cos_poly(r, rt);
    case 2:
        return -detail::sin_poly(r, rt);
    default:
        return -detail::cos_poly(r, rt);
    }
}

[[nodiscard]] inline double cos(double x) noexcept
{
    if (!std::isfinite(x))
    {
        return std::cos(x);
    }
    double r = 0.0;
    double rt = 0.0;
    switch (detail::reduce_pio2(x, r, rt))
    {
    case 0:
        return detail::cos_poly(r, rt);
    case 1:
        return -detail::sin_poly(r, rt);
    case 2:
        return -detail::cos_poly(r, rt);
    default:
        return detail::sin_poly(r, rt);
    }
}

// sin and cos of the same argument — one reduction, both polynomials.
inline void sincos(double x, double& s_out, double& c_out) noexcept
{
    if (!std::isfinite(x))
    {
        s_out = std::sin(x);
        c_out = std::cos(x);
        return;
    }
    double r = 0.0;
    double rt = 0.0;
    const int q = detail::reduce_pio2(x, r, rt);
    const double sp = detail::sin_poly(r, rt);
    const double cp = detail::cos_poly(r, rt);
    switch (q)
    {
    case 0:
        s_out = sp;
        c_out = cp;
        break;
    case 1:
        s_out = cp;
        c_out = -sp;
        break;
    case 2:
        s_out = -sp;
        c_out = -cp;
        break;
    default:
        s_out = -cp;
        c_out = sp;
        break;
    }
}

[[nodiscard]] inline double tan(double x) noexcept
{
    if (!std::isfinite(x))
    {
        return std::tan(x);
    }
    double s = 0.0;
    double c = 0.0;
    sincos(x, s, c);
    return s / c;
}

// ───────────────────────── f32 overloads (through the f64 core, correctly rounded into f32) ─────────────────────
[[nodiscard]] inline float sin(float x) noexcept { return static_cast<float>(sin(static_cast<double>(x))); }
[[nodiscard]] inline float cos(float x) noexcept { return static_cast<float>(cos(static_cast<double>(x))); }
[[nodiscard]] inline float tan(float x) noexcept { return static_cast<float>(tan(static_cast<double>(x))); }
[[nodiscard]] inline float atan(float x) noexcept { return static_cast<float>(atan(static_cast<double>(x))); }
[[nodiscard]] inline float asin(float x) noexcept { return static_cast<float>(asin(static_cast<double>(x))); }
[[nodiscard]] inline float acos(float x) noexcept { return static_cast<float>(acos(static_cast<double>(x))); }
[[nodiscard]] inline float atan2(float y, float x) noexcept
{
    return static_cast<float>(atan2(static_cast<double>(y), static_cast<double>(x)));
}
inline void sincos(float x, float& s_out, float& c_out) noexcept
{
    double sd = 0.0;
    double cd = 0.0;
    sincos(static_cast<double>(x), sd, cd);
    s_out = static_cast<float>(sd);
    c_out = static_cast<float>(cd);
}

} // namespace crd::math
