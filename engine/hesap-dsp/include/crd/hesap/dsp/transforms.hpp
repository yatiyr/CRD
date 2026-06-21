#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-q — spectral transforms.
//
//   goertzel   single-frequency DFT value via the Goertzel recurrence (DTMF /
//              tone detection — O(N) per frequency, no full FFT).
//   czt        chirp-z transform on a unit-circle arc (zoom-FFT): the DFT
//              evaluated at M points z_k = A·W^{-k}, |A|=|W|=1 (scipy.signal.czt
//              default + zoom; general spiral |W|≠1 is a follow-on). Bluestein
//              chirp-convolution over the v10 FFT engine.
//   rceps      real cepstrum = ifft(log|fft(x)|)  — echo/pitch detection.
//   cceps      complex cepstrum = ifft(log(fft(x))) with phase unwrap.
//   fwht       fast Walsh-Hadamard transform (natural order), self-inverse /n.
//
// Gate: goertzel vs the direct DFT bin; czt(default) == FFT + a zoom arc vs the
// direct sum; rceps recovers an echo delay; fwht is self-inverse. Lower-layer.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dsp/polynomial.hpp> // roots (residuez)
#include <crd/hesap/fft/fft.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <numbers>

namespace crd::hesap::dsp
{

// Generalized Goertzel: the DFT value at normalized frequency f0 (cycles/sample) of x. O(N), no FFT.
template <typename T>
[[nodiscard]] Complex<T> goertzel(crd::containers::ConstSpan<T> x, T f0)
{
    const T w = static_cast<T>(2.0 * std::numbers::pi_v<double>) * f0;
    const T cw = std::cos(w), sw = std::sin(w);
    const T coeff = T(2) * cw;
    T s1 = T(0), s2 = T(0);
    for (crd::usize n = 0; n < x.size(); ++n)
    {
        const T s0 = x[n] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    // X = e^{jw}·s1 - s2 — the standard Goertzel output (DFT value Σ x[n] e^{-j2πf0 n} at f0).
    return Complex<T>{cw * s1 - s2, sw * s1};
}

namespace detail
{
template <typename T> [[nodiscard]] inline Complex<T> cexpj(T theta) noexcept
{
    return Complex<T>{std::cos(theta), std::sin(theta)};
}
template <typename T> [[nodiscard]] inline Complex<T> cmulx(Complex<T> a, Complex<T> b) noexcept
{
    return Complex<T>{a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
}
} // namespace detail

// Chirp-z transform on a unit-circle arc: X[k] = Σ_n x[n]·e^{-j·θa·n}·e^{j·θw·nk}, k=0..m-1.
// θw = -2π/m, θa = 0 ⇒ the m-point DFT. θa = 2π·f0, θw = -2π·df ⇒ a zoom-FFT from f0 with step df.
//
// CztPlan caches the FFT plan + the constant chirp-FFT (H) + the in/out chirps ⇒ execute() does just 2 FFT runs
// (no twiddle-table or chirp rebuild). The realistic repeated-CZT hot path (the FftConvolver/HilbertTransformer
// pattern) — Cerid's FFT execute beats PocketFFT, so the cached plan BEATS scipy's CZT.
template <typename T> class CztPlan
{
public:
    [[nodiscard]] static crd::usize czt_len(crd::usize n, crd::usize m) noexcept
    {
        crd::usize l = 4;
        while (l < n + m - 1)
        {
            l <<= 1;
        }
        return l;
    }
    CztPlan(crd::memory::IAllocator* alloc, crd::usize n, crd::usize m, T theta_w, T theta_a)
        : m_n(n), m_m(m), m_l(czt_len(n, m)), m_plan(alloc, czt_len(n, m)), m_gchirp(alloc), m_outchirp(alloc),
          m_h(alloc), m_scratch(alloc)
    {
        m_gchirp.resize(n);
        m_outchirp.resize(m);
        m_h.resize(m_l);
        m_scratch.resize(m_l);
        for (crd::usize i = 0; i < n; ++i) // g-chirp: e^{-jθa n} e^{jθw n²/2}
        {
            const T nn = static_cast<T>(i);
            m_gchirp[i] = detail::cmulx<T>(detail::cexpj<T>(-theta_a * nn), detail::cexpj<T>(theta_w * nn * nn / T(2)));
        }
        for (crd::usize k = 0; k < m; ++k) // out-chirp: e^{jθw k²/2}
        {
            const T kk = static_cast<T>(k);
            m_outchirp[k] = detail::cexpj<T>(theta_w * kk * kk / T(2));
        }
        for (crd::usize i = 0; i < m_l; ++i)
        {
            m_h[i] = Complex<T>{T(0), T(0)};
        }
        for (crd::usize k = 0; k < m; ++k) // h[k] = e^{-jθw k²/2}; negative indices wrap to L-j
        {
            const T kk = static_cast<T>(k);
            m_h[k] = detail::cexpj<T>(-theta_w * kk * kk / T(2));
        }
        for (crd::usize j = 1; j < n; ++j)
        {
            const T jj = static_cast<T>(j);
            m_h[m_l - j] = detail::cexpj<T>(-theta_w * jj * jj / T(2));
        }
        m_plan.execute(crd::containers::Span<Complex<T>>(m_h.data(), m_l), fft::FftDirection::Forward); // H once
    }

    // X[k], k=0..m-1, into `out` (length m). Reuses the cached plan + H.
    void execute(crd::containers::ConstSpan<T> x, crd::containers::Span<Complex<T>> out)
    {
        Complex<T>* g = m_scratch.data();
        for (crd::usize i = 0; i < m_l; ++i)
        {
            g[i] = (i < m_n) ? detail::cmulx<T>(Complex<T>{x[i], T(0)}, m_gchirp[i]) : Complex<T>{T(0), T(0)};
        }
        m_plan.execute(crd::containers::Span<Complex<T>>(g, m_l), fft::FftDirection::Forward);
        for (crd::usize i = 0; i < m_l; ++i)
        {
            g[i] = detail::cmulx<T>(g[i], m_h[i]);
        }
        m_plan.execute(crd::containers::Span<Complex<T>>(g, m_l), fft::FftDirection::Inverse); // unnormalized
        const T inv = T(1) / static_cast<T>(m_l);
        for (crd::usize k = 0; k < m_m; ++k)
        {
            out[k] = detail::cmulx<T>(m_outchirp[k], Complex<T>{g[k].re * inv, g[k].im * inv});
        }
    }

private:
    crd::usize m_n, m_m, m_l;
    fft::FftPlan<T> m_plan;
    crd::containers::Array<Complex<T>> m_gchirp, m_outchirp, m_h, m_scratch;
};

// One-shot CZT (builds a transient plan). For repeated transforms use CztPlan.
template <typename T>
[[nodiscard]] crd::containers::Array<Complex<T>> czt(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                                     crd::usize m, T theta_w, T theta_a)
{
    crd::containers::Array<Complex<T>> out(alloc);
    if (x.size() == 0 || m == 0)
    {
        return out;
    }
    out.resize(m);
    CztPlan<T> plan(alloc, x.size(), m, theta_w, theta_a);
    plan.execute(x, crd::containers::Span<Complex<T>>(out.data(), m));
    return out;
}

// Real cepstrum: ifft(log|fft(x)|), real part. x zero-padded to a power of two (length returned = nfft).
template <typename T>
[[nodiscard]] crd::containers::Array<T> rceps(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x)
{
    const crd::usize n = x.size();
    crd::usize nf = 4;
    while (nf < n)
    {
        nf <<= 1;
    }
    crd::containers::Array<Complex<T>> sp(alloc);
    sp.resize(nf);
    for (crd::usize i = 0; i < nf; ++i)
    {
        sp[i] = Complex<T>{(i < n) ? x[i] : T(0), T(0)};
    }
    fft::fft<T>(alloc, crd::containers::Span<Complex<T>>(sp.data(), nf), fft::FftDirection::Forward);
    const T floor = static_cast<T>(1e-30);
    for (crd::usize i = 0; i < nf; ++i)
    {
        sp[i] = Complex<T>{std::log(std::hypot(sp[i].re, sp[i].im) + floor), T(0)};
    }
    fft::ifft_normalized<T>(alloc, crd::containers::Span<Complex<T>>(sp.data(), nf));
    crd::containers::Array<T> c(alloc);
    c.resize(nf);
    for (crd::usize i = 0; i < nf; ++i)
    {
        c[i] = sp[i].re;
    }
    return c;
}

// Complex cepstrum: ifft(log(fft(x))) with phase unwrap, real part.
template <typename T>
[[nodiscard]] crd::containers::Array<T> cceps(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x)
{
    const crd::usize n = x.size();
    crd::usize nf = 4;
    while (nf < n)
    {
        nf <<= 1;
    }
    crd::containers::Array<Complex<T>> sp(alloc);
    sp.resize(nf);
    for (crd::usize i = 0; i < nf; ++i)
    {
        sp[i] = Complex<T>{(i < n) ? x[i] : T(0), T(0)};
    }
    fft::fft<T>(alloc, crd::containers::Span<Complex<T>>(sp.data(), nf), fft::FftDirection::Forward);
    const T two_pi = static_cast<T>(2.0 * std::numbers::pi_v<double>);
    const T pi = static_cast<T>(std::numbers::pi_v<double>);
    T prev = T(0), cum = T(0);
    for (crd::usize i = 0; i < nf; ++i)
    {
        const T mag = std::log(std::hypot(sp[i].re, sp[i].im) + static_cast<T>(1e-30));
        T ph = std::atan2(sp[i].im, sp[i].re);
        if (i > 0) // running unwrap
        {
            T dd = ph - prev;
            T ddm = std::fmod(dd + pi, two_pi);
            if (ddm < T(0))
            {
                ddm += two_pi;
            }
            ddm -= pi;
            if (std::abs(dd) >= pi)
            {
                cum += (ddm - dd);
            }
        }
        prev = ph;
        sp[i] = Complex<T>{mag, ph + cum};
    }
    fft::ifft_normalized<T>(alloc, crd::containers::Span<Complex<T>>(sp.data(), nf));
    crd::containers::Array<T> c(alloc);
    c.resize(nf);
    for (crd::usize i = 0; i < nf; ++i)
    {
        c[i] = sp[i].re;
    }
    return c;
}

// Inverse Z-transform (causal): the first n samples of the impulse response of B(z)/A(z) — the time sequence whose
// Z-transform is the rational H(z). Via the difference equation (a[0] normalizes).
template <typename T>
[[nodiscard]] crd::containers::Array<T> impz(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> b,
                                             crd::containers::ConstSpan<T> a, crd::usize n)
{
    crd::containers::Array<T> y(alloc);
    y.resize(n);
    const T a0 = (a.size() > 0) ? a[0] : T(1);
    for (crd::usize k = 0; k < n; ++k)
    {
        T s = (k < b.size()) ? b[k] : T(0); // b[k]·δ[0] is the only surviving numerator term at sample k
        for (crd::usize j = 1; j < a.size(); ++j)
        {
            if (k >= j)
            {
                s -= a[j] * y[k - j];
            }
        }
        y[k] = s / a0;
    }
    return y;
}

// Partial-fraction (residue) inverse Z-transform for DISTINCT poles: H(z) = Σ_i r_i/(1 - p_i z⁻¹). Returns the
// poles p_i (= roots of A in z) and residues r_i so that h[n] = Σ_i r_i p_iⁿ (causal, deg B < deg A). scipy residuez.
template <typename T>
void residuez(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> b, crd::containers::ConstSpan<T> a,
              crd::containers::Array<Complex<T>>& poles, crd::containers::Array<Complex<T>>& residues)
{
    const T a0 = a[0];
    poles = roots<T>(alloc, a); // A(z) roots in z = the poles p_i
    const crd::usize np = poles.size();
    residues.resize(np);
    for (crd::usize i = 0; i < np; ++i)
    {
        const Complex<T> pi = poles[i];
        const Complex<T> wi = detail::cmulx<T>(Complex<T>{T(1), T(0)}, Complex<T>{pi.re, -pi.im}); // start 1/p_i below
        // w = 1/p_i:
        const T den = pi.re * pi.re + pi.im * pi.im;
        const Complex<T> w{pi.re / den, -pi.im / den};
        // B(w) = Σ b[k] w^k
        Complex<T> bw{T(0), T(0)}, wk{T(1), T(0)};
        for (crd::usize k = 0; k < b.size(); ++k)
        {
            bw = Complex<T>{bw.re + b[k] * wk.re, bw.im + b[k] * wk.im};
            wk = detail::cmulx<T>(wk, w);
        }
        // prod_{j≠i} (1 - p_j / p_i)
        Complex<T> prod{T(1), T(0)};
        for (crd::usize j = 0; j < np; ++j)
        {
            if (j == i)
            {
                continue;
            }
            const Complex<T> ratio = detail::cmulx<T>(poles[j], w); // p_j / p_i = p_j · (1/p_i)
            prod = detail::cmulx<T>(prod, Complex<T>{T(1) - ratio.re, -ratio.im});
        }
        const Complex<T> denom{a0 * prod.re, a0 * prod.im};
        const T dn = denom.re * denom.re + denom.im * denom.im;
        residues[i] = Complex<T>{(bw.re * denom.re + bw.im * denom.im) / dn, (bw.im * denom.re - bw.re * denom.im) / dn};
        (void)wi;
    }
}

// Cepstral liftering: window a cepstrum to separate slow (spectral envelope) from fast (excitation) quefrencies.
// lowpass=true keeps q < cutoff (the envelope); lowpass=false keeps q >= cutoff (the excitation). In place.
template <typename T> void lifter(crd::containers::Span<T> cepstrum, crd::usize cutoff, bool lowpass) noexcept
{
    for (crd::usize q = 0; q < cepstrum.size(); ++q)
    {
        const bool keep = lowpass ? (q < cutoff) : (q >= cutoff);
        if (!keep)
        {
            cepstrum[q] = T(0);
        }
    }
}

// Fast Walsh-Hadamard transform (natural / Hadamard order), in place. Length must be a power of two.
// Self-inverse up to 1/n: fwht(fwht(x)) == n·x.
template <typename T> void fwht(crd::containers::Span<T> a) noexcept
{
    const crd::usize n = a.size();
    for (crd::usize len = 1; len < n; len <<= 1)
    {
        for (crd::usize i = 0; i < n; i += (len << 1))
        {
            for (crd::usize j = i; j < i + len; ++j)
            {
                const T u = a[j], v = a[j + len];
                a[j] = u + v;
                a[j + len] = u - v;
            }
        }
    }
}

} // namespace crd::hesap::dsp
