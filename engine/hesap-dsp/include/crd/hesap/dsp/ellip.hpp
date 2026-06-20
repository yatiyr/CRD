#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-f — Elliptic (Cauer) filter design.
//
// The elliptic prototype is built from the verified elliptic-function substrate
// (elliptic_fn.hpp, each gated vs scipy.special): ellipdeg gives the modulus m
// for (N, selectivity); the imaginary zeros come from ellipj at the j-grid; the
// poles from ellipj at the displaced argument v0 = capk*arc_jac_sc(1/eps,ck1)/(N*K1).
// Equiripple in BOTH bands ⇒ the minimal order for given specs. Faithful
// scipy.signal.ellipap; the digital ellip() runs the v11-e bilinear chain.
// ---------------------------------------------------------------------------

#include <crd/hesap/complex.hpp>
#include <crd/hesap/dsp/elliptic_fn.hpp>
#include <crd/hesap/dsp/filter.hpp> // Zpk<T>
#include <crd/hesap/dsp/iir.hpp>    // bilinear chain
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::dsp
{

// Elliptic analog lowpass prototype (scipy ellipap): rp dB passband ripple, rs dB stopband attenuation, order N.
template <typename T> [[nodiscard]] Zpk<T> ellipap(crd::memory::IAllocator* alloc, crd::usize n, T rp, T rs)
{
    Zpk<T> zpk(alloc);
    if (n == 1) // first-order special case
    {
        const T p = -std::sqrt(T(1) / (std::pow(T(10), T(0.1) * rp) - T(1)));
        zpk.p.push_back(Complex<T>{p, T(0)});
        zpk.k = -p;
        return zpk;
    }
    const T eps_sq = std::pow(T(10), T(0.1) * rp) - T(1);
    const T eps = std::sqrt(eps_sq);
    const T ck1_sq = eps_sq / (std::pow(T(10), T(0.1) * rs) - T(1));
    const T m = ellipdeg<T>(n, ck1_sq);
    const T capk = ellipk<T>(m);
    const T k1 = ellipk<T>(ck1_sq);
    const T sqm = std::sqrt(m);
    const T eps_t = static_cast<T>(1e-12);

    // displaced argument for the poles.
    const T r = arc_jac_sc<T>(T(1) / eps, ck1_sq);
    const T v0 = capk * r / (static_cast<T>(n) * k1);
    T sv = 0, cv = 0, dv = 0;
    ellipj<T>(v0, T(1) - m, sv, cv, dv);

    // j-grid: arange(1 - N%2, N, 2).
    const long long start = 1 - static_cast<long long>(n % 2);
    for (long long jv = start; jv < static_cast<long long>(n); jv += 2)
    {
        T s = 0, c = 0, d = 0;
        ellipj<T>(static_cast<T>(jv) * capk / static_cast<T>(n), m, s, c, d);
        // pole p = -(c d sv cv + i s dv) / (1 - (d sv)^2).
        const T denom = T(1) - (d * sv) * (d * sv);
        const Complex<T> pnum{-(c * d * sv * cv), -(s * dv)};
        const Complex<T> p{pnum.re / denom, pnum.im / denom};
        zpk.p.push_back(p);
        if (std::abs(p.im) > eps_t) // complex pole ⇒ add its conjugate
        {
            zpk.p.push_back(Complex<T>{p.re, -p.im});
        }
        // zero z = i / (sqrt(m) s) (only where s != 0; the j=0 term for odd N has s=0 ⇒ no finite zero).
        if (std::abs(s) > eps_t)
        {
            const Complex<T> z{T(0), T(1) / (sqm * s)};
            zpk.z.push_back(z);
            zpk.z.push_back(Complex<T>{z.re, -z.im});
        }
    }

    // gain k = Re(prod(-p) / prod(-z)); if N even, k /= sqrt(1 + eps^2).
    Complex<T> pnum{T(1), T(0)}, pden{T(1), T(0)};
    for (crd::usize i = 0; i < zpk.p.size(); ++i)
    {
        pnum = detail::cmul<T>(pnum, Complex<T>{-zpk.p[i].re, -zpk.p[i].im});
    }
    for (crd::usize i = 0; i < zpk.z.size(); ++i)
    {
        pden = detail::cmul<T>(pden, Complex<T>{-zpk.z[i].re, -zpk.z[i].im});
    }
    zpk.k = detail::cdiv<T>(pnum, pden).re;
    if (n % 2 == 0)
    {
        zpk.k = zpk.k / std::sqrt(T(1) + eps_sq);
    }
    return zpk;
}

// digital elliptic lowpass (fs = 2 ⇒ Wn in [0,1] of Nyquist). Returns digital zpk; caller -> zpk_to_sos.
template <typename T> [[nodiscard]] Zpk<T> ellip(crd::memory::IAllocator* alloc, crd::usize n, T rp, T rs, T wn)
{
    return detail::iir_lowpass_digital<T>(alloc, ellipap<T>(alloc, n, rp, rs), wn);
}

} // namespace crd::hesap::dsp
