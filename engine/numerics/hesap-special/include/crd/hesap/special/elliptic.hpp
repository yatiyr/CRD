#pragma once

// crd-hesap-special v12-d — elliptic integrals, the CANONICAL family (the hesap-dsp ellipk/ellipj are a DSP-internal
// filter-design subset; this is the special-functions home — SANITY rule 8). Carlson symmetric integrals R_F/R_C/R_D/
// R_J (the gold-standard duplication algorithm, NR §6.11) are the engine; Legendre-form K/E/F/E/Π are thin wrappers.
//   R_F(x,y,z)=½∫₀^∞ dt/√((t+x)(t+y)(t+z));  K(m)=R_F(0,1−m,1);  E(m)=R_F − (m/3)R_D;
//   F(φ,m)=sinφ·R_F(cos²φ, 1−m sin²φ, 1);  Π(n,φ,m) via R_J.  Parameter convention m=k² (scipy.special).
// Gated vs scipy.special elliprf/elliprc/elliprd/elliprj + ellipk/ellipe/ellipkinc/ellipeinc to <1e-12. Internals f64.

#include <crd/hesap/special/gamma.hpp>     // Real concept
#include <crd/hesap/special/poly_eval.hpp> // detail::horner_t

#include "elliptic_poly.inc" // GENERATED iteration-free K(m)/E(m) Cody-form coeffs (gen_elliptic_poly.py; v12-d perf)

#include <algorithm>
#include <crd/math/cmath.hpp>

namespace crd::hesap::special
{
namespace detail
{
// R_F(x,y,z): x,y,z ≥ 0, at most one zero. Duplication theorem + 5th-order series (NR `rf`).
[[nodiscard]] inline double carlson_rf(double x, double y, double z) noexcept
{
    constexpr double errtol = 0.0066; // series error ~errtol⁶ ≈ 8e-14 < the 1e-12 gate ⇒ one fewer duplication (v12-d)
    constexpr double third = 1.0 / 3.0;
    constexpr double c1 = 1.0 / 24.0, c2 = 0.1, c3 = 3.0 / 44.0, c4 = 1.0 / 14.0;
    double xt = x;
    double yt = y;
    double zt = z;
    double delx;
    double dely;
    double delz;
    double ave;
    do
    {
        const double sx = crd::math::sqrt(xt);
        const double sy = crd::math::sqrt(yt);
        const double sz = crd::math::sqrt(zt);
        const double lam = sx * (sy + sz) + sy * sz;
        xt = 0.25 * (xt + lam);
        yt = 0.25 * (yt + lam);
        zt = 0.25 * (zt + lam);
        ave = third * (xt + yt + zt);
        delx = (ave - xt) / ave;
        dely = (ave - yt) / ave;
        delz = (ave - zt) / ave;
    } while (std::max({crd::math::fabs(delx), crd::math::fabs(dely), crd::math::fabs(delz)}) > errtol);
    const double e2 = delx * dely - delz * delz;
    const double e3 = delx * dely * delz;
    return (1.0 + (c1 * e2 - c2 - c3 * e3) * e2 + c4 * e3) / crd::math::sqrt(ave);
}

// R_C(x,y): degenerate R_F(x,y,y) (NR `rc`; handles y < 0 by Cauchy PV).
[[nodiscard]] inline double carlson_rc(double x, double y) noexcept
{
    constexpr double errtol = 0.0012;
    constexpr double third = 1.0 / 3.0;
    constexpr double c1 = 0.3, c2 = 1.0 / 7.0, c3 = 0.375, c4 = 9.0 / 22.0;
    double w;
    double xt;
    double yt;
    if (y > 0.0)
    {
        xt = x;
        yt = y;
        w = 1.0;
    }
    else
    {
        xt = x - y;
        yt = -y;
        w = crd::math::sqrt(x) / crd::math::sqrt(xt);
    }
    double ave;
    double s;
    do
    {
        const double lam = 2.0 * crd::math::sqrt(xt) * crd::math::sqrt(yt) + yt;
        xt = 0.25 * (xt + lam);
        yt = 0.25 * (yt + lam);
        ave = third * (xt + yt + yt);
        s = (yt - ave) / ave;
    } while (crd::math::fabs(s) > errtol);
    return w * (1.0 + s * s * (c1 + s * (c2 + s * (c3 + s * c4)))) / crd::math::sqrt(ave);
}

// R_D(x,y,z) = R_J(x,y,z,z) (NR `rd`).
[[nodiscard]] inline double carlson_rd(double x, double y, double z) noexcept
{
    constexpr double errtol = 0.0010;
    constexpr double c1 = 3.0 / 14.0, c2 = 1.0 / 6.0, c3 = 9.0 / 22.0, c4 = 3.0 / 26.0, c5 = 0.25 * c3, c6 = 1.5 * c4;
    double xt = x;
    double yt = y;
    double zt = z;
    double sum = 0.0;
    double fac = 1.0;
    double delx;
    double dely;
    double delz;
    double ave;
    do
    {
        const double sx = crd::math::sqrt(xt);
        const double sy = crd::math::sqrt(yt);
        const double sz = crd::math::sqrt(zt);
        const double lam = sx * (sy + sz) + sy * sz;
        sum += fac / (sz * (zt + lam));
        fac *= 0.25;
        xt = 0.25 * (xt + lam);
        yt = 0.25 * (yt + lam);
        zt = 0.25 * (zt + lam);
        ave = 0.2 * (xt + yt + 3.0 * zt);
        delx = (ave - xt) / ave;
        dely = (ave - yt) / ave;
        delz = (ave - zt) / ave;
    } while (std::max({crd::math::fabs(delx), crd::math::fabs(dely), crd::math::fabs(delz)}) > errtol);
    const double ea = delx * dely;
    const double eb = delz * delz;
    const double ec = ea - eb;
    const double ed = ea - 6.0 * eb;
    const double ee = ed + ec + ec;
    return 3.0 * sum + fac *
                           (1.0 + ed * (-c1 + c5 * ed - c6 * delz * ee) +
                            delz * (c2 * ee + delz * (-c3 * ec + delz * c4 * ea))) /
                           (ave * crd::math::sqrt(ave));
}

// R_J(x,y,z,p) (NR `rj`; p > 0 path — the standard Π argument range).
[[nodiscard]] inline double carlson_rj(double x, double y, double z, double p) noexcept
{
    constexpr double errtol = 0.0010;
    constexpr double c1 = 3.0 / 14.0, c2 = 1.0 / 3.0, c3 = 3.0 / 22.0, c4 = 3.0 / 26.0;
    constexpr double c5 = 0.75 * c3, c6 = 1.5 * c4, c7 = 0.5 * c2, c8 = c3 + c3;
    double xt = x;
    double yt = y;
    double zt = z;
    double pt = p;
    double sum = 0.0;
    double fac = 1.0;
    double delx;
    double dely;
    double delz;
    double delp;
    double ave;
    do
    {
        const double sx = crd::math::sqrt(xt);
        const double sy = crd::math::sqrt(yt);
        const double sz = crd::math::sqrt(zt);
        const double lam = sx * (sy + sz) + sy * sz;
        const double alpha = (pt * (sx + sy + sz) + sx * sy * sz) * (pt * (sx + sy + sz) + sx * sy * sz);
        const double beta = pt * (pt + lam) * (pt + lam);
        sum += fac * carlson_rc(alpha, beta);
        fac *= 0.25;
        xt = 0.25 * (xt + lam);
        yt = 0.25 * (yt + lam);
        zt = 0.25 * (zt + lam);
        pt = 0.25 * (pt + lam);
        ave = 0.2 * (xt + yt + zt + pt + pt);
        delx = (ave - xt) / ave;
        dely = (ave - yt) / ave;
        delz = (ave - zt) / ave;
        delp = (ave - pt) / ave;
    } while (std::max({crd::math::fabs(delx), crd::math::fabs(dely), crd::math::fabs(delz), crd::math::fabs(delp)}) > errtol);
    const double ea = delx * (dely + delz) + dely * delz;
    const double eb = delx * dely * delz;
    const double ec = delp * delp;
    const double ed = ea - 3.0 * ec;
    const double ee = eb + 2.0 * delp * (ea - ec);
    return 3.0 * sum +
           fac *
               (1.0 + ed * (-c1 + c5 * ed - c6 * ee) + eb * (c7 + delp * (-c8 + delp * c4)) +
                delp * ea * (c2 - delp * c3) - c2 * delp * ec) /
               (ave * crd::math::sqrt(ave));
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

// Jacobi elliptic functions sn/cn/dn at argument u, parameter m=k² (scipy.special.ellipj convention), via the
// descending-Landen / AGM iteration (NR `sncndn`). The CANONICAL home for the Jacobi functions (hesap-dsp's ellipj
// is a filter-design copy). Gated vs scipy.special.ellipj to <1e-13.
template <Real T>
inline void ellipj(T u_in, T m, T& sn, T& cn, T& dn) noexcept
{
    constexpr double ca = 1.0e-8; // AGM convergence (gives full double precision after the back-substitution)
    double emc = 1.0 - static_cast<double>(m);
    double u = static_cast<double>(u_in);
    if (emc != 0.0)
    {
        bool bo = (emc < 0.0);
        double d = 1.0;
        if (bo) // m > 1: imaginary-modulus transformation
        {
            d = 1.0 - emc;
            emc /= -1.0 / d;
            d = crd::math::sqrt(d);
            u *= d;
        }
        double a = 1.0;
        double dnn = 1.0;
        double em[14];
        double en[14];
        int l = 0;
        double c = 0.0;
        for (int i = 1; i <= 13; ++i)
        {
            l = i;
            em[i] = a;
            emc = crd::math::sqrt(emc);
            en[i] = emc;
            c = 0.5 * (a + emc);
            if (crd::math::fabs(a - emc) <= ca * a)
            {
                break;
            }
            emc *= a;
            a = c;
        }
        u *= c;
        double snn = crd::math::sin(u);
        double cnn = crd::math::cos(u);
        if (snn != 0.0)
        {
            a = cnn / snn;
            c *= a;
            for (int ii = l; ii >= 1; --ii)
            {
                const double b = em[ii];
                a *= c;
                c *= dnn;
                dnn = (en[ii] + a) / (b + a);
                a = c / b;
            }
            a = 1.0 / crd::math::sqrt(c * c + 1.0);
            snn = (snn >= 0.0) ? a : -a;
            cnn = c * snn;
        }
        if (bo)
        {
            const double t = dnn;
            dnn = cnn;
            cnn = t;
            snn /= d;
        }
        sn = static_cast<T>(snn);
        cn = static_cast<T>(cnn);
        dn = static_cast<T>(dnn);
    }
    else // m = 1
    {
        cn = static_cast<T>(1.0 / crd::math::cosh(u));
        dn = cn;
        sn = static_cast<T>(crd::math::tanh(u));
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
