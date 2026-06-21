#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-comms v11c-b — pulse shaping + matched filtering.
//
//   rrc_pulse / rc_pulse    (square-)raised-cosine pulses (via crd-hesap-dsp).
//   gaussian_pulse          GFSK/GMSK Gaussian filter (BT product).
//   pulse_shape             upsample symbols by sps + FIR (complex over a real
//                           tap kernel) — the TX shaping filter.
//   matched_filter          FIR with the time-reversed pulse (RRC is symmetric)
//                           — the RX matched filter; max-SNR + Nyquist with RRC.
//   eye_segments            fold the waveform into overlapping traces (eye).
//   peak_distortion         worst-case ISI at the sampling instants.
//
// Gate (ADR-0093): RRC-TX ⊛ RRC-RX = raised cosine ⇒ ZERO ISI at the symbol
// instants — a noise-free round trip recovers the symbols exactly (the Nyquist
// criterion, the real spec) + Gaussian-pulse properties + matched-filter SNR.
// Lower-layer raw Complex<T> over real taps. Deterministic ⇒ run-twice moat.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dsp/fir_special.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <numbers>

namespace crd::hesap::comms
{

// Root-raised-cosine TX/RX pulse (length span*sps+1, unit energy). Matches MATLAB rcosdesign(beta,span,sps,'sqrt').
template <typename T>
[[nodiscard]] crd::containers::Array<T> rrc_pulse(crd::memory::IAllocator* alloc, T beta, crd::usize span,
                                                  crd::usize sps)
{
    return dsp::root_raised_cosine<T>(alloc, beta, span, sps);
}

// Raised-cosine (full Nyquist) pulse.
template <typename T>
[[nodiscard]] crd::containers::Array<T> rc_pulse(crd::memory::IAllocator* alloc, T beta, crd::usize span, crd::usize sps)
{
    return dsp::raised_cosine<T>(alloc, beta, span, sps);
}

// GFSK/GMSK Gaussian filter: h(t) = exp(-t²/(2σ²)), σ = √(ln2)/(2π·BT)·Ts (Ts = sps samples). Length span*sps+1,
// normalized to unit sum (DC gain 1). `bt` = bandwidth-time product.
template <typename T>
[[nodiscard]] crd::containers::Array<T> gaussian_pulse(crd::memory::IAllocator* alloc, T bt, crd::usize span,
                                                       crd::usize sps)
{
    const crd::usize n = span * sps + 1;
    crd::containers::Array<T> h(alloc);
    h.resize(n);
    const T pi = static_cast<T>(std::numbers::pi_v<double>);
    const T ts = static_cast<T>(sps);
    const T sigma = std::sqrt(std::log(T(2))) / (T(2) * pi * bt) * ts;
    const crd::isize half = static_cast<crd::isize>(n / 2);
    T sum = T(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        const T t = static_cast<T>(static_cast<crd::isize>(i) - half);
        h[i] = std::exp(-t * t / (T(2) * sigma * sigma));
        sum += h[i];
    }
    for (crd::usize i = 0; i < n; ++i)
    {
        h[i] /= sum;
    }
    return h;
}

// Pulse-shape: upsample `syms` by sps (zero-stuff) then convolve with the real `taps`. Output length
// nsym*sps + L - 1. The k-th symbol's pulse is centred at k*sps + (L-1)/2.
template <typename T>
[[nodiscard]] crd::containers::Array<Complex<T>> pulse_shape(crd::memory::IAllocator* alloc,
                                                             crd::containers::ConstSpan<Complex<T>> syms,
                                                             crd::containers::ConstSpan<T> taps, crd::usize sps)
{
    const crd::usize nsym = syms.size();
    const crd::usize l = taps.size();
    const crd::usize out_len = (nsym == 0) ? 0 : (nsym * sps + l - 1);
    crd::containers::Array<Complex<T>> out(alloc);
    out.resize(out_len);
    for (crd::usize n = 0; n < out_len; ++n)
    {
        Complex<T> acc{T(0), T(0)};
        // upsampled[m] is nonzero (== syms[m/sps]) only at m = k*sps. out[n] = Σ_k taps[n - k*sps]·syms[k].
        const crd::isize nm = static_cast<crd::isize>(n);
        crd::isize kmin = (nm - static_cast<crd::isize>(l) + 1 + static_cast<crd::isize>(sps) - 1) /
                          static_cast<crd::isize>(sps);
        if (kmin < 0)
        {
            kmin = 0;
        }
        const crd::isize kmax = nm / static_cast<crd::isize>(sps); // k*sps <= n
        for (crd::isize k = kmin; k <= kmax && k < static_cast<crd::isize>(nsym); ++k)
        {
            const crd::isize ti = nm - k * static_cast<crd::isize>(sps);
            acc.re += taps[static_cast<crd::usize>(ti)] * syms[static_cast<crd::usize>(k)].re;
            acc.im += taps[static_cast<crd::usize>(ti)] * syms[static_cast<crd::usize>(k)].im;
        }
        out[n] = acc;
    }
    return out;
}

// Matched filter: full convolution of the complex `x` with the real `taps` (RRC is symmetric ⇒ matched == itself).
template <typename T>
[[nodiscard]] crd::containers::Array<Complex<T>> matched_filter(crd::memory::IAllocator* alloc,
                                                                crd::containers::ConstSpan<Complex<T>> x,
                                                                crd::containers::ConstSpan<T> taps)
{
    const crd::usize n = x.size(), l = taps.size();
    const crd::usize out_len = (n == 0) ? 0 : (n + l - 1);
    crd::containers::Array<Complex<T>> out(alloc);
    out.resize(out_len);
    for (crd::usize m = 0; m < out_len; ++m)
    {
        Complex<T> acc{T(0), T(0)};
        const crd::isize mm = static_cast<crd::isize>(m);
        crd::isize jmin = mm - static_cast<crd::isize>(n) + 1;
        if (jmin < 0)
        {
            jmin = 0;
        }
        const crd::isize jmax = (mm < static_cast<crd::isize>(l) - 1) ? mm : static_cast<crd::isize>(l) - 1;
        for (crd::isize j = jmin; j <= jmax; ++j)
        {
            const Complex<T> xv = x[static_cast<crd::usize>(mm - j)];
            acc.re += taps[static_cast<crd::usize>(j)] * xv.re;
            acc.im += taps[static_cast<crd::usize>(j)] * xv.im;
        }
        out[m] = acc;
    }
    return out;
}

// Fold a real waveform into `traces` overlapping segments of width `width` (typically 2*sps) for an eye diagram.
// Returns a (traces × width) row-major matrix.
template <typename T>
[[nodiscard]] crd::containers::Array<T> eye_segments(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                                     crd::usize sps, crd::usize width, crd::usize traces)
{
    crd::containers::Array<T> out(alloc);
    out.resize(traces * width);
    for (crd::usize t = 0; t < traces; ++t)
    {
        const crd::usize start = t * sps;
        for (crd::usize j = 0; j < width; ++j)
        {
            const crd::usize idx = start + j;
            out[t * width + j] = (idx < x.size()) ? x[idx] : T(0);
        }
    }
    return out;
}

// Peak distortion (worst-case ISI): max over the sampling instants of |recovered - symbol| / |symbol_scale|.
// Returns the max absolute deviation of the matched-filtered, symbol-sampled values from the transmitted symbols.
template <typename T>
[[nodiscard]] T peak_distortion(crd::containers::ConstSpan<Complex<T>> recovered,
                                crd::containers::ConstSpan<Complex<T>> syms) noexcept
{
    T worst = T(0);
    const crd::usize n = (recovered.size() < syms.size()) ? recovered.size() : syms.size();
    for (crd::usize k = 0; k < n; ++k)
    {
        const T dr = recovered[k].re - syms[k].re;
        const T di = recovered[k].im - syms[k].im;
        const T d = std::sqrt(dr * dr + di * di);
        worst = (d > worst) ? d : worst;
    }
    return worst;
}

} // namespace crd::hesap::comms
