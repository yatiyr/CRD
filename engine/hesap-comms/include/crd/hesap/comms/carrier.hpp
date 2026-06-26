#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-comms v11c-d — carrier recovery.
//
//   CostasLoop<T>     decision-directed PSK carrier-phase/frequency recovery:
//                     an NCO de-rotates each symbol, the phase error e =
//                     Im{y·conj(decision)} drives a 2nd-order (α,β) loop.
//   Pll<T>            generic phase-locked loop tracking a complex tone.
//   estimate_cfo_mpsk M-th-power coarse carrier-frequency-offset estimate.
//
// Gate (ADR-0093): the loop LOCKS — a PSK stream with a phase + frequency
// offset is de-rotated to recover the symbols (zero errors in steady state);
// the M-th-power AFC estimate matches the applied offset; PLL tracks a ramp.
// Lower-layer raw Complex<T>, alloc-free streaming ⇒ run-twice moat.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/comms/loop.hpp>
#include <crd/hesap/comms/modulation.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <numbers>

namespace crd::hesap::comms
{

// Decision-directed Costas loop for M-PSK. 2nd-order (α,β) gains from the loop bandwidth + damping.
template <typename T> class CostasLoop
{
public:
    CostasLoop(const Modem<T>* psk, T loop_bw, T zeta) noexcept : m_psk(psk)
    {
        LoopFilter2<T> lf(loop_bw, zeta);
        m_alpha = lf.alpha();
        m_beta = lf.beta();
    }

    void reset() noexcept
    {
        m_phase = T(0);
        m_freq = T(0);
    }

    void process(crd::containers::ConstSpan<Complex<T>> in, crd::containers::Array<Complex<T>>& out)
    {
        for (crd::usize i = 0; i < in.size(); ++i)
        {
            const T c = crd::math::cos(m_phase), s = crd::math::sin(m_phase);
            const Complex<T> y{in[i].re * c + in[i].im * s, -in[i].re * s + in[i].im * c}; // de-rotate by -phase
            const crd::u32 d = m_psk->demodulate(y);
            const Complex<T> dec = m_psk->constellation(d);
            const T e = y.im * dec.re - y.re * dec.im; // Im{y·conj(dec)} (unit-energy PSK)
            m_freq += m_beta * e;
            m_phase += m_freq + m_alpha * e;
            m_phase = wrap(m_phase);
            out.push_back(y);
        }
    }

    [[nodiscard]] T frequency() const noexcept { return m_freq; }
    [[nodiscard]] T phase() const noexcept { return m_phase; }

private:
    [[nodiscard]] static T wrap(T p) noexcept
    {
        const T tp = static_cast<T>(2.0 * std::numbers::pi_v<double>);
        while (p > static_cast<T>(std::numbers::pi_v<double>))
        {
            p -= tp;
        }
        while (p < -static_cast<T>(std::numbers::pi_v<double>))
        {
            p += tp;
        }
        return p;
    }
    const Modem<T>* m_psk;
    T m_alpha = T(0), m_beta = T(0), m_phase = T(0), m_freq = T(0);
};

// Generic PLL tracking the phase of a complex input (phase detector = the angle of y·conj(reference-phase)).
template <typename T> class Pll
{
public:
    Pll(T loop_bw, T zeta) noexcept
    {
        LoopFilter2<T> lf(loop_bw, zeta);
        m_alpha = lf.alpha();
        m_beta = lf.beta();
    }
    void reset() noexcept { m_phase = T(0); m_freq = T(0); }

    // Track one input sample; returns the NCO phase estimate after the update.
    [[nodiscard]] T track(Complex<T> x) noexcept
    {
        const T in_phase = crd::math::atan2(x.im, x.re);
        T e = in_phase - m_phase;
        e = wrap(e);
        m_freq += m_beta * e;
        m_phase = wrap(m_phase + m_freq + m_alpha * e);
        return m_phase;
    }
    [[nodiscard]] T frequency() const noexcept { return m_freq; }
    [[nodiscard]] T phase() const noexcept { return m_phase; }

private:
    [[nodiscard]] static T wrap(T p) noexcept
    {
        const T tp = static_cast<T>(2.0 * std::numbers::pi_v<double>);
        while (p > static_cast<T>(std::numbers::pi_v<double>))
        {
            p -= tp;
        }
        while (p < -static_cast<T>(std::numbers::pi_v<double>))
        {
            p += tp;
        }
        return p;
    }
    T m_alpha = T(0), m_beta = T(0), m_phase = T(0), m_freq = T(0);
};

// M-th-power coarse CFO estimate (rad/sample): raising M-PSK to the M-th power strips the modulation; the average
// phase increment of y^M divided by M is the carrier frequency offset.
template <typename T>
[[nodiscard]] T estimate_cfo_mpsk(crd::containers::ConstSpan<Complex<T>> y, crd::u32 m_order) noexcept
{
    auto powm = [&](Complex<T> z) {
        Complex<T> r{T(1), T(0)};
        for (crd::u32 k = 0; k < m_order; ++k)
        {
            r = Complex<T>{r.re * z.re - r.im * z.im, r.re * z.im + r.im * z.re};
        }
        return r;
    };
    T acc_re = T(0), acc_im = T(0); // average of y[n]^M · conj(y[n-1]^M)
    for (crd::usize n = 1; n < y.size(); ++n)
    {
        const Complex<T> a = powm(y[n]);
        const Complex<T> b = powm(y[n - 1]);
        acc_re += a.re * b.re + a.im * b.im;
        acc_im += a.im * b.re - a.re * b.im;
    }
    return crd::math::atan2(acc_im, acc_re) / static_cast<T>(m_order);
}

} // namespace crd::hesap::comms
