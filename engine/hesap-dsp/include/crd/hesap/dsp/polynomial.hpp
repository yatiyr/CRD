#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-a — polynomial substrate for filter representations.
//
// DSP polynomials are written in the filter convention: coefficients in
// ASCENDING NEGATIVE powers of z,  P(z) = c[0] + c[1]z^{-1} + ... + c[M]z^{-M}
// (scipy/MATLAB `b`, `a`). `poly_eval_negpow` evaluates that at a point via
// Horner. `roots` factors a polynomial through the companion-matrix
// eigenvalues (crd-hesap-dense `eig`); `poly_from_roots` multiplies the
// (z - r) factors back. Lower-layer raw scalars / raw Complex (ADR-0078 §5).
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/eig_nonsym.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::dsp
{

// P(zi) = c[0] + c[1]*zi + c[2]*zi^2 + ... + c[M]*zi^M, with zi = z^{-1}.
// (The filter convention: pass zi = e^{-jw} to evaluate H on the unit circle.)
template <typename T>
[[nodiscard]] Complex<T> poly_eval_negpow(crd::containers::ConstSpan<T> c, Complex<T> zi) noexcept
{
    Complex<T> acc{T(0), T(0)};
    for (crd::usize k = c.size(); k-- > 0;)
    {
        // acc = acc*zi + c[k]
        const T re = acc.re * zi.re - acc.im * zi.im + c[k];
        const T im = acc.re * zi.im + acc.im * zi.re;
        acc = Complex<T>{re, im};
    }
    return acc;
}

// Derivative evaluated the same way: P'(zi) in the zi variable (chain rule to w
// is applied by the caller). Returns {P(zi), dP/dzi(zi)} in one Horner sweep.
template <typename T>
void poly_eval_with_deriv(crd::containers::ConstSpan<T> c, Complex<T> zi, Complex<T>& p, Complex<T>& dp) noexcept
{
    Complex<T> val{T(0), T(0)};
    Complex<T> der{T(0), T(0)};
    for (crd::usize k = c.size(); k-- > 0;)
    {
        // der = der*zi + val ; val = val*zi + c[k]
        const T dre = der.re * zi.re - der.im * zi.im + val.re;
        const T dim = der.re * zi.im + der.im * zi.re + val.im;
        der = Complex<T>{dre, dim};
        const T vre = val.re * zi.re - val.im * zi.im + c[k];
        const T vim = val.re * zi.im + val.im * zi.re;
        val = Complex<T>{vre, vim};
    }
    p = val;
    dp = der;
}

// roots of  c[0]x^n + c[1]x^{n-1} + ... + c[n]  (DESCENDING powers — the
// numpy.roots convention) via the companion-matrix eigenvalues. Leading zeros
// are trimmed; trailing zeros become roots at the origin. Generally complex.
template <typename T>
[[nodiscard]] crd::containers::Array<Complex<T>> roots(crd::memory::IAllocator* alloc,
                                                       crd::containers::ConstSpan<T> c)
{
    crd::containers::Array<Complex<T>> out(alloc);
    // trim leading zeros.
    crd::usize lead = 0;
    while (lead < c.size() && c[lead] == T(0))
    {
        ++lead;
    }
    if (c.size() - lead < 2) // constant ⇒ no roots
    {
        return out;
    }
    // strip trailing zeros → roots at 0.
    crd::usize trail = 0;
    crd::usize last = c.size();
    while (last - 1 > lead && c[last - 1] == T(0))
    {
        --last;
        ++trail;
    }
    const crd::usize n = last - lead - 1; // degree of the non-trivial part
    if (n >= 1)
    {
        // monic companion matrix (n x n): first row = -c[lead+1..]/c[lead], sub-diagonal ones.
        dense::Matrix<T> comp(alloc, n, n);
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                comp(i, j) = T(0);
            }
        }
        const T inv0 = T(1) / c[lead];
        for (crd::usize j = 0; j < n; ++j)
        {
            comp(0, j) = -c[lead + 1 + j] * inv0;
        }
        for (crd::usize i = 1; i < n; ++i)
        {
            comp(i, i - 1) = T(1);
        }
        const dense::EigNonsym<T> e = dense::eig(alloc, comp);
        for (crd::usize i = 0; i < n; ++i)
        {
            out.push_back(Complex<T>{e.values(i).re, e.values(i).im});
        }
    }
    for (crd::usize i = 0; i < trail; ++i)
    {
        out.push_back(Complex<T>{T(0), T(0)});
    }
    return out;
}

// Expand  k * prod_i (x - r[i])  into DESCENDING-power coefficients (numpy.poly).
// Result is complex; for conjugate-symmetric root sets the imaginary parts
// cancel to ~0 (use real_part_of_poly to extract).
template <typename T>
[[nodiscard]] crd::containers::Array<Complex<T>> poly_from_roots(crd::memory::IAllocator* alloc,
                                                                 crd::containers::ConstSpan<Complex<T>> r, T k = T(1))
{
    crd::containers::Array<Complex<T>> p(alloc);
    p.push_back(Complex<T>{k, T(0)}); // leading coefficient
    for (crd::usize i = 0; i < r.size(); ++i)
    {
        // multiply p(x) by (x - r[i]): new[j] = old[j] (x-term) - r[i]*old[j] (shifted).
        crd::containers::Array<Complex<T>> np(alloc);
        np.resize(p.size() + 1);
        for (crd::usize j = 0; j < np.size(); ++j)
        {
            np[j] = Complex<T>{T(0), T(0)};
        }
        for (crd::usize j = 0; j < p.size(); ++j)
        {
            np[j].re += p[j].re; // x * old[j]
            np[j].im += p[j].im;
            // - r[i] * old[j]
            np[j + 1].re -= r[i].re * p[j].re - r[i].im * p[j].im;
            np[j + 1].im -= r[i].re * p[j].im + r[i].im * p[j].re;
        }
        p = std::move(np);
    }
    return p;
}

// Extract the real part of a complex polynomial (for conjugate-symmetric roots).
template <typename T>
[[nodiscard]] crd::containers::Array<T> real_part_of_poly(crd::memory::IAllocator* alloc,
                                                          crd::containers::ConstSpan<Complex<T>> p)
{
    crd::containers::Array<T> out(alloc);
    out.resize(p.size());
    for (crd::usize i = 0; i < p.size(); ++i)
    {
        out[i] = p[i].re;
    }
    return out;
}

} // namespace crd::hesap::dsp
