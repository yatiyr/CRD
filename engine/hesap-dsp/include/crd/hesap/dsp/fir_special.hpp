#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-d — special FIR designs: Savitzky-Golay + raised-cosine / RRC.
//
//   savgol_coeffs — the smoothing/derivative coefficients of a Savitzky-Golay
//     filter: a local polynomial least-squares fit. Closed form (a small normal-
//     equation solve) ⇒ FULL scipy coefficient match.
//   raised_cosine / root_raised_cosine — the (square-root) raised-cosine pulse
//     shapes (the Nyquist-ISI-free comms pulses). Closed-form formulas with the
//     two removable singularities handled ⇒ match MATLAB rcosdesign / liquid.
// Lower-layer raw scalars.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/lu.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <numbers>

namespace crd::hesap::dsp
{

// Savitzky-Golay filter coefficients (scipy.signal.savgol_coeffs, use='conv'). window_length ODD; polyorder <
// window_length. deriv = derivative order (0 = smoothing). Solves the (polyorder+1) normal equations A Aᵀ z = y,
// coeffs[j] = Σ_k x[j]^k z[k]. Closed-form ⇒ matches scipy to ~1e-12.
template <typename T>
[[nodiscard]] crd::containers::Array<T> savgol_coeffs(crd::memory::IAllocator* alloc, crd::usize window_length,
                                                      crd::usize polyorder, crd::usize deriv = 0, T delta = T(1))
{
    CRD_ASSERT(window_length % 2 == 1 && polyorder < window_length);
    const crd::usize po1 = polyorder + 1;
    const T pos = static_cast<T>(window_length / 2); // odd ⇒ centre
    // x[j] = pos - j  (the conv-ordered positions).
    crd::containers::Array<T> x(alloc);
    x.resize(window_length);
    for (crd::usize j = 0; j < window_length; ++j)
    {
        x[j] = pos - static_cast<T>(j);
    }
    // M[k][l] = Σ_j x[j]^(k+l)  (= A Aᵀ, SPD).
    dense::Matrix<T> Mtx(alloc, po1, po1);
    for (crd::usize k = 0; k < po1; ++k)
    {
        for (crd::usize l = 0; l < po1; ++l)
        {
            T s = T(0);
            for (crd::usize j = 0; j < window_length; ++j)
            {
                s += std::pow(x[j], static_cast<T>(k + l));
            }
            Mtx(k, l) = s;
        }
    }
    crd::containers::Array<T> z(alloc);
    z.resize(po1);
    for (crd::usize k = 0; k < po1; ++k)
    {
        z[k] = T(0);
    }
    T fact = T(1);
    for (crd::usize i = 2; i <= deriv; ++i)
    {
        fact *= static_cast<T>(i);
    }
    if (deriv <= polyorder)
    {
        z[deriv] = fact / std::pow(delta, static_cast<T>(deriv));
    }
    dense::LU<T> lu(alloc, po1);
    dense::factor_lu<T, dense::Layout::RowMajor>(lu, Mtx);
    dense::solve_lu<T, dense::Layout::RowMajor>(lu, crd::containers::Span<T>(z.data(), po1)); // z ← M⁻¹ y

    crd::containers::Array<T> h(alloc);
    h.resize(window_length);
    for (crd::usize j = 0; j < window_length; ++j)
    {
        T s = T(0);
        T xp = T(1);
        for (crd::usize k = 0; k < po1; ++k)
        {
            s += xp * z[k];
            xp *= x[j];
        }
        h[j] = s;
    }
    return h;
}

// Root-raised-cosine pulse (MATLAB rcosdesign(beta, span, sps, 'sqrt')): length span*sps+1, unit-energy
// normalized. `beta` rolloff in (0,1], `sps` samples/symbol, `span` symbols. The two removable singularities
// (t=0 and t=±Ts/(4β)) are handled in closed form.
template <typename T>
[[nodiscard]] crd::containers::Array<T> root_raised_cosine(crd::memory::IAllocator* alloc, T beta, crd::usize span,
                                                           crd::usize sps)
{
    const crd::usize n = span * sps + 1;
    crd::containers::Array<T> h(alloc);
    h.resize(n);
    const T pi = static_cast<T>(std::numbers::pi_v<double>);
    const crd::usize half = (n - 1) / 2;
    // the RRC pulse is SYMMETRIC about the centre (h[half+k]=h[half-k]) ⇒ compute the first half + centre, mirror
    // (halves the sin/cos evaluations).
    for (crd::usize i = 0; i <= half; ++i)
    {
        const T t = (static_cast<T>(i) - static_cast<T>(half)) / static_cast<T>(sps); // in symbol periods (Ts=1)
        T v;
        if (std::abs(t) < static_cast<T>(1e-12))
        {
            v = (T(1) + beta * (T(4) / pi - T(1)));
        }
        else if (std::abs(std::abs(t) - T(1) / (T(4) * beta)) < static_cast<T>(1e-9))
        {
            v = (beta / std::sqrt(T(2))) *
                ((T(1) + T(2) / pi) * std::sin(pi / (T(4) * beta)) + (T(1) - T(2) / pi) * std::cos(pi / (T(4) * beta)));
        }
        else
        {
            const T num = std::sin(pi * t * (T(1) - beta)) + T(4) * beta * t * std::cos(pi * t * (T(1) + beta));
            const T den = pi * t * (T(1) - (T(4) * beta * t) * (T(4) * beta * t));
            v = num / den;
        }
        h[i] = v;
        h[n - 1 - i] = v;
    }
    // unit-energy normalization (MATLAB rcosdesign).
    T e = T(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        e += h[i] * h[i];
    }
    const T inv = T(1) / std::sqrt(e);
    for (crd::usize i = 0; i < n; ++i)
    {
        h[i] *= inv;
    }
    return h;
}

// Raised-cosine pulse (MATLAB rcosdesign(..., 'normal')): the Nyquist-ISI-free pulse, peak normalized to 1/sps
// then unit-energy scaled to match MATLAB. Removable singularity at t=±Ts/(2β).
template <typename T>
[[nodiscard]] crd::containers::Array<T> raised_cosine(crd::memory::IAllocator* alloc, T beta, crd::usize span,
                                                      crd::usize sps)
{
    const crd::usize n = span * sps + 1;
    crd::containers::Array<T> h(alloc);
    h.resize(n);
    const T pi = static_cast<T>(std::numbers::pi_v<double>);
    const crd::usize half = (n - 1) / 2;
    for (crd::usize i = 0; i <= half; ++i) // symmetric pulse ⇒ compute half + mirror
    {
        const T t = (static_cast<T>(i) - static_cast<T>(half)) / static_cast<T>(sps);
        T v;
        if (std::abs(std::abs(t) - T(1) / (T(2) * beta)) < static_cast<T>(1e-9))
        {
            v = (pi / T(4)) * (std::sin(pi * t) / (pi * t)); // l'Hopital limit
        }
        else
        {
            const T sinc = (std::abs(t) < static_cast<T>(1e-12)) ? T(1) : std::sin(pi * t) / (pi * t);
            v = sinc * std::cos(pi * beta * t) / (T(1) - (T(2) * beta * t) * (T(2) * beta * t));
        }
        h[i] = v;
        h[n - 1 - i] = v;
    }
    T e = T(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        e += h[i] * h[i];
    }
    const T inv = T(1) / std::sqrt(e);
    for (crd::usize i = 0; i < n; ++i)
    {
        h[i] *= inv;
    }
    return h;
}

} // namespace crd::hesap::dsp
