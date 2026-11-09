#pragma once

// crd-hesap-special v12-d — elliptic integrals, the CANONICAL family (the hesap-dsp ellipk/ellipj are a DSP-internal
// filter-design subset; this is the special-functions home — SANITY rule 8). Carlson symmetric integrals R_F/R_C/R_D/
// R_J (the duplication algorithm of Carlson 1995, Numer. Algorithms 10, DLMF §19.36) are the engine; Legendre-form
// K/E/F/E/Π are thin wrappers.
//   R_F(x,y,z)=½∫₀^∞ dt/√((t+x)(t+y)(t+z));  K(m)=R_F(0,1−m,1);  E(m)=R_F − (m/3)R_D;
//   F(φ,m)=sinφ·R_F(cos²φ, 1−m sin²φ, 1);  Π(n,φ,m) via R_J.  Parameter convention m=k² (scipy.special).
// Gated vs scipy.special elliprf/elliprc/elliprd/elliprj + ellipk/ellipe/ellipkinc/ellipeinc to <1e-12. Internals f64.

#include <crd/hesap/special/gamma.hpp>     // Real concept
#include <crd/hesap/special/poly_eval.hpp> // detail::horner_t

#include "elliptic_poly.inc" // GENERATED iteration-free K(m)/E(m) Cody-form coeffs (gen_elliptic_poly.py; v12-d perf)

#include <algorithm>
#include <crd/math/cmath.hpp>
#include <limits>

namespace crd::hesap::special
{
namespace detail
{
// A converged duplication leaves every relative deviation below the series tolerance (≤ 0.0066); one still above this
// bound has not closed.
inline constexpr double kUnclosed = 0.5;
// Past 4^{−n} below this (n ≥ 33 steps, only for arguments many orders of magnitude apart) the mean is re-formed.
inline constexpr double kManySteps = 1.0e-20;

// All four integrals use the duplication of Carlson 1995 (DLMF §19.36(i)): each step replaces the arguments by
// (arg + λ)/4 with λ a symmetric root combination, which leaves the integral fixed (up to the explicit R_C / sum
// terms) and shrinks the arguments' spread about their weighted mean A by a factor 4 per step. The spread at step n is
// therefore 4^{−n}·(A₀ − arg₀): the deviations are formed once, from the starting arguments, and only the running mean
// A_n = (A_{n−1} + λ)/4 and the scale 4^{−n} are carried; the relative spread at step n is (4^{−n}/A_n)·(A₀ − arg₀),
// so the stopping test needs no division and the deviations are normalised once, after the last step, as the bounded
// ratios (A_n − arg_n)/A_n of the carried values (never as scale/A_n, which overflows for a subnormal A_n). A spread
// that did not close, so that a ratio is still of order one (two zero arguments, or A_n flushed to zero), is the
// diverging integral and gives NaN. The weighted means are summed from quarter-scaled arguments (a power of two, exact)
// and rescaled once, so a huge argument cannot overflow the sum.

// R_F(x,y,z): x,y,z ≥ 0, at most one zero. Series through the fifth order in the scaled deviations X,Y,Z (Z = −X−Y):
// R_F = A^{−1/2}·(1 − E₂/10 + E₃/14 + E₂²/24 − 3E₂E₃/44), E₂ = XY − Z², E₃ = XYZ (DLMF §19.36).
[[nodiscard]] inline double carlson_rf(double x, double y, double z) noexcept
{
    constexpr double errtol = 0.0066; // series error ~errtol⁶ ≈ 8e-14 < the 1e-12 gate ⇒ one fewer duplication (v12-d)
    constexpr double third = 1.0 / 3.0;
    double mean = (0.25 * x + 0.25 * y + 0.25 * z) * (4.0 * third);
    const double spread_x = mean - x;
    const double spread_y = mean - y;
    const double widest =
        std::max({crd::math::fabs(spread_x), crd::math::fabs(spread_y), crd::math::fabs(spread_x + spread_y)});
    double scale = 1.0;
    double rx = x;
    double ry = y;
    double rz = z;
    for (;;)
    {
        const double sx = crd::math::sqrt(rx);
        const double sy = crd::math::sqrt(ry);
        const double sz = crd::math::sqrt(rz);
        const double lambda = sx * (sy + sz) + sy * sz;
        rx = 0.25 * (rx + lambda);
        ry = 0.25 * (ry + lambda);
        rz = 0.25 * (rz + lambda);
        mean = 0.25 * (mean + lambda);
        scale *= 0.25;
        if (!(widest * scale > errtol * mean))
        {
            break;
        }
    }
    // Cold path: after many steps the carried mean has drifted from the final arguments by one rounding per step, and
    // a mean flushed towards zero can fake convergence; re-form it from the arguments and require them to have closed.
    if (scale < kManySteps || mean < 1.0e-290)
    {
        mean = (0.25 * rx + 0.25 * ry + 0.25 * rz) * (4.0 * third);
        // The spread never closed (two zero arguments: 4^{−n} or A_n underflowed first): the integral diverges.
        if (scale < 1.0e-290 ||
            !(crd::math::fabs(mean - rx) + crd::math::fabs(mean - ry) + crd::math::fabs(mean - rz) < kUnclosed * mean))
        {
            return std::numeric_limits<double>::quiet_NaN();
        }
    }
    const double big_x = (mean - rx) / mean;
    const double big_y = (mean - ry) / mean;
    const double big_z = -(big_x + big_y);
    const double e2 = big_x * big_y - big_z * big_z;
    const double e3 = big_x * big_y * big_z;
    return (1.0 + e2 * (e2 / 24.0 - 0.1 - 3.0 * e3 / 44.0) + e3 / 14.0) / crd::math::sqrt(mean);
}

// R_C(x,y): degenerate R_F(x,y,y) (handles y < 0 by the Cauchy principal value, DLMF §19.2). One duplication step is
// λ = 2√x√y + y; with s = (y − A)/A and A = (x+2y)/3, R_C = A^{−1/2}·(1 + 3s²/10 + s³/7 + 3s⁴/8 + 9s⁵/22 + …).
[[nodiscard]] inline double carlson_rc(double x, double y) noexcept
{
    constexpr double errtol = 0.0012;
    constexpr double third = 1.0 / 3.0;
    double rx = x;
    double ry = y;
    double pv_factor = 1.0;
    if (!(y > 0.0))
    {
        rx = x - y;
        ry = -y;
        pv_factor = crd::math::sqrt(x) / crd::math::sqrt(rx);
    }
    double mean = (0.25 * rx + 0.5 * ry) * (4.0 * third);
    const double spread = ry - mean;
    const double spread_abs = crd::math::fabs(spread);
    double scale = 1.0;
    for (;;)
    {
        const double lambda = 2.0 * crd::math::sqrt(rx) * crd::math::sqrt(ry) + ry;
        rx = 0.25 * (rx + lambda);
        ry = 0.25 * (ry + lambda);
        mean = 0.25 * (mean + lambda);
        scale *= 0.25;
        if (!(spread_abs * scale > errtol * mean))
        {
            break;
        }
    }
    // Cold path: after many steps the carried mean has drifted from the final arguments by one rounding per step, and
    // a mean flushed towards zero can fake convergence; re-form it from the arguments and require them to have closed.
    if (scale < kManySteps || mean < 1.0e-290)
    {
        mean = (0.25 * rx + 0.5 * ry) * (4.0 * third);
        // the spread never closed (y = 0: 4^{−n} or A_n underflowed first): the integral diverges
        if (scale < 1.0e-290 || !(crd::math::fabs(ry - mean) < kUnclosed * mean))
        {
            return std::numeric_limits<double>::quiet_NaN();
        }
    }
    const double s = (ry - mean) / mean;
    return pv_factor * (1.0 + s * s * (0.3 + s * (1.0 / 7.0 + s * (0.375 + s * (9.0 / 22.0))))) / crd::math::sqrt(mean);
}

// R_D(x,y,z) = R_J(x,y,z,z). Weighted mean A = (x+y+3z)/5; the duplication also accumulates Σ 4^{−m}/(√z_m(z_m+λ_m)):
// R_D = 3·Σ + 4^{−n}·A^{−3/2}·(1 − 3E₂/14 + E₃/6 + 9E₂²/88 − 3E₄/22 − 9E₂E₃/52 + 3E₅/26) with Z = −(X+Y)/3,
// E₂ = XY − 6Z², E₃ = (3XY − 8Z²)Z, E₄ = 3(XY − Z²)Z², E₅ = XYZ³ (DLMF §19.36, terms through the fifth order).
[[nodiscard]] inline double carlson_rd(double x, double y, double z) noexcept
{
    constexpr double errtol = 0.0010;
    double mean = (0.25 * x + 0.25 * y + 0.75 * z) * 0.8;
    const double spread_x = mean - x;
    const double spread_y = mean - y;
    // |mean − z|, since the weighted deviations sum to zero
    const double spread_z = spread_x * (1.0 / 3.0) + spread_y * (1.0 / 3.0);
    const double widest = std::max({crd::math::fabs(spread_x), crd::math::fabs(spread_y), crd::math::fabs(spread_z)});
    double rx = x;
    double ry = y;
    double rz = z;
    double sum = 0.0;
    double scale = 1.0;
    for (;;)
    {
        const double sx = crd::math::sqrt(rx);
        const double sy = crd::math::sqrt(ry);
        const double sz = crd::math::sqrt(rz);
        const double lambda = sx * (sy + sz) + sy * sz;
        sum += scale / (sz * (rz + lambda));
        scale *= 0.25;
        rx = 0.25 * (rx + lambda);
        ry = 0.25 * (ry + lambda);
        rz = 0.25 * (rz + lambda);
        mean = 0.25 * (mean + lambda);
        if (!(widest * scale > errtol * mean))
        {
            break;
        }
    }
    // Cold path: after many steps the carried mean has drifted from the final arguments by one rounding per step, and
    // a mean flushed towards zero can fake convergence; re-form it from the arguments and require them to have closed.
    if (scale < kManySteps || mean < 1.0e-290)
    {
        mean = (0.25 * rx + 0.25 * ry + 0.75 * rz) * 0.8;
        // The spread never closed (two zero arguments: 4^{−n} or A_n underflowed first): the integral diverges.
        if (scale < 1.0e-290 ||
            !(crd::math::fabs(mean - rx) + crd::math::fabs(mean - ry) + crd::math::fabs(mean - rz) < kUnclosed * mean))
        {
            return std::numeric_limits<double>::quiet_NaN();
        }
    }
    const double big_x = (mean - rx) / mean;
    const double big_y = (mean - ry) / mean;
    const double big_z = -(big_x + big_y) * (1.0 / 3.0);
    const double xy = big_x * big_y;
    const double zz = big_z * big_z;
    const double e2 = xy - 6.0 * zz;
    const double e3 = (3.0 * xy - 8.0 * zz) * big_z;
    const double e4 = 3.0 * (xy - zz) * zz;
    const double e5 = xy * zz * big_z;
    const double series =
        1.0 + e2 * (9.0 / 88.0 * e2 - 3.0 / 14.0 - 9.0 / 52.0 * e3) + e3 / 6.0 - 3.0 / 22.0 * e4 + 3.0 / 26.0 * e5;
    return 3.0 * sum + scale * series / (mean * crd::math::sqrt(mean));
}

// R_J(x,y,z,p) (p > 0 path — the standard Π argument range): weighted mean A = (x+y+z+2p)/5; each duplication adds
// 3·4^{−m}·R_C(α_m, β_m), α = (p(√x+√y+√z) + √x√y√z)², β = p(p+λ)² (Carlson 1995; DLMF §19.26), and the tail
// is 4^{−n}·A^{−3/2}·(1 − 3E₂/14 + E₃/6 + 9E₂²/88 − 3E₄/22 − 9E₂E₃/52 + 3E₅/26) with P = −(X+Y+Z)/2,
// E₂ = XY+XZ+YZ − 3P², E₃ = XYZ + 2E₂P + 4P³, E₄ = (2XYZ + E₂P + 3P³)P, E₅ = XYZP² (DLMF §19.36).
[[nodiscard]] inline double carlson_rj(double x, double y, double z, double p) noexcept
{
    constexpr double errtol = 0.0010;
    double mean = (0.25 * x + 0.25 * y + 0.25 * z + 0.5 * p) * 0.8;
    const double spread_x = mean - x;
    const double spread_y = mean - y;
    const double spread_z = mean - z;
    const double spread_p = -(0.5 * spread_x + 0.5 * spread_y + 0.5 * spread_z);
    const double widest = std::max(
        {crd::math::fabs(spread_x), crd::math::fabs(spread_y), crd::math::fabs(spread_z), crd::math::fabs(spread_p)});
    double rx = x;
    double ry = y;
    double rz = z;
    double rp = p;
    double sum = 0.0;
    double scale = 1.0;
    for (;;)
    {
        const double sx = crd::math::sqrt(rx);
        const double sy = crd::math::sqrt(ry);
        const double sz = crd::math::sqrt(rz);
        const double lambda = sx * (sy + sz) + sy * sz;
        const double root = rp * (sx + sy + sz) + sx * sy * sz;
        const double shift = rp + lambda;
        sum += scale * carlson_rc(root * root, rp * shift * shift);
        scale *= 0.25;
        rx = 0.25 * (rx + lambda);
        ry = 0.25 * (ry + lambda);
        rz = 0.25 * (rz + lambda);
        rp = 0.25 * (rp + lambda);
        mean = 0.25 * (mean + lambda);
        if (!(widest * scale > errtol * crd::math::fabs(mean)))
        {
            break;
        }
    }
    // Cold path: after many steps the carried mean has drifted from the final arguments by one rounding per step, and
    // a mean flushed towards zero can fake convergence; re-form it from the arguments and require them to have closed.
    if (scale < kManySteps || crd::math::fabs(mean) < 1.0e-290)
    {
        mean = (0.25 * rx + 0.25 * ry + 0.25 * rz + 0.5 * rp) * 0.8;
        // The spread never closed (two zero arguments: 4^{−n} or A_n underflowed first): the integral diverges.
        if (scale < 1.0e-290 || !(crd::math::fabs(mean - rx) + crd::math::fabs(mean - ry) + crd::math::fabs(mean - rz) <
                                  kUnclosed * crd::math::fabs(mean)))
        {
            return std::numeric_limits<double>::quiet_NaN();
        }
    }
    const double big_x = (mean - rx) / mean;
    const double big_y = (mean - ry) / mean;
    const double big_z = (mean - rz) / mean;
    const double big_p = -0.5 * (big_x + big_y + big_z);
    const double pp = big_p * big_p;
    const double xyz = big_x * big_y * big_z;
    const double e2 = big_x * big_y + big_x * big_z + big_y * big_z - 3.0 * pp;
    const double e3 = xyz + 2.0 * e2 * big_p + 4.0 * pp * big_p;
    const double e4 = (2.0 * xyz + e2 * big_p + 3.0 * pp * big_p) * big_p;
    const double e5 = xyz * pp;
    const double series =
        1.0 + e2 * (9.0 / 88.0 * e2 - 3.0 / 14.0 - 9.0 / 52.0 * e3) + e3 / 6.0 - 3.0 / 22.0 * e4 + 3.0 / 26.0 * e5;
    return 3.0 * sum + scale * series / (mean * crd::math::sqrt(mean));
}
} // namespace detail

// Carlson symmetric integrals (public).
template <Real T>
[[nodiscard]] T elliprf(T x, T y, T z) noexcept
{
    return static_cast<T>(detail::carlson_rf(static_cast<double>(x), static_cast<double>(y), static_cast<double>(z)));
}
template <Real T>
[[nodiscard]] T elliprc(T x, T y) noexcept
{
    return static_cast<T>(detail::carlson_rc(static_cast<double>(x), static_cast<double>(y)));
}
template <Real T>
[[nodiscard]] T elliprd(T x, T y, T z) noexcept
{
    return static_cast<T>(detail::carlson_rd(static_cast<double>(x), static_cast<double>(y), static_cast<double>(z)));
}
template <Real T>
[[nodiscard]] T elliprj(T x, T y, T z, T p) noexcept
{
    return static_cast<T>(
        detail::carlson_rj(static_cast<double>(x), static_cast<double>(y), static_cast<double>(z),
                           static_cast<double>(p)));
}

namespace detail
{
// Complete K(m) + E(m) via the arithmetic-geometric mean (the canonical fast method — ~5 iterations, no sqrt-heavy
// Carlson duplication). K(m)=π/(2·AGM(1,√(1−m))); E(m)=K(m)·(1 − Σ 2^{n−1} c_n²). Parameter m = k².
inline void ellint_ke_agm(double m, double& kk, double& ee) noexcept
{
    double a = 1.0;
    double b = crd::math::sqrt(1.0 - m);
    double sum = 0.5 * m; // n=0: 2^{−1} c₀², c₀ = √m
    double twon = 1.0;
    for (int n = 1; n <= 60; ++n)
    {
        const double c = 0.5 * (a - b);
        const double an = 0.5 * (a + b);
        const double bn = crd::math::sqrt(a * b);
        twon *= 2.0;
        sum += 0.5 * twon * c * c; // 2^{n−1} c_n²
        a = an;
        b = bn;
        if (crd::math::fabs(c) <= 1e-17 * a)
        {
            break;
        }
    }
    kk = 1.5707963267948966192313216916397514 / a; // π/(2·AGM)
    ee = kk * (1.0 - sum);
}
} // namespace detail

// Complete: K(m), E(m), parameter m=k² (scipy.special.ellipk/ellipe). Iteration-free Cody form on m∈[0,1) (v12-d
// perf): F = A(m1) + (−ln m1)·B(m1), m1 = 1−m — the −ln m1 carries the m→1 log-branch, so one fast path covers [0,1)
// with no AGM sqrt-chain. AGM fallback for m<0 (real K) and m≥1.
template <Real T>
[[nodiscard]] T ellint_k(T m) noexcept
{
    const double md = static_cast<double>(m);
    if (md >= 0.0 && md < 1.0)
    {
        const double m1 = 1.0 - md;
        return static_cast<T>(detail::horner_t(detail::kEllKcodyA, m1) -
                              crd::math::log(m1) * detail::horner_t(detail::kEllKcodyB, m1));
    }
    double kk = 0.0;
    double ee = 0.0;
    detail::ellint_ke_agm(md, kk, ee);
    return static_cast<T>(kk);
}
template <Real T>
[[nodiscard]] T ellint_e(T m) noexcept
{
    const double md = static_cast<double>(m);
    if (md >= 0.0 && md < 1.0)
    {
        const double m1 = 1.0 - md;
        return static_cast<T>(detail::horner_t(detail::kEllEcodyA, m1) -
                              crd::math::log(m1) * detail::horner_t(detail::kEllEcodyB, m1));
    }
    double kk = 0.0;
    double ee = 0.0;
    detail::ellint_ke_agm(md, kk, ee);
    return static_cast<T>(ee);
}

// Incomplete: F(φ,m)=sinφ·R_F(cos²φ,1−m sin²φ,1); E(φ,m)=sinφ·R_F − (m/3)sin³φ·R_D (scipy ellipkinc/ellipeinc).
template <Real T>
[[nodiscard]] T ellint_f(T phi, T m) noexcept
{
    const double s = crd::math::sin(static_cast<double>(phi));
    const double c = crd::math::cos(static_cast<double>(phi));
    const double md = static_cast<double>(m);
    return static_cast<T>(s * detail::carlson_rf(c * c, 1.0 - md * s * s, 1.0));
}
template <Real T>
[[nodiscard]] T ellint_e_inc(T phi, T m) noexcept
{
    const double s = crd::math::sin(static_cast<double>(phi));
    const double c = crd::math::cos(static_cast<double>(phi));
    const double md = static_cast<double>(m);
    const double rf = detail::carlson_rf(c * c, 1.0 - md * s * s, 1.0);
    const double rd = detail::carlson_rd(c * c, 1.0 - md * s * s, 1.0);
    return static_cast<T>(s * rf - md / 3.0 * s * s * s * rd);
}

// Jacobi elliptic functions sn/cn/dn at argument u, parameter m=k² (scipy.special.ellipj convention). The AGM chain
// a_i = (a_{i−1}+b_{i−1})/2, b_i = √(a_{i−1}b_{i−1}) (a₀ = 1, b₀ = k′) is the descending Landen/Gauss transformation
// (DLMF §22.7(i), §19.8): the modulus k_i = (a_{i−1}−b_{i−1})/(a_{i−1}+b_{i−1}) shrinks quadratically and the argument
// scales by a_i/a_{i−1}. At the bottom of the chain sn, cn, dn are sin, cos, 1; DLMF 22.7.1–22.7.3 carries them up one
// level (A = 1+k_i = a_{i−1}/a_i, B = 1−k_i = b_{i−1}/a_i):
//   sn = A·s/(1+k_i s²),  cn = c·d/(1+k_i s²),  dn = (B + k_i c²)/(1+k_i s²).
// Dividing through by sn² turns this into a recurrence for x = cn/sn and d = dn alone,
//   x_up = x·d/A,   d_up = (B + x²)/(A + x²),
// whose x-update uses the previous d (so a level's division overlaps the next level's work); at the top
// sn = ±1/√(1+x²), cn = x·sn (the sign of sn is that of sin of the bottom phase: every factor above is positive).
// The CANONICAL home for the Jacobi functions (hesap-dsp's ellipj is a filter-design copy). Gated vs
// scipy.special.ellipj to <1e-13.
template <Real T>
inline void ellipj(T u_in, T m, T& sn, T& cn, T& dn) noexcept
{
    constexpr double ca = 1.0e-8; // AGM convergence (gives full double precision after the back-substitution)
    constexpr int kLevels = 13;
    double emc = 1.0 - static_cast<double>(m);
    double u = static_cast<double>(u_in);
    if (emc == 0.0) // m = 1
    {
        cn = static_cast<T>(1.0 / crd::math::cosh(u));
        dn = cn;
        sn = static_cast<T>(crd::math::tanh(u));
        return;
    }
    const bool reciprocal = (emc < 0.0);
    double root_m = 1.0;
    if (reciprocal) // m > 1: reciprocal modulus, sn(u|m) = sn(√m·u | 1/m)/√m (DLMF §22.17(i), 22.17.2–22.17.4)
    {
        root_m = crd::math::sqrt(1.0 - emc);
        emc = -emc / (1.0 - emc);
        u *= root_m;
    }
    double up[kLevels];     // A of each level
    double down[kLevels];   // B of each level
    double up_inv[kLevels]; // 1/A of each level
    double a = 1.0;
    double b = crd::math::sqrt(emc);
    double a_recip = 1.0; // 1/a_{i−1}
    int levels = 0;
    for (;;)
    {
        const double a_next = 0.5 * (a + b);
        const double next_recip = 1.0 / a_next;
        up[levels] = a * next_recip;
        down[levels] = b * next_recip;
        up_inv[levels] = a_next * a_recip;
        ++levels;
        if (crd::math::fabs(a - b) <= ca * a || levels == kLevels)
        {
            a = a_next;
            break;
        }
        b = crd::math::sqrt(a * b);
        a = a_next;
        a_recip = next_recip;
    }
    const double phase = u * a;
    double s0;
    double c0;
    crd::math::sincos(phase, s0, c0);
    // |u| < 1e-150: sn = u, cn = dn = 1 to full precision (and x = cn/sn would overflow).
    if (crd::math::fabs(s0) < 1.0e-150)
    {
        sn = u_in;
        cn = static_cast<T>(1);
        dn = static_cast<T>(1);
        return;
    }
    double x = c0 / s0;
    double d = 1.0;
    for (int level = levels - 1; level >= 0; --level)
    {
        const double x2 = x * x;
        const double d_up = (down[level] + x2) / (up[level] + x2);
        x = x * d * up_inv[level];
        d = d_up;
    }
    const double sn_mag = 1.0 / crd::math::sqrt(1.0 + x * x);
    const double s = (s0 >= 0.0) ? sn_mag : -sn_mag;
    const double c = x * s;
    if (reciprocal)
    {
        sn = static_cast<T>(s / root_m);
        cn = static_cast<T>(d);
        dn = static_cast<T>(c);
    }
    else
    {
        sn = static_cast<T>(s);
        cn = static_cast<T>(c);
        dn = static_cast<T>(d);
    }
}

// Π(n,φ,m) = ∫₀^φ dθ/((1−n sin²θ)√(1−m sin²θ)) = sinφ·R_F + (n/3)sin³φ·R_J(...,1−n sin²φ). (Boost/DLMF sign of n.)
template <Real T>
[[nodiscard]] T ellint_pi(T n, T phi, T m) noexcept
{
    const double s = crd::math::sin(static_cast<double>(phi));
    const double c = crd::math::cos(static_cast<double>(phi));
    const double md = static_cast<double>(m);
    const double nd = static_cast<double>(n);
    const double s2 = s * s;
    const double rf = detail::carlson_rf(c * c, 1.0 - md * s2, 1.0);
    const double rj = detail::carlson_rj(c * c, 1.0 - md * s2, 1.0, 1.0 - nd * s2);
    return static_cast<T>(s * rf + nd / 3.0 * s * s2 * rj);
}

} // namespace crd::hesap::special
