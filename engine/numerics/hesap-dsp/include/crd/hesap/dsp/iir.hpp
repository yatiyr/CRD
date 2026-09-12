#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-e — IIR design: analog prototypes + bilinear transform.
//
// Classic IIR design (the scipy/MATLAB chain): start from an ANALOG lowpass
// prototype in zpk (Butterworth poles on a circle, Chebyshev on an ellipse,
// elliptic at v11-f), frequency-transform to the target band (lp2lp here;
// lp2hp/bp/bs at v11-g), then the BILINEAR TRANSFORM maps the analog s-plane to
// the digital z-plane — all in zpk, NEVER tf (the v11-a data-flow rule: design
// in zpk, convert zpk->sos directly; tf is Wilkinson-ill-conditioned at order).
//
// Butterworth poles are closed-form (exp) ⇒ 1e-12 vs scipy; Chebyshev use
// sinh/asinh (transcendental, deterministic) ⇒ ~1e-10. Lower-layer raw scalars.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dsp/filter.hpp> // Zpk<T>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <numbers>

namespace crd::hesap::dsp
{

namespace detail
{
template <typename T> [[nodiscard]] Complex<T> cmul(Complex<T> a, Complex<T> b) noexcept
{
    return Complex<T>{a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
}
template <typename T> [[nodiscard]] Complex<T> cdiv(Complex<T> a, Complex<T> b) noexcept
{
    const T d = b.re * b.re + b.im * b.im;
    return Complex<T>{(a.re * b.re + a.im * b.im) / d, (a.im * b.re - a.re * b.im) / d};
}
// complex sinh(x+iy) = sinh x cos y + i cosh x sin y.
template <typename T> [[nodiscard]] Complex<T> csinh(T re, T im) noexcept
{
    return Complex<T>{crd::math::sinh(re) * crd::math::cos(im), crd::math::cosh(re) * crd::math::sin(im)};
}
} // namespace detail

// Butterworth analog lowpass prototype (scipy buttap): no zeros, poles -exp(j pi m /(2N)), k=1.
template <typename T> [[nodiscard]] Zpk<T> buttap(crd::memory::IAllocator* alloc, crd::usize n)
{
    Zpk<T> zpk(alloc);
    const T pi = static_cast<T>(std::numbers::pi_v<double>);
    for (crd::usize i = 0; i < n; ++i)
    {
        const T m = static_cast<T>(-static_cast<long long>(n) + 1 + 2 * static_cast<long long>(i));
        const T th = pi * m / (T(2) * static_cast<T>(n));
        zpk.p.push_back(Complex<T>{-crd::math::cos(th), -crd::math::sin(th)}); // -exp(j th)
    }
    zpk.k = T(1);
    return zpk;
}

// Chebyshev-I analog lowpass prototype (scipy cheb1ap): rp dB passband ripple. Poles on an ellipse.
template <typename T> [[nodiscard]] Zpk<T> cheb1ap(crd::memory::IAllocator* alloc, crd::usize n, T rp)
{
    Zpk<T> zpk(alloc);
    const T pi = static_cast<T>(std::numbers::pi_v<double>);
    const T eps = crd::math::sqrt(crd::math::pow(T(10), T(0.1) * rp) - T(1));
    const T mu = crd::math::asinh(T(1) / eps) / static_cast<T>(n);
    T kprod = T(1);
    for (crd::usize i = 0; i < n; ++i)
    {
        const T m = static_cast<T>(-static_cast<long long>(n) + 1 + 2 * static_cast<long long>(i));
        const T th = pi * m / (T(2) * static_cast<T>(n));
        const Complex<T> p{-detail::csinh<T>(mu, th).re, -detail::csinh<T>(mu, th).im}; // -sinh(mu + i th)
        zpk.p.push_back(p);
        kprod *= crd::math::hypot(-p.re, -p.im); // |−p|; product is real (conjugate pairs)
    }
    // k = Re(prod(-p)); since poles come in conjugate pairs, prod(-p) is real and positive.
    T kre = T(1), kim = T(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        const Complex<T> mp{-zpk.p[i].re, -zpk.p[i].im};
        const T nr = kre * mp.re - kim * mp.im;
        const T ni = kre * mp.im + kim * mp.re;
        kre = nr;
        kim = ni;
    }
    zpk.k = kre;
    if (n % 2 == 0)
    {
        zpk.k = zpk.k / crd::math::sqrt(T(1) + eps * eps);
    }
    (void)kprod;
    return zpk;
}

// Chebyshev-II (inverse Chebyshev) analog lowpass prototype (scipy cheb2ap): rs dB stopband attenuation.
template <typename T> [[nodiscard]] Zpk<T> cheb2ap(crd::memory::IAllocator* alloc, crd::usize n, T rs)
{
    Zpk<T> zpk(alloc);
    const T pi = static_cast<T>(std::numbers::pi_v<double>);
    const T de = T(1) / crd::math::sqrt(crd::math::pow(T(10), T(0.1) * rs) - T(1));
    const T mu = crd::math::asinh(T(1) / de) / static_cast<T>(n);
    // zeros z = i / sin(m pi/(2N)); for N odd the centre m=0 is skipped (zero at infinity).
    for (crd::usize i = 0; i < n; ++i)
    {
        const long long mm = -static_cast<long long>(n) + 1 + 2 * static_cast<long long>(i);
        if (mm == 0)
        {
            continue; // N odd ⇒ skip (zero at infinity)
        }
        const T m = static_cast<T>(mm);
        zpk.z.push_back(Complex<T>{T(0), T(1) / crd::math::sin(m * pi / (T(2) * static_cast<T>(n)))});
    }
    // poles p = -1 / sinh(mu + i theta).
    for (crd::usize i = 0; i < n; ++i)
    {
        const T m = static_cast<T>(-static_cast<long long>(n) + 1 + 2 * static_cast<long long>(i));
        const T th = pi * m / (T(2) * static_cast<T>(n));
        const Complex<T> sh = detail::csinh<T>(mu, th);
        const Complex<T> inv = detail::cdiv<T>(Complex<T>{T(1), T(0)}, sh);
        zpk.p.push_back(Complex<T>{-inv.re, -inv.im});
    }
    // k = Re(prod(-p)/prod(-z)).
    Complex<T> num{T(1), T(0)}, den{T(1), T(0)};
    for (crd::usize i = 0; i < zpk.p.size(); ++i)
    {
        num = detail::cmul<T>(num, Complex<T>{-zpk.p[i].re, -zpk.p[i].im});
    }
    for (crd::usize i = 0; i < zpk.z.size(); ++i)
    {
        den = detail::cmul<T>(den, Complex<T>{-zpk.z[i].re, -zpk.z[i].im});
    }
    zpk.k = detail::cdiv<T>(num, den).re;
    return zpk;
}

// Bessel analog lowpass prototype, DELAY-normalized (scipy besselap norm='delay'): maximally-flat GROUP DELAY.
// Poles = roots of the reverse Bessel polynomial θ_N(s) = Σ a_k s^k, a_k = (2N-k)!/(2^{N-k} k! (N-k)!); no zeros;
// k = θ_N(0) = a_0 = (2N-1)!!. Group delay = 1 at DC. (Roots via the companion-matrix eig of polynomial.hpp.)
template <typename T> [[nodiscard]] Zpk<T> besselap(crd::memory::IAllocator* alloc, crd::usize n)
{
    Zpk<T> zpk(alloc);
    if (n == 0)
    {
        zpk.k = T(1);
        return zpk;
    }
    crd::containers::Array<T> a(alloc); // θ_N coefficients a[k], k=0..N (double): a_0=(2N-1)!!, ratio recurrence
    a.resize(n + 1);
    double a0 = 1.0;
    for (crd::usize j = 1; j <= n; ++j)
    {
        a0 *= static_cast<double>(2 * j - 1);
    }
    a[0] = static_cast<T>(a0);
    for (crd::usize k = 0; k < n; ++k)
    {
        const double next = static_cast<double>(a[k]) * (2.0 * static_cast<double>(n - k)) /
                            (static_cast<double>(2 * n - k) * static_cast<double>(k + 1));
        a[k + 1] = static_cast<T>(next);
    }
    crd::containers::Array<T> desc(alloc); // descending powers for roots(): desc[i] = a[N-i]
    desc.resize(n + 1);
    for (crd::usize i = 0; i <= n; ++i)
    {
        desc[i] = a[n - i];
    }
    zpk.p = roots<T>(alloc, crd::containers::ConstSpan<T>(desc.data(), n + 1));
    zpk.k = a[0];
    return zpk;
}

// lp2lp_zpk: scale an analog lowpass prototype to cutoff wo.
template <typename T> [[nodiscard]] Zpk<T> lp2lp_zpk(crd::memory::IAllocator* alloc, const Zpk<T>& in, T wo)
{
    Zpk<T> out(alloc);
    const crd::usize degree = in.p.size() - in.z.size();
    for (crd::usize i = 0; i < in.z.size(); ++i)
    {
        out.z.push_back(Complex<T>{in.z[i].re * wo, in.z[i].im * wo});
    }
    for (crd::usize i = 0; i < in.p.size(); ++i)
    {
        out.p.push_back(Complex<T>{in.p[i].re * wo, in.p[i].im * wo});
    }
    out.k = in.k * crd::math::pow(wo, static_cast<T>(degree));
    return out;
}

// bilinear_zpk: map analog s-plane zpk to the digital z-plane (fs = sample rate).
template <typename T> [[nodiscard]] Zpk<T> bilinear_zpk(crd::memory::IAllocator* alloc, const Zpk<T>& in, T fs)
{
    Zpk<T> out(alloc);
    const T fs2 = T(2) * fs;
    const crd::usize degree = in.p.size() - in.z.size();
    Complex<T> knum{T(1), T(0)}, kden{T(1), T(0)};
    for (crd::usize i = 0; i < in.z.size(); ++i)
    {
        const Complex<T> z = in.z[i];
        out.z.push_back(detail::cdiv<T>(Complex<T>{fs2 + z.re, z.im}, Complex<T>{fs2 - z.re, -z.im}));
        knum = detail::cmul<T>(knum, Complex<T>{fs2 - z.re, -z.im});
    }
    for (crd::usize i = 0; i < in.p.size(); ++i)
    {
        const Complex<T> p = in.p[i];
        out.p.push_back(detail::cdiv<T>(Complex<T>{fs2 + p.re, p.im}, Complex<T>{fs2 - p.re, -p.im}));
        kden = detail::cmul<T>(kden, Complex<T>{fs2 - p.re, -p.im});
    }
    for (crd::usize i = 0; i < degree; ++i)
    {
        out.z.push_back(Complex<T>{T(-1), T(0)}); // analog zeros at infinity ⇒ z = -1
    }
    out.k = in.k * detail::cdiv<T>(knum, kden).re;
    return out;
}

// --- digital lowpass designs (fs = 2 ⇒ Wn in [0,1] of Nyquist). Returns digital zpk; caller -> zpk_to_sos. ---
namespace detail
{
template <typename T> [[nodiscard]] Zpk<T> iir_lowpass_digital(crd::memory::IAllocator* alloc, const Zpk<T>& proto, T wn)
{
    const T fs = T(2);
    const T pi = static_cast<T>(std::numbers::pi_v<double>);
    const T warped = T(2) * fs * crd::math::tan(pi * wn / fs);
    const Zpk<T> lp = lp2lp_zpk<T>(alloc, proto, warped);
    return bilinear_zpk<T>(alloc, lp, fs);
}
} // namespace detail

template <typename T> [[nodiscard]] Zpk<T> butter(crd::memory::IAllocator* alloc, crd::usize n, T wn)
{
    return detail::iir_lowpass_digital<T>(alloc, buttap<T>(alloc, n), wn);
}
template <typename T> [[nodiscard]] Zpk<T> cheby1(crd::memory::IAllocator* alloc, crd::usize n, T rp, T wn)
{
    return detail::iir_lowpass_digital<T>(alloc, cheb1ap<T>(alloc, n, rp), wn);
}
template <typename T> [[nodiscard]] Zpk<T> cheby2(crd::memory::IAllocator* alloc, crd::usize n, T rs, T wn)
{
    return detail::iir_lowpass_digital<T>(alloc, cheb2ap<T>(alloc, n, rs), wn);
}
template <typename T> [[nodiscard]] Zpk<T> bessel(crd::memory::IAllocator* alloc, crd::usize n, T wn)
{
    return detail::iir_lowpass_digital<T>(alloc, besselap<T>(alloc, n), wn);
}

} // namespace crd::hesap::dsp
