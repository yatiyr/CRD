#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-h — RBJ audio EQ biquads (the DAW cookbook).
//
// Robert Bristow-Johnson's "Audio EQ Cookbook" closed-form biquads — the
// workhorse of every parametric equalizer, synth filter, and channel strip:
//   lowpass / highpass / bandpass(0 dB peak) / notch / allpass / peaking /
//   lowshelf / highshelf
// Each is an exact closed form in (w0, alpha[, A]); we return a normalized
// Biquad (a0 == 1). Cascade them into SecondOrderSections for a multi-band EQ.
//
// Convention (consistent with the rest of hesap-dsp — the lower raw-scalar
// numerical layer of ADR-0078; typed Frequency/dB wrapping is a DAW consumer's
// job, not the kernel's): f0 is a NYQUIST FRACTION in (0,1) ⇒ w0 = pi*f0.
// Shelves use the Q form of alpha (alpha = sin(w0)/(2Q)), matching Web-Audio /
// the cookbook's Q alternative. Gain is in dB (peaking/shelves only).
//
// DESIGN slice ⇒ the honest gate is the closed-form coeffs (vs an independent
// cookbook transcription, ~1e-12) + the spec properties (DC/Nyquist/centre-
// frequency gains). No perf bench (one-time coefficient setup).
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/hesap/dsp/filter.hpp> // Biquad<T>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <numbers>

namespace crd::hesap::dsp
{

namespace detail
{
template <typename T> struct RbjCommon
{
    T w0, cw, sw, alpha;
};

template <typename T> [[nodiscard]] RbjCommon<T> rbj_common(T f0, T q) noexcept
{
    const T pi = static_cast<T>(std::numbers::pi_v<double>);
    RbjCommon<T> c;
    c.w0 = pi * f0; // f0 is the Nyquist fraction ⇒ w0 = pi*f0 = 2*pi*(f0/2)/1
    c.cw = crd::math::cos(c.w0);
    c.sw = crd::math::sin(c.w0);
    c.alpha = c.sw / (T(2) * q);
    return c;
}

template <typename T> [[nodiscard]] Biquad<T> rbj_normalize(T b0, T b1, T b2, T a0, T a1, T a2) noexcept
{
    Biquad<T> bq;
    bq.b0 = b0 / a0;
    bq.b1 = b1 / a0;
    bq.b2 = b2 / a0;
    bq.a1 = a1 / a0;
    bq.a2 = a2 / a0;
    return bq;
}
} // namespace detail

// Lowpass (RBJ): passes DC, blocks Nyquist; -3 dB-ish at f0 for Q=1/sqrt(2).
template <typename T> [[nodiscard]] Biquad<T> rbj_lowpass(T f0, T q) noexcept
{
    const auto c = detail::rbj_common<T>(f0, q);
    return detail::rbj_normalize<T>((T(1) - c.cw) / T(2), T(1) - c.cw, (T(1) - c.cw) / T(2), T(1) + c.alpha,
                                    T(-2) * c.cw, T(1) - c.alpha);
}

// Highpass (RBJ): blocks DC, passes Nyquist.
template <typename T> [[nodiscard]] Biquad<T> rbj_highpass(T f0, T q) noexcept
{
    const auto c = detail::rbj_common<T>(f0, q);
    return detail::rbj_normalize<T>((T(1) + c.cw) / T(2), -(T(1) + c.cw), (T(1) + c.cw) / T(2), T(1) + c.alpha,
                                    T(-2) * c.cw, T(1) - c.alpha);
}

// Bandpass (RBJ, constant 0 dB peak gain): unity at f0, zero at DC + Nyquist.
template <typename T> [[nodiscard]] Biquad<T> rbj_bandpass(T f0, T q) noexcept
{
    const auto c = detail::rbj_common<T>(f0, q);
    return detail::rbj_normalize<T>(c.alpha, T(0), -c.alpha, T(1) + c.alpha, T(-2) * c.cw, T(1) - c.alpha);
}

// Notch (RBJ): unity everywhere except a null at f0.
template <typename T> [[nodiscard]] Biquad<T> rbj_notch(T f0, T q) noexcept
{
    const auto c = detail::rbj_common<T>(f0, q);
    return detail::rbj_normalize<T>(T(1), T(-2) * c.cw, T(1), T(1) + c.alpha, T(-2) * c.cw, T(1) - c.alpha);
}

// Allpass (RBJ): unity magnitude everywhere, phase shift around f0.
template <typename T> [[nodiscard]] Biquad<T> rbj_allpass(T f0, T q) noexcept
{
    const auto c = detail::rbj_common<T>(f0, q);
    return detail::rbj_normalize<T>(T(1) - c.alpha, T(-2) * c.cw, T(1) + c.alpha, T(1) + c.alpha, T(-2) * c.cw,
                                    T(1) - c.alpha);
}

// Peaking EQ (RBJ): boost/cut of gain_db at f0, unity elsewhere.
template <typename T> [[nodiscard]] Biquad<T> rbj_peaking(T f0, T q, T gain_db) noexcept
{
    const auto c = detail::rbj_common<T>(f0, q);
    const T a = crd::math::pow(T(10), gain_db / T(40));
    return detail::rbj_normalize<T>(T(1) + c.alpha * a, T(-2) * c.cw, T(1) - c.alpha * a, T(1) + c.alpha / a,
                                    T(-2) * c.cw, T(1) - c.alpha / a);
}

// Low shelf (RBJ): gain_db below f0, unity above.
template <typename T> [[nodiscard]] Biquad<T> rbj_lowshelf(T f0, T q, T gain_db) noexcept
{
    const auto c = detail::rbj_common<T>(f0, q);
    const T a = crd::math::pow(T(10), gain_db / T(40));
    const T sa = crd::math::sqrt(a);
    const T ap1 = a + T(1), am1 = a - T(1);
    return detail::rbj_normalize<T>(a * (ap1 - am1 * c.cw + T(2) * sa * c.alpha), T(2) * a * (am1 - ap1 * c.cw),
                                    a * (ap1 - am1 * c.cw - T(2) * sa * c.alpha), ap1 + am1 * c.cw + T(2) * sa * c.alpha,
                                    T(-2) * (am1 + ap1 * c.cw), ap1 + am1 * c.cw - T(2) * sa * c.alpha);
}

// High shelf (RBJ): gain_db above f0, unity below.
template <typename T> [[nodiscard]] Biquad<T> rbj_highshelf(T f0, T q, T gain_db) noexcept
{
    const auto c = detail::rbj_common<T>(f0, q);
    const T a = crd::math::pow(T(10), gain_db / T(40));
    const T sa = crd::math::sqrt(a);
    const T ap1 = a + T(1), am1 = a - T(1);
    return detail::rbj_normalize<T>(a * (ap1 + am1 * c.cw + T(2) * sa * c.alpha), T(-2) * a * (am1 + ap1 * c.cw),
                                    a * (ap1 + am1 * c.cw - T(2) * sa * c.alpha), ap1 - am1 * c.cw + T(2) * sa * c.alpha,
                                    T(2) * (am1 - ap1 * c.cw), ap1 - am1 * c.cw - T(2) * sa * c.alpha);
}

} // namespace crd::hesap::dsp
