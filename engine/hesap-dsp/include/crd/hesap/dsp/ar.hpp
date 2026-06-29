#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-o — parametric AR (autoregressive) spectral estimation.
//
//   aryule   Yule-Walker: biased autocorrelation → Levinson-Durbin recursion.
//   arburg   Burg: forward/backward prediction-error minimization (better for
//            short records + spectral-line resolution).
//   ar_psd   the AR power spectral density from a model: σ²/|A(e^{jω})|².
//   aic / mdl   model-order selection from the prediction-error variance.
//
// Returns an ArModel: A(z) = 1 + a[1]z⁻¹ + … + a[p]z⁻p (the MATLAB/scipy
// convention, a[0]=1), the white-noise variance, and the reflection
// coefficients. Gate: recover a known AR process's coefficients + AR-PSD vs
// MATLAB `aryule`/`arburg`/`pyulear`/`pburg` + reflection-coeff |k|<1 stability.
// Lower-layer raw scalars.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/special/erf.hpp> // ndtri (normal ppf) → the asymptotic AR-PSD CI (v12-z)
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <numbers>

namespace crd::hesap::dsp
{

template <typename T> struct ArModel
{
    crd::containers::Array<T> a;          // A(z) = 1 + a[1]z⁻¹ + … (a[0] == 1)
    crd::containers::Array<T> reflection; // p reflection coefficients k[1..p]
    T variance = T(1);                    // white-noise / prediction-error variance
    explicit ArModel(crd::memory::IAllocator* alloc) : a(alloc), reflection(alloc) {}
};

// Yule-Walker AR estimate (biased autocorrelation + Levinson-Durbin), order p.
template <typename T>
[[nodiscard]] ArModel<T> aryule(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x, crd::usize p)
{
    const crd::usize nn = x.size();
    crd::containers::Array<T> r(alloc);
    r.resize(p + 1);
    for (crd::usize k = 0; k <= p; ++k) // biased autocorrelation r[k] = (1/N) Σ x[n] x[n+k]
    {
        T s = T(0);
        for (crd::usize n = 0; n + k < nn; ++n)
        {
            s += x[n] * x[n + k];
        }
        r[k] = s / static_cast<T>(nn);
    }
    ArModel<T> m(alloc);
    m.a.resize(p + 1);
    m.reflection.resize(p);
    for (crd::usize i = 0; i <= p; ++i)
    {
        m.a[i] = T(0);
    }
    m.a[0] = T(1);
    T e = r[0];
    for (crd::usize i = 1; i <= p; ++i) // Levinson-Durbin
    {
        T acc = r[i];
        for (crd::usize j = 1; j < i; ++j)
        {
            acc += m.a[j] * r[i - j];
        }
        const T k = (e > T(0)) ? (-acc / e) : T(0);
        // symmetric in-place update of a[1..i-1]
        for (crd::usize j = 1; j <= i / 2; ++j)
        {
            const T tmp = m.a[j] + k * m.a[i - j];
            m.a[i - j] += k * m.a[j];
            m.a[j] = tmp;
        }
        m.a[i] = k;
        m.reflection[i - 1] = k;
        e *= (T(1) - k * k);
    }
    m.variance = e;
    return m;
}

// Burg AR estimate, order p.
template <typename T>
[[nodiscard]] ArModel<T> arburg(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x, crd::usize p)
{
    const crd::usize nn = x.size();
    crd::containers::Array<T> f(alloc), b(alloc);
    f.resize(nn);
    b.resize(nn);
    T den0 = T(0);
    for (crd::usize i = 0; i < nn; ++i)
    {
        f[i] = x[i];
        b[i] = x[i];
        den0 += x[i] * x[i];
    }
    ArModel<T> m(alloc);
    m.a.resize(p + 1);
    m.reflection.resize(p);
    for (crd::usize i = 0; i <= p; ++i)
    {
        m.a[i] = T(0);
    }
    m.a[0] = T(1);
    T e = den0 / static_cast<T>(nn);
    for (crd::usize i = 1; i <= p; ++i)
    {
        // k = -2·Σ f[n]b[n-1] / Σ(f[n]²+b[n-1]²) over n = i..N-1 (computed fresh each order).
        T num = T(0);
        for (crd::usize n = i; n < nn; ++n)
        {
            num += f[n] * b[n - 1];
        }
        T d = T(0);
        for (crd::usize n = i; n < nn; ++n)
        {
            d += f[n] * f[n] + b[n - 1] * b[n - 1];
        }
        const T k = (d > T(0)) ? (-T(2) * num / d) : T(0);
        for (crd::usize j = 1; j <= i / 2; ++j)
        {
            const T tmp = m.a[j] + k * m.a[i - j];
            m.a[i - j] += k * m.a[j];
            m.a[j] = tmp;
        }
        m.a[i] = k;
        m.reflection[i - 1] = k;
        e *= (T(1) - k * k);
        // update forward/backward errors (descending so in-place is safe): f_m[n], b_m[n] both from f[n], b[n-1].
        for (crd::usize n = nn - 1; n >= i; --n)
        {
            const T fn = f[n] + k * b[n - 1];
            const T bn = b[n - 1] + k * f[n];
            f[n] = fn;
            b[n] = bn; // the new backward error is stored at index n (NOT n-1)
            if (n == i)
            {
                break; // unsigned guard
            }
        }
    }
    m.variance = e;
    return m;
}

namespace detail
{
// Covariance / modified-covariance AR: minimize the forward (+ backward) prediction error over the valid window
// (no autocorrelation windowing) ⇒ a p×p normal-equation solve. Sharper line resolution than Yule-Walker/Burg.
template <typename T>
[[nodiscard]] ArModel<T> ar_covariance(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x, crd::usize p,
                                       bool modified)
{
    const crd::usize n = x.size();
    auto cov = [&](crd::usize j, crd::usize k) -> T
    {
        T s = T(0);
        for (crd::usize i = p; i < n; ++i) // forward
        {
            s += x[i - j] * x[i - k];
        }
        if (modified) // backward
        {
            for (crd::usize i = 0; i + p < n; ++i)
            {
                s += x[i + j] * x[i + k];
            }
        }
        return s;
    };
    crd::containers::Array<T> a(alloc), b(alloc); // p×p system a·sol = b, b[i] = -cov(i+1,0)
    a.resize(p * p);
    b.resize(p);
    for (crd::usize i = 0; i < p; ++i)
    {
        for (crd::usize j = 0; j < p; ++j)
        {
            a[i * p + j] = cov(i + 1, j + 1);
        }
        b[i] = -cov(i + 1, 0);
    }
    for (crd::usize c = 0; c < p; ++c) // Gaussian elimination with partial pivoting
    {
        crd::usize piv = c;
        for (crd::usize i = c + 1; i < p; ++i)
        {
            if (std::abs(a[i * p + c]) > std::abs(a[piv * p + c]))
            {
                piv = i;
            }
        }
        if (piv != c)
        {
            for (crd::usize j = 0; j < p; ++j)
            {
                std::swap(a[c * p + j], a[piv * p + j]);
            }
            std::swap(b[c], b[piv]);
        }
        const T d0 = a[c * p + c];
        for (crd::usize i = c + 1; i < p; ++i)
        {
            const T f = a[i * p + c] / d0;
            for (crd::usize j = c; j < p; ++j)
            {
                a[i * p + j] -= f * a[c * p + j];
            }
            b[i] -= f * b[c];
        }
    }
    ArModel<T> m(alloc);
    m.a.resize(p + 1);
    m.reflection.resize(p);
    m.a[0] = T(1);
    for (crd::usize ii = p; ii-- > 0;)
    {
        T s = b[ii];
        for (crd::usize j = ii + 1; j < p; ++j)
        {
            s -= a[ii * p + j] * m.a[j + 1];
        }
        m.a[ii + 1] = s / a[ii * p + ii];
    }
    T var = cov(0, 0);
    for (crd::usize k = 1; k <= p; ++k)
    {
        var += m.a[k] * cov(0, k);
    }
    m.variance = var / static_cast<T>(modified ? 2 * (n - p) : (n - p));
    for (crd::usize k = 0; k < p; ++k) // reflection coeffs not produced by the direct solve — leave 0 (not used here)
    {
        m.reflection[k] = T(0);
    }
    return m;
}
} // namespace detail

// Covariance-method AR (forward prediction error only).
template <typename T>
[[nodiscard]] ArModel<T> arcov(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x, crd::usize p)
{
    return detail::ar_covariance<T>(alloc, x, p, false);
}
// Modified-covariance AR (forward + backward prediction error — the best line resolution of the four).
template <typename T>
[[nodiscard]] ArModel<T> armcov(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x, crd::usize p)
{
    return detail::ar_covariance<T>(alloc, x, p, true);
}

// AR power spectral density at nfft uniform frequencies [0, fs): σ²/|A(e^{jω})|² (one-sided scaling left to caller).
template <typename T>
[[nodiscard]] crd::containers::Array<T> ar_psd(crd::memory::IAllocator* alloc, const ArModel<T>& m, crd::usize nfft,
                                               T fs = T(1))
{
    const T two_pi = static_cast<T>(2.0 * std::numbers::pi_v<double>);
    crd::containers::Array<T> psd(alloc);
    psd.resize(nfft);
    const crd::usize p = m.a.size() - 1;
    for (crd::usize bin = 0; bin < nfft; ++bin)
    {
        const T w = two_pi * static_cast<T>(bin) / static_cast<T>(nfft);
        T re = T(0), im = T(0);
        for (crd::usize j = 0; j <= p; ++j) // A(e^{jω}) = Σ a[j] e^{-jωj}
        {
            re += m.a[j] * crd::math::cos(w * static_cast<T>(j));
            im -= m.a[j] * crd::math::sin(w * static_cast<T>(j));
        }
        psd[bin] = m.variance / (fs * (re * re + im * im));
    }
    return psd;
}

template <typename T> struct ArPsdCI
{
    crd::containers::Array<T> psd;
    crd::containers::Array<T> lower;
    crd::containers::Array<T> upper;
    explicit ArPsdCI(crd::memory::IAllocator* alloc) : psd(alloc), lower(alloc), upper(alloc) {}
};

// AR PSD + asymptotic-normal (1−α) confidence interval. Berk (1974): Var{log Ŝ(f)} ≈ 2p/N, so a constant-relative-width
// band Ŝ(f)·exp(±z·√(2p/N)) with z = ndtri(1−α/2). NOTE: unlike the multitaper χ² CI, NO scipy/MATLAB peer returns this
// — it is gated analytically against the closed-form log-width + bracketing (v12-z; the honest "no gold peer" case).
template <typename T>
[[nodiscard]] ArPsdCI<T> ar_psd_ci(crd::memory::IAllocator* alloc, const ArModel<T>& m, crd::usize n_samples,
                                   crd::usize nfft, double alpha = 0.05, T fs = T(1))
{
    ArPsdCI<T> out(alloc);
    out.psd = ar_psd<T>(alloc, m, nfft, fs);
    const crd::usize p = m.a.size() - 1;
    const T z = special::ndtri<T>(static_cast<T>(1.0 - alpha / 2.0));
    const T sigma = crd::math::sqrt(static_cast<T>(2 * p) / static_cast<T>(n_samples));
    const T mlo = crd::math::exp(-z * sigma);
    const T mhi = crd::math::exp(z * sigma);
    out.lower.resize(nfft);
    out.upper.resize(nfft);
    for (crd::usize i = 0; i < nfft; ++i)
    {
        out.lower[i] = out.psd[i] * mlo;
        out.upper[i] = out.psd[i] * mhi;
    }
    return out;
}

template <typename T> [[nodiscard]] T ar_aic(crd::usize n, T variance, crd::usize p) noexcept
{
    return static_cast<T>(n) * crd::math::log(variance) + T(2) * static_cast<T>(p);
}
template <typename T> [[nodiscard]] T ar_mdl(crd::usize n, T variance, crd::usize p) noexcept
{
    return static_cast<T>(n) * crd::math::log(variance) + static_cast<T>(p) * crd::math::log(static_cast<T>(n));
}

} // namespace crd::hesap::dsp
