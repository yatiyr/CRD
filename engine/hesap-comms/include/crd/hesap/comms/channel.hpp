#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-comms v11c-f — channel models + AGC.
//
//   awgn_add          add complex AWGN at a given noise variance (deterministic
//                     Philox NormalSampler ⇒ reproducible channels = the moat).
//   noise_sigma_for_snr   σ per complex dim for a target SNR (dB) given signal power.
//   rayleigh_flat / rician_flat   flat fading gains (unit average power).
//   add_cfo           apply a carrier frequency + phase offset.
//   Agc<T>            automatic gain control (1-pole power tracking to a target).
//
// Gate (ADR-0093): the measured SNR/fading statistics match the spec; AGC output
// power converges to the target; reproducible across runs. Lower-layer raw.
// ---------------------------------------------------------------------------

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/stats/normal.hpp>
#include <crd/hesap/stats/philox.hpp>

#include <cmath>
#include <numbers>

namespace crd::hesap::comms
{

// Add complex AWGN with per-complex-dimension std-dev `sigma` (total noise power = 2·sigma²) in place.
template <typename T>
void awgn_add(crd::containers::Span<Complex<T>> x, T sigma, crd::hesap::stats::NormalSampler& noise) noexcept
{
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        x[i].re += sigma * static_cast<T>(noise.next());
        x[i].im += sigma * static_cast<T>(noise.next());
    }
}

// σ per complex dimension for a target SNR (dB), given the average signal power. SNR = signal_power/(2σ²).
template <typename T> [[nodiscard]] T noise_sigma_for_snr(T signal_power, T snr_db) noexcept
{
    const T snr = std::pow(T(10), snr_db / T(10));
    return std::sqrt(signal_power / (T(2) * snr));
}

// Average power of a complex signal.
template <typename T> [[nodiscard]] T signal_power(crd::containers::ConstSpan<Complex<T>> x) noexcept
{
    T p = T(0);
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        p += x[i].re * x[i].re + x[i].im * x[i].im;
    }
    return (x.size() > 0) ? p / static_cast<T>(x.size()) : T(0);
}

// Flat Rayleigh fading gains (E[|g|²] = 1): g = (a + jb)/√2, a,b ~ N(0,1), applied per sample in place.
template <typename T>
void rayleigh_flat(crd::containers::Span<Complex<T>> x, crd::hesap::stats::NormalSampler& g) noexcept
{
    const T s = static_cast<T>(1.0 / std::numbers::sqrt2_v<double>);
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        const Complex<T> gain{s * static_cast<T>(g.next()), s * static_cast<T>(g.next())};
        x[i] = Complex<T>{x[i].re * gain.re - x[i].im * gain.im, x[i].re * gain.im + x[i].im * gain.re};
    }
}

// Flat Rician fading gains (E[|g|²] = 1, K = LOS/scatter power ratio).
template <typename T>
void rician_flat(crd::containers::Span<Complex<T>> x, T k_factor, crd::hesap::stats::NormalSampler& g) noexcept
{
    const T los = std::sqrt(k_factor / (k_factor + T(1)));
    const T sca = std::sqrt(T(1) / (T(2) * (k_factor + T(1))));
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        const Complex<T> gain{los + sca * static_cast<T>(g.next()), sca * static_cast<T>(g.next())};
        x[i] = Complex<T>{x[i].re * gain.re - x[i].im * gain.im, x[i].re * gain.im + x[i].im * gain.re};
    }
}

// Apply a carrier frequency offset `dphi` (rad/sample) + initial phase `phi0` in place.
template <typename T> void add_cfo(crd::containers::Span<Complex<T>> x, T dphi, T phi0 = T(0)) noexcept
{
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        const T ph = phi0 + dphi * static_cast<T>(i);
        const T c = std::cos(ph), s = std::sin(ph);
        x[i] = Complex<T>{x[i].re * c - x[i].im * s, x[i].re * s + x[i].im * c};
    }
}

// Automatic gain control: a 1-pole loop drives the output power toward `target`. g[n+1] = g[n]·(1 + α(target − p)).
template <typename T> class Agc
{
public:
    Agc(T target, T alpha) noexcept : m_target(target), m_alpha(alpha), m_gain(T(1)), m_p(target) {}
    void reset() noexcept
    {
        m_gain = T(1);
        m_p = m_target;
    }
    [[nodiscard]] Complex<T> process(Complex<T> x) noexcept
    {
        const Complex<T> y{x.re * m_gain, x.im * m_gain};
        const T inst = y.re * y.re + y.im * y.im;
        m_p += m_alpha * (inst - m_p);                 // smoothed output power estimate
        m_gain *= T(1) + m_alpha * (m_target - m_p);   // pull power toward target
        if (m_gain < T(0))
        {
            m_gain = T(0);
        }
        return y;
    }
    [[nodiscard]] T gain() const noexcept { return m_gain; }

private:
    T m_target, m_alpha, m_gain, m_p;
};

} // namespace crd::hesap::comms
