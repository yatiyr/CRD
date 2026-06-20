#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-b — window functions (the full scipy.signal.windows set).
//
// Windows taper a finite signal to control spectral leakage. The cosine-sum
// family (hann/hamming/blackman/...) shares one `general_cosine` kernel using
// scipy's exact fac = linspace(-pi, pi, M) form (so the agreement is ~1e-13, not
// just spec). Kaiser needs the modified Bessel I0; Dolph-Chebyshev (chebwin) is
// equiripple via a frequency-domain construction; DPSS/Slepian are the leading
// eigenvectors of a symmetric tridiagonal matrix (-> crd-hesap-eigen / dense).
//
// `sym=true` (default) = symmetric, for FILTER DESIGN. `sym=false` (periodic /
// "fftbins") = for SPECTRAL ANALYSIS: compute length M+1, drop the last sample.
// Gate: vs scipy.signal.windows reference vectors (window_refs.inc) + ENBW.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/eig_sym.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/fft/fft.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <numbers>

namespace crd::hesap::dsp
{

// Modified Bessel function of the first kind, order 0: I0(x) = Σ ((x/2)^k/k!)^2.
// Converges to machine precision for the Kaiser beta range (≤ ~20).
template <typename T> [[nodiscard]] T bessel_i0(T x) noexcept
{
    const double xd = static_cast<double>(x);
    double sum = 1.0;
    double term = 1.0;
    const double y = (xd * xd) / 4.0;
    for (int k = 1; k < 64; ++k)
    {
        term *= y / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < 1e-18 * sum)
        {
            break;
        }
    }
    return static_cast<T>(sum);
}

namespace detail
{
// scipy's _extend: for a periodic window compute length M+1 and truncate to M.
// Returns the working length and whether to drop the last sample.
inline crd::usize win_len(crd::usize m, bool sym, bool& trunc) noexcept
{
    trunc = !sym;
    return sym ? m : m + 1;
}
} // namespace detail

// general_cosine: w[i] = Σ_k a[k] cos(k * fac[i]), fac = linspace(-pi, pi, N).
// Optimized: ONE std::cos per sample via the Chebyshev recurrence cos(k x) = 2 cos(x) cos((k-1)x) - cos((k-2)x)
// (vs K transcendental calls — the blackmanharris/nuttall hot spot), and symmetry w[i] = w[m-1-i] for the
// symmetric (sym=true) case (compute half). Matches scipy to ~1e-13 (the recurrence error is ~K·eps for K≤5).
template <typename T>
[[nodiscard]] crd::containers::Array<T> general_cosine(crd::memory::IAllocator* alloc, crd::usize m,
                                                       crd::containers::ConstSpan<double> a, bool sym = true)
{
    crd::containers::Array<T> w(alloc);
    bool trunc = false;
    const crd::usize n = detail::win_len(m, sym, trunc);
    w.resize(m);
    if (n == 1)
    {
        if (m >= 1)
        {
            w[0] = T(1);
        }
        return w;
    }
    constexpr double pi = std::numbers::pi_v<double>;
    const crd::usize kt = a.size();
    auto sample = [&](crd::usize i) -> double
    {
        const double fac = -pi + 2.0 * pi * static_cast<double>(i) / static_cast<double>(n - 1);
        const double c1 = std::cos(fac); // the only transcendental call
        double s = a[0];
        if (kt > 1)
        {
            s += a[1] * c1;
        }
        double cm2 = 1.0; // cos(0 x)
        double cm1 = c1;  // cos(1 x)
        for (crd::usize k = 2; k < kt; ++k)
        {
            const double ck = 2.0 * c1 * cm1 - cm2; // cos(k x)
            s += a[k] * ck;
            cm2 = cm1;
            cm1 = ck;
        }
        return s;
    };
    const crd::usize half = sym ? (m + 1) / 2 : m;
    for (crd::usize i = 0; i < half; ++i)
    {
        const double s = sample(i);
        w[i] = static_cast<T>(s);
        if (sym)
        {
            w[m - 1 - i] = static_cast<T>(s); // symmetric mirror
        }
    }
    return w;
}

template <typename T>
[[nodiscard]] crd::containers::Array<T> hann(crd::memory::IAllocator* alloc, crd::usize m, bool sym = true)
{
    static const double a[] = {0.5, 0.5};
    return general_cosine<T>(alloc, m, crd::containers::ConstSpan<double>(a, 2), sym);
}
template <typename T>
[[nodiscard]] crd::containers::Array<T> hamming(crd::memory::IAllocator* alloc, crd::usize m, bool sym = true)
{
    static const double a[] = {0.54, 0.46};
    return general_cosine<T>(alloc, m, crd::containers::ConstSpan<double>(a, 2), sym);
}
template <typename T>
[[nodiscard]] crd::containers::Array<T> blackman(crd::memory::IAllocator* alloc, crd::usize m, bool sym = true)
{
    static const double a[] = {0.42, 0.5, 0.08};
    return general_cosine<T>(alloc, m, crd::containers::ConstSpan<double>(a, 3), sym);
}
template <typename T>
[[nodiscard]] crd::containers::Array<T> blackmanharris(crd::memory::IAllocator* alloc, crd::usize m, bool sym = true)
{
    static const double a[] = {0.35875, 0.48829, 0.14128, 0.01168};
    return general_cosine<T>(alloc, m, crd::containers::ConstSpan<double>(a, 4), sym);
}
template <typename T>
[[nodiscard]] crd::containers::Array<T> nuttall(crd::memory::IAllocator* alloc, crd::usize m, bool sym = true)
{
    static const double a[] = {0.3635819, 0.4891775, 0.1365995, 0.0106411};
    return general_cosine<T>(alloc, m, crd::containers::ConstSpan<double>(a, 4), sym);
}
template <typename T>
[[nodiscard]] crd::containers::Array<T> flattop(crd::memory::IAllocator* alloc, crd::usize m, bool sym = true)
{
    static const double a[] = {0.21557895, 0.41663158, 0.277263158, 0.083578947, 0.006947368};
    return general_cosine<T>(alloc, m, crd::containers::ConstSpan<double>(a, 5), sym);
}

template <typename T> [[nodiscard]] crd::containers::Array<T> boxcar(crd::memory::IAllocator* alloc, crd::usize m)
{
    crd::containers::Array<T> w(alloc);
    w.resize(m);
    for (crd::usize i = 0; i < m; ++i)
    {
        w[i] = T(1);
    }
    return w;
}

template <typename T>
[[nodiscard]] crd::containers::Array<T> bartlett(crd::memory::IAllocator* alloc, crd::usize m, bool sym = true)
{
    crd::containers::Array<T> w(alloc);
    bool trunc = false;
    const crd::usize n = detail::win_len(m, sym, trunc);
    w.resize(m);
    for (crd::usize i = 0; i < m; ++i)
    {
        const double x = static_cast<double>(i);
        const double nm1 = static_cast<double>(n - 1);
        w[i] = static_cast<T>((i <= (n - 1) / 2) ? (2.0 * x / nm1) : (2.0 - 2.0 * x / nm1));
    }
    return w;
}

template <typename T>
[[nodiscard]] crd::containers::Array<T> triang(crd::memory::IAllocator* alloc, crd::usize m, bool sym = true)
{
    crd::containers::Array<T> w(alloc);
    bool trunc = false;
    const crd::usize n = detail::win_len(m, sym, trunc);
    w.resize(m);
    for (crd::usize i = 0; i < m; ++i)
    {
        const double k = static_cast<double>(i) + 1.0;
        if (n % 2 == 0)
        {
            w[i] = static_cast<T>((2.0 * k - 1.0) / static_cast<double>(n));
            if (i >= (n + 1) / 2)
            {
                w[i] = static_cast<T>(2.0 - (2.0 * k - 1.0) / static_cast<double>(n));
            }
        }
        else
        {
            w[i] = static_cast<T>(2.0 * k / (static_cast<double>(n) + 1.0));
            if (i >= (n + 1) / 2)
            {
                w[i] = static_cast<T>(2.0 - 2.0 * k / (static_cast<double>(n) + 1.0));
            }
        }
    }
    return w;
}

template <typename T>
[[nodiscard]] crd::containers::Array<T> cosine(crd::memory::IAllocator* alloc, crd::usize m, bool sym = true)
{
    crd::containers::Array<T> w(alloc);
    bool trunc = false;
    const crd::usize n = detail::win_len(m, sym, trunc);
    w.resize(m);
    constexpr double pi = std::numbers::pi_v<double>;
    for (crd::usize i = 0; i < m; ++i)
    {
        w[i] = static_cast<T>(std::sin(pi / static_cast<double>(n) * (static_cast<double>(i) + 0.5)));
    }
    return w;
}

template <typename T>
[[nodiscard]] crd::containers::Array<T> lanczos(crd::memory::IAllocator* alloc, crd::usize m, bool sym = true)
{
    crd::containers::Array<T> w(alloc);
    bool trunc = false;
    const crd::usize n = detail::win_len(m, sym, trunc);
    w.resize(m);
    constexpr double pi = std::numbers::pi_v<double>;
    auto sinc = [](double x) { return (std::abs(x) < 1e-15) ? 1.0 : std::sin(pi * x) / (pi * x); };
    for (crd::usize i = 0; i < m; ++i)
    {
        w[i] = static_cast<T>(sinc(2.0 * static_cast<double>(i) / static_cast<double>(n - 1) - 1.0));
    }
    return w;
}

template <typename T>
[[nodiscard]] crd::containers::Array<T> kaiser(crd::memory::IAllocator* alloc, crd::usize m, double beta,
                                               bool sym = true)
{
    crd::containers::Array<T> w(alloc);
    bool trunc = false;
    const crd::usize n = detail::win_len(m, sym, trunc);
    w.resize(m);
    const double alpha = (static_cast<double>(n) - 1.0) / 2.0;
    const double inv_denom = 1.0 / bessel_i0<double>(beta);
    // symmetry w[i] = w[m-1-i]: halve the (expensive) Bessel-I0 evaluations for the symmetric case.
    const crd::usize half = sym ? (m + 1) / 2 : m;
    for (crd::usize i = 0; i < half; ++i)
    {
        const double r = (static_cast<double>(i) - alpha) / alpha;
        const double v = bessel_i0<double>(beta * std::sqrt(1.0 - r * r)) * inv_denom;
        w[i] = static_cast<T>(v);
        if (sym)
        {
            w[m - 1 - i] = static_cast<T>(v);
        }
    }
    return w;
}

template <typename T>
[[nodiscard]] crd::containers::Array<T> gaussian(crd::memory::IAllocator* alloc, crd::usize m, double std_dev,
                                                 bool sym = true)
{
    crd::containers::Array<T> w(alloc);
    bool trunc = false;
    const crd::usize n = detail::win_len(m, sym, trunc);
    w.resize(m);
    const double c = (static_cast<double>(n) - 1.0) / 2.0;
    const crd::usize half = sym ? (m + 1) / 2 : m; // symmetry: halve the exp() calls
    for (crd::usize i = 0; i < half; ++i)
    {
        const double x = static_cast<double>(i) - c;
        const double v = std::exp(-0.5 * (x / std_dev) * (x / std_dev));
        w[i] = static_cast<T>(v);
        if (sym)
        {
            w[m - 1 - i] = static_cast<T>(v);
        }
    }
    return w;
}

template <typename T>
[[nodiscard]] crd::containers::Array<T> general_gaussian(crd::memory::IAllocator* alloc, crd::usize m, double p,
                                                         double sig, bool sym = true)
{
    crd::containers::Array<T> w(alloc);
    bool trunc = false;
    const crd::usize n = detail::win_len(m, sym, trunc);
    w.resize(m);
    const double c = (static_cast<double>(n) - 1.0) / 2.0;
    const crd::usize half = sym ? (m + 1) / 2 : m;
    for (crd::usize i = 0; i < half; ++i)
    {
        const double x = static_cast<double>(i) - c;
        const double v = std::exp(-0.5 * std::pow(std::abs(x / sig), 2.0 * p));
        w[i] = static_cast<T>(v);
        if (sym)
        {
            w[m - 1 - i] = static_cast<T>(v);
        }
    }
    return w;
}

template <typename T>
[[nodiscard]] crd::containers::Array<T> exponential(crd::memory::IAllocator* alloc, crd::usize m, double tau = 1.0,
                                                    bool sym = true)
{
    crd::containers::Array<T> w(alloc);
    bool trunc = false;
    const crd::usize n = detail::win_len(m, sym, trunc);
    w.resize(m);
    const double center = (static_cast<double>(n) - 1.0) / 2.0; // scipy default center = (M-1)/2
    for (crd::usize i = 0; i < m; ++i)
    {
        w[i] = static_cast<T>(std::exp(-std::abs(static_cast<double>(i) - center) / tau));
    }
    return w;
}

template <typename T>
[[nodiscard]] crd::containers::Array<T> barthann(crd::memory::IAllocator* alloc, crd::usize m, bool sym = true)
{
    crd::containers::Array<T> w(alloc);
    bool trunc = false;
    const crd::usize n = detail::win_len(m, sym, trunc);
    w.resize(m);
    constexpr double pi = std::numbers::pi_v<double>;
    for (crd::usize i = 0; i < m; ++i)
    {
        const double fac = std::abs(static_cast<double>(i) / static_cast<double>(n - 1) - 0.5);
        w[i] = static_cast<T>(0.62 - 0.48 * fac + 0.38 * std::cos(2.0 * pi * fac));
    }
    return w;
}

template <typename T>
[[nodiscard]] crd::containers::Array<T> bohman(crd::memory::IAllocator* alloc, crd::usize m, bool sym = true)
{
    crd::containers::Array<T> w(alloc);
    bool trunc = false;
    const crd::usize n = detail::win_len(m, sym, trunc);
    w.resize(m);
    constexpr double pi = std::numbers::pi_v<double>;
    for (crd::usize i = 0; i < m; ++i)
    {
        // scipy: fac over linspace(-1,1,N) interior; endpoints forced 0.
        if (i == 0 || i == n - 1)
        {
            w[i] = T(0);
            continue;
        }
        const double x = std::abs(-1.0 + 2.0 * static_cast<double>(i) / static_cast<double>(n - 1));
        w[i] = static_cast<T>((1.0 - x) * std::cos(pi * x) + (1.0 / pi) * std::sin(pi * x));
    }
    return w;
}

template <typename T>
[[nodiscard]] crd::containers::Array<T> parzen(crd::memory::IAllocator* alloc, crd::usize m, bool sym = true)
{
    crd::containers::Array<T> w(alloc);
    bool trunc = false;
    const crd::usize n = detail::win_len(m, sym, trunc);
    w.resize(m);
    const double nd = static_cast<double>(n);
    for (crd::usize i = 0; i < m; ++i)
    {
        const double na = static_cast<double>(i) - (nd - 1.0) / 2.0; // centered index
        const double a = std::abs(na);
        const double half = nd / 2.0;
        if (a <= (nd - 1.0) / 4.0)
        {
            const double r = a / half;
            w[i] = static_cast<T>(1.0 - 6.0 * r * r + 6.0 * r * r * r);
        }
        else
        {
            const double r = 1.0 - a / half;
            w[i] = static_cast<T>(2.0 * r * r * r);
        }
    }
    return w;
}

template <typename T>
[[nodiscard]] crd::containers::Array<T> tukey(crd::memory::IAllocator* alloc, crd::usize m, double alpha,
                                              bool sym = true)
{
    crd::containers::Array<T> w(alloc);
    bool trunc = false;
    const crd::usize n = detail::win_len(m, sym, trunc);
    w.resize(m);
    if (alpha <= 0.0)
    {
        for (crd::usize i = 0; i < m; ++i)
        {
            w[i] = T(1);
        }
        return w;
    }
    constexpr double pi = std::numbers::pi_v<double>;
    const double nm1 = static_cast<double>(n - 1);
    const double width = alpha * nm1 / 2.0;
    for (crd::usize i = 0; i < m; ++i)
    {
        const double x = static_cast<double>(i);
        if (x < width)
        {
            w[i] = static_cast<T>(0.5 * (1.0 + std::cos(pi * (x / width - 1.0))));
        }
        else if (x <= nm1 - width)
        {
            w[i] = T(1);
        }
        else
        {
            w[i] = static_cast<T>(0.5 * (1.0 + std::cos(pi * (x / width - 2.0 / alpha + 1.0))));
        }
    }
    return w;
}

// Taylor window (radar / antenna): a Dolph-Chebyshev approximation with `nbar` near-constant-level sidelobes
// at `sll` dB then a 1/x decay — lower close-in sidelobes than uniform without Chebyshev's far-out pedestal.
// Faithful scipy.signal.windows.taylor port. `norm=true` scales the peak to 1.
template <typename T>
[[nodiscard]] crd::containers::Array<T> taylor(crd::memory::IAllocator* alloc, crd::usize m, crd::usize nbar = 4,
                                               double sll = 30.0, bool norm = false, bool sym = true)
{
    crd::containers::Array<T> w(alloc);
    bool trunc = false;
    const crd::usize n = detail::win_len(m, sym, trunc);
    w.resize(m);
    constexpr double pi = std::numbers::pi_v<double>;
    const double bdb = std::pow(10.0, std::abs(sll) / 20.0);
    const double A = std::acosh(bdb) / pi;
    const double s2 = static_cast<double>(nbar * nbar) / (A * A + (static_cast<double>(nbar) - 0.5) * (static_cast<double>(nbar) - 0.5));
    crd::containers::Array<double> Fm(alloc);
    Fm.resize(nbar - 1);
    for (crd::usize mi = 1; mi < nbar; ++mi)
    {
        double numer = ((mi + 1) % 2 == 0) ? 1.0 : -1.0; // (-1)^{mi+1}
        for (crd::usize j = 1; j < nbar; ++j)
        {
            numer *= 1.0 - static_cast<double>(mi * mi) / s2 /
                               (A * A + (static_cast<double>(j) - 0.5) * (static_cast<double>(j) - 0.5));
        }
        double denom = 2.0;
        for (crd::usize k = 1; k < nbar; ++k)
        {
            if (k != mi)
            {
                denom *= 1.0 - static_cast<double>(mi * mi) / static_cast<double>(k * k);
            }
        }
        Fm[mi - 1] = numer / denom;
    }
    double mx = 0.0;
    for (crd::usize i = 0; i < m; ++i)
    {
        const double xi = static_cast<double>(i) - static_cast<double>(n) / 2.0 + 0.5;
        double s = 1.0;
        for (crd::usize mi = 1; mi < nbar; ++mi)
        {
            s += 2.0 * Fm[mi - 1] * std::cos(2.0 * pi * static_cast<double>(mi) * xi / static_cast<double>(n));
        }
        w[i] = static_cast<T>(s);
        if (s > mx)
        {
            mx = s;
        }
    }
    if (norm)
    {
        for (crd::usize i = 0; i < m; ++i)
        {
            w[i] = w[i] / static_cast<T>(mx);
        }
    }
    return w;
}

// DPSS / Slepian (the maximally-concentrated window, Thomson multitaper's tapers): the leading eigenvector of the
// symmetric tridiagonal matrix  d[n] = ((N-1)/2 - n)^2 cos(2*pi*W),  e[n] = n(N-n)/2,  W = NW/N. The eigenvector
// of the LARGEST eigenvalue maximises in-band energy concentration. scipy sign convention: the first taper has a
// positive sum (symmetric, even). -> crd-hesap-dense eig_sym (symmetric tridiagonal). Returns the 0th DPSS.
template <typename T>
[[nodiscard]] crd::containers::Array<T> dpss(crd::memory::IAllocator* alloc, crd::usize m, double nw)
{
    crd::containers::Array<T> w(alloc);
    w.resize(m);
    if (m == 0)
    {
        return w;
    }
    const double bigW = nw / static_cast<double>(m);
    constexpr double pi = std::numbers::pi_v<double>;
    dense::Symmetric<double> A(alloc, m);
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < m; ++j)
        {
            A.at(i, j) = 0.0;
        }
    }
    for (crd::usize n = 0; n < m; ++n)
    {
        const double centered = (static_cast<double>(m) - 1.0) / 2.0 - static_cast<double>(n);
        A.at(n, n) = centered * centered * std::cos(2.0 * pi * bigW);
        if (n + 1 < m)
        {
            const double off = static_cast<double>(n + 1) * (static_cast<double>(m) - static_cast<double>(n + 1)) / 2.0;
            A.at(n, n + 1) = off;
        }
    }
    const dense::EigSym<double> e = dense::eig_sym<double>(alloc, A); // values ascending
    const crd::usize top = m - 1;                                     // largest-eigenvalue eigenvector = DPSS 0
    double sum = 0.0;
    for (crd::usize n = 0; n < m; ++n)
    {
        sum += e.vectors(n, top);
    }
    const double sign = (sum >= 0.0) ? 1.0 : -1.0; // scipy: first taper has positive sum (even-index convention)
    // scipy default norm for Kmax=None is 'approximate': divide by max, then for EVEN M apply the energy
    // correction M^2/(M^2 + NW). (Odd M needs no correction — the centre sample is unity after /max.)
    double mx = 0.0;
    for (crd::usize n = 0; n < m; ++n)
    {
        const double v = sign * e.vectors(n, top);
        if (v > mx)
        {
            mx = v;
        }
    }
    double correction = 1.0;
    if (m % 2 == 0)
    {
        const double md = static_cast<double>(m);
        correction = md * md / (md * md + nw);
    }
    for (crd::usize n = 0; n < m; ++n)
    {
        w[n] = static_cast<T>(sign * e.vectors(n, top) / mx * correction);
    }
    return w;
}

// Dolph-Chebyshev (chebwin): the EQUIRIPPLE window — minimises the main-lobe width for a given sidelobe
// attenuation `at` (dB), all sidelobes at exactly -at. Constructed in the frequency domain: sample the
// Chebyshev polynomial T_{M-1}(beta cos(pi k/M)) then inverse-transform. Faithful scipy port (even/odd M)
// over crd-hesap-fft. `at` in dB (positive). Gate: vs scipy.signal.windows.chebwin.
template <typename T>
[[nodiscard]] crd::containers::Array<T> chebwin(crd::memory::IAllocator* alloc, crd::usize m, double at)
{
    crd::containers::Array<T> w(alloc);
    w.resize(m);
    if (m == 1)
    {
        w[0] = T(1);
        return w;
    }
    constexpr double pi = std::numbers::pi_v<double>;
    const double order = static_cast<double>(m) - 1.0;
    const double beta = std::cosh(std::acosh(std::pow(10.0, std::abs(at) / 20.0)) / order);
    // p[k] = T_order(beta cos(pi k / M)), the three-branch Chebyshev evaluation.
    crd::containers::Array<Complex<double>> p(alloc);
    p.resize(m);
    for (crd::usize k = 0; k < m; ++k)
    {
        const double x = beta * std::cos(pi * static_cast<double>(k) / static_cast<double>(m));
        double pk;
        if (x > 1.0)
        {
            pk = std::cosh(order * std::acosh(x));
        }
        else if (x < -1.0)
        {
            const double s = (static_cast<int>(order) % 2 == 0) ? 1.0 : -1.0;
            pk = s * std::cosh(order * std::acosh(-x));
        }
        else
        {
            pk = std::cos(order * std::acos(x));
        }
        p[k] = Complex<double>{pk, 0.0};
    }
    crd::containers::Array<double> wr(alloc);
    wr.resize(m);
    if (m % 2 == 1) // odd M: real FFT of p, then mirror
    {
        crd::hesap::fft::FftPlan<double> plan(alloc, m);
        plan.execute(crd::containers::Span<Complex<double>>(p.data(), m), crd::hesap::fft::FftDirection::Forward);
        for (crd::usize k = 0; k < m; ++k)
        {
            wr[k] = p[k].re;
        }
        const crd::usize n = (m + 1) / 2;
        for (crd::usize i = 0; i < n; ++i)
        {
            w[n - 1 + i] = static_cast<T>(wr[i]);
            if (i > 0)
            {
                w[n - 1 - i] = static_cast<T>(wr[i]);
            }
        }
    }
    else // even M: phase-rotate p, FFT, then mirror
    {
        for (crd::usize k = 0; k < m; ++k)
        {
            const double ph = pi * static_cast<double>(k) / static_cast<double>(m);
            const double cr = std::cos(ph);
            const double ci = std::sin(ph);
            p[k] = Complex<double>{p[k].re * cr, p[k].re * ci};
        }
        crd::hesap::fft::FftPlan<double> plan(alloc, m);
        plan.execute(crd::containers::Span<Complex<double>>(p.data(), m), crd::hesap::fft::FftDirection::Forward);
        for (crd::usize k = 0; k < m; ++k)
        {
            wr[k] = p[k].re;
        }
        const crd::usize n = m / 2 + 1; // = M/2+1
        // concat(wr[n-1:0:-1], wr[1:n]) ⇒ [wr[n-1..1], wr[1..n-1]]
        crd::usize idx = 0;
        for (crd::usize i = n - 1; i >= 1; --i)
        {
            w[idx++] = static_cast<T>(wr[i]);
            if (i == 1)
            {
                break;
            }
        }
        for (crd::usize i = 1; i <= n - 1; ++i)
        {
            w[idx++] = static_cast<T>(wr[i]);
        }
    }
    // normalize by the max.
    T mx = w[0];
    for (crd::usize i = 1; i < m; ++i)
    {
        if (w[i] > mx)
        {
            mx = w[i];
        }
    }
    for (crd::usize i = 0; i < m; ++i)
    {
        w[i] = w[i] / mx;
    }
    return w;
}

// Equivalent Noise Bandwidth (bins): N * Σ w^2 / (Σ w)^2 — the standard window metric.
template <typename T> [[nodiscard]] double enbw(crd::containers::ConstSpan<T> w) noexcept
{
    double s1 = 0.0;
    double s2 = 0.0;
    for (crd::usize i = 0; i < w.size(); ++i)
    {
        s1 += static_cast<double>(w[i]);
        s2 += static_cast<double>(w[i]) * static_cast<double>(w[i]);
    }
    return static_cast<double>(w.size()) * s2 / (s1 * s1);
}

} // namespace crd::hesap::dsp
