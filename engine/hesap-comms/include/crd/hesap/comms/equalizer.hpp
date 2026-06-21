#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-comms v11c-e — adaptive equalizers.
//
//   LmsEqualizer<T>   complex LMS linear equalizer (training + decision-directed):
//                     y = Σ w_k·x_k, e = d − y, w += μ·conj(x)·e.
//   CmaEqualizer<T>   Constant Modulus Algorithm — BLIND equalizer (no training):
//                     e = y·(R2 − |y|²), w += μ·conj(x)·e.
//   DfeEqualizer<T>   decision-feedback equalizer (feedforward + feedback taps).
//   mlse_viterbi      maximum-likelihood sequence estimation over a known FIR
//                     channel (the Viterbi trellis) — optimal vs brute force.
//
// Gate (ADR-0093): each equalizer OPENS a multipath (ISI) channel — the MSE
// converges and the symbols are recovered (BER→0 after training / CMA lock);
// MLSE == the brute-force ML sequence on a short channel. Lower-layer raw
// Complex<T>, alloc-free stateful ⇒ run-twice determinism moat.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/comms/modulation.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::comms
{

namespace detail
{
template <typename T> [[nodiscard]] inline Complex<T> cmul(Complex<T> a, Complex<T> b) noexcept
{
    return Complex<T>{a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
}
} // namespace detail

// Complex LMS linear equalizer. ntaps-long FIR over a delay line; train(x,d) or adapt decision-directed.
template <typename T> class LmsEqualizer
{
public:
    LmsEqualizer(crd::memory::IAllocator* alloc, crd::usize ntaps, T mu) noexcept
        : m_w(alloc), m_x(alloc), m_ntaps(ntaps), m_mu(mu)
    {
        m_w.resize(ntaps);
        m_x.resize(ntaps);
        reset();
    }
    void reset() noexcept
    {
        for (crd::usize i = 0; i < m_ntaps; ++i)
        {
            m_w[i] = Complex<T>{T(0), T(0)};
            m_x[i] = Complex<T>{T(0), T(0)};
        }
        m_w[m_ntaps / 2] = Complex<T>{T(1), T(0)}; // centre-spike init
    }

    // Push a new input sample, return the current equalizer output y = Σ w_k·x_k.
    [[nodiscard]] Complex<T> filter(Complex<T> in) noexcept
    {
        for (crd::usize i = m_ntaps - 1; i > 0; --i)
        {
            m_x[i] = m_x[i - 1];
        }
        m_x[0] = in;
        Complex<T> y{T(0), T(0)};
        for (crd::usize i = 0; i < m_ntaps; ++i)
        {
            y.re += m_w[i].re * m_x[i].re - m_w[i].im * m_x[i].im;
            y.im += m_w[i].re * m_x[i].im + m_w[i].im * m_x[i].re;
        }
        return y;
    }

    // Update the taps toward desired `d` given the last output `y`: w += μ·conj(x)·(d−y).
    void update(Complex<T> y, Complex<T> d) noexcept
    {
        const Complex<T> e{d.re - y.re, d.im - y.im};
        for (crd::usize i = 0; i < m_ntaps; ++i)
        {
            // conj(x)·e
            const T gr = m_x[i].re * e.re + m_x[i].im * e.im;
            const T gi = m_x[i].re * e.im - m_x[i].im * e.re;
            m_w[i].re += m_mu * gr;
            m_w[i].im += m_mu * gi;
        }
    }

    [[nodiscard]] crd::usize ntaps() const noexcept { return m_ntaps; }

private:
    crd::containers::Array<Complex<T>> m_w, m_x;
    crd::usize m_ntaps;
    T m_mu;
};

// Constant Modulus Algorithm — blind equalizer. R2 = E[|s|⁴]/E[|s|²] (1 for unit-energy PSK).
template <typename T> class CmaEqualizer
{
public:
    CmaEqualizer(crd::memory::IAllocator* alloc, crd::usize ntaps, T mu, T r2) noexcept
        : m_w(alloc), m_x(alloc), m_ntaps(ntaps), m_mu(mu), m_r2(r2)
    {
        m_w.resize(ntaps);
        m_x.resize(ntaps);
        reset();
    }
    void reset() noexcept
    {
        for (crd::usize i = 0; i < m_ntaps; ++i)
        {
            m_w[i] = Complex<T>{T(0), T(0)};
            m_x[i] = Complex<T>{T(0), T(0)};
        }
        m_w[m_ntaps / 2] = Complex<T>{T(1), T(0)};
    }

    [[nodiscard]] Complex<T> filter(Complex<T> in) noexcept
    {
        for (crd::usize i = m_ntaps - 1; i > 0; --i)
        {
            m_x[i] = m_x[i - 1];
        }
        m_x[0] = in;
        Complex<T> y{T(0), T(0)};
        for (crd::usize i = 0; i < m_ntaps; ++i)
        {
            y.re += m_w[i].re * m_x[i].re - m_w[i].im * m_x[i].im;
            y.im += m_w[i].re * m_x[i].im + m_w[i].im * m_x[i].re;
        }
        return y;
    }

    // Blind CMA update from the last output y: e = y·(R2 − |y|²); w += μ·conj(x)·e.
    void update(Complex<T> y) noexcept
    {
        const T mod = m_r2 - (y.re * y.re + y.im * y.im);
        const Complex<T> e{y.re * mod, y.im * mod};
        for (crd::usize i = 0; i < m_ntaps; ++i)
        {
            const T gr = m_x[i].re * e.re + m_x[i].im * e.im;
            const T gi = m_x[i].re * e.im - m_x[i].im * e.re;
            m_w[i].re += m_mu * gr;
            m_w[i].im += m_mu * gi;
        }
    }

private:
    crd::containers::Array<Complex<T>> m_w, m_x;
    crd::usize m_ntaps;
    T m_mu, m_r2;
};

// Decision-feedback equalizer: nff feedforward taps over the input + nfb feedback taps over past decisions.
template <typename T> class DfeEqualizer
{
public:
    DfeEqualizer(crd::memory::IAllocator* alloc, crd::usize nff, crd::usize nfb, T mu) noexcept
        : m_wff(alloc), m_wfb(alloc), m_xff(alloc), m_dfb(alloc), m_nff(nff), m_nfb(nfb), m_mu(mu)
    {
        m_wff.resize(nff);
        m_wfb.resize(nfb);
        m_xff.resize(nff);
        m_dfb.resize(nfb);
        reset();
    }
    void reset() noexcept
    {
        for (crd::usize i = 0; i < m_nff; ++i)
        {
            m_wff[i] = Complex<T>{T(0), T(0)};
            m_xff[i] = Complex<T>{T(0), T(0)};
        }
        for (crd::usize i = 0; i < m_nfb; ++i)
        {
            m_wfb[i] = Complex<T>{T(0), T(0)};
            m_dfb[i] = Complex<T>{T(0), T(0)};
        }
        m_wff[m_nff / 2] = Complex<T>{T(1), T(0)};
    }

    // y = Σ wff·xff − Σ wfb·dfb (feedforward minus the ISI estimate from past decisions).
    [[nodiscard]] Complex<T> filter(Complex<T> in) noexcept
    {
        for (crd::usize i = m_nff - 1; i > 0; --i)
        {
            m_xff[i] = m_xff[i - 1];
        }
        m_xff[0] = in;
        Complex<T> y{T(0), T(0)};
        for (crd::usize i = 0; i < m_nff; ++i)
        {
            y.re += m_wff[i].re * m_xff[i].re - m_wff[i].im * m_xff[i].im;
            y.im += m_wff[i].re * m_xff[i].im + m_wff[i].im * m_xff[i].re;
        }
        for (crd::usize i = 0; i < m_nfb; ++i)
        {
            y.re -= m_wfb[i].re * m_dfb[i].re - m_wfb[i].im * m_dfb[i].im;
            y.im -= m_wfb[i].re * m_dfb[i].im + m_wfb[i].im * m_dfb[i].re;
        }
        return y;
    }

    // Update both tap sets toward decision/desired d, then push d into the feedback line.
    void update(Complex<T> y, Complex<T> d) noexcept
    {
        const Complex<T> e{d.re - y.re, d.im - y.im};
        for (crd::usize i = 0; i < m_nff; ++i)
        {
            const T gr = m_xff[i].re * e.re + m_xff[i].im * e.im;
            const T gi = m_xff[i].re * e.im - m_xff[i].im * e.re;
            m_wff[i].re += m_mu * gr;
            m_wff[i].im += m_mu * gi;
        }
        for (crd::usize i = 0; i < m_nfb; ++i) // feedback taps subtract ⇒ gradient sign flips
        {
            const T gr = m_dfb[i].re * e.re + m_dfb[i].im * e.im;
            const T gi = m_dfb[i].re * e.im - m_dfb[i].im * e.re;
            m_wfb[i].re -= m_mu * gr;
            m_wfb[i].im -= m_mu * gi;
        }
        for (crd::usize i = m_nfb - 1; i > 0; --i)
        {
            m_dfb[i] = m_dfb[i - 1];
        }
        if (m_nfb > 0)
        {
            m_dfb[0] = d;
        }
    }

private:
    crd::containers::Array<Complex<T>> m_wff, m_wfb, m_xff, m_dfb;
    crd::usize m_nff, m_nfb;
    T m_mu;
};

// MLSE via Viterbi over a known FIR channel `h` (length L). Returns the ML symbol-index sequence. State = the last
// (L-1) symbols; branch metric = |r[n] − Σ_l h[l]·constel[state-history]|². O(n·M^L) — for short channels.
template <typename T>
[[nodiscard]] crd::containers::Array<crd::u32> mlse_viterbi(crd::memory::IAllocator* alloc,
                                                            crd::containers::ConstSpan<Complex<T>> r,
                                                            crd::containers::ConstSpan<Complex<T>> h, const Modem<T>& modem)
{
    const crd::usize n = r.size();
    const crd::u32 m = modem.order();
    const crd::usize ltap = h.size();
    crd::usize nstates = 1;
    for (crd::usize i = 1; i < ltap; ++i) // M^(L-1) states
    {
        nstates *= m;
    }
    const T big = static_cast<T>(1e30);
    crd::containers::Array<T> metric(alloc), nmetric(alloc);
    metric.resize(nstates);
    nmetric.resize(nstates);
    crd::containers::Array<crd::u32> back(alloc); // n × nstates predecessor symbol
    back.resize(n * nstates);
    for (crd::usize s = 0; s < nstates; ++s)
    {
        metric[s] = (s == 0) ? T(0) : big; // assume known start (all-zero history)
    }
    crd::containers::Array<crd::u32> prevstate(alloc);
    prevstate.resize(n * nstates);
    for (crd::usize step = 0; step < n; ++step)
    {
        for (crd::usize s = 0; s < nstates; ++s)
        {
            nmetric[s] = big;
        }
        for (crd::usize s = 0; s < nstates; ++s) // current state = history (newest-first packing)
        {
            if (metric[s] >= big)
            {
                continue;
            }
            for (crd::u32 sym = 0; sym < m; ++sym) // the new symbol entering the channel
            {
                // channel output = h[0]·sym + Σ_{l>=1} h[l]·history[l-1]
                Complex<T> pred = detail::cmul<T>(h[0], modem.constellation(sym));
                crd::usize hist = s;
                for (crd::usize l = 1; l < ltap; ++l)
                {
                    const crd::u32 past = static_cast<crd::u32>(hist % m);
                    hist /= m;
                    pred.re += h[l].re * modem.constellation(past).re - h[l].im * modem.constellation(past).im;
                    pred.im += h[l].re * modem.constellation(past).im + h[l].im * modem.constellation(past).re;
                }
                const T dr = r[step].re - pred.re, di = r[step].im - pred.im;
                const T bm = metric[s] + dr * dr + di * di;
                // next state = (sym, oldest dropped): shift sym in as newest
                const crd::usize nstate = (s % (nstates / m == 0 ? 1 : (nstates / m))) * m + sym;
                if (bm < nmetric[nstate])
                {
                    nmetric[nstate] = bm;
                    back[step * nstates + nstate] = sym;
                    prevstate[step * nstates + nstate] = static_cast<crd::u32>(s);
                }
            }
        }
        for (crd::usize s = 0; s < nstates; ++s)
        {
            metric[s] = nmetric[s];
        }
    }
    // trace back from the best final state
    crd::usize bs = 0;
    T bestm = big;
    for (crd::usize s = 0; s < nstates; ++s)
    {
        if (metric[s] < bestm)
        {
            bestm = metric[s];
            bs = s;
        }
    }
    crd::containers::Array<crd::u32> out(alloc);
    out.resize(n);
    crd::usize cur = bs;
    for (crd::usize step = n; step-- > 0;)
    {
        out[step] = back[step * nstates + cur];
        cur = prevstate[step * nstates + cur];
    }
    return out;
}

} // namespace crd::hesap::comms
