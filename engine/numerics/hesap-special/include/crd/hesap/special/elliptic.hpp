#pragma once

// crd-hesap-special v12-d — elliptic integrals, the CANONICAL family (the hesap-dsp ellipk/ellipj are a DSP-internal
// filter-design subset; this is the special-functions home — SANITY rule 8). Carlson symmetric integrals R_F/R_C/R_D/
// R_J (the duplication algorithm of Carlson 1995, Numer. Algorithms 10, DLMF §19.36) are the engine; Legendre-form
// K/E/F/E/Π are thin wrappers.
//   R_F(x,y,z)=½∫₀^∞ dt/√((t+x)(t+y)(t+z));  K(m)=R_F(0,1−m,1);  E(m)=R_F − (m/3)R_D;
//   F(φ,m)=sinφ·R_F(cos²φ, 1−m sin²φ, 1);  Π(n,φ,m) via R_J.  Parameter convention m=k² (scipy.special).
// Gated vs scipy.special elliprf/elliprc/elliprd/elliprj + ellipk/ellipe/ellipkinc/ellipeinc to <1e-12. Internals f64.
// F, E and Π continue quasi-periodically past |φ| = π/2 (DLMF §19.2(ii), 19.7(i)): F(φ+jπ) = F(φ) + 2jK, likewise E, Π.
// Non-finite and degenerate arguments of the Carlson integrals follow the conventions of scipy.special: a NaN or
// negative argument gives NaN; an integral that diverges (two vanishing arguments, R_D with z = 0) gives +∞; an
// infinite argument gives 0. Arguments over the whole double range, to the smallest subnormal and the largest finite
// value, are valued by all four integrals. F and E at φ = ±∞ are ±∞ and E at m = −∞ is ∞ (F(φ, −∞) = 0); Π at
// φ = ±∞ is ±∞ with the sign of its complete integral, which at m = 1 or n = 1 is itself infinite.

#include <crd/hesap/special/gamma.hpp>     // Real concept
#include <crd/hesap/special/poly_eval.hpp> // detail::horner_t

#include "elliptic_poly.inc" // GENERATED iteration-free K(m)/E(m) Cody-form coeffs (gen_elliptic_poly.py; v12-d perf)

#include <algorithm>
#include <cmath>
#include <crd/core/platform.hpp> // CRD_NOINLINE
#include <crd/math/cmath.hpp>
#include <limits>
#include <utility>

namespace crd::hesap::special
{
namespace detail
{
// A converged duplication leaves every relative deviation below the series tolerance (≤ 0.0066); one still above this
// bound has not closed.
inline constexpr double kUnclosed = 0.5;
// Past 4^{−n} below this (n ≥ 33 steps, only for arguments many orders of magnitude apart) the mean is re-formed.
inline constexpr double kManySteps = 1.0e-20;
// Arguments all below kLiftBelow (down to the smallest subnormal, 2^−1074, whose products with each other the loops
// cannot form) are lifted by 2^600 first, an exact scaling; R(c·args) = c^{−1/2}R(args) (R_D: c^{−3/2}) gives the value
// back through the exact powers of two kLiftRoot = 2^300 and kLiftCube = 2^900.
inline constexpr double kLiftBelow = 1.0e-200;
inline constexpr double kLift = 0x1p600;
inline constexpr double kLiftRoot = 0x1p300;
inline constexpr double kLiftCube = 0x1p900;
// The principal value of R_J with y + |p| beyond the double range is formed from the arguments scaled by 2^−700 (the
// loops then hold them all; R_J(c·) = c^{−3/2}R_J, so the value is multiplied by 2^−1050). It underflows: it is below
// 10⁻⁴⁴⁰.
inline constexpr double kDrop = 0x1p-700;
inline constexpr double kDropCube = 0x1p-1050;
inline constexpr double kInfinity = std::numeric_limits<double>::infinity();
inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// R_J: the duplication forms α = (p(√x+√y+√z) + √x√y√z)² and β = p(p+λ)², of the third degree in the arguments, and
// takes R_C(α, β) of them. R_C is logarithmic in β beside α (R_C(α, β) ≈ ln(4α/β)/(2√α)), so a β in the subnormal
// range, which holds fewer than 52 bits, moves the sum by up to 10⁻³, and a β that underflows to zero diverges. A β or
// α beyond 4·10³⁰⁷ overflows the R_C step to NaN. The loop therefore hands the arguments to carlson_rj_slow at the
// first β that is not a normal number, and again if the result is not finite; a p ≤ 0 or NaN goes there before the
// loop starts. A mean below kNormalMin is subnormal: R_F, R_C and R_D hand it to their edge function, while a mean
// from there up to 10⁻²⁹⁰ keeps the loop (the loops are accurate as long as the arguments are normal numbers).
inline constexpr double kNormalMin = std::numeric_limits<double>::min();
inline constexpr double kFiniteMax = std::numeric_limits<double>::max();

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

// The loops below take `kClassify`: a closing guard that fails (NaN, ∞, a diverging integral) returns the classified
// value of the edge functions, which in turn may retry a scaled argument with the loop that does not classify.
template <bool kClassify> [[nodiscard]] inline double carlson_rf_impl(double x, double y, double z) noexcept;
template <bool kClassify> [[nodiscard]] inline double carlson_rc_impl(double x, double y) noexcept;
template <bool kClassify> [[nodiscard]] inline double carlson_rd_impl(double x, double y, double z) noexcept;

// What the loops cannot value, classified (reached when the mean did not close or left the double range: a NaN or
// infinite argument, a diverging integral, an argument above 6·10³⁰⁷ where the duplication's own sums overflow, or
// arguments so small that the subnormal range loses them). A finite value the loop that does not classify gives is
// the value (a huge argument beside a subnormal one overflows nothing); otherwise the homogeneity R(c·args) =
// c^{−1/2}R(args) (R_D: c^{−3/2}) with an exact power of two as c moves the arguments away from the edge of the range:
// c = 2^600 where all of them are tiny (the subnormal range holds too few bits for the loops), c = 1/16 below an
// overflow.
CRD_NOINLINE inline double carlson_rf_edge(double x, double y, double z) noexcept
{
    if (!(x >= 0.0 && y >= 0.0 && z >= 0.0)) // NaN or negative
    {
        return kNaN;
    }
    if (x == kInfinity || y == kInfinity || z == kInfinity)
    {
        return 0.0;
    }
    if (static_cast<int>(x == 0.0) + static_cast<int>(y == 0.0) + static_cast<int>(z == 0.0) >= 2)
    {
        return kInfinity; // the integral diverges at t = 0
    }
    if (std::max({x, y, z}) < kLiftBelow)
    {
        return kLiftRoot * carlson_rf_impl<false>(kLift * x, kLift * y, kLift * z);
    }
    const double direct = carlson_rf_impl<false>(x, y, z);
    if (direct == direct)
    {
        return direct;
    }
    return 0.25 * carlson_rf_impl<false>(0.0625 * x, 0.0625 * y, 0.0625 * z);
}
CRD_NOINLINE inline double carlson_rd_edge(double x, double y, double z) noexcept
{
    if (!(x >= 0.0 && y >= 0.0 && z >= 0.0))
    {
        return kNaN;
    }
    if (z == 0.0)
    {
        return kInfinity; // the weight t^{−3/2} of z is not integrable at t = 0
    }
    if (x == kInfinity || y == kInfinity || z == kInfinity)
    {
        return 0.0;
    }
    if (x == 0.0 && y == 0.0)
    {
        return kInfinity;
    }
    if (std::max({x, y, z}) < kLiftBelow)
    {
        return kLiftCube * carlson_rd_impl<false>(kLift * x, kLift * y, kLift * z);
    }
    const double direct = carlson_rd_impl<false>(x, y, z);
    if (direct == direct)
    {
        return direct;
    }
    if (z < 1.0e-280)
    {
        // Two arguments of at least 10²⁰⁰ overflowed the sums, and the third is too small to survive the scaling: its
        // weight t^{−3/2} dominates, R_D = 3/√(xyz) (the next term is smaller by √(z/min(x, y)) < 10⁻⁴⁵).
        return ((3.0 / crd::math::sqrt(z)) / crd::math::sqrt(x)) / crd::math::sqrt(y);
    }
    return 0.015625 * carlson_rd_impl<false>(0.0625 * x, 0.0625 * y, 0.0625 * z);
}
CRD_NOINLINE inline double carlson_rc_edge(double x, double y) noexcept
{
    if (!(x >= 0.0) || y != y)
    {
        return kNaN;
    }
    if (x == kInfinity)
    {
        return (y > 0.0) ? 0.0 : kNaN;
    }
    if (y == kInfinity || y == -kInfinity)
    {
        return 0.0;
    }
    if (y == 0.0)
    {
        return kInfinity; // the weight 1/t of the integrand is not integrable at t = 0
    }
    if (std::max(x, crd::math::fabs(y)) < kLiftBelow)
    {
        return kLiftRoot * carlson_rc_impl<false>(kLift * x, kLift * y);
    }
    const double direct = carlson_rc_impl<false>(x, y);
    if (direct == direct)
    {
        return direct;
    }
    return 0.25 * carlson_rc_impl<false>(0.0625 * x, 0.0625 * y);
}

// R_F(x,y,z): x,y,z ≥ 0, at most one zero. Series through the fifth order in the scaled deviations X,Y,Z (Z = −X−Y):
// R_F = A^{−1/2}·(1 − E₂/10 + E₃/14 + E₂²/24 − 3E₂E₃/44), E₂ = XY − Z², E₃ = XYZ (DLMF §19.36).
template <bool kClassify> [[nodiscard]] inline double carlson_rf_impl(double x, double y, double z) noexcept
{
    constexpr double errtol = 0.0066; // series error ~errtol⁶ ≈ 8e-14 < the 1e-12 gate ⇒ one fewer duplication (v12-d)
    constexpr double third = 1.0 / 3.0;
    double mean = (0.25 * x + 0.25 * y + 0.25 * z) * (4.0 * third);
    const double spread_x = mean - x;
    const double spread_y = mean - y;
    const double widest =
        std::max({crd::math::fabs(spread_x), crd::math::fabs(spread_y), crd::math::fabs(spread_x + spread_y)});
    // 16·mean overflows (to ∞, and 0·∞ is NaN) exactly when the mean is beyond 1.1·10³⁰⁷, infinite or NaN, so that the
    // scale is NaN there and the loop, which cannot hold such arguments, leaves after one step for the cold block.
    double scale = kClassify ? 1.0 + 0.0 * (16.0 * mean) : 1.0;
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
    if (kClassify ? !(scale >= kManySteps && mean >= kNormalMin) : (scale < kManySteps || mean < 1.0e-290))
    {
        if (kClassify && !(mean >= kNormalMin)) // a subnormal mean: too small for the loop's precision (or NaN)
        {
            return carlson_rf_edge(x, y, z);
        }
        mean = (0.25 * rx + 0.25 * ry + 0.25 * rz) * (4.0 * third);
        // The spread never closed (two zero arguments: 4^{−n} or A_n underflowed first): the integral diverges. A NaN
        // scale is the mark of a mean beyond 1.1·10³⁰⁷ (or NaN, or infinite) from the start: the loop stopped after one
        // step, and the edge function values it.
        if (scale != scale || scale < 1.0e-290 ||
            !(crd::math::fabs(mean - rx) + crd::math::fabs(mean - ry) + crd::math::fabs(mean - rz) < kUnclosed * mean))
        {
            return kClassify ? carlson_rf_edge(x, y, z) : kNaN;
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
template <bool kClassify> [[nodiscard]] inline double carlson_rc_impl(double x, double y) noexcept
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
    // As in R_F: a NaN scale (a mean beyond 1.1·10³⁰⁷, infinite or NaN) sends the cold block to the edge function.
    double scale = kClassify ? 1.0 + 0.0 * (16.0 * mean) : 1.0;
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
    if (kClassify ? !(scale >= kManySteps && mean >= kNormalMin) : (scale < kManySteps || mean < 1.0e-290))
    {
        if (kClassify && !(mean >= kNormalMin)) // a subnormal mean: too small for the loop's precision (or NaN)
        {
            return carlson_rc_edge(x, y);
        }
        mean = (0.25 * rx + 0.5 * ry) * (4.0 * third);
        // the spread never closed (y = 0: 4^{−n} or A_n underflowed first): the integral diverges
        if (scale != scale || scale < 1.0e-290 || !(crd::math::fabs(ry - mean) < kUnclosed * mean))
        {
            return kClassify ? carlson_rc_edge(x, y) : kNaN;
        }
    }
    const double s = (ry - mean) / mean;
    return pv_factor * (1.0 + s * s * (0.3 + s * (1.0 / 7.0 + s * (0.375 + s * (9.0 / 22.0))))) / crd::math::sqrt(mean);
}

// R_D(x,y,z) = R_J(x,y,z,z). Weighted mean A = (x+y+3z)/5; the duplication also accumulates Σ 4^{−m}/(√z_m(z_m+λ_m)):
// R_D = 3·Σ + 4^{−n}·A^{−3/2}·(1 − 3E₂/14 + E₃/6 + 9E₂²/88 − 3E₄/22 − 9E₂E₃/52 + 3E₅/26) with Z = −(X+Y)/3,
// E₂ = XY − 6Z², E₃ = (3XY − 8Z²)Z, E₄ = 3(XY − Z²)Z², E₅ = XYZ³ (DLMF §19.36, terms through the fifth order).
template <bool kClassify> [[nodiscard]] inline double carlson_rd_impl(double x, double y, double z) noexcept
{
    constexpr double errtol = 0.0010;
    double mean = (0.25 * x + 0.25 * y + 0.75 * z) * 0.8;
    const double spread_x = mean - x;
    const double spread_y = mean - y;
    // The deviations of x, y, z from the weighted mean sum to zero with weights 1, 1, 3, so that |mean − z| is at most
    // two thirds of the larger of the other two: the widest is one of those.
    const double widest = std::max(crd::math::fabs(spread_x), crd::math::fabs(spread_y));
    // 0·(16·mean) is NaN exactly when the mean is beyond 1.1·10³⁰⁷, infinite or NaN, and the mean then leaves the loop
    // after one step for the cold block and the edge function (which values the scaled arguments or classifies them).
    if (kClassify)
    {
        mean += 0.0 * (16.0 * mean);
    }
    double rx = x;
    double ry = y;
    double rz = z + 0.0; // −0 becomes +0, whose sum diverges to +∞ rather than −∞
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
    if (kClassify ? (scale < kManySteps || !(mean >= kNormalMin)) : (scale < kManySteps || mean < 1.0e-290))
    {
        if (kClassify && !(mean >= kNormalMin)) // a subnormal mean: too small for the loop's precision (or NaN)
        {
            return carlson_rd_edge(x, y, z);
        }
        mean = (0.25 * rx + 0.25 * ry + 0.75 * rz) * 0.8;
        // The spread never closed (two zero arguments: 4^{−n} or A_n underflowed first): the integral diverges.
        if (scale < 1.0e-290 ||
            !(crd::math::fabs(mean - rx) + crd::math::fabs(mean - ry) + crd::math::fabs(mean - rz) < kUnclosed * mean))
        {
            return kClassify ? carlson_rd_edge(x, y, z) : kNaN;
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

[[nodiscard]] inline double carlson_rf(double x, double y, double z) noexcept
{
    return carlson_rf_impl<true>(x, y, z);
}
[[nodiscard]] inline double carlson_rc(double x, double y) noexcept
{
    return carlson_rc_impl<true>(x, y);
}
[[nodiscard]] inline double carlson_rd(double x, double y, double z) noexcept
{
    return carlson_rd_impl<true>(x, y, z);
}

// R_J(x,y,z,p) for p > 0: weighted mean A = (x+y+z+2p)/5; each duplication adds
// 3·4^{−m}·R_C(α_m, β_m), α = (p(√x+√y+√z) + √x√y√z)², β = p(p+λ)² (Carlson 1995; DLMF §19.26), and the tail
// is 4^{−n}·A^{−3/2}·(1 − 3E₂/14 + E₃/6 + 9E₂²/88 − 3E₄/22 − 9E₂E₃/52 + 3E₅/26) with P = −(X+Y+Z)/2,
// E₂ = XY+XZ+YZ − 3P², E₃ = XYZ + 2E₂P + 4P³, E₄ = (2XYZ + E₂P + 3P³)P, E₅ = XYZP² (DLMF §19.36).
// The cases the loop cannot carry (a p ≤ 0, NaN or infinite arguments, a diverging integral, α or β outside the
// double range) are recomputed by carlson_rj_slow, which hands the positive p to carlson_rj_wide.
[[nodiscard]] inline double carlson_rj(double x, double y, double z, double p) noexcept;

// R_J for a positive p when the main duplication cannot hold α and β: the same iteration, with the R_C step written in
// degree-one quantities. R_C is homogeneous, R_C(c²a, c²b) = R_C(a, b)/c, so with c = p+λ the step is
// R_C(q², p)/(p+λ), q = (p(√x+√y+√z) + √x√y√z)/(p+λ) = (p/(p+λ))(√x+√y+√z) + (√x√y/(p+λ))√z, all factors of which
// are at most the largest square root. The scale and the tail are divided out in an order that keeps their quotients
// representable. A p beyond 10⁴⁰ times every other argument would need more than 66 steps (and 4^{−n} leaves the
// double range when p exceeds them by 10²⁹⁰): there R_J = 3R_F(x,y,z)/p, the integral beyond t ≈ p being a relative
// √(max/p) ≤ 10⁻²⁰ of it. Returns weight·R_J: the iteration is linear in its scale, so a factor that the caller would
// multiply by (carlson_rj_principal) goes in at the start, where it cannot meet an underflowed R_J.
// x, y, z ≥ 0 with at most one zero, p > 0, all finite.
CRD_NOINLINE inline double carlson_rj_wide(double x, double y, double z, double p, double weight = 1.0) noexcept
{
    constexpr double errtol = 0.0010;
    if (std::max({x, y, z, p}) > 1.0e300) // the sums λ and mean of the duplication overflow near the top of the range
    {
        return 0.015625 *
               carlson_rj_wide(0.0625 * x, 0.0625 * y, 0.0625 * z, 0.0625 * p, weight); // R_J(c·) = c^{−3/2}R_J
    }
    if (p > 1.0e40 * std::max({x, y, z}))
    {
        return 3.0 * (weight / p) * carlson_rf(x, y, z);
    }
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
        const double shift = rp + lambda;
        const double q = (rp / shift) * (sx + sy + sz) + (sx * sy / shift) * sz;
        sum += (scale * weight / shift) * carlson_rc_impl<false>(q * q, rp);
        scale *= 0.25;
        rx = 0.25 * (rx + lambda);
        ry = 0.25 * (ry + lambda);
        rz = 0.25 * (rz + lambda);
        rp = 0.25 * (rp + lambda);
        mean = 0.25 * (mean + lambda);
        if (!(widest * scale > errtol * mean))
        {
            break;
        }
    }
    // After many steps the carried mean has drifted from the final arguments by one rounding per step: re-form it.
    if (scale < kManySteps || mean < 1.0e-290)
    {
        mean = (0.25 * rx + 0.25 * ry + 0.25 * rz + 0.5 * rp) * 0.8;
        if (scale < 1.0e-290 ||
            !(crd::math::fabs(mean - rx) + crd::math::fabs(mean - ry) + crd::math::fabs(mean - rz) < kUnclosed * mean))
        {
            return kNaN;
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
    return 3.0 * sum + (scale * weight * series / mean) / crd::math::sqrt(mean);
}

// a·c/b for b > 0, with the quotient a/b kept from leaving the double range.
[[nodiscard]] inline double ratio_times(double a, double b, double c) noexcept
{
    const double ratio = a / b;
    const double size = crd::math::fabs(ratio);
    return (size > 1.0e-290 && size < 1.0e290) ? ratio * c : (a * c) / b;
}

// Gauss–Legendre rule of 16 points: the positive nodes and their weights on [−1, 1] (the rule is symmetric).
inline constexpr double kGlNode[8] = {9.50125098376374405e-02, 2.81603550779258915e-01, 4.58016777657227370e-01,
                                      6.17876244402643771e-01, 7.55404408355002999e-01, 8.65631202387831755e-01,
                                      9.44575023073232600e-01, 9.89400934991649939e-01};
inline constexpr double kGlWeight[8] = {1.89450610455068502e-01, 1.82603415044923584e-01, 1.69156519395002536e-01,
                                        1.49595988816576736e-01, 1.24628971255533877e-01, 9.51585116824927857e-02,
                                        6.22535239386478936e-02, 2.71524594117540964e-02};
// Where the result of the relation of carlson_rj_principal is below this fraction of its terms (the terms good to about
// 10⁻¹⁵ each, so that the result keeps 2·10⁻¹³ or better above it) carlson_rj_cancelled is tried.
inline constexpr double kRjCancelled = 0.005;

// g(t) = (1/√((t+x)(t+y)) − 1/(t+c))/√(t+z) for c = (x+y)/2 + e, without the cancellation of its two terms: with
// Q = (t+x)(t+y), 1/√Q − 1/(t+c) = ((t+c)² − Q)/((t+c)√Q((t+c) + √Q)) and (t+c)² − Q = h² + e(2(t+c) − e), h = (y−x)/2
// (half2 = h², slack = e).
[[nodiscard]] inline double rj_gap(double t, double x, double y, double z, double centre, double half2,
                                   double slack) noexcept
{
    const double root = crd::math::sqrt(t + x) * crd::math::sqrt(t + y);
    const double shifted = t + centre;
    return (half2 + slack * (2.0 * shifted - slack)) / (crd::math::sqrt(t + z) * shifted * root * (shifted + root));
}

// The integrand of the remainder, [g(t) − g(q)·4q²/(t+q)²]/(t−q): the pole of the principal value at t = q is removed
// by a function that equals g there (gq) and whose principal value over (0, ∞) is known.
[[nodiscard]] inline double rj_remainder_term(double t, double gq, double q, double x, double y, double z,
                                              double centre, double half2, double slack) noexcept
{
    const double ratio = q / (t + q);
    return (rj_gap(t, x, y, z, centre, half2, slack) - gq * 4.0 * ratio * ratio) / (t - q);
}

// The Gauss–Legendre sum of the remainder's integrand over [lo, hi]; `size` gains the same sum of the magnitudes.
[[nodiscard]] inline double rj_panel(double lo, double hi, double& size, double gq, double q, double x, double y,
                                     double z, double centre, double half2, double slack) noexcept
{
    const double middle = 0.5 * (lo + hi);
    const double radius = 0.5 * (hi - lo);
    double sum = 0.0;
    double sum_size = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        const double offset = radius * kGlNode[i];
        const double below = rj_remainder_term(middle - offset, gq, q, x, y, z, centre, half2, slack);
        const double above = rj_remainder_term(middle + offset, gq, q, x, y, z, centre, half2, slack);
        sum += kGlWeight[i] * (below + above);
        sum_size += kGlWeight[i] * (crd::math::fabs(below) + crd::math::fabs(above));
    }
    size += radius * sum_size;
    return radius * sum;
}

// R_J(c, c, z, −q) = 3(R_C(z, −q) − R_C(z, c))/(q+c) for 0 < c ≤ z (the partial fractions of 1/((t−q)(t+c)), with
// R_C(z, −q) = asinh√(z/q)/√(z+q), the principal value of DLMF §19.2(iii), and R_C(z, c) = asinh√((z−c)/c)/√(z−c), DLMF
// §19.6): the difference of the two is a sum of logarithms of numbers near one, so that nothing cancels, even where the
// result is a part in z/c of either R_C. `size` receives the sum of the magnitudes of the two terms.
[[nodiscard]] inline double rj_equal_pair(double centre, double z, double q, double& size) noexcept
{
    const double root_c = crd::math::sqrt(centre);
    const double root_z = crd::math::sqrt(z);
    const double root_zq = crd::math::sqrt(z + q);
    const double root_zc = crd::math::sqrt(z - centre);
    // asinh√(z/q) − asinh√((z−c)/c) = ½ln(c/q) + ln((√z+√(z+q))/(√z+√(z−c))) = ½ln(c/q) + ln(1 + u), whose quotient by
    // q+c is formed with u/(q+c) = 1/denominator, so that a u below the subnormal range costs nothing
    const double denominator = (root_zq + root_zc) * (root_z + root_zc);
    const double u = (q + centre) / denominator;
    const double log_ratio = (u < 1.0e-5) ? 1.0 - u * (0.5 - u * (1.0 / 3.0 - u * 0.25)) : crd::math::log1p(u) / u;
    // asinh(w)/√(z−c) for w = √((z−c)/c), which is 1/√c for w → 0 and, beyond w = 10⁸ (where w itself may overflow),
    // uses asinh(w) = ln(2w) = ln(2√(z−c)) − ln√c
    const double w = root_zc / root_c;
    double second = 0.0;
    if (w > 1.0e-3)
    {
        const double asinh_w =
            (w > 1.0e8) ? crd::math::log(2.0 * root_zc) - crd::math::log(root_c) : crd::math::asinh(w);
        second = asinh_w / (root_zc * (root_zq + root_zc));
    }
    else
    {
        second = (1.0 - w * w * (1.0 / 6.0 - w * w * (3.0 / 40.0))) / (root_c * (root_zq + root_zc));
    }
    // the common factor 1/√(z+q) divided last, so that a result beyond the double range is ∞ rather than ∞ − ∞
    const double first = 0.5 * crd::math::log1p((centre - q) / q) / (q + centre) + log_ratio / denominator;
    size = 3.0 * (crd::math::fabs(first) + crd::math::fabs(second)) / root_zq;
    return 3.0 * (first - second) / root_zq;
}

// R_J(x, y, z, −q), the Cauchy principal value, for 0 < x ≤ y ≤ z and q > 0 when the terms of the relation of
// carlson_rj_principal cancel (p near −y with x near y and z ≫ y: the value is a part in z/y of the terms). R_J is
// (3/2)·PV∫₀^∞ dt/((t−q)√((t+x)(t+y)(t+z))) (DLMF §19.16(i)). With the mean c = (x+y)/2 in place of the two smaller
// arguments the integral is elementary (rj_equal_pair), which for x = y is the whole value, at any range of the
// arguments. What remains, (3/2)·PV∫ g(t)/(t−q) dt with g from rj_gap, is smooth and as small as (y−x)². It is
// integrated by Gauss–Legendre: on one panel [0, a₀] below the smallest scale (a₀ = q4^{−k} ≤ min(x, q), within which g
// is analytic far beyond the panel), then on the panels [a₀4^j, a₀4^{j+1}] (ratio 4, which keeps the singularities of g
// at the negative arguments far enough from every panel for 16 points to give 10⁻¹⁵) up to 10⁶ times the largest scale,
// beyond which the term that does not cancel, g(q)·4q²/(t+q)²/(t−q), is integrated in closed form (the rest falls like
// t^{−7/2} and is below 10⁻²⁰). The pole is subtracted as g(q)·4q²/(t+q)², whose principal value PV∫₀^∞
// 4q²/((t+q)²(t−q)) dt is −2 and is added back: the integrand has a removable pole at t = q, which no node meets (q is
// a panel edge). The arguments are first scaled by a power of four so that y is near one (the result by its exact power
// of two). NaN is returned where that leaves q or x outside 10^{±290}. `size` receives the sum of the magnitudes of the
// terms added (in the units of the result), whose ratio to the result is what limits its precision.
CRD_NOINLINE inline double carlson_rj_cancelled(double x, double y, double z, double q, double& size) noexcept
{
    double elementary_size = 0.0;
    const double elementary = rj_equal_pair(x + 0.5 * (y - x), z, q, elementary_size);
    if (x == y)
    {
        size = elementary_size;
        return elementary;
    }
    int exponent = 0;
    static_cast<void>(std::frexp(y, &exponent));
    const int shift = 2 * (exponent / 2);
    // A z beyond 10²⁹⁰ times y is replaced by 10²⁹⁰: g(t) is a multiple of 1/√(t+z), which is z^{−1/2} wherever g is
    // not negligible (the relative error is below 10⁻²⁹⁰), so that the remainder is that of 10²⁹⁰ times √(10²⁹⁰/z).
    const double root_z = std::ldexp(crd::math::sqrt(z), -shift / 2);
    const double z_factor = (root_z > 1.0e145) ? 1.0e145 / root_z : 1.0;
    x = std::ldexp(x, -shift);
    y = std::ldexp(y, -shift);
    z = std::min(std::ldexp(z, -shift), 1.0e290);
    q = std::ldexp(q, -shift);
    if (!(x >= 1.0e-290 && q >= 1.0e-290 && q <= 1.0e290))
    {
        return kNaN;
    }
    const double half = 0.5 * (y - x);
    const double centre = x + half;
    const double slack = (centre - x) - half; // the rounding of the mean, which the identity of rj_gap carries exactly
    const double half2 = half * half;

    const double gq = rj_gap(q, x, y, z, centre, half2, slack);
    double lower = q;
    const double smallest = std::min(x, q);
    while (lower > smallest)
    {
        lower *= 0.25;
    }
    double magnitude = 2.0 * crd::math::fabs(gq);
    double remainder = rj_panel(0.0, lower, magnitude, gq, q, x, y, z, centre, half2, slack) - 2.0 * gq;
    const double top = 1.0e6 * std::max(z, q);
    while (lower < top)
    {
        remainder += rj_panel(lower, 4.0 * lower, magnitude, gq, q, x, y, z, centre, half2, slack);
        lower *= 4.0;
    }
    // beyond `lower`: ∫ 4q²/((t+q)²(t−q)) dt = ln((S+1)/(S−1)) − 2/(S+1) from S = lower/q to ∞
    const double tail = gq * (crd::math::log1p(2.0 / (lower / q - 1.0)) - 2.0 / (lower / q + 1.0));
    remainder -= tail;
    magnitude += crd::math::fabs(tail);
    const int unscale = -(3 * shift) / 2;
    size = elementary_size + std::ldexp(1.5 * magnitude * z_factor, unscale);
    return elementary + std::ldexp(1.5 * remainder * z_factor, unscale);
}

// R_J for a negative p, the Cauchy principal value (DLMF §19.20.14): with x ≤ y ≤ z and γ = y + (z−y)(y−x)/(y−p) > y,
//   (y−p)·R_J(x,y,z,p) = (γ−y)·R_J(x,y,z,γ) − 3·R_F(x,y,z) + 3·R_C(xz/y, pγ/y),
// whose R_C has a negative second argument (the principal value, DLMF §19.2(iii)). y > 0: two zero arguments diverge.
// The right side subtracts terms of the size of R_F, each good to about 10⁻¹³ (the module gate): where the result is
// less than the fraction kRjCancelled of them (for x = y at p near −y with z ≫ y, losing log₁₀(z/y) digits) the
// principal value is also evaluated without the subtraction by carlson_rj_cancelled (x > 0 and a moderate ratio of the
// arguments; for a zero x the relation is kept) and the better conditioned of the two is returned. A |p| beyond 10⁴⁰
// times every other argument gives R_J = 3R_F/p, as for a positive p (carlson_rj_wide).
CRD_NOINLINE inline double carlson_rj_principal(double x, double y, double z, double p) noexcept
{
    if (x > y)
    {
        std::swap(x, y);
    }
    if (y > z)
    {
        std::swap(y, z);
    }
    if (x > y)
    {
        std::swap(x, y);
    }
    if (-p > 1.0e40 * z)
    {
        return 3.0 * carlson_rf(x, y, z) / p;
    }
    const double gap = y - p;
    if (gap == kInfinity) // y + |p| beyond the double range: R_F < 10⁻¹⁴⁵ beside y − p > 10³⁰⁸, so the value underflows
    {
        // to zero, with the sign that the scaled problem (finite gap) gives
        return kDropCube * carlson_rj_principal(kDrop * x, kDrop * y, kDrop * z, kDrop * p);
    }
    const double lead = ratio_times(y - x, gap, z - y); // γ − y = (z−y)(y−x)/(y−p), without the cancellation
    const double gamma = y + lead;
    double lead_term = 0.0; // (γ−y)·R_J(x,y,z,γ)
    if (lead != 0.0)
    {
        const double rj_gamma = carlson_rj(x, y, z, gamma);
        // a product that would underflow or overflow is formed inside the iteration instead
        lead_term =
            (rj_gamma > 1.0e-200 && rj_gamma < 1.0e200) ? lead * rj_gamma : carlson_rj_wide(x, y, z, gamma, lead);
    }
    const double rf_term = 3.0 * carlson_rf(x, y, z);
    const double rc_term = 3.0 * carlson_rc(ratio_times(x, y, z), ratio_times(p, y, gamma));
    const double sum = lead_term - rf_term + rc_term;
    const double terms = crd::math::fabs(lead_term) + rf_term + crd::math::fabs(rc_term);
    if (x > 0.0 && !(crd::math::fabs(sum) >= kRjCancelled * terms))
    {
        double size = 0.0;
        const double split = carlson_rj_cancelled(x, y, z, -p, size);
        // the better conditioned of the two: terms/|sum| and size/|split| are the factors by which their roundings grow
        // (an overflow of the result or a relation that is not a number decides for the new evaluation)
        if (split == split &&
            (crd::math::fabs(split) == kInfinity || !(size * crd::math::fabs(sum) >= terms * crd::math::fabs(split))))
        {
            return split;
        }
    }
    return sum / gap;
}

// Everything the main duplication does not do: classify NaN, negative, infinite and diverging arguments, then the
// principal value for p < 0 and the wide-range iteration for p > 0.
CRD_NOINLINE inline double carlson_rj_slow(double x, double y, double z, double p) noexcept
{
    if (!(x >= 0.0 && y >= 0.0 && z >= 0.0) || p != p || p == 0.0)
    {
        return kNaN; // NaN, a negative x, y or z, or the pole at p = 0
    }
    if (static_cast<int>(x == 0.0) + static_cast<int>(y == 0.0) + static_cast<int>(z == 0.0) >= 2)
    {
        return kInfinity; // the integral diverges at t = 0
    }
    if (x == kInfinity || y == kInfinity || z == kInfinity || p == kInfinity || p == -kInfinity)
    {
        return 0.0;
    }
    return (p < 0.0) ? carlson_rj_principal(x, y, z, p) : carlson_rj_wide(x, y, z, p);
}

[[nodiscard]] inline double carlson_rj(double x, double y, double z, double p) noexcept
{
    constexpr double errtol = 0.0010;
    if (CRD_UNLIKELY(!(p > 0.0))) // a pole, a principal value, or NaN
    {
        return carlson_rj_slow(x, y, z, p);
    }
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
        const double alpha = root * root;
        const double beta = rp * shift * shift;
        if (CRD_UNLIKELY(!(beta >= kNormalMin))) // a subnormal, zero or negative β, or NaN
        {
            return carlson_rj_slow(x, y, z, p);
        }
        sum += scale * carlson_rc_impl<false>(alpha, beta);
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
            return carlson_rj_slow(x, y, z, p);
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
    const double value = 3.0 * sum + scale * series / (mean * crd::math::sqrt(mean));
    if (CRD_UNLIKELY(!(crd::math::fabs(value) <= kFiniteMax))) // an overflow of α or β, or NaN
    {
        return carlson_rj_slow(x, y, z, p);
    }
    return value;
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
    if (!(b > 0.0 && b < kInfinity)) // the limits of the chain: m = 1 (AGM = 0), m = −∞, and m > 1 or NaN (no real K)
    {
        kk = (b == 0.0) ? kInfinity : ((b == kInfinity) ? 0.0 : kNaN); // K(1) = ∞, K(−∞) = 0
        ee = (b == 0.0) ? 1.0 : ((b == kInfinity) ? kInfinity : kNaN); // E(1) = 1, E(−∞) = ∞
        return;
    }
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
// with no AGM sqrt-chain. A and B are minimax polynomials for m1 ≥ kEllTail and the exact expansions about m1 = 0
// (DLMF 19.12.1, and E from it) below, where a polynomial cannot follow the logarithm's coefficients to full precision.
// AGM fallback for m<0 (real K) and m≥1 (m = 1: K = ∞, E = 1; m = −∞: K = 0, E = ∞; m > 1: NaN).
template <Real T>
[[nodiscard]] T ellint_k(T m) noexcept
{
    const double md = static_cast<double>(m);
    if (md >= 0.0 && md < 1.0)
    {
        const double m1 = 1.0 - md;
        if (m1 < detail::kEllTail)
        {
            return static_cast<T>(detail::horner_t(detail::kEllKtailA, m1) -
                                  crd::math::log(m1) * detail::horner_t(detail::kEllKtailB, m1));
        }
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
        if (m1 < detail::kEllTail)
        {
            return static_cast<T>(detail::horner_t(detail::kEllEtailA, m1) -
                                  crd::math::log(m1) * detail::horner_t(detail::kEllEtailB, m1));
        }
        return static_cast<T>(detail::horner_t(detail::kEllEcodyA, m1) -
                              crd::math::log(m1) * detail::horner_t(detail::kEllEcodyB, m1));
    }
    double kk = 0.0;
    double ee = 0.0;
    detail::ellint_ke_agm(md, kk, ee);
    return static_cast<T>(ee);
}

namespace detail
{
inline constexpr double kHalfPi = 1.5707963267948966192313216916397514;
inline constexpr double kInvPi = 0.31830988618379067153776752674502872;

// φ outside [−π/2, π/2]: the integrands have period π, so with j the integer nearest φ/π the value is the complete
// integral taken 2j times plus the principal-range value at φ − jπ (DLMF §19.2(ii)): F(φ) = 2jK + F(φ−jπ), and the
// same for E and Π. Since sin(φ−jπ) = (−1)^j sin φ and cos² is unchanged, the principal-range formula evaluated at φ
// itself is that value up to the sign (−1)^j. Returns j (never 0, for |φ| beyond the double nearest π/2, so that an
// infinite K is not multiplied by zero), and sets `flip` when it is odd.
// Within a few ulps of an odd multiple of π/2 the rounded |φ|/π can name the neighbouring j, which leaves φ − jπ a hair
// outside [−π/2, π/2]. There cos(φ − jπ) = (−1)^j cos φ must not be negative, so a sign of c = cos φ that disagrees
// with the parity of j moves j by one turn toward φ − jπ, whose sign is that of sin(φ − jπ) = (−1)^j sin φ.
[[nodiscard]] inline double quasi_period(double phi, double s, double c, bool& flip) noexcept
{
    double turns = crd::math::copysign(std::max(1.0, crd::math::floor(crd::math::fabs(phi) * kInvPi + 0.5)), phi);
    flip = (turns - 2.0 * crd::math::floor(0.5 * turns)) != 0.0; // (turns is even above 2^53)
    if ((c < 0.0) != flip)
    {
        turns += ((s > 0.0) != flip) ? 1.0 : -1.0;
        flip = !flip;
    }
    return turns;
}

// 1 − a·sin²φ as cos²φ + (1 − a)·sin²φ, which does not cancel for a ≤ 1 (1 − a·sin²φ loses the digits of a rounded
// sin²φ near 1 when a is near 1, and is 0 for the double nearest π/2 at a = 1, where cos²φ is not). Above 1 both
// forms cancel only where the integrand is singular; the cos² form carries the smaller rounding up to a = 2, the plain
// form beyond it.
[[nodiscard]] inline double one_minus_a_sin2(double a, double s2, double c2) noexcept
{
    return (a <= 2.0) ? c2 + (1.0 - a) * s2 : 1.0 - a * s2;
}

// The second argument 1 − m·sin²φ of F, E and Π. At the edge of the real domain for m > 1 (sin²φ = 1/m) its true value
// is 0 and the rounding of the terms can make it negative, a NaN where the integral is finite: a value below 0 by no
// more than 8 units of 2⁻⁵³ in c² + (m − 1)·s², the size of those terms, is taken as 0. Past that the input lies
// outside the domain and stays NaN. For m ≤ 1 the value is a sum of non-negative terms and never below 0.
[[nodiscard]] inline double one_minus_m_sin2(double m, double s2, double c2) noexcept
{
    const double delta = one_minus_a_sin2(m, s2, c2);
    if (delta < 0.0) [[unlikely]]
    {
        const double terms = c2 + (m - 1.0) * s2;
        if (-delta <= 0x1p-50 * terms)
        {
            return 0.0;
        }
    }
    return delta;
}

// The last argument p = 1 − n·sin²φ of Π. Near the pole n·sin²φ = 1 for n in (√2, 4) both forms above cancel, at a = 2
// and φ the double nearest π/4 to exactly 0 (s = c there) where p = cos 2φ ≈ 6.1e-17: there p is formed as
// ((2 − n) + n·cos 2φ)/2, from the exact 2φ and the exact 2 − n, whose rounding is |2 − n|·2⁻⁵³ against the
// (c² + |1 − n|·s²)·2⁻⁵³ of the cheap form. Only a cheap value below 2⁻⁶ of its terms pays for the extra cosine.
[[nodiscard]] inline double one_minus_n_sin2(double n, double phi, double s2, double c2) noexcept
{
    const double p = one_minus_a_sin2(n, s2, c2);
    if (n > 1.4142135623730951 && n < 4.0)
    {
        const double terms = c2 + crd::math::fabs(1.0 - n) * s2;
        const double twice = 2.0 * phi;
        if (crd::math::fabs(p) < 0x1p-6 * terms && crd::math::fabs(twice) < kInfinity) [[unlikely]]
        {
            return 0.5 * ((2.0 - n) + n * crd::math::cos(twice));
        }
    }
    return p;
}

// F and E where their formulas meet sin(∞) or 0·∞ and so give NaN: φ = ±∞ (F and E grow by 2K and 2E every half turn,
// so ±∞ for m ≤ 1) and m = −∞ (K(−∞) = 0 and F(φ, −∞) = 0; the integrand of E grows without bound, so E(φ, −∞) = ∞ for
// φ ≠ 0), with an exactly vanishing φ. A NaN argument, an m above 1 and φ = ±∞ with m = −∞ stay NaN.
[[nodiscard]] inline double incomplete_limit(double phi, double m, bool energy) noexcept
{
    if (phi != phi || m != m)
    {
        return kNaN;
    }
    if (m == -kInfinity)
    {
        if (phi == 0.0)
        {
            return phi;
        }
        if (energy)
        {
            return crd::math::copysign(kInfinity, phi);
        }
        return (crd::math::fabs(phi) == kInfinity) ? kNaN : crd::math::copysign(0.0, phi);
    }
    if (crd::math::fabs(phi) == kInfinity && m <= 1.0)
    {
        return crd::math::copysign(kInfinity, phi);
    }
    return kNaN;
}
} // namespace detail

// Incomplete: F(φ,m)=sinφ·R_F(cos²φ,1−m sin²φ,1); E(φ,m)=sinφ·R_F − (m/3)sin³φ·R_D (scipy ellipkinc/ellipeinc).
template <Real T>
[[nodiscard]] T ellint_f(T phi, T m) noexcept
{
    const double phid = static_cast<double>(phi);
    double s;
    double c;
    crd::math::sincos(phid, s, c); // one argument reduction for both, the same values as sin and cos
    const double md = static_cast<double>(m);
    const double c2 = c * c;
    const double principal = s * detail::carlson_rf(c2, detail::one_minus_m_sin2(md, s * s, c2), 1.0);
    if (principal != principal)
    {
        return static_cast<T>(detail::incomplete_limit(phid, md, false));
    }
    if (!(crd::math::fabs(phid) > detail::kHalfPi))
    {
        return static_cast<T>(principal);
    }
    bool flip = false;
    const double turns = detail::quasi_period(phid, s, c, flip);
    return static_cast<T>(2.0 * turns * ellint_k(md) + (flip ? -principal : principal));
}
template <Real T>
[[nodiscard]] T ellint_e_inc(T phi, T m) noexcept
{
    const double phid = static_cast<double>(phi);
    double s;
    double c;
    crd::math::sincos(phid, s, c); // one argument reduction for both, the same values as sin and cos
    const double md = static_cast<double>(m);
    const double c2 = c * c;
    const double delta = detail::one_minus_m_sin2(md, s * s, c2);
    const double rf = detail::carlson_rf(c2, delta, 1.0);
    const double rd = detail::carlson_rd(c2, delta, 1.0);
    const double principal = s * rf - md / 3.0 * s * s * s * rd;
    if (principal != principal)
    {
        return static_cast<T>(detail::incomplete_limit(phid, md, true));
    }
    if (!(crd::math::fabs(phid) > detail::kHalfPi))
    {
        return static_cast<T>(principal);
    }
    bool flip = false;
    const double turns = detail::quasi_period(phid, s, c, flip);
    return static_cast<T>(2.0 * turns * ellint_e(md) + (flip ? -principal : principal));
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
// For n sin²φ > 1 the integral is the Cauchy principal value (R_J with a negative last argument); past |φ| = π/2 it
// continues by Π(n,φ+jπ,m) = Π(n,φ,m) + 2jΠ(n,m), the complete integral Π(n,m) = R_F(0,1−m,1) + (n/3)R_J(0,1−m,1,1−n).
// The complete integral diverges at m = 1 (to +∞ for n ≤ 1, to −∞ for n > 1) and at n = 1 (to +∞), and is then ±∞ past
// π/2 and at φ = ±∞; the sums above are ∞ − ∞ or NaN there.
template <Real T>
[[nodiscard]] T ellint_pi(T n, T phi, T m) noexcept
{
    const double phid = static_cast<double>(phi);
    double s;
    double c;
    crd::math::sincos(phid, s, c); // one argument reduction for both, the same values as sin and cos
    const double md = static_cast<double>(m);
    const double nd = static_cast<double>(n);
    const double s2 = s * s;
    const double c2 = c * c;
    const double delta = detail::one_minus_m_sin2(md, s2, c2);
    const double rf = detail::carlson_rf(c2, delta, 1.0);
    const double rj = detail::carlson_rj(c2, delta, 1.0, detail::one_minus_n_sin2(nd, phid, s2, c2));
    const double principal = s * rf + nd / 3.0 * s * s2 * rj;
    if (!(crd::math::fabs(phid) > detail::kHalfPi))
    {
        return static_cast<T>(principal);
    }
    double complete =
        detail::carlson_rf(0.0, 1.0 - md, 1.0) + nd / 3.0 * detail::carlson_rj(0.0, 1.0 - md, 1.0, 1.0 - nd);
    if (md == 1.0)
    {
        // The integrand 1/((1 − n sin²θ)|cosθ|) of the complete integral is not integrable at π/2: positive there for
        // n ≤ 1, negative for n > 1 (the sums above are ∞ − ∞ or the wrong sign of ∞ for some n).
        if (nd > 1.0)
        {
            complete = -detail::kInfinity;
        }
        else if (nd <= 1.0)
        {
            complete = detail::kInfinity;
        }
    }
    else if (nd == 1.0 && md < 1.0 && md > -detail::kInfinity)
    {
        complete = detail::kInfinity; // n = 1: the integrand is 1/(cos²θ√(1 − m sin²θ)), positive with a pole at π/2
    }
    if (crd::math::fabs(phid) == detail::kInfinity)
    {
        // every half turn adds the complete integral: ∞ with its sign, odd in φ (a vanishing or NaN one stays NaN)
        if (complete != 0.0 && complete == complete)
        {
            return static_cast<T>(crd::math::copysign(detail::kInfinity, (complete < 0.0) ? -phid : phid));
        }
        return static_cast<T>(detail::kNaN);
    }
    bool flip = false;
    const double turns = detail::quasi_period(phid, s, c, flip);
    return static_cast<T>(2.0 * turns * complete + (flip ? -principal : principal));
}

} // namespace crd::hesap::special
