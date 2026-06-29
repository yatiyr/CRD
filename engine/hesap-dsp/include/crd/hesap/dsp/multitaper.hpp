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
#include <crd/hesap/special/incomplete.hpp> // gammainc_p_inv → chi-square ppf for the v12-z spectral CI back-wire
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <numbers>

namespace crd::hesap::dsp
{

// Thomson multitaper PSD (one-sided, length nfft/2+1). nw = time-bandwidth product, K tapers (default 2·NW−1).
// adaptive=true ⇒ Thomson's adaptive eigenvalue weighting (downweights the lower-concentration tapers); else the
// unweighted average.
// `dof_out` (optional, length nfft/2+1): per-frequency equivalent χ² degrees of freedom — 2K for the unweighted
// average; the Thomson/Percival-Walden ν(f) = 2·(Σ d_k²)² / Σ d_k⁴ for the adaptive estimate. Feeds the χ² CI below.
template <typename T>
[[nodiscard]] crd::containers::Array<T> multitaper_psd(crd::memory::IAllocator* alloc,
                                                       crd::containers::ConstSpan<T> x, double nw, crd::usize k_tapers,
                                                       crd::usize nfft, bool adaptive = false,
                                                       crd::containers::Array<T>* dof_out = nullptr)
{
    const crd::usize m = x.size();
    const crd::usize half = nfft / 2 + 1;
    crd::containers::Array<T> psd(alloc);
    psd.resize(half);
    for (crd::usize i = 0; i < half; ++i)
    {
        psd[i] = T(0);
    }
    if (dof_out != nullptr)
    {
        dof_out->resize(half);
        for (crd::usize i = 0; i < half; ++i)
        {
            (*dof_out)[i] = T(0);
        }
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
        a.at(nidx, nidx) = static_cast<T>(centered * centered * crd::math::cos(2.0 * pi * bigW));
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
            sinc[k] = static_cast<T>(crd::math::sin(2.0 * pi * bw * static_cast<double>(k)) /
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
        if (dof_out != nullptr)
        {
            for (crd::usize i = 0; i < half; ++i)
            {
                (*dof_out)[i] = static_cast<T>(2 * kk); // 2K dof: average of K independent χ²₂ periodograms
            }
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
        if (dof_out != nullptr) // Thomson/Percival-Walden ν(f) = 2·(Σ d_k²)² / Σ d_k⁴ (d_k² = the adaptive weight w)
        {
            T sw = T(0);
            T sw2 = T(0);
            for (crd::usize t = 0; t < kk; ++t)
            {
                const T dk = lambda[t] * s / (lambda[t] * s + sigma2 * (T(1) - lambda[t]) + static_cast<T>(1e-30));
                const T w = dk * dk * lambda[t];
                sw += w;
                sw2 += w * w;
            }
            (*dof_out)[i] = (sw2 > T(0)) ? T(2) * sw * sw / sw2 : static_cast<T>(2 * kk);
        }
    }
    return psd;
}

// Multitaper PSD + the χ² (1−α) confidence interval. The estimate has ν(f) equivalent degrees of freedom (2K
// unweighted; the Thomson ν(f) adaptive), so [ν·Ŝ/χ²_{ν,1−α/2}, ν·Ŝ/χ²_{ν,α/2}] with χ²_{ν,q} = 2·gammainc_p_inv(ν/2,q)
// from crd-hesap-special. Back-wires the v11 deferral (needed the χ² ppf). vs MATLAB `pmtm`'s `pxxc`.
template <typename T> struct MultitaperCI
{
    crd::containers::Array<T> psd;
    crd::containers::Array<T> lower;
    crd::containers::Array<T> upper;
    explicit MultitaperCI(crd::memory::IAllocator* alloc) : psd(alloc), lower(alloc), upper(alloc) {}
};

template <typename T>
[[nodiscard]] MultitaperCI<T> multitaper_psd_ci(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                                double nw, crd::usize k_tapers, crd::usize nfft, double alpha = 0.05,
                                                bool adaptive = false)
{
    MultitaperCI<T> out(alloc);
    crd::containers::Array<T> dof(alloc);
    out.psd = multitaper_psd<T>(alloc, x, nw, k_tapers, nfft, adaptive, &dof);
    const crd::usize half = nfft / 2 + 1;
    out.lower.resize(half);
    out.upper.resize(half);
    const T pl = static_cast<T>(1.0 - alpha / 2.0); // χ² upper percentile ⇒ the LOWER PSD bound
    const T pu = static_cast<T>(alpha / 2.0);
    for (crd::usize i = 0; i < half; ++i)
    {
        const T nu = dof[i];
        if (nu <= T(0))
        {
            out.lower[i] = T(0);
            out.upper[i] = T(0);
            continue;
        }
        const T a = nu / T(2);
        const T chi_hi = T(2) * special::gammainc_p_inv<T>(a, pl);
        const T chi_lo = T(2) * special::gammainc_p_inv<T>(a, pu);
        out.lower[i] = nu * out.psd[i] / chi_hi;
        out.upper[i] = nu * out.psd[i] / chi_lo;
    }
    return out;
}

} // namespace crd::hesap::dsp
