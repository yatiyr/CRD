#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-c — FIR design by the WINDOW METHOD (firwin / firwin2).
//
// firwin: the ideal brick-wall impulse response (a sum of sincs over the pass
// bands) truncated by a window. Closed-form (sinc x window x normalize) — so it
// gets the FULL 1e-12 coefficient match to scipy AND the perf crush, unlike the
// transcendental-iterative designs (remez/ellip) which gate on spec-compliance.
//
// Faithful scipy.signal.firwin: cutoff normalized to Nyquist (1.0 == fs/2);
// bands built from the pass_zero flag; h = Σ_band right*sinc(right*m) -
// left*sinc(left*m), m centred; h *= window; scaled so the first passband
// centre has unit gain. np.sinc(x) = sin(pi x)/(pi x). Lower-layer raw scalars.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/fft/fft.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <numbers>

namespace crd::hesap::dsp
{

// Normalized sinc (np.sinc): sin(pi x)/(pi x), sinc(0) = 1.
template <typename T> [[nodiscard]] T np_sinc(T x) noexcept
{
    if (std::abs(x) < static_cast<T>(1e-300))
    {
        return T(1);
    }
    const T px = static_cast<T>(std::numbers::pi_v<double>) * x;
    return crd::math::sin(px) / px;
}

// firwin core. `cutoff` = band-edge frequencies normalized to Nyquist (each in (0,1), strictly increasing).
// `window` = numtaps SYMMETRIC window samples (caller generates via windows.hpp; scipy default = hamming).
// `pass_zero` = true ⇒ the band starting at 0 is a passband (lowpass/bandstop); false ⇒ highpass/bandpass.
// `scale` = normalize the first passband centre to unit gain. Returns numtaps linear-phase taps.
template <typename T>
[[nodiscard]] crd::containers::Array<T> firwin(crd::memory::IAllocator* alloc, crd::usize numtaps,
                                               crd::containers::ConstSpan<T> cutoff,
                                               crd::containers::ConstSpan<T> window, bool pass_zero = true,
                                               bool scale = true)
{
    crd::containers::Array<T> h(alloc);
    h.resize(numtaps);
    CRD_ASSERT(window.size() == numtaps && cutoff.size() >= 1);

    // bands = [0?(pass_zero)] ++ cutoff ++ [1?(pass_nyquist)], reshaped into (left,right) pairs.
    const bool pass_nyquist = ((cutoff.size() % 2 == 0) == pass_zero);
    crd::containers::Array<T> edges(alloc);
    if (pass_zero)
    {
        edges.push_back(T(0));
    }
    for (crd::usize i = 0; i < cutoff.size(); ++i)
    {
        edges.push_back(cutoff[i]);
    }
    if (pass_nyquist)
    {
        edges.push_back(T(1));
    }
    const crd::usize nband = edges.size() / 2;

    const T alpha = T(0.5) * static_cast<T>(numtaps - 1);
    // FIR taps are linear-phase SYMMETRIC: m(N-1-i) = -m(i) and sinc is even ⇒ v(N-1-i) = v(i), and the window is
    // symmetric ⇒ h[i] = h[N-1-i]. Compute only the first half (halves the sinc/sin evaluations).
    const crd::usize half = (numtaps + 1) / 2;
    for (crd::usize i = 0; i < half; ++i)
    {
        const T m = static_cast<T>(i) - alpha;
        T v = T(0);
        for (crd::usize b = 0; b < nband; ++b)
        {
            const T left = edges[2 * b];
            const T right = edges[2 * b + 1];
            v += right * np_sinc<T>(right * m) - left * np_sinc<T>(left * m);
        }
        const T hv = v * window[i];
        h[i] = hv;
        h[numtaps - 1 - i] = hv; // symmetric mirror
    }

    if (scale)
    {
        const T left = edges[0];
        const T right = edges[1];
        const T scale_freq = (left == T(0)) ? T(0) : ((right == T(1)) ? T(1) : T(0.5) * (left + right));
        const T pi = static_cast<T>(std::numbers::pi_v<double>);
        T s = T(0);
        for (crd::usize i = 0; i < numtaps; ++i)
        {
            const T m = static_cast<T>(i) - alpha;
            s += h[i] * crd::math::cos(pi * m * scale_freq);
        }
        for (crd::usize i = 0; i < numtaps; ++i)
        {
            h[i] = h[i] / s;
        }
    }
    return h;
}

// firwin2: design a linear-phase FIR from arbitrary (frequency, gain) breakpoints by frequency sampling. The
// desired response is linearly interpolated onto a dense grid, given linear phase, and inverse-transformed, then
// windowed. `freq` in [0,1] (1=Nyquist), strictly increasing, must start at 0 and end at 1; `gain` the desired
// magnitude at each. Faithful scipy.signal.firwin2 (irfft realized as a Hermitian-extended inverse complex FFT
// over crd-hesap-fft). Closed-form ⇒ 1e-12 coefficient match. `nfreqs` 0 = scipy default 1 + 2^ceil(log2(numtaps)).
template <typename T>
[[nodiscard]] crd::containers::Array<T> firwin2(crd::memory::IAllocator* alloc, crd::usize numtaps,
                                                crd::containers::ConstSpan<T> freq, crd::containers::ConstSpan<T> gain,
                                                crd::containers::ConstSpan<T> window, crd::usize nfreqs = 0)
{
    CRD_ASSERT(freq.size() == gain.size() && freq.size() >= 2 && window.size() == numtaps);
    if (nfreqs == 0)
    {
        crd::usize p = 1;
        while ((crd::usize{1} << p) < numtaps)
        {
            ++p;
        }
        nfreqs = 1 + (crd::usize{1} << p);
    }
    const crd::usize nfft = 2 * (nfreqs - 1); // irfft output length (power of two)
    const T pi = static_cast<T>(std::numbers::pi_v<double>);

    // linear interpolation of `gain` at x (np.interp; freq monotone 0..1, x in [0,1]).
    auto interp = [&](T x) -> T
    {
        if (x <= freq[0])
        {
            return gain[0];
        }
        if (x >= freq[freq.size() - 1])
        {
            return gain[gain.size() - 1];
        }
        crd::usize j = 0;
        while (j + 1 < freq.size() && freq[j + 1] < x)
        {
            ++j;
        }
        const T t = (x - freq[j]) / (freq[j + 1] - freq[j]);
        return gain[j] + t * (gain[j + 1] - gain[j]);
    };

    // build the full Hermitian spectrum (length nfft): half from the interpolated, phase-shifted response.
    crd::containers::Array<Complex<T>> spec(alloc);
    spec.resize(nfft);
    const T alpha = T(0.5) * static_cast<T>(numtaps - 1);
    for (crd::usize k = 0; k < nfreqs; ++k)
    {
        const T x = static_cast<T>(k) / static_cast<T>(nfreqs - 1); // x in [0,1], nyq=1
        const T fx = interp(x);
        const T phase = -alpha * pi * x; // linear-phase shift
        spec[k] = Complex<T>{fx * crd::math::cos(phase), fx * crd::math::sin(phase)};
    }
    for (crd::usize k = 1; k < nfreqs - 1; ++k) // Hermitian-extend the upper half
    {
        spec[nfft - k] = Complex<T>{spec[k].re, -spec[k].im};
    }

    fft::FftPlan<T> plan(alloc, nfft);
    plan.execute(crd::containers::Span<Complex<T>>(spec.data(), nfft), fft::FftDirection::Inverse); // unnormalized

    crd::containers::Array<T> out(alloc);
    out.resize(numtaps);
    const T inv = T(1) / static_cast<T>(nfft); // irfft normalization
    for (crd::usize i = 0; i < numtaps; ++i)
    {
        out[i] = spec[i].re * inv * window[i];
    }
    return out;
}

} // namespace crd::hesap::dsp
