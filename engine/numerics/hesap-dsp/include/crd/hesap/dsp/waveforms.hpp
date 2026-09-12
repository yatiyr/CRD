#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-r — waveform generators (scipy.signal).
//
//   chirp        frequency-swept cosine (linear / quadratic / logarithmic /
//                hyperbolic), faithful scipy _chirp_phase (vertex_zero).
//   sawtooth     periodic ramp, rise fraction `width`.
//   square       periodic square, high fraction `duty`.
//   gausspulse   Gaussian-modulated sinusoid (in-phase) + envelope.
//   sweep_poly   cosine whose instantaneous frequency is a polynomial of t.
//   unit_impulse Kronecker delta.
//
// Generators (one-time signal construction, not a streaming hot loop) ⇒ gate is
// correctness vs scipy (~1e-10); no perf bench (the honest-gate rule, as windows).
// Lower-layer raw scalars; the caller supplies the time vector.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <numbers>

namespace crd::hesap::dsp
{

enum class ChirpMethod
{
    Linear,
    Quadratic,
    Logarithmic,
    Hyperbolic
};

namespace detail
{
template <typename T> [[nodiscard]] T chirp_phase(T t, T f0, T t1, T f1, ChirpMethod m) noexcept
{
    const T two_pi = static_cast<T>(2.0 * std::numbers::pi_v<double>);
    if (m == ChirpMethod::Linear)
    {
        const T beta = (f1 - f0) / t1;
        return two_pi * (f0 * t + T(0.5) * beta * t * t);
    }
    if (m == ChirpMethod::Quadratic)
    {
        const T beta = (f1 - f0) / (t1 * t1);
        return two_pi * (f0 * t + beta * t * t * t / T(3)); // vertex_zero
    }
    if (m == ChirpMethod::Logarithmic)
    {
        if (f0 == f1)
        {
            return two_pi * f0 * t;
        }
        const T beta = t1 / crd::math::log(f1 / f0);
        return two_pi * beta * f0 * (crd::math::pow(f1 / f0, t / t1) - T(1));
    }
    // Hyperbolic
    if (f0 == f1)
    {
        return two_pi * f0 * t;
    }
    const T sing = -f1 * t1 / (f0 - f1);
    return two_pi * (-sing * f0) * crd::math::log(std::abs(T(1) - t / sing));
}
} // namespace detail

// chirp: cos(phase(t) + phi). phi in degrees (scipy convention).
template <typename T>
[[nodiscard]] crd::containers::Array<T> chirp(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> t, T f0,
                                              T t1, T f1, ChirpMethod method = ChirpMethod::Linear, T phi_deg = T(0))
{
    const T phi = phi_deg * static_cast<T>(std::numbers::pi_v<double>) / T(180);
    crd::containers::Array<T> y(alloc);
    y.resize(t.size());
    for (crd::usize i = 0; i < t.size(); ++i)
    {
        y[i] = crd::math::cos(detail::chirp_phase<T>(t[i], f0, t1, f1, method) + phi);
    }
    return y;
}

// sawtooth: period 2π, rises -1→1 over [0, 2π·width], falls 1→-1 over the rest.
template <typename T>
[[nodiscard]] crd::containers::Array<T> sawtooth(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> t,
                                                 T width = T(1))
{
    const T two_pi = static_cast<T>(2.0 * std::numbers::pi_v<double>);
    const T pi = static_cast<T>(std::numbers::pi_v<double>);
    crd::containers::Array<T> y(alloc);
    y.resize(t.size());
    for (crd::usize i = 0; i < t.size(); ++i)
    {
        T tmod = crd::math::fmod(t[i], two_pi);
        if (tmod < T(0))
        {
            tmod += two_pi; // numpy mod ⇒ non-negative
        }
        // falling-edge divisor pi·(1−width) is 0 only at width==1, where the first branch always wins (tmod<two_pi),
        // so that path is unreachable — the guard makes MSVC's LTCG see no divide-by-0 (C4723) without changing values.
        const T fall_denom = pi * (T(1) - width);
        y[i] = (tmod < width * two_pi)
                   ? (tmod / (pi * width) - T(1))
                   : ((pi * (width + T(1)) - tmod) / (fall_denom != T(0) ? fall_denom : T(1)));
    }
    return y;
}

// square: period 2π, +1 over the first `duty` fraction, -1 over the rest.
template <typename T>
[[nodiscard]] crd::containers::Array<T> square(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> t,
                                               T duty = T(0.5))
{
    const T two_pi = static_cast<T>(2.0 * std::numbers::pi_v<double>);
    crd::containers::Array<T> y(alloc);
    y.resize(t.size());
    for (crd::usize i = 0; i < t.size(); ++i)
    {
        T tmod = crd::math::fmod(t[i], two_pi);
        if (tmod < T(0))
        {
            tmod += two_pi;
        }
        y[i] = (tmod < duty * two_pi) ? T(1) : T(-1);
    }
    return y;
}

// gausspulse: in-phase Gaussian-modulated sinusoid yI = exp(-a t²) cos(2π fc t). bwr < 0 dB.
template <typename T>
[[nodiscard]] crd::containers::Array<T> gausspulse(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> t,
                                                   T fc = T(1000), T bw = T(0.5), T bwr = T(-6))
{
    const T pi = static_cast<T>(std::numbers::pi_v<double>);
    const T ref = crd::math::pow(T(10), bwr / T(20));
    const T a = -(pi * fc * bw) * (pi * fc * bw) / (T(4) * crd::math::log(ref));
    crd::containers::Array<T> y(alloc);
    y.resize(t.size());
    for (crd::usize i = 0; i < t.size(); ++i)
    {
        y[i] = crd::math::exp(-a * t[i] * t[i]) * crd::math::cos(T(2) * pi * fc * t[i]);
    }
    return y;
}

// sweep_poly: cos(2π · ∫f(τ)dτ + phi), f(t) = polyval(poly, t). poly = coeffs highest-degree-first (numpy convention).
template <typename T>
[[nodiscard]] crd::containers::Array<T> sweep_poly(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> t,
                                                   crd::containers::ConstSpan<T> poly, T phi_deg = T(0))
{
    const T two_pi = static_cast<T>(2.0 * std::numbers::pi_v<double>);
    const T phi = phi_deg * static_cast<T>(std::numbers::pi_v<double>) / T(180);
    // polyint: integral coeffs (descending), constant of integration 0 appended.
    crd::containers::Array<T> ic(alloc);
    ic.resize(poly.size() + 1);
    const crd::usize deg = poly.size() - 1;
    for (crd::usize k = 0; k < poly.size(); ++k)
    {
        ic[k] = poly[k] / static_cast<T>(deg - k + 1);
    }
    ic[poly.size()] = T(0);
    crd::containers::Array<T> y(alloc);
    y.resize(t.size());
    for (crd::usize i = 0; i < t.size(); ++i)
    {
        T p = T(0); // Horner over the integral polynomial
        for (crd::usize k = 0; k < ic.size(); ++k)
        {
            p = p * t[i] + ic[k];
        }
        y[i] = crd::math::cos(two_pi * p + phi);
    }
    return y;
}

// unit_impulse: length n, a single 1 at index idx (else 0).
template <typename T>
[[nodiscard]] crd::containers::Array<T> unit_impulse(crd::memory::IAllocator* alloc, crd::usize n, crd::usize idx = 0)
{
    crd::containers::Array<T> y(alloc);
    y.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        y[i] = T(0);
    }
    if (idx < n)
    {
        y[idx] = T(1);
    }
    return y;
}

} // namespace crd::hesap::dsp
