#pragma once

// crd-math trig family — deterministic sin/cos/sincos/tan, ≤1 ulp, faster-than-libm, over the whole finite range.
// π/2 argument reduction (Cody-Waite while k·(π/2)'s parts multiply exactly, Payne–Hanek above) + minimax sin/cos on
// |r| ≤ π/4 + a quadrant combine. -ffp-contract=off + explicit fma ⇒ bit-identical on every platform (the moat).
// Same contract as transcendental.hpp; the C library appears ONLY in the non-finite edge fallback.

#include <crd/math/libm.hpp> // edge fallback (NaN/Inf) + floor/fma reduction primitives ONLY
#include <crd/math/scalar_kernels.hpp>

namespace crd::math::inline CRD_MATH_LEVEL_NS
{
// π/2 in four parts: kPio2Part1+kPio2Part2+kPio2Part3 = π/2 to ~150 bits, so x − k·(π/2) keeps full precision
// (Cody-Waite); kPio2Part4, the tail, takes it to ~200 bits (≤1 ulp at the zeros).
inline constexpr double kPio2Part1 = 1.57079632673412561417e+00; // π/2 part 1 (low 33 bits zero ⇒ k·part exact)
inline constexpr double kPio2Part2 = 6.07710050630396597660e-11; // π/2 part 2 (low bits zero ⇒ k·part exact)
inline constexpr double kPio2Part3 = 2.02226624871116645580e-21; // π/2 part 3 (low bits zero ⇒ k·part exact)
inline constexpr double kPio2Part4 = 8.47842766036889956997e-32; // π/2 part 4 (tail)
inline constexpr double k2OverPi = 6.36619772367581382433e-01;   // 2/π

// Relative-error minimax on |r| ≤ π/4 (tests/foundation/math/gen_minimax_coeffs.py): sin 0.034·2^-53, cos 0.0005·2^-53.
// sin(r) = r·(1 + Σ kSin_j s^j), s = r²
inline constexpr double kSin1 = -1.66666666666666297e-01, kSin2 = 8.33333333332184067e-03,
                        kSin3 = -1.98412698293585278e-04, kSin4 = 2.75573135385140264e-06,
                        kSin5 = -2.50507343891704837e-08, kSin6 = 1.58954332802290073e-10;
// cos(r) = 1 − ½s + Σ kCos_j s^j, s = r²
inline constexpr double kCos2 = 4.16666666666665950e-02, kCos3 = -1.38888888888733420e-03,
                        kCos4 = 2.48015872890020962e-05, kCos5 = -2.75573142170505708e-07,
                        kCos6 = 2.08757053986308588e-09, kCos7 = -1.13587494271171182e-11;

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

namespace trig_detail
{
// sin(r + rt) on |r| ≤ π/4, rt = the reduction tail (low part). sin(r+rt) ≈ sin(r) + rt·cos(r); the +rt restores
// the low bits the reduction cancellation drops near multiples of π (without it, sin near its zeros is ~7 ulp).
[[nodiscard]] inline double sin_poly(double r, double rt) noexcept
{
    const double s = r * r;
    double p = kSin6;
    p = libm::fma(p, s, kSin5);
    p = libm::fma(p, s, kSin4);
    p = libm::fma(p, s, kSin3);
    p = libm::fma(p, s, kSin2);
    p = libm::fma(p, s, kSin1);
    // sin(r) = r + r·s·p; tail: + rt·cos(r) ≈ rt·(1 − ½s). r dominates ⇒ add the corrections then r.
    return r + libm::fma(r * s, p, rt * (1.0 - 0.5 * s));
}
// cos(r + rt) on |r| ≤ π/4. cos(r+rt) ≈ cos(r) − rt·sin(r) ≈ cos(r) − rt·r. (1−½s carried for accuracy near 1.)
[[nodiscard]] inline double cos_poly(double r, double rt) noexcept
{
    const double s = r * r;
    double p = kCos7;
    p = libm::fma(p, s, kCos6);
    p = libm::fma(p, s, kCos5);
    p = libm::fma(p, s, kCos4);
    p = libm::fma(p, s, kCos3);
    p = libm::fma(p, s, kCos2);
    const double hz = 0.5 * s;
    const double w = 1.0 - hz;
    return w + (((1.0 - w) - hz) + libm::fma(s * s, p, -r * rt));
}
// 2/π = Σ kTwoOverPiBits[w]·2^(−32(w+1)). The largest double (exponent 971) reads up to word 36; the reference
// generator (tests/foundation/math/gen_transcendental_refs.py) checks every word against mpmath.
inline constexpr crd::u32 kTwoOverPiBits[37] = {
    0xA2F9836EU, 0x4E441529U, 0xFC2757D1U, 0xF534DDC0U, 0xDB629599U, 0x3C439041U, 0xFE5163ABU, 0xDEBBC561U,
    0xB7246E3AU, 0x424DD2E0U, 0x06492EEAU, 0x09D1921CU, 0xFE1DEB1CU, 0xB129A73EU, 0xE88235F5U, 0x2EBB4484U,
    0xE99C7026U, 0xB45F7E41U, 0x3991D639U, 0x835339F4U, 0x9C845F8BU, 0xBDF9283BU, 0x1FF897FFU, 0xDE05980FU,
    0xEF2F118BU, 0x5A0A6D1FU, 0x6D367ECFU, 0x27CB09B7U, 0x4F463F66U, 0x9E5FEA2DU, 0x7527BAC7U, 0xEBE5F17BU,
    0x3D0739F7U, 0x8A5292EAU, 0x6BFB5FB1U, 0x1F8D5D08U, 0x56033046U};

// The 32 bits of the little-endian limbs p from bit pos up (pos ≤ 256).
[[nodiscard]] inline crd::u32 limb_bits(const crd::u32 (&p)[10], int pos) noexcept
{
    const int word = pos >> 5;
    const crd::u64 pair = (static_cast<crd::u64>(p[word + 1]) << 32U) | p[word];
    return static_cast<crd::u32>(pair >> static_cast<unsigned>(pos & 31));
}

// Payne–Hanek, for |k| > 2^20, where Cody-Waite's k·part products stop being exact. |x| = m·2^e with m < 2^53: the
// bits of 2/π above 2^(2−e) add only multiples of 4 to x·2/π, so m times the next 224 bits gives k mod 4 and the
// fraction, and the bits below the window add less than 2^−138. The fraction keeps at least 97 significant bits, so
// even the double nearest a multiple of π/2 (|r| ≈ 2^−61, the worst case over all doubles) reduces to ~2^−97. Integer
// arithmetic and exact conversions throughout: the same bits on every compiler and level.
[[nodiscard]] inline int reduce_pio2_large(double x, double& r, double& rt) noexcept
{
    const crd::u64 bits = __builtin_bit_cast(crd::u64, x);
    const crd::u64 m = (bits & 0x000FFFFFFFFFFFFFULL) | (1ULL << 52U); // |x| > 2^20 is normal
    const int e = static_cast<int>((bits >> 52U) & 0x7FFU) - 1075;     // e in [−32, 971]
    const int j0 = e >= 2 ? (e - 2) / 32 : 0;                          // the window starts at bit 32·j0 + 1 ≤ e − 1
    crd::u32 p[10] = {};                                               // m · window: 277 bits
    const crd::u32 mw[2] = {static_cast<crd::u32>(m), static_cast<crd::u32>(m >> 32U)};
    for (int i = 0; i < 2; ++i)
    {
        crd::u64 carry = 0U;
        for (int j = 0; j < 7; ++j)
        {
            const crd::u64 t = static_cast<crd::u64>(mw[i]) * kTwoOverPiBits[j0 + 6 - j] + p[i + j] + carry;
            p[i + j] = static_cast<crd::u32>(t);
            carry = t >> 32U;
        }
        p[i + 7] = static_cast<crd::u32>(carry);
    }
    const int point = 32 * (j0 + 7) - e; // x·2/π = p·2^(−point), point in [191, 256]
    int k = static_cast<int>(limb_bits(p, point) & 3U);
    crd::u32 f[5] = {}; // the fraction, most significant limb first
    for (int t = 0; t < 5; ++t)
    {
        f[t] = limb_bits(p, point - 32 * (t + 1));
    }
    bool negative = (bits >> 63U) != 0U;
    if ((f[0] >> 31U) != 0U)
    {
        // a fraction of 1/2 or more rounds k up and leaves f − 1: negate the limbs (two's complement)
        k = (k + 1) & 3;
        crd::u64 carry = 1U;
        for (int t = 4; t >= 0; --t)
        {
            const crd::u64 v = static_cast<crd::u64>(f[t] ^ 0xFFFFFFFFU) + carry;
            f[t] = static_cast<crd::u32>(v);
            carry = v >> 32U;
        }
        negative = !negative;
    }
    // |fraction| as a double-double from its first nonzero limb: each limb converts and scales exactly
    constexpr double limb_weight[5] = {0x1p-32, 0x1p-64, 0x1p-96, 0x1p-128, 0x1p-160};
    double d[8] = {};
    for (int t = 0; t < 5; ++t)
    {
        d[t] = static_cast<double>(f[t]) * limb_weight[t];
    }
    int i0 = 0;
    while (i0 < 4 && f[i0] == 0U)
    {
        ++i0;
    }
    const double hi = d[i0] + d[i0 + 1]; // d[i0] ≥ d[i0 + 1]: exact error below
    const double lo = (d[i0 + 1] - (hi - d[i0])) + (d[i0 + 2] + d[i0 + 3]);
    // × π/2 in double-double
    const double ph = hi * kHalfPi;
    const double pl = libm::fma(hi, kHalfPi, -ph) + libm::fma(hi, kHalfPiLo, lo * kHalfPi);
    r = ph + pl;
    rt = pl - (r - ph);
    if (negative)
    {
        r = -r;
        rt = -rt;
    }
    return (bits >> 63U) != 0U ? (4 - k) & 3 : k;
}

// reduce x to r + rt = x − k·(π/2), |r| ≤ π/4, as a double-double; returns the quadrant k & 3. While |k| ≤ 2^20 the
// low-bits-zero 3-part π/2 (Cody-Waite) multiplies k exactly and the fma tails carry the rest; above, Payne–Hanek.
[[nodiscard]] inline int reduce_pio2(double x, double& r, double& rt) noexcept
{
    const double fn = libm::floor(libm::fma(x, k2OverPi, 0.5)); // k = round(x·2/π), deterministic
    if (libm::fabs(fn) > 0x1p20)
    {
        return reduce_pio2_large(x, r, rt);
    }
    const double t = x - fn * kPio2Part1; // fn·kPio2Part1 exact (|k| ≤ 2^20) ⇒ exact subtraction
    // two_sum(t, −fn·pio2_2): r = round, rt = exact tail (robust — no operand-order assumption)
    const double w2 = fn * kPio2Part2;
    r = t - w2;
    const double b2 = r - t;
    rt = (t - (r - b2)) - (w2 + b2);
    // subtract fn·pio2_3, accumulating its tail
    const double w3 = fn * kPio2Part3;
    const double r3 = r - w3;
    const double b3 = r3 - r;
    rt += (r - (r3 - b3)) - (w3 + b3);
    r = r3;
    rt -= fn * kPio2Part4; // the tiny 4th part ⇒ reduced arg good to ~200 bits ⇒ ≤1 ulp even at sin's zeros
    return static_cast<int>(fn - 4.0 * libm::floor(fn * 0.25)); // k mod 4 in [0, 3]
}
// atan(t), |t| ≤ tan(π/8), via t·(1 + s·P(s)), s = t²
[[nodiscard]] inline double atan_core(double t) noexcept
{
    const double s = t * t;
    double p = kAtan11;
    p = libm::fma(p, s, kAtan10);
    p = libm::fma(p, s, kAtan9);
    p = libm::fma(p, s, kAtan8);
    p = libm::fma(p, s, kAtan7);
    p = libm::fma(p, s, kAtan6);
    p = libm::fma(p, s, kAtan5);
    p = libm::fma(p, s, kAtan4);
    p = libm::fma(p, s, kAtan3);
    p = libm::fma(p, s, kAtan2);
    p = libm::fma(p, s, kAtan1);
    return libm::fma(t * s, p, t); // t·(1 + s·h(s))
}
} // namespace trig_detail

#if !CRD_MATH_SCALAR_DISPATCH // general code of an x64 build without FMA calls the scalar table instead
// atan(x): reduce to |t| ≤ tan(π/8) via x↦1/x (|x|>1, +π/2) and the π/4 half-angle (|t|>tan(π/8)).
[[nodiscard]] inline double atan(double x) noexcept
{
    if (libm::is_nan(x))
    {
        return x;
    }
    const double a = libm::fabs(x);
    if (a > 0x1p100)
    {
        return libm::copysign(kHalfPi, x); // atan(±huge) = ±π/2
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
    double r = base + trig_detail::atan_core(t);
    if (inv)
    {
        r = (kHalfPi - r) + kHalfPiLo; // atan(a) = π/2 − atan(1/a); the lo term keeps it ≤1 ulp
    }
    return libm::copysign(r, x);
}

// atan2(y, x): full quadrant. Edge cases (zeros, ±Inf) resolve to exact deterministic constants.
[[nodiscard]] inline double atan2(double y, double x) noexcept
{
    if (libm::is_nan(x) || libm::is_nan(y))
    {
        return x + y; // NaN
    }
    if (x == 0.0 || libm::is_inf(x) || libm::is_inf(y))
    {
        return libm::atan2(y, x); // edges (zeros / ±Inf) → exact π/4·k constants, bit-identical on every platform
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
    if (!(libm::fabs(x) <= 1.0))
    {
        return libm::asin(x); // |x|>1 / NaN → NaN
    }
    return atan(x / libm::sqrt((1.0 - x) * (1.0 + x)));
}

// acos(x) = 2·atan(√((1−x)/(1+x))), |x| ≤ 1  (no π/2−asin cancellation near x=1; →0 at 1, →π at −1).
[[nodiscard]] inline double acos(double x) noexcept
{
    if (!(libm::fabs(x) <= 1.0))
    {
        return libm::acos(x);
    }
    return 2.0 * atan(libm::sqrt((1.0 - x) / (1.0 + x)));
}

[[nodiscard]] inline double sin(double x) noexcept
{
    if (!libm::is_finite(x))
    {
        return libm::sin(x); // NaN/±Inf → NaN
    }
    double r = 0.0;
    double rt = 0.0;
    switch (trig_detail::reduce_pio2(x, r, rt))
    {
    case 0:
        return trig_detail::sin_poly(r, rt);
    case 1:
        return trig_detail::cos_poly(r, rt);
    case 2:
        return -trig_detail::sin_poly(r, rt);
    default:
        return -trig_detail::cos_poly(r, rt);
    }
}

[[nodiscard]] inline double cos(double x) noexcept
{
    if (!libm::is_finite(x))
    {
        return libm::cos(x);
    }
    double r = 0.0;
    double rt = 0.0;
    switch (trig_detail::reduce_pio2(x, r, rt))
    {
    case 0:
        return trig_detail::cos_poly(r, rt);
    case 1:
        return -trig_detail::sin_poly(r, rt);
    case 2:
        return -trig_detail::cos_poly(r, rt);
    default:
        return trig_detail::sin_poly(r, rt);
    }
}

// sin and cos of the same argument — one reduction, both polynomials.
inline void sincos(double x, double& s_out, double& c_out) noexcept
{
    if (!libm::is_finite(x))
    {
        s_out = libm::sin(x);
        c_out = libm::cos(x);
        return;
    }
    double r = 0.0;
    double rt = 0.0;
    const int q = trig_detail::reduce_pio2(x, r, rt);
    const double sp = trig_detail::sin_poly(r, rt);
    const double cp = trig_detail::cos_poly(r, rt);
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
    if (!libm::is_finite(x))
    {
        return libm::tan(x);
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

#endif

} // namespace crd::math::inline CRD_MATH_LEVEL_NS
