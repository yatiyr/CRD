#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-n — Thomson multitaper spectral estimate.
//
//   multitaper_psd   the average of K DPSS-tapered periodograms: low variance
//                    + low leakage (the K = 2·NW−1 well-concentrated Slepian
//                    tapers each give a near-independent spectral estimate).
//
// The K tapers are the top-K eigenvectors of the same tridiagonal matrix the
// single-taper `dpss` uses (crd-hesap-dense eig_sym). Each taper windows x, an
// rFFT gives its eigenspectrum |X_k|², and the (unweighted, K=2NW−1 ⇒ λ≈1)
// average is the estimate. Gate: white noise → flat PSD + a tone → a sharp peak
// + lower variance than a single periodogram. (Adaptive eigenvalue weighting +
// the parallel-batched FFT moat = follow-ons.) vs MATLAB `pmtm`.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/eig_sym.hpp>
#include <crd/hesap/fft/real_fft.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <numbers>

namespace crd::hesap::dsp
{

// Thomson multitaper PSD (one-sided, length nfft/2+1). nw = time-bandwidth product, K tapers (default 2·NW−1).
// adaptive=true ⇒ Thomson's adaptive eigenvalue weighting (downweights the lower-concentration tapers); else the
// unweighted average.
template <typename T>
[[nodiscard]] crd::containers::Array<T> multitaper_psd(crd::memory::IAllocator* alloc,
                                                       crd::containers::ConstSpan<T> x, double nw, crd::usize k_tapers,
                                                       crd::usize nfft, bool adaptive = false)
{
    const crd::usize m = x.size();
    const crd::usize half = nfft / 2 + 1;
    crd::containers::Array<T> psd(alloc);
    psd.resize(half);
    for (crd::usize i = 0; i < half; ++i)
    {
        psd[i] = T(0);
    }
    if (m == 0 || k_tapers == 0)
    {
        return psd;
    }
    // DPSS tridiagonal (Slepian): eigenvectors are the tapers; the K largest eigenvalues ⇒ best concentration.
    const double bigW = nw / static_cast<double>(m);
    constexpr double pi = std::numbers::pi_v<double>;
    dense::Symmetric<T> a(alloc, m);
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < m; ++j)
        {
            a.at(i, j) = T(0);
        }
    }
    for (crd::usize nidx = 0; nidx < m; ++nidx)
    {
        const double centered = (static_cast<double>(m) - 1.0) / 2.0 - static_cast<double>(nidx);
        a.at(nidx, nidx) = static_cast<T>(centered * centered * std::cos(2.0 * pi * bigW));
        if (nidx + 1 < m)
        {
            const double off =
                static_cast<double>(nidx + 1) * (static_cast<double>(m) - static_cast<double>(nidx + 1)) / 2.0;
            a.at(nidx, nidx + 1) = static_cast<T>(off);
        }
    }
    const dense::EigSym<T> e = dense::eig_sym<T>(alloc, a); // values ascending ⇒ tapers are columns m-1, m-2, …
    const crd::usize kk = (k_tapers < m) ? k_tapers : m;

    fft::RealFftPlan<T> plan(alloc, nfft);
    crd::containers::Array<T> tapered(alloc), sk(alloc), lambda(alloc), sinc(alloc);
    crd::containers::Array<Complex<T>> spec(alloc);
    tapered.resize(nfft);
    spec.resize(half);
    sk.resize(kk * half); // the K eigenspectra |X_k(f)|²
    lambda.resize(kk);
    if (adaptive) // sinc (prolate) kernel row c[k] = sin(2πW·k)/(π·k), c[0] = 2W — for the concentration eigenvalues
    {
        sinc.resize(m);
        const double bw = nw / static_cast<double>(m);
        sinc[0] = static_cast<T>(2.0 * bw);
        for (crd::usize k = 1; k < m; ++k)
        {
            sinc[k] = static_cast<T>(std::sin(2.0 * pi * bw * static_cast<double>(k)) /
                                     (pi * static_cast<double>(k)));
        }
    }
    for (crd::usize t = 0; t < kk; ++t)
    {
        const crd::usize col = m - 1 - t; // descending eigenvalue (best-concentrated first)
        for (crd::usize i = 0; i < nfft; ++i)
        {
            tapered[i] = (i < m) ? e.vectors(i, col) * x[i] : T(0);
        }
        plan.rfft(crd::containers::ConstSpan<T>(tapered.data(), nfft),
                  crd::containers::Span<Complex<T>>(spec.data(), half));
        for (crd::usize i = 0; i < half; ++i)
        {
            sk[t * half + i] = spec[i].re * spec[i].re + spec[i].im * spec[i].im;
        }
        if (adaptive) // λ_t = vᵀ·C·v (Toeplitz sinc kernel) — the energy concentration of taper t
        {
            T lam = T(0);
            for (crd::usize i = 0; i < m; ++i)
            {
                T cv = T(0);
                for (crd::usize j = 0; j < m; ++j)
                {
                    cv += sinc[(i > j) ? (i - j) : (j - i)] * e.vectors(j, col);
                }
                lam += e.vectors(i, col) * cv;
            }
            lambda[t] = lam;
        }
    }
    if (!adaptive)
    {
        const T inv = T(1) / static_cast<T>(kk); // unweighted average
        for (crd::usize i = 0; i < half; ++i)
        {
            T s = T(0);
            for (crd::usize t = 0; t < kk; ++t)
            {
                s += sk[t * half + i];
            }
            psd[i] = s * inv;
        }
        return psd;
    }
    // Thomson adaptive weighting: per-frequency d_k(f)² = λ_k S(f) / (λ_k S(f) + σ²(1-λ_k)), iterate.
    T sigma2 = T(0); // signal variance estimate
    for (crd::usize i = 0; i < m; ++i)
    {
        sigma2 += x[i] * x[i];
    }
    sigma2 /= static_cast<T>(m);
    for (crd::usize i = 0; i < half; ++i)
    {
        T s = (sk[0 * half + i] + (kk > 1 ? sk[1 * half + i] : sk[0 * half + i])) / T(2); // init from top-2 tapers
        for (int iter = 0; iter < 6; ++iter)
        {
            T num = T(0), den = T(0);
            for (crd::usize t = 0; t < kk; ++t)
            {
                const T dk = lambda[t] * s / (lambda[t] * s + sigma2 * (T(1) - lambda[t]) + static_cast<T>(1e-30));
                const T w = dk * dk * lambda[t];
                num += w * sk[t * half + i];
                den += w;
            }
            s = (den > T(0)) ? num / den : s;
        }
        psd[i] = s;
    }
    return psd;
}

} // namespace crd::hesap::dsp
