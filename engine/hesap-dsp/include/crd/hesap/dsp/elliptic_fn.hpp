#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-f — elliptic special functions for Cauer/elliptic filters.
//
// The elliptic (Cauer) filter is the elite IIR design — minimal order for given
// pass/stop specs, equiripple in BOTH bands — and the one most libraries get
// subtly wrong, because it needs:
//   ellipk(m)        complete elliptic integral K(m), m = k^2 (the parameter)
//   ellipj(u, m)     Jacobi elliptic functions sn, cn, dn
//   ellipdeg(N, m1)  the degree equation: the modulus m for order N + selectivity
//   arc_jac_sc(w, m) the inverse Jacobi sc = sn/cn
// Each is built + gated INDEPENDENTLY vs scipy.special to ~1e-12 before ellipap
// composes them (v11-f). AGM + descending-Landen, standard Abramowitz-Stegun.
// Lower-layer raw scalars.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>

#include <cmath>
#include <limits>
#include <numbers>

namespace crd::hesap::dsp
{

// Complete elliptic integral of the first kind K(m), parameter m = k^2 (the scipy.special.ellipk convention).
// K(m) = pi / (2 * AGM(1, sqrt(1-m))). Converges quadratically.
template <typename T> [[nodiscard]] T ellipk(T m) noexcept
{
    if (m >= T(1))
    {
        return std::numeric_limits<T>::infinity();
    }
    T a = T(1);
    T b = std::sqrt(T(1) - m);
    for (int i = 0; i < 60; ++i)
    {
        const T an = (a + b) / T(2);
        const T bn = std::sqrt(a * b);
        if (std::abs(a - b) <= static_cast<T>(1e-16) * std::abs(a))
        {
            a = an;
            break;
        }
        a = an;
        b = bn;
    }
    return static_cast<T>(std::numbers::pi_v<double>) / (T(2) * a);
}

// K(1-m) — the complementary integral (scipy.special.ellipkm1 takes p = 1-m, returns K(1-p) = K(m)... note:
// scipy ellipkm1(p) = K(1-p). We expose ellipk_compl(m) = K(1-m) for the degree equation.
template <typename T> [[nodiscard]] T ellipk_compl(T m) noexcept { return ellipk<T>(T(1) - m); }

// Jacobi elliptic functions sn, cn, dn at argument u, parameter m = k^2 (scipy.special.ellipj convention).
// Descending Landen (AGM) transformation, Abramowitz-Stegun 16.4. Real u, 0 <= m <= 1.
template <typename T> void ellipj(T u, T m, T& sn, T& cn, T& dn) noexcept
{
    if (m < static_cast<T>(1e-15)) // m -> 0: circular functions
    {
        sn = std::sin(u);
        cn = std::cos(u);
        dn = T(1);
        return;
    }
    if (m > T(1) - static_cast<T>(1e-15)) // m -> 1: hyperbolic
    {
        const T t = std::tanh(u);
        sn = t;
        cn = T(1) / std::cosh(u);
        dn = cn;
        return;
    }
    // build the descending sequence of moduli a[i], c[i]; b not retained.
    T a[40];
    T c[40];
    a[0] = T(1);
    T b = std::sqrt(T(1) - m);
    c[0] = std::sqrt(m); // c[0] = k
    int n = 0;
    for (int i = 1; i < 40; ++i)
    {
        a[i] = (a[i - 1] + b) / T(2);
        c[i] = (a[i - 1] - b) / T(2);
        const T bn = std::sqrt(a[i - 1] * b);
        b = bn;
        n = i;
        if (std::abs(c[i]) < static_cast<T>(1e-16) * std::abs(a[i]))
        {
            break;
        }
    }
    // phi descent.
    T phi = std::ldexp(a[n] * u, n); // 2^n * a[n] * u
    for (int i = n; i >= 1; --i)
    {
        phi = (phi + std::asin((c[i] / a[i]) * std::sin(phi))) / T(2);
    }
    sn = std::sin(phi);
    cn = std::cos(phi);
    dn = std::sqrt(T(1) - m * sn * sn);
}

// the degree equation: given order N and the inner selectivity m1 = ck1^2 (the squared ratio of ripple to
// attenuation factors), return the modulus m so the elliptic filter of order N hits both specs. Via the nome
// transformation q = q1^(1/N), m = (theta2/theta3)^4 from the theta series. (scipy _ellipdeg.)
template <typename T> [[nodiscard]] T ellipdeg(crd::usize N, T m1) noexcept
{
    const T K1 = ellipk<T>(m1);
    const T K1p = ellipk_compl<T>(m1); // K(1-m1)
    const T pi = static_cast<T>(std::numbers::pi_v<double>);
    const T q1 = std::exp(-pi * K1p / K1);
    const T q = std::pow(q1, T(1) / static_cast<T>(N));
    T num = T(0);
    T den = T(0);
    for (int j = 0; j < 25; ++j) // theta2 numerator: sum q^(j(j+1)); theta3 denom: 1 + 2 sum q^(j^2)
    {
        num += std::pow(q, static_cast<T>(j * (j + 1)));
    }
    for (int j = 1; j < 25; ++j)
    {
        den += std::pow(q, static_cast<T>(j * j));
    }
    const T ratio = num / (T(1) + T(2) * den); // (theta2/theta3) up to the sqrt(q) factor folded below
    const T m = T(16) * q * ratio * ratio * ratio * ratio;
    return m;
}

namespace detail
{
// minimal complex transcendentals (crd Complex, not std::complex) for the inverse Jacobi functions.
template <typename T> [[nodiscard]] Complex<T> csqrt(Complex<T> z) noexcept
{
    const T r = std::hypot(z.re, z.im);
    const T re = std::sqrt((r + z.re) / T(2));
    T im = std::sqrt((r - z.re) / T(2));
    if (z.im < T(0))
    {
        im = -im;
    }
    return Complex<T>{re, im};
}
template <typename T> [[nodiscard]] Complex<T> clog(Complex<T> z) noexcept
{
    return Complex<T>{std::log(std::hypot(z.re, z.im)), std::atan2(z.im, z.re)};
}
template <typename T> [[nodiscard]] Complex<T> cmul2(Complex<T> a, Complex<T> b) noexcept
{
    return Complex<T>{a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
}
// asin(z) = -i log(i z + sqrt(1 - z^2)).
template <typename T> [[nodiscard]] Complex<T> casin(Complex<T> z) noexcept
{
    const Complex<T> z2{z.re * z.re - z.im * z.im, T(2) * z.re * z.im};
    const Complex<T> s = csqrt<T>(Complex<T>{T(1) - z2.re, -z2.im});      // sqrt(1 - z^2)
    const Complex<T> iz{-z.im, z.re};                                    // i z
    const Complex<T> lg = clog<T>(Complex<T>{iz.re + s.re, iz.im + s.im}); // log(i z + sqrt(1-z^2))
    return Complex<T>{lg.im, -lg.re};                                     // -i * lg
}
} // namespace detail

// arc_jac_sc(w, m): solve for z (real) in w = sc(z, 1-m), w real. Via z = Im(arc_jac_sn(i w, m)) (scipy
// _arc_jac_sc1), with arc_jac_sn the complex inverse Jacobi sn (Orfanidis ascending Landen). Used by ellipap
// for the pole angle. Gated independently vs scipy _arc_jac_sc1.
template <typename T> [[nodiscard]] T arc_jac_sc(T w, T m) noexcept
{
    const T k = std::sqrt(m);
    // descending modulus sequence (Landen) until ~0.
    T ks[40];
    ks[0] = k;
    int nk = 0;
    for (int i = 1; i < 40; ++i)
    {
        const T kp = std::sqrt((T(1) - ks[i - 1]) * (T(1) + ks[i - 1]));
        ks[i] = (T(1) - kp) / (T(1) + kp);
        nk = i;
        if (ks[i] < static_cast<T>(1e-16))
        {
            break;
        }
    }
    T Kprod = T(1);
    for (int i = 1; i <= nk; ++i)
    {
        Kprod *= (T(1) + ks[i]);
    }
    const T Kc = Kprod * static_cast<T>(std::numbers::pi_v<double>) / T(2);
    // ascending recursion on wn, starting from i*w (purely imaginary).
    Complex<T> wn{T(0), w};
    for (int i = 0; i < nk; ++i)
    {
        const T kn = ks[i];
        const T knext = ks[i + 1];
        // _complement(kn*wn) = sqrt((1 - kn wn)(1 + kn wn)) = sqrt(1 - (kn wn)^2).
        const Complex<T> kw{kn * wn.re, kn * wn.im};
        const Complex<T> kw2{kw.re * kw.re - kw.im * kw.im, T(2) * kw.re * kw.im};
        const Complex<T> comp = detail::csqrt<T>(Complex<T>{T(1) - kw2.re, -kw2.im});
        const Complex<T> den{(T(1) + knext) * (T(1) + comp.re), (T(1) + knext) * comp.im};
        // wnext = 2 wn / den
        const T dd = den.re * den.re + den.im * den.im;
        const Complex<T> num{T(2) * wn.re, T(2) * wn.im};
        wn = Complex<T>{(num.re * den.re + num.im * den.im) / dd, (num.im * den.re - num.re * den.im) / dd};
    }
    const Complex<T> u = detail::casin<T>(wn); // 2/pi * asin(wn) * K, then Im
    const T two_over_pi = T(2) / static_cast<T>(std::numbers::pi_v<double>);
    // z = K * (2/pi) * asin(wn); return Im(z).
    return Kc * two_over_pi * u.im;
}

} // namespace crd::hesap::dsp
