#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-p — subspace (super-resolution) line-spectrum estimation.
//
//   music_spectrum   the MUSIC pseudospectrum P(f) = 1/Σ|eₖᴴ a(f)|² over the
//                    noise-subspace eigenvectors — sharp peaks at the tones.
//   root_music       the roots of the MUSIC null polynomial near the unit
//                    circle ⇒ the frequencies directly (no grid search).
//
// The covariance is the biased autocorrelation Toeplitz matrix (M×M); its
// symmetric eigendecomposition (crd-hesap-dense eig_sym) splits signal vs noise
// subspaces. K real tones ⇒ signal-subspace dimension 2K. The killer property
// (the gate): resolve tones separated by LESS than the FFT bin width 1/N — the
// super-resolution an FFT/periodogram cannot. ESPRIT (general eig) = follow-on.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/eig_nonsym.hpp> // dense::eig (ESPRIT rotation eigenvalues)
#include <crd/hesap/dense/eig_sym.hpp>
#include <crd/hesap/dsp/polynomial.hpp> // roots
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <numbers>

namespace crd::hesap::dsp
{

namespace detail
{
// M×M biased-autocorrelation Toeplitz covariance + its symmetric eigendecomposition.
template <typename T>
[[nodiscard]] dense::EigSym<T> subspace_eig(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                            crd::usize m)
{
    const crd::usize n = x.size();
    crd::containers::Array<T> r(alloc);
    r.resize(m);
    for (crd::usize k = 0; k < m; ++k)
    {
        T s = T(0);
        for (crd::usize i = 0; i + k < n; ++i)
        {
            s += x[i] * x[i + k];
        }
        r[k] = s / static_cast<T>(n);
    }
    dense::Symmetric<T> a(alloc, m);
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < m; ++j)
        {
            a.at(i, j) = r[(i > j) ? (i - j) : (j - i)];
        }
    }
    return dense::eig_sym<T>(alloc, a); // values ascending ⇒ noise subspace = the first (m - 2K) columns
}
} // namespace detail

// MUSIC pseudospectrum over `nfreq` uniform frequencies in [0, 0.5) cycles/sample. k_signals = number of real tones.
template <typename T>
[[nodiscard]] crd::containers::Array<T> music_spectrum(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                                       crd::usize m, crd::usize k_signals, crd::usize nfreq)
{
    const auto e = detail::subspace_eig<T>(alloc, x, m);
    const crd::usize nnoise = m - 2 * k_signals; // noise subspace = columns 0..nnoise-1 (smallest eigenvalues)
    const T two_pi = static_cast<T>(2.0 * std::numbers::pi_v<double>);
    crd::containers::Array<T> p(alloc);
    p.resize(nfreq);
    for (crd::usize fi = 0; fi < nfreq; ++fi)
    {
        const T f = T(0.5) * static_cast<T>(fi) / static_cast<T>(nfreq);
        T denom = T(0);
        for (crd::usize c = 0; c < nnoise; ++c) // Σ |eₖᴴ a(f)|²
        {
            T re = T(0), im = T(0);
            for (crd::usize mm = 0; mm < m; ++mm)
            {
                const T w = two_pi * f * static_cast<T>(mm);
                re += e.vectors(mm, c) * crd::math::cos(w);
                im -= e.vectors(mm, c) * crd::math::sin(w);
            }
            denom += re * re + im * im;
        }
        p[fi] = T(1) / denom;
    }
    return p;
}

// root-MUSIC: the K positive frequencies (cycles/sample), ascending. K real tones (signal-subspace dim 2K).
template <typename T>
[[nodiscard]] crd::containers::Array<T> root_music(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                                   crd::usize m, crd::usize k_signals)
{
    const auto e = detail::subspace_eig<T>(alloc, x, m);
    const crd::usize nnoise = m - 2 * k_signals;
    // C[d] = Σ_k Σ_i eₖ[i] eₖ[i+d] (the d-diagonal sums of the noise projection), d = 0..m-1; C[-d]=C[d].
    crd::containers::Array<T> c(alloc);
    c.resize(m);
    for (crd::usize d = 0; d < m; ++d)
    {
        T s = T(0);
        for (crd::usize col = 0; col < nnoise; ++col)
        {
            for (crd::usize i = 0; i + d < m; ++i)
            {
                s += e.vectors(i, col) * e.vectors(i + d, col);
            }
        }
        c[d] = s;
    }
    // polynomial (descending) of degree 2(m-1), symmetric coeffs [C[m-1]..C[1],C[0],C[1]..C[m-1]].
    crd::containers::Array<T> poly(alloc);
    poly.resize(2 * m - 1);
    for (crd::usize i = 0; i < m; ++i)
    {
        poly[i] = c[m - 1 - i];
        poly[2 * m - 2 - i] = c[m - 1 - i];
    }
    const auto rts = roots<T>(alloc, crd::containers::ConstSpan<T>(poly.data(), poly.size()));
    // pick the 2K roots INSIDE the unit circle closest to it; keep the positive-frequency representatives.
    crd::containers::Array<T> freqs(alloc);
    crd::containers::Array<T> mags(alloc);
    const T two_pi = static_cast<T>(2.0 * std::numbers::pi_v<double>);
    for (crd::usize i = 0; i < rts.size(); ++i)
    {
        const T mag = crd::math::hypot(rts[i].re, rts[i].im);
        if (mag < T(1) && mag > T(1e-6))
        {
            const T f = crd::math::atan2(rts[i].im, rts[i].re) / two_pi;
            if (f > T(1e-6)) // positive-frequency root
            {
                freqs.push_back(f);
                mags.push_back(mag);
            }
        }
    }
    // keep the K with largest |z| (closest to the unit circle), then sort ascending by frequency.
    for (crd::usize i = 0; i + 1 < freqs.size(); ++i)
    {
        for (crd::usize j = i + 1; j < freqs.size(); ++j)
        {
            if (mags[j] > mags[i])
            {
                std::swap(mags[i], mags[j]);
                std::swap(freqs[i], freqs[j]);
            }
        }
    }
    crd::containers::Array<T> out(alloc);
    const crd::usize keep = (k_signals < freqs.size()) ? k_signals : freqs.size();
    for (crd::usize i = 0; i < keep; ++i)
    {
        out.push_back(freqs[i]);
    }
    for (crd::usize i = 0; i + 1 < out.size(); ++i) // ascending by frequency
    {
        for (crd::usize j = i + 1; j < out.size(); ++j)
        {
            if (out[j] < out[i])
            {
                std::swap(out[i], out[j]);
            }
        }
    }
    return out;
}

// ESPRIT: the K positive frequencies via rotational invariance of the signal subspace (no grid, no root-finding).
// Es1 = signal subspace rows 0..m-2, Es2 = rows 1..m-1; the eigenvalues of Φ = Es1⁺·Es2 are e^{jω_i}.
template <typename T>
[[nodiscard]] crd::containers::Array<T> esprit(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                               crd::usize m, crd::usize k_signals)
{
    const auto e = detail::subspace_eig<T>(alloc, x, m);
    const crd::usize d = 2 * k_signals; // signal-subspace dimension (K real tones)
    // A = Es1ᵀEs1, B = Es1ᵀEs2 (d×d). Es column i = eigenvector for the i-th LARGEST eigenvalue (col m-1-i).
    crd::containers::Array<T> a(alloc), b(alloc);
    a.resize(d * d);
    b.resize(d * d);
    for (crd::usize i = 0; i < d; ++i)
    {
        for (crd::usize j = 0; j < d; ++j)
        {
            T aij = T(0), bij = T(0);
            for (crd::usize r = 0; r + 1 < m; ++r)
            {
                const T es1_ri = e.vectors(r, m - 1 - i);
                aij += es1_ri * e.vectors(r, m - 1 - j);     // Es1[r][i]·Es1[r][j]
                bij += es1_ri * e.vectors(r + 1, m - 1 - j); // Es1[r][i]·Es2[r][j]
            }
            a[i * d + j] = aij;
            b[i * d + j] = bij;
        }
    }
    // solve A·Φ = B (Gaussian elimination with partial pivoting; B is overwritten with Φ).
    for (crd::usize c = 0; c < d; ++c)
    {
        crd::usize piv = c;
        for (crd::usize i = c + 1; i < d; ++i)
        {
            if (std::abs(a[i * d + c]) > std::abs(a[piv * d + c]))
            {
                piv = i;
            }
        }
        if (piv != c)
        {
            for (crd::usize j = 0; j < d; ++j)
            {
                std::swap(a[c * d + j], a[piv * d + j]);
                std::swap(b[c * d + j], b[piv * d + j]);
            }
        }
        const T dd = a[c * d + c];
        for (crd::usize i = 0; i < d; ++i)
        {
            if (i == c)
            {
                continue;
            }
            const T f = a[i * d + c] / dd;
            for (crd::usize j = 0; j < d; ++j)
            {
                a[i * d + j] -= f * a[c * d + j];
                b[i * d + j] -= f * b[c * d + j];
            }
        }
        for (crd::usize j = 0; j < d; ++j)
        {
            b[c * d + j] /= dd;
        }
    }
    dense::Matrix<T> phi(alloc, d, d);
    for (crd::usize i = 0; i < d; ++i)
    {
        for (crd::usize j = 0; j < d; ++j)
        {
            phi(i, j) = b[i * d + j];
        }
    }
    const auto eg = dense::eig<T>(alloc, phi); // eigenvalues e^{jω_i}
    const T two_pi = static_cast<T>(2.0 * std::numbers::pi_v<double>);
    crd::containers::Array<T> freqs(alloc);
    for (crd::usize i = 0; i < d; ++i)
    {
        const T f = crd::math::atan2(eg.values(i).im, eg.values(i).re) / two_pi;
        if (f > static_cast<T>(1e-6))
        {
            freqs.push_back(f);
        }
    }
    for (crd::usize i = 0; i + 1 < freqs.size(); ++i) // ascending
    {
        for (crd::usize j = i + 1; j < freqs.size(); ++j)
        {
            if (freqs[j] < freqs[i])
            {
                std::swap(freqs[i], freqs[j]);
            }
        }
    }
    crd::containers::Array<T> out(alloc);
    for (crd::usize i = 0; i < freqs.size() && i < k_signals; ++i)
    {
        out.push_back(freqs[i]);
    }
    return out;
}

// Min-norm: the K positive frequencies from the roots of the min-norm noise polynomial d = Eₙ·Eₙᵀ·e₁, nearest the
// unit circle. Lower spurious-root bias than root-MUSIC's full sum.
template <typename T>
[[nodiscard]] crd::containers::Array<T> min_norm(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                                 crd::usize m, crd::usize k_signals)
{
    const auto e = detail::subspace_eig<T>(alloc, x, m);
    const crd::usize nnoise = m - 2 * k_signals;
    crd::containers::Array<T> dvec(alloc); // d[i] = Σ_c Eₙ[i][c]·Eₙ[0][c]  (the first column of Eₙ·Eₙᵀ)
    dvec.resize(m);
    for (crd::usize i = 0; i < m; ++i)
    {
        T s = T(0);
        for (crd::usize c = 0; c < nnoise; ++c)
        {
            s += e.vectors(i, c) * e.vectors(0, c);
        }
        dvec[i] = s;
    }
    const auto rts = roots<T>(alloc, crd::containers::ConstSpan<T>(dvec.data(), m)); // D(z) roots
    const T two_pi = static_cast<T>(2.0 * std::numbers::pi_v<double>);
    crd::containers::Array<T> freqs(alloc), dist(alloc);
    for (crd::usize i = 0; i < rts.size(); ++i)
    {
        const T f = crd::math::atan2(rts[i].im, rts[i].re) / two_pi;
        if (f > static_cast<T>(1e-6))
        {
            freqs.push_back(f);
            dist.push_back(std::abs(crd::math::hypot(rts[i].re, rts[i].im) - T(1))); // closeness to the unit circle
        }
    }
    for (crd::usize i = 0; i + 1 < freqs.size(); ++i) // keep the K closest to |z|=1
    {
        for (crd::usize j = i + 1; j < freqs.size(); ++j)
        {
            if (dist[j] < dist[i])
            {
                std::swap(dist[i], dist[j]);
                std::swap(freqs[i], freqs[j]);
            }
        }
    }
    crd::containers::Array<T> out(alloc);
    const crd::usize keep = (k_signals < freqs.size()) ? k_signals : freqs.size();
    for (crd::usize i = 0; i < keep; ++i)
    {
        out.push_back(freqs[i]);
    }
    for (crd::usize i = 0; i + 1 < out.size(); ++i)
    {
        for (crd::usize j = i + 1; j < out.size(); ++j)
        {
            if (out[j] < out[i])
            {
                std::swap(out[i], out[j]);
            }
        }
    }
    return out;
}

} // namespace crd::hesap::dsp
