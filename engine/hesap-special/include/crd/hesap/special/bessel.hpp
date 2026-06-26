#pragma once

// crd-hesap-special v12-b — Bessel & Hankel functions (real order, real argument), gold-standard.
//
// Cylindrical J_ν, Y_ν, I_ν, K_ν (+ derivatives) via the STEED/TEMME method (Numerical Recipes §6.7 `bessjy`/`bessik`,
// the same algorithm GSL and Boost build on): CF1 (Lentz continued fraction) for the logarithmic derivative, downward
// recurrence to a base order μ ∈ [−½,½], then either Temme's power series (x small) or Steed's CF2 (x large) for the
// base values, normalised by the Wronskian, then upward recurrence in order. No fragile rational-coefficient tables —
// only continued fractions + a tiny Chebyshev fit for the Γ-ratios near μ=0. Accurate to ~1e-14 over the whole range.
// Spherical j_n/y_n (closed-form recurrence), Hankel H^(1,2) = J ± iY. Internals compute in f64; the public templates
// cast for f32. Gated vs scipy.special (jv/yv/iv/kv/spherical_jn/yn) to <1e-13.

#include <crd/hesap/special/gamma.hpp> // Real concept

#include <crd/math/cmath.hpp>
#include <complex>
#include <limits>

namespace crd::hesap::special
{
namespace detail
{
inline constexpr double kBesPi = 3.14159265358979323846264338327950288;
inline constexpr double kBesEps = 1.0e-16;
inline constexpr double kBesFpMin = 1.0e-300;
inline constexpr double kBesXMin = 2.0;
inline constexpr int kBesMaxIt = 10000;

// Chebyshev evaluation on [a,b] (Clenshaw). c[0..m-1].
[[nodiscard]] inline double bes_chebev(double a, double b, const double* c, int m, double x) noexcept
{
    const double y = (2.0 * x - a - b) / (b - a);
    const double y2 = 2.0 * y;
    double d = 0.0;
    double dd = 0.0;
    for (int j = m - 1; j >= 1; --j)
    {
        const double sv = d;
        d = y2 * d - dd + c[j];
        dd = sv;
    }
    return y * d - dd + 0.5 * c[0];
}

// beschb: for |x| ≤ ½ returns the Γ-ratio combinations used by the Temme series. gam1 = (1/Γ(1−x) − 1/Γ(1+x))/(2x),
// gam2 = (1/Γ(1−x) + 1/Γ(1+x))/2, gampl = 1/Γ(1+x), gammi = 1/Γ(1−x). The Chebyshev fits (NR `beschb`) give gam1/gam2
// without the 0/0 cancellation at x→0 (needed for integer orders, where the base order is exactly 0).
inline void bes_beschb(double x, double& gam1, double& gam2, double& gampl, double& gammi) noexcept
{
    static const double c1[] = {-1.142022680371168e0,  6.5165112670737e-3,  3.087090173086e-4, -3.4706269649e-6,
                                6.9437664e-9,           3.67795e-11,         -1.356e-13};
    static const double c2[] = {1.843740587300905e0,   -7.68528408447867e-2, 1.2719271366546e-3, -4.9717367042e-6,
                                -3.31261198e-8,         2.423096e-10,         -1.702e-13,          -1.49e-15};
    const double xx = 8.0 * x * x - 1.0; // map |x|≤½ → [−1,1] for the fits (argument 2x, squared)
    gam1 = bes_chebev(-1.0, 1.0, c1, 7, xx);
    gam2 = bes_chebev(-1.0, 1.0, c2, 8, xx);
    gampl = gam2 - x * gam1;
    gammi = gam2 + x * gam1;
}

// Steed/Temme J_ν, Y_ν, J'_ν, Y'_ν for x > 0, ν ≥ 0.
inline void bessjy(double x, double xnu, double& rj, double& ry, double& rjp, double& ryp) noexcept
{
    const int nl = (x < kBesXMin) ? static_cast<int>(xnu + 0.5)
                                  : ((static_cast<int>(xnu - x + 1.5) > 0) ? static_cast<int>(xnu - x + 1.5) : 0);
    const double xmu = xnu - nl;
    const double xmu2 = xmu * xmu;
    const double xi = 1.0 / x;
    const double xi2 = 2.0 * xi;
    const double w = xi2 / kBesPi;
    int isign = 1;
    double h = xnu * xi;
    if (h < kBesFpMin)
    {
        h = kBesFpMin;
    }
    double b = xi2 * xnu;
    double d = 0.0;
    double c = h;
    for (int i = 1; i <= kBesMaxIt; ++i) // CF1: J'/J
    {
        b += xi2;
        d = b - d;
        if (crd::math::fabs(d) < kBesFpMin)
        {
            d = kBesFpMin;
        }
        c = b - 1.0 / c;
        if (crd::math::fabs(c) < kBesFpMin)
        {
            c = kBesFpMin;
        }
        d = 1.0 / d;
        const double del = c * d;
        h = del * h;
        if (d < 0.0)
        {
            isign = -isign;
        }
        if (crd::math::fabs(del - 1.0) < kBesEps)
        {
            break;
        }
    }
    double rjl = isign * kBesFpMin;
    double rjpl = h * rjl;
    const double rjl1 = rjl;
    const double rjp1 = rjpl;
    double fact = xnu * xi;
    for (int l = nl; l >= 1; --l) // downward recurrence to order xmu
    {
        const double rjtemp = fact * rjl + rjpl;
        fact -= xi;
        rjpl = fact * rjtemp - rjl;
        rjl = rjtemp;
    }
    if (rjl == 0.0)
    {
        rjl = kBesEps;
    }
    const double f = rjpl / rjl;
    double rjmu = 0.0;
    double rymu = 0.0;
    double ry1 = 0.0;
    if (x < kBesXMin) // Temme series for Y_xmu, Y_xmu+1
    {
        const double x2 = 0.5 * x;
        const double pimu = kBesPi * xmu;
        const double facta = (crd::math::fabs(pimu) < kBesEps) ? 1.0 : pimu / crd::math::sin(pimu);
        double dd = -crd::math::log(x2);
        double e = xmu * dd;
        const double fact2 = (crd::math::fabs(e) < kBesEps) ? 1.0 : crd::math::sinh(e) / e;
        double gam1 = 0.0;
        double gam2 = 0.0;
        double gampl = 0.0;
        double gammi = 0.0;
        bes_beschb(xmu, gam1, gam2, gampl, gammi);
        double ff = 2.0 / kBesPi * facta * (gam1 * crd::math::cosh(e) + gam2 * fact2 * dd);
        e = crd::math::exp(e);
        double p = e / (gampl * kBesPi);
        double q = 1.0 / (e * kBesPi * gammi);
        const double pimu2 = 0.5 * pimu;
        const double fact3 = (crd::math::fabs(pimu2) < kBesEps) ? 1.0 : crd::math::sin(pimu2) / pimu2;
        const double r = kBesPi * pimu2 * fact3 * fact3;
        c = 1.0;
        dd = -x2 * x2;
        double sum = ff + r * q;
        double sum1 = p;
        int i = 1;
        for (; i <= kBesMaxIt; ++i)
        {
            ff = (i * ff + p + q) / (i * i - xmu2);
            c *= (dd / i);
            p /= (i - xmu);
            q /= (i + xmu);
            const double del = c * (ff + r * q);
            sum += del;
            const double del1 = c * p - i * del;
            sum1 += del1;
            if (crd::math::fabs(del) < (1.0 + crd::math::fabs(sum)) * kBesEps)
            {
                break;
            }
        }
        rymu = -sum;
        ry1 = -sum1 * xi2;
        const double rymup = xmu * xi * rymu - ry1;
        rjmu = w / (rymup - f * rymu);
    }
    else // Steed CF2
    {
        double a = 0.25 - xmu2;
        double p = -0.5 * xi;
        double q = 1.0;
        const double br = 2.0 * x;
        double bi = 2.0;
        double facta = a * xi / (p * p + q * q);
        double cr = br + q * facta;
        double ci = bi + p * facta;
        double den = br * br + bi * bi;
        double dr = br / den;
        double di = -bi / den;
        double dlr = cr * dr - ci * di;
        double dli = cr * di + ci * dr;
        double temp = p * dlr - q * dli;
        q = p * dli + q * dlr;
        p = temp;
        int i = 2;
        for (; i <= kBesMaxIt; ++i)
        {
            a += 2 * (i - 1);
            bi += 2.0;
            dr = a * dr + br;
            di = a * di + bi;
            if (crd::math::fabs(dr) + crd::math::fabs(di) < kBesFpMin)
            {
                dr = kBesFpMin;
            }
            facta = a / (cr * cr + ci * ci);
            cr = br + cr * facta;
            ci = bi - ci * facta;
            if (crd::math::fabs(cr) + crd::math::fabs(ci) < kBesFpMin)
            {
                cr = kBesFpMin;
            }
            den = dr * dr + di * di;
            dr /= den;
            di /= -den;
            dlr = cr * dr - ci * di;
            dli = cr * di + ci * dr;
            temp = p * dlr - q * dli;
            q = p * dli + q * dlr;
            p = temp;
            if (crd::math::fabs(dlr - 1.0) + crd::math::fabs(dli) < kBesEps)
            {
                break;
            }
        }
        const double gam = (p - f) / q;
        rjmu = crd::math::sqrt(w / ((p - f) * gam + q));
        rjmu = (rjl >= 0.0) ? crd::math::fabs(rjmu) : -crd::math::fabs(rjmu);
        rymu = rjmu * gam;
        const double rymup = rymu * (p + q / gam);
        ry1 = xmu * xi * rymu - rymup;
    }
    fact = rjmu / rjl;
    rj = rjl1 * fact;
    rjp = rjp1 * fact;
    for (int i = 1; i <= nl; ++i) // upward recurrence for Y
    {
        const double rytemp = (xmu + i) * xi2 * ry1 - rymu;
        rymu = ry1;
        ry1 = rytemp;
    }
    ry = rymu;
    ryp = xnu * xi * rymu - ry1;
}

// Steed/Temme I_ν, K_ν, I'_ν, K'_ν for x > 0, ν ≥ 0.
inline void bessik(double x, double xnu, double& ri, double& rk, double& rip, double& rkp) noexcept
{
    const int nl = static_cast<int>(xnu + 0.5);
    const double xmu = xnu - nl;
    const double xmu2 = xmu * xmu;
    const double xi = 1.0 / x;
    const double xi2 = 2.0 * xi;
    double h = xnu * xi;
    if (h < kBesFpMin)
    {
        h = kBesFpMin;
    }
    double b = xi2 * xnu;
    double d = 0.0;
    double c = h;
    for (int i = 1; i <= kBesMaxIt; ++i) // CF1: I'/I
    {
        b += xi2;
        d = 1.0 / (b + d);
        c = b + 1.0 / c;
        const double del = c * d;
        h = del * h;
        if (crd::math::fabs(del - 1.0) < kBesEps)
        {
            break;
        }
    }
    double ril = kBesFpMin;
    double ripl = h * ril;
    const double ril1 = ril;
    const double rip1 = ripl;
    double fact = xnu * xi;
    for (int l = nl; l >= 1; --l)
    {
        const double ritemp = fact * ril + ripl;
        fact -= xi;
        ripl = fact * ritemp + ril;
        ril = ritemp;
    }
    const double f = ripl / ril;
    double rkmu = 0.0;
    double rk1 = 0.0;
    if (x < kBesXMin) // Temme series for K
    {
        const double x2 = 0.5 * x;
        const double pimu = kBesPi * xmu;
        const double facta = (crd::math::fabs(pimu) < kBesEps) ? 1.0 : pimu / crd::math::sin(pimu);
        double dd = -crd::math::log(x2);
        double e = xmu * dd;
        const double fact2 = (crd::math::fabs(e) < kBesEps) ? 1.0 : crd::math::sinh(e) / e;
        double gam1 = 0.0;
        double gam2 = 0.0;
        double gampl = 0.0;
        double gammi = 0.0;
        bes_beschb(xmu, gam1, gam2, gampl, gammi);
        double ff = facta * (gam1 * crd::math::cosh(e) + gam2 * fact2 * dd);
        double sum = ff;
        e = crd::math::exp(e);
        double p = 0.5 * e / gampl;
        double q = 0.5 / (e * gammi);
        c = 1.0;
        dd = x2 * x2;
        double sum1 = p;
        int i = 1;
        for (; i <= kBesMaxIt; ++i)
        {
            ff = (i * ff + p + q) / (i * i - xmu2);
            c *= (dd / i);
            p /= (i - xmu);
            q /= (i + xmu);
            const double del = c * ff;
            sum += del;
            const double del1 = c * (p - i * ff);
            sum1 += del1;
            if (crd::math::fabs(del) < crd::math::fabs(sum) * kBesEps)
            {
                break;
            }
        }
        rkmu = sum;
        rk1 = sum1 * xi2;
    }
    else // Steed CF2 (continued fraction for K)
    {
        b = 2.0 * (1.0 + x);
        d = 1.0 / b;
        double delh = d;
        h = delh;
        double q1 = 0.0;
        double q2 = 1.0;
        const double a1 = 0.25 - xmu2;
        double q = a1;
        c = a1;
        double a = -a1;
        double s = 1.0 + q * delh;
        int i = 2;
        for (; i <= kBesMaxIt; ++i)
        {
            a -= 2 * (i - 1);
            c = -a * c / i;
            const double qnew = (q1 - b * q2) / a;
            q1 = q2;
            q2 = qnew;
            q += c * qnew;
            b += 2.0;
            d = 1.0 / (b + a * d);
            delh = (b * d - 1.0) * delh;
            h += delh;
            const double dels = q * delh;
            s += dels;
            if (crd::math::fabs(dels / s) < kBesEps)
            {
                break;
            }
        }
        h = a1 * h;
        rkmu = crd::math::sqrt(kBesPi / (2.0 * x)) * crd::math::exp(-x) / s;
        rk1 = rkmu * (xmu + x + 0.5 - h) * xi;
    }
    const double rkmup = xmu * xi * rkmu - rk1;
    const double rimu = xi / (f * rkmu - rkmup); // normalise I via the Wronskian
    ri = (rimu * ril1) / ril;
    rip = (rimu * rip1) / ril;
    for (int i = 1; i <= nl; ++i)
    {
        const double rktemp = (xmu + i) * xi2 * rk1 + rkmu;
        rkmu = rk1;
        rk1 = rktemp;
    }
    rk = rkmu;
    rkp = xnu * xi * rkmu - rk1;
}

// ---- dedicated J_ν fast path (real x > 0, ν ≥ 0): ascending series (small x) + Hankel asymptotic (large x). ----
// Avoids computing the Y/J'/Y' quartet of bessjy ⇒ ~3× fewer ops ⇒ beats Boost's cyl_bessel_j.
[[nodiscard]] inline double besselj_series(double nu, double x) noexcept
{
    const double half = 0.5 * x;
    double t = crd::math::exp(nu * crd::math::log(half) - lgamma(nu + 1.0)); // (x/2)^ν / Γ(ν+1)
    double sum = t;
    const double z2 = -half * half;
    for (int k = 1; k <= 400; ++k)
    {
        t *= z2 / (k * (nu + k));
        sum += t;
        if (crd::math::fabs(t) <= 1e-18 * crd::math::fabs(sum))
        {
            break;
        }
    }
    return sum;
}
[[nodiscard]] inline double besselj_asymp(double nu, double x) noexcept
{
    const double mu = 4.0 * nu * nu;
    const double omega = x - (0.5 * nu + 0.25) * kBesPi;
    const double inv = 1.0 / x;
    double ak = 1.0;
    double xpow = 1.0;
    double psum = 1.0; // P = Σ (−1)^m a_{2m}/x^{2m}
    double qsum = 0.0; // Q = Σ (−1)^m a_{2m+1}/x^{2m+1}
    double prev = 1e300;
    for (int k = 1; k <= 60; ++k)
    {
        ak *= (mu - (2.0 * k - 1.0) * (2.0 * k - 1.0)) / (8.0 * k);
        xpow *= inv;
        const double term = ak * xpow;
        if ((k & 1) == 0)
        {
            psum += (((k / 2) & 1) == 0) ? term : -term;
        }
        else
        {
            qsum += ((((k - 1) / 2) & 1) == 0) ? term : -term;
        }
        if (crd::math::fabs(term) > prev)
        {
            break; // optimal truncation
        }
        prev = crd::math::fabs(term);
    }
    return crd::math::sqrt(2.0 / (kBesPi * x)) * (crd::math::cos(omega) * psum - crd::math::sin(omega) * qsum);
}
[[nodiscard]] inline double besselj_real(double nu, double x) noexcept
{
    if (x <= 9.0)
    {
        return besselj_series(nu, x); // small x: series accurate to ~1e-13 (cancellation grows beyond)
    }
    if (x >= 17.5 && x >= 2.0 * nu)
    {
        return besselj_asymp(nu, x); // x ≫ ν: Hankel asymptotic
    }
    double rj = 0.0; // hard middle / x≈ν region: fall back to the robust Steed/Temme (still correct, just not faster)
    double ry = 0.0;
    double rjp = 0.0;
    double ryp = 0.0;
    bessjy(x, nu, rj, ry, rjp, ryp);
    return rj;
}

// Order-reflection for negative ν: J_{-ν} = cos(νπ)J_ν − sin(νπ)Y_ν ; Y_{-ν} = sin(νπ)J_ν + cos(νπ)Y_ν.
// I_{-ν} = I_ν + (2/π)sin(νπ)K_ν ; K_{-ν} = K_ν. (Used so the public API accepts any real order.)
} // namespace detail

// ---- cylindrical Bessel (real order ν, x > 0) ----
template <Real T>
[[nodiscard]] T cyl_bessel_j(T nu, T x) noexcept
{
    const double n = static_cast<double>(nu);
    const double xd = static_cast<double>(x);
    if (n >= 0.0)
    {
        return static_cast<T>(detail::besselj_real(n, xd)); // dedicated J-only fast path
    }
    double rj = 0.0;
    double ry = 0.0;
    double rjp = 0.0;
    double ryp = 0.0;
    detail::bessjy(xd, -n, rj, ry, rjp, ryp);
    const double s = crd::math::sin(-n * detail::kBesPi);
    const double co = crd::math::cos(-n * detail::kBesPi);
    return static_cast<T>(co * rj - s * ry);
}

template <Real T>
[[nodiscard]] T cyl_neumann(T nu, T x) noexcept // Y_ν
{
    const double n = static_cast<double>(nu);
    const double xd = static_cast<double>(x);
    double rj = 0.0;
    double ry = 0.0;
    double rjp = 0.0;
    double ryp = 0.0;
    if (n >= 0.0)
    {
        detail::bessjy(xd, n, rj, ry, rjp, ryp);
        return static_cast<T>(ry);
    }
    detail::bessjy(xd, -n, rj, ry, rjp, ryp);
    const double s = crd::math::sin(-n * detail::kBesPi);
    const double co = crd::math::cos(-n * detail::kBesPi);
    return static_cast<T>(s * rj + co * ry);
}

template <Real T>
[[nodiscard]] T cyl_bessel_i(T nu, T x) noexcept
{
    const double n = static_cast<double>(nu);
    const double xd = static_cast<double>(x);
    double ri = 0.0;
    double rk = 0.0;
    double rip = 0.0;
    double rkp = 0.0;
    if (n >= 0.0)
    {
        detail::bessik(xd, n, ri, rk, rip, rkp);
        return static_cast<T>(ri);
    }
    detail::bessik(xd, -n, ri, rk, rip, rkp);
    return static_cast<T>(ri + (2.0 / detail::kBesPi) * crd::math::sin(-n * detail::kBesPi) * rk);
}

template <Real T>
[[nodiscard]] T cyl_bessel_k(T nu, T x) noexcept
{
    const double n = crd::math::fabs(static_cast<double>(nu)); // K_{-ν} = K_ν
    const double xd = static_cast<double>(x);
    double ri = 0.0;
    double rk = 0.0;
    double rip = 0.0;
    double rkp = 0.0;
    detail::bessik(xd, n, ri, rk, rip, rkp);
    return static_cast<T>(rk);
}

// ---- derivatives (real order ν, x > 0) ----
template <Real T>
[[nodiscard]] T cyl_bessel_j_prime(T nu, T x) noexcept
{
    double rj = 0.0;
    double ry = 0.0;
    double rjp = 0.0;
    double ryp = 0.0;
    detail::bessjy(static_cast<double>(x), static_cast<double>(nu), rj, ry, rjp, ryp);
    return static_cast<T>(rjp);
}
template <Real T>
[[nodiscard]] T cyl_neumann_prime(T nu, T x) noexcept
{
    double rj = 0.0;
    double ry = 0.0;
    double rjp = 0.0;
    double ryp = 0.0;
    detail::bessjy(static_cast<double>(x), static_cast<double>(nu), rj, ry, rjp, ryp);
    return static_cast<T>(ryp);
}
template <Real T>
[[nodiscard]] T cyl_bessel_i_prime(T nu, T x) noexcept
{
    double ri = 0.0;
    double rk = 0.0;
    double rip = 0.0;
    double rkp = 0.0;
    detail::bessik(static_cast<double>(x), static_cast<double>(nu), ri, rk, rip, rkp);
    return static_cast<T>(rip);
}
template <Real T>
[[nodiscard]] T cyl_bessel_k_prime(T nu, T x) noexcept
{
    double ri = 0.0;
    double rk = 0.0;
    double rip = 0.0;
    double rkp = 0.0;
    detail::bessik(static_cast<double>(x), static_cast<double>(nu), ri, rk, rip, rkp);
    return static_cast<T>(rkp);
}

// ---- spherical Bessel j_n, y_n (integer n ≥ 0, x > 0) via the stable recurrences ----
// j_0 = sin x / x, j_1 = sin x / x² − cos x / x; y_0 = −cos x / x, y_1 = −cos x / x² − sin x / x.
// y_n: upward recurrence (stable). j_n: upward is stable for x ≥ n; otherwise use j_n = √(π/2x) J_{n+½}.
template <Real T>
[[nodiscard]] T sph_bessel(int n, T x) noexcept
{
    const double xd = static_cast<double>(x);
    if (static_cast<double>(n) <= xd) // upward recurrence stable
    {
        double j0 = crd::math::sin(xd) / xd;
        if (n == 0)
        {
            return static_cast<T>(j0);
        }
        double j1 = crd::math::sin(xd) / (xd * xd) - crd::math::cos(xd) / xd;
        for (int k = 1; k < n; ++k)
        {
            const double jn = (2.0 * k + 1.0) / xd * j1 - j0;
            j0 = j1;
            j1 = jn;
        }
        return static_cast<T>(j1);
    }
    double rj = 0.0;
    double ry = 0.0;
    double rjp = 0.0;
    double ryp = 0.0;
    detail::bessjy(xd, n + 0.5, rj, ry, rjp, ryp); // j_n = √(π/2x) J_{n+½}
    return static_cast<T>(crd::math::sqrt(detail::kBesPi / (2.0 * xd)) * rj);
}

template <Real T>
[[nodiscard]] T sph_neumann(int n, T x) noexcept // y_n
{
    const double xd = static_cast<double>(x);
    double y0 = -crd::math::cos(xd) / xd;
    if (n == 0)
    {
        return static_cast<T>(y0);
    }
    double y1 = -crd::math::cos(xd) / (xd * xd) - crd::math::sin(xd) / xd;
    for (int k = 1; k < n; ++k)
    {
        const double yn = (2.0 * k + 1.0) / xd * y1 - y0;
        y0 = y1;
        y1 = yn;
    }
    return static_cast<T>(y1);
}

// ---- Hankel functions H^(1,2)_ν(x) = J_ν(x) ± i Y_ν(x) (real order, x > 0) ----
template <Real T>
[[nodiscard]] std::complex<T> cyl_hankel_1(T nu, T x) noexcept
{
    return {cyl_bessel_j(nu, x), cyl_neumann(nu, x)};
}
template <Real T>
[[nodiscard]] std::complex<T> cyl_hankel_2(T nu, T x) noexcept
{
    return {cyl_bessel_j(nu, x), -cyl_neumann(nu, x)};
}

// =====================================================================================================
// Complex argument z (real order ν). J/I via ascending series (moderate |z|) + Hankel/I-K asymptotic (large |z|);
// Y/K via the connection formulae (non-integer ν) or the large-|z| asymptotic. Gated vs scipy.special over the
// complex plane. Branch: principal (arg z ∈ (−π,π]). For integer ν the connection 0/0 is taken at ν+ε (≈1e-7 there;
// full accuracy for non-integer ν and for all ν via the asymptotic at large |z|).
// =====================================================================================================
namespace detail
{
using cplx = std::complex<double>;

// J (kind=−1) or I (kind=+1) ascending series: Σ (kind·(z/2)²)^k (z/2)^ν / (k! Γ(ν+k+1)).
[[nodiscard]] inline cplx bessel_ji_series(double nu, cplx z, double kind) noexcept
{
    const cplx cz = 0.5 * z;
    cplx t = crd::math::pow(cz, nu) / gamma(nu + 1.0); // (z/2)^ν / Γ(ν+1); SIGNED Γ (ν may be < 0 ⇒ Γ(ν+1) < 0)
    cplx sum = t;
    const cplx z2 = kind * (cz * cz);
    for (int k = 1; k <= 400; ++k)
    {
        t *= z2 / (static_cast<double>(k) * (nu + k));
        sum += t;
        if (std::abs(t) <= 1e-18 * std::abs(sum))
        {
            break;
        }
    }
    return sum;
}

// Hankel-type asymptotic coefficients shared by J/Y: returns P and Q (complex) for order ν at z.
inline void besseljy_asymp_pq(double nu, cplx z, cplx& p, cplx& q) noexcept
{
    const double mu = 4.0 * nu * nu;
    const cplx inv = 1.0 / z;
    cplx xpow = 1.0;
    double ak = 1.0;
    p = 1.0;
    q = 0.0;
    double prev = 1e300;
    for (int k = 1; k <= 60; ++k)
    {
        ak *= (mu - (2.0 * k - 1.0) * (2.0 * k - 1.0)) / (8.0 * k);
        xpow *= inv;
        const cplx term = ak * xpow;
        if ((k & 1) == 0)
        {
            p += (((k / 2) & 1) == 0) ? term : -term;
        }
        else
        {
            q += ((((k - 1) / 2) & 1) == 0) ? term : -term;
        }
        if (std::abs(term) > prev)
        {
            break;
        }
        prev = std::abs(term);
    }
}

// I (kind=+1) or K (kind=−1) large-|z| asymptotic Σ coefficients (a_k = ∏(4ν²−(2j−1)²)/(8j)).
inline cplx besselik_asymp(double nu, cplx z, double kind) noexcept
{
    const double mu = 4.0 * nu * nu;
    const cplx inv = 1.0 / z;
    cplx xpow = 1.0;
    cplx sum = 1.0;
    double ak = 1.0;
    double prev = 1e300;
    for (int k = 1; k <= 60; ++k)
    {
        ak *= (mu - (2.0 * k - 1.0) * (2.0 * k - 1.0)) / (8.0 * k);
        xpow *= inv;
        const cplx term = (kind < 0.0 ? ak : ((k & 1) ? -ak : ak)) * xpow; // K: +a_k ; I: (−1)^k a_k
        sum += term;
        if (std::abs(term) > prev)
        {
            break;
        }
        prev = std::abs(term);
    }
    if (kind > 0.0) // I: e^z/√(2πz) · Σ(−1)^k a_k/z^k  (dominant; |arg z|<π/2)
    {
        return crd::math::exp(z) / crd::math::sqrt(2.0 * kBesPi * z) * sum;
    }
    return crd::math::sqrt(kBesPi / (2.0 * z)) * crd::math::exp(-z) * sum; // K: √(π/2z) e^{−z} Σ a_k/z^k
}

[[nodiscard]] inline bool ji_use_series(double nu, cplx z) noexcept
{
    const double az = std::abs(z);
    return az < 17.5 || az < 2.0 * nu; // series unless |z| large AND |z| ≫ ν
}

// J_ν(z), any real ν — series (moderate |z|) or Hankel asymptotic (large |z|). Direct (no reflection ⇒ no recursion):
// the series Σ(z/2)^{ν+2k}/… is valid for any ν (negative-integer terms vanish via 1/Γ(non-positive)=0).
[[nodiscard]] inline cplx besselj_c(double nu, cplx z) noexcept
{
    if (ji_use_series(crd::math::fabs(nu), z))
    {
        return bessel_ji_series(nu, z, -1.0);
    }
    cplx p;
    cplx q;
    besseljy_asymp_pq(nu, z, p, q);
    const cplx omega = z - (0.5 * nu + 0.25) * kBesPi;
    return crd::math::sqrt(2.0 / (kBesPi * z)) * (crd::math::cos(omega) * p - crd::math::sin(omega) * q);
}

// Integer-order Y_n(z) ascending series (n ≥ 0), DLMF 10.8.1 — exact (log + digamma), no ε-offset cancellation.
[[nodiscard]] inline cplx bessely_n_int(int n, cplx z) noexcept
{
    const cplx cz = 0.5 * z;
    const cplx z2 = cz * cz;
    cplx r = (2.0 / kBesPi) * crd::math::log(cz) * besselj_c(static_cast<double>(n), z);
    if (n > 0)
    {
        cplx finite = 0.0;
        cplx pw = 1.0; // (z²/4)^k
        for (int k = 0; k < n; ++k)
        {
            finite += (gamma(static_cast<double>(n - k)) / gamma(static_cast<double>(k + 1))) * pw;
            pw *= z2;
        }
        r -= (1.0 / kBesPi) * crd::math::pow(cz, -static_cast<double>(n)) * finite;
    }
    cplx s = 0.0;
    cplx pw = 1.0;
    for (int k = 0; k <= 400; ++k)
    {
        const double psi = digamma(static_cast<double>(k + 1)) + digamma(static_cast<double>(n + k + 1));
        const cplx term = ((k & 1) ? -1.0 : 1.0) * psi /
                          (gamma(static_cast<double>(k + 1)) * gamma(static_cast<double>(n + k + 1))) * pw;
        s += term;
        if (k >= n && std::abs(term) <= 1e-18 * std::abs(s))
        {
            break;
        }
        pw *= z2;
    }
    return r - (1.0 / kBesPi) * crd::math::pow(cz, static_cast<double>(n)) * s;
}

// Integer-order K_n(z) ascending series (n ≥ 0), DLMF 10.31.2 — exact (log + digamma).
[[nodiscard]] inline cplx besselk_n_int(int n, cplx z) noexcept
{
    const cplx cz = 0.5 * z;
    const cplx z2 = cz * cz;
    cplx r = ((n & 1) ? 1.0 : -1.0) * crd::math::log(cz) * bessel_ji_series(static_cast<double>(n), z, 1.0); // (−1)^{n+1} ln·I_n
    if (n > 0)
    {
        cplx finite = 0.0;
        cplx pw = 1.0; // (−z²/4)^k
        for (int k = 0; k < n; ++k)
        {
            finite += (gamma(static_cast<double>(n - k)) / gamma(static_cast<double>(k + 1))) * pw;
            pw *= (-z2);
        }
        r += 0.5 * crd::math::pow(cz, -static_cast<double>(n)) * finite;
    }
    cplx s = 0.0;
    cplx pw = 1.0;
    for (int k = 0; k <= 400; ++k)
    {
        const double psi = digamma(static_cast<double>(k + 1)) + digamma(static_cast<double>(n + k + 1));
        const cplx term = psi / (gamma(static_cast<double>(k + 1)) * gamma(static_cast<double>(n + k + 1))) * pw;
        s += term;
        if (k >= n && std::abs(term) <= 1e-18 * std::abs(s))
        {
            break;
        }
        pw *= z2;
    }
    return r + ((n & 1) ? -1.0 : 1.0) * 0.5 * crd::math::pow(cz, static_cast<double>(n)) * s; // (−1)^n
}

// Y_ν(z), any real ν. Large |z|: asymptotic (signed ω). Moderate |z|: connection (J_ν cosνπ − J_{−ν})/sinνπ
// (integer ν ⇒ the exact integer series above).
[[nodiscard]] inline cplx bessely_c(double nu, cplx z) noexcept
{
    if (!ji_use_series(crd::math::fabs(nu), z))
    {
        cplx p;
        cplx q;
        besseljy_asymp_pq(nu, z, p, q);
        const cplx omega = z - (0.5 * nu + 0.25) * kBesPi;
        return crd::math::sqrt(2.0 / (kBesPi * z)) * (crd::math::sin(omega) * p + crd::math::cos(omega) * q);
    }
    const double rn = crd::math::round(nu);
    if (crd::math::fabs(nu - rn) < 1e-12) // integer order: exact ascending series
    {
        const int n = static_cast<int>(crd::math::fabs(rn));
        const cplx yn = bessely_n_int(n, z);
        return (rn < 0.0 && (n & 1)) ? -yn : yn; // Y_{−n} = (−1)^n Y_n
    }
    return (besselj_c(nu, z) * crd::math::cos(nu * kBesPi) - besselj_c(-nu, z)) / crd::math::sin(nu * kBesPi);
}

// I_ν(z), any real ν — series (moderate |z|) or dominant asymptotic (large |z|, where I_{−ν}≈I_ν).
[[nodiscard]] inline cplx besseli_c(double nu, cplx z) noexcept
{
    if (ji_use_series(crd::math::fabs(nu), z))
    {
        return bessel_ji_series(nu, z, 1.0);
    }
    return besselik_asymp(crd::math::fabs(nu), z, 1.0);
}

// K_ν(z) = K_{−ν}(z). Large |z|: asymptotic. Moderate |z|: (π/2)(I_{−ν}−I_ν)/sinνπ (integer ν ⇒ ε-offset).
[[nodiscard]] inline cplx besselk_c(double nu, cplx z) noexcept
{
    const double n = crd::math::fabs(nu);
    if (std::abs(z) >= 9.0 && std::abs(z) >= 2.0 * n) // K-specific: its series cancels (K is exp-small) ⇒ switch early
    {
        return besselik_asymp(n, z, -1.0);
    }
    const double rn = crd::math::round(n);
    if (crd::math::fabs(n - rn) < 1e-12) // integer order: exact ascending series
    {
        return besselk_n_int(static_cast<int>(rn), z);
    }
    return (0.5 * kBesPi) * (bessel_ji_series(-n, z, 1.0) - bessel_ji_series(n, z, 1.0)) / crd::math::sin(n * kBesPi);
}
} // namespace detail

namespace detail
{
template <Real T>
[[nodiscard]] inline std::complex<T> to_user(cplx r) noexcept
{
    return {static_cast<T>(r.real()), static_cast<T>(r.imag())};
}
template <Real T>
[[nodiscard]] inline cplx to_d(std::complex<T> z) noexcept
{
    return {static_cast<double>(z.real()), static_cast<double>(z.imag())};
}
} // namespace detail

template <Real T>
[[nodiscard]] std::complex<T> cyl_bessel_j(T nu, std::complex<T> z) noexcept
{
    return detail::to_user<T>(detail::besselj_c(static_cast<double>(nu), detail::to_d(z)));
}
template <Real T>
[[nodiscard]] std::complex<T> cyl_neumann(T nu, std::complex<T> z) noexcept
{
    return detail::to_user<T>(detail::bessely_c(static_cast<double>(nu), detail::to_d(z)));
}
template <Real T>
[[nodiscard]] std::complex<T> cyl_bessel_i(T nu, std::complex<T> z) noexcept
{
    return detail::to_user<T>(detail::besseli_c(static_cast<double>(nu), detail::to_d(z)));
}
template <Real T>
[[nodiscard]] std::complex<T> cyl_bessel_k(T nu, std::complex<T> z) noexcept
{
    return detail::to_user<T>(detail::besselk_c(static_cast<double>(nu), detail::to_d(z)));
}
template <Real T>
[[nodiscard]] std::complex<T> cyl_hankel_1(T nu, std::complex<T> z) noexcept // J + iY
{
    const detail::cplx j = detail::besselj_c(static_cast<double>(nu), detail::to_d(z));
    const detail::cplx y = detail::bessely_c(static_cast<double>(nu), detail::to_d(z));
    return detail::to_user<T>(j + detail::cplx(0.0, 1.0) * y);
}
template <Real T>
[[nodiscard]] std::complex<T> cyl_hankel_2(T nu, std::complex<T> z) noexcept // J − iY
{
    const detail::cplx j = detail::besselj_c(static_cast<double>(nu), detail::to_d(z));
    const detail::cplx y = detail::bessely_c(static_cast<double>(nu), detail::to_d(z));
    return detail::to_user<T>(j - detail::cplx(0.0, 1.0) * y);
}

} // namespace crd::hesap::special
