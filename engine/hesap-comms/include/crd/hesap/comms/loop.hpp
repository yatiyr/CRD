#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-comms — shared synchronization substrate (loop filter + NCO).
//
//   LoopFilter2<T>   2nd-order proportional-integral loop filter, designed from
//                    a normalized loop bandwidth + damping (the standard
//                    Gardner / liquid-dsp design). Used by timing AND carrier
//                    recovery.
//   Nco<T>           numerically-controlled oscillator: a phase accumulator +
//                    complex mix (down/up conversion).
//
// Lower-layer raw Complex<T>; alloc-free stateful kernels (the SDR hot loop) ⇒
// run-twice bit-deterministic.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>

#include <crd/math/cmath.hpp>
#include <numbers>

namespace crd::hesap::comms
{

// 2nd-order PI loop filter. Gains from the normalized loop bandwidth `bw` (cycles/sample) and damping `zeta`:
//   θ = bw/(ζ + 1/(4ζ));  denom = 1 + 2ζθ + θ²;  α = 4ζθ/denom (proportional);  β = 4θ²/denom (integral).
template <typename T> class LoopFilter2
{
public:
    LoopFilter2(T bw, T zeta) noexcept { design(bw, zeta); }

    void design(T bw, T zeta) noexcept
    {
        const T theta = bw / (zeta + T(1) / (T(4) * zeta));
        const T denom = T(1) + T(2) * zeta * theta + theta * theta;
        m_alpha = (T(4) * zeta * theta) / denom;
        m_beta = (T(4) * theta * theta) / denom;
        m_integ = T(0);
    }

    // Feed the phase/timing error, return the loop control (the filtered estimate to drive the NCO/interpolator).
    [[nodiscard]] T advance(T error) noexcept
    {
        m_integ += m_beta * error;
        return m_alpha * error + m_integ;
    }

    void reset() noexcept { m_integ = T(0); }
    [[nodiscard]] T alpha() const noexcept { return m_alpha; }
    [[nodiscard]] T beta() const noexcept { return m_beta; }

private:
    T m_alpha = T(0), m_beta = T(0), m_integ = T(0);
};

// Numerically-controlled oscillator: a wrapped phase accumulator + complex mix.
template <typename T> class Nco
{
public:
    explicit Nco(T phase = T(0), T freq = T(0)) noexcept : m_phase(phase), m_freq(freq) {}

    [[nodiscard]] T phase() const noexcept { return m_phase; }
    [[nodiscard]] T frequency() const noexcept { return m_freq; }
    void set_frequency(T f) noexcept { m_freq = f; }
    void adjust_phase(T d) noexcept { m_phase = wrap(m_phase + d); }
    void adjust_frequency(T d) noexcept { m_freq += d; }

    [[nodiscard]] Complex<T> exp_j() const noexcept { return Complex<T>{crd::math::cos(m_phase), crd::math::sin(m_phase)}; }

    // Mix a sample DOWN by the NCO (multiply by e^{-jφ}).
    [[nodiscard]] Complex<T> mix_down(Complex<T> x) const noexcept
    {
        const T c = crd::math::cos(m_phase), s = crd::math::sin(m_phase);
        return Complex<T>{x.re * c + x.im * s, -x.re * s + x.im * c};
    }
    // Mix UP (multiply by e^{+jφ}).
    [[nodiscard]] Complex<T> mix_up(Complex<T> x) const noexcept
    {
        const T c = crd::math::cos(m_phase), s = crd::math::sin(m_phase);
        return Complex<T>{x.re * c - x.im * s, x.re * s + x.im * c};
    }

    void step() noexcept { m_phase = wrap(m_phase + m_freq); } // advance one sample

private:
    [[nodiscard]] static T wrap(T p) noexcept
    {
        const T two_pi = static_cast<T>(2.0 * std::numbers::pi_v<double>);
        while (p > static_cast<T>(std::numbers::pi_v<double>))
        {
            p -= two_pi;
        }
        while (p < -static_cast<T>(std::numbers::pi_v<double>))
        {
            p += two_pi;
        }
        return p;
    }
    T m_phase = T(0), m_freq = T(0);
};

// Cubic (4-point Farrow / Lagrange) interpolation at fractional offset mu in [0,1) between x[1] and x[2].
template <typename T> [[nodiscard]] inline Complex<T> cubic_interp(const Complex<T>* x, T mu) noexcept
{
    // cubic Lagrange through x[0..3] (nodes 0,1,2,3) evaluated at 1+mu (between x[1] and x[2]).
    const T a = -mu * (mu - T(1)) * (mu - T(2)) / T(6);
    const T b = (mu + T(1)) * (mu - T(1)) * (mu - T(2)) / T(2);
    const T c = -(mu + T(1)) * mu * (mu - T(2)) / T(2);
    const T d = (mu + T(1)) * mu * (mu - T(1)) / T(6);
    return Complex<T>{a * x[0].re + b * x[1].re + c * x[2].re + d * x[3].re,
                      a * x[0].im + b * x[1].im + c * x[2].im + d * x[3].im};
}

} // namespace crd::hesap::comms
