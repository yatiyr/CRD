#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-comms v11c-g — OFDM.
//
//   OfdmModulator<T>   FFT-based OFDM: modulate = IFFT(subcarriers) + cyclic
//                      prefix; demodulate = strip CP + FFT. Built on the v10 FFT
//                      engine (crd-hesap-fft).
//   estimate_channel_pilots   per-subcarrier channel estimate H[k] from scattered
//                      pilots (linear interpolation) for 1-tap zero-forcing EQ.
//   zf_equalize        X̂[k] = Y[k]/H[k].
//
// Gate (ADR-0093): OFDM round trip is identity; through a multipath channel
// (memory ≤ CP) the cyclic prefix makes the channel circular ⇒ 1-tap per-
// subcarrier equalization (known-H or pilot-estimated) recovers the symbols
// (BER→0). Lower-layer raw Complex<T>; deterministic-plan FFT ⇒ run-twice moat.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/fft/fft.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::comms
{

template <typename T> class OfdmModulator
{
public:
    OfdmModulator(crd::memory::IAllocator* alloc, crd::usize nfft, crd::usize cp_len)
        : m_plan(alloc, nfft), m_scratch(alloc), m_nfft(nfft), m_cp(cp_len)
    {
        m_scratch.resize(nfft);
    }

    [[nodiscard]] crd::usize nfft() const noexcept { return m_nfft; }
    [[nodiscard]] crd::usize cp_len() const noexcept { return m_cp; }
    [[nodiscard]] crd::usize symbol_len() const noexcept { return m_nfft + m_cp; }

    // Modulate one OFDM symbol: freq subcarriers [nfft] → time samples [nfft + cp] (CP prepended).
    void modulate(crd::containers::ConstSpan<Complex<T>> freq, crd::containers::Span<Complex<T>> out)
    {
        for (crd::usize k = 0; k < m_nfft; ++k)
        {
            m_scratch[k] = freq[k];
        }
        m_plan.execute(crd::containers::Span<Complex<T>>(m_scratch.data(), m_nfft), fft::FftDirection::Inverse);
        const T inv = T(1) / static_cast<T>(m_nfft);
        for (crd::usize k = 0; k < m_nfft; ++k)
        {
            m_scratch[k] = Complex<T>{m_scratch[k].re * inv, m_scratch[k].im * inv};
        }
        for (crd::usize i = 0; i < m_cp; ++i) // cyclic prefix = the last cp samples of the IFFT block
        {
            out[i] = m_scratch[m_nfft - m_cp + i];
        }
        for (crd::usize i = 0; i < m_nfft; ++i)
        {
            out[m_cp + i] = m_scratch[i];
        }
    }

    // Demodulate one OFDM symbol: time [nfft + cp] → freq subcarriers [nfft] (strip CP, FFT).
    void demodulate(crd::containers::ConstSpan<Complex<T>> in, crd::containers::Span<Complex<T>> freq_out)
    {
        for (crd::usize i = 0; i < m_nfft; ++i)
        {
            m_scratch[i] = in[m_cp + i];
        }
        m_plan.execute(crd::containers::Span<Complex<T>>(m_scratch.data(), m_nfft), fft::FftDirection::Forward);
        for (crd::usize k = 0; k < m_nfft; ++k)
        {
            freq_out[k] = m_scratch[k];
        }
    }

private:
    fft::FftPlan<T> m_plan;
    crd::containers::Array<Complex<T>> m_scratch;
    crd::usize m_nfft, m_cp;
};

// Channel frequency response from impulse response h (length ≤ nfft), via the OFDM FFT: H[k] = Σ_l h[l] e^{-j2πkl/N}.
template <typename T>
void channel_freq_response(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<Complex<T>> h, crd::usize nfft,
                           crd::containers::Span<Complex<T>> hf)
{
    crd::containers::Array<Complex<T>> buf(alloc);
    buf.resize(nfft);
    for (crd::usize i = 0; i < nfft; ++i)
    {
        buf[i] = (i < h.size()) ? h[i] : Complex<T>{T(0), T(0)};
    }
    fft::fft<T>(alloc, crd::containers::Span<Complex<T>>(buf.data(), nfft), fft::FftDirection::Forward);
    for (crd::usize k = 0; k < nfft; ++k)
    {
        hf[k] = buf[k];
    }
}

// Estimate H[k] from scattered pilots: pilot subcarrier indices `pilot_idx` carry known `pilot_val`. H at pilots =
// Y/X; linearly interpolate magnitude+phase (complex linear) across the gaps. Writes H[nfft].
template <typename T>
void estimate_channel_pilots(crd::containers::ConstSpan<Complex<T>> y,
                             crd::containers::ConstSpan<crd::usize> pilot_idx,
                             crd::containers::ConstSpan<Complex<T>> pilot_val, crd::containers::Span<Complex<T>> hf)
{
    const crd::usize np = pilot_idx.size();
    // H at each pilot.
    auto hpilot = [&](crd::usize p) {
        const Complex<T> yv = y[pilot_idx[p]];
        const Complex<T> xv = pilot_val[p];
        const T d = xv.re * xv.re + xv.im * xv.im;
        return Complex<T>{(yv.re * xv.re + yv.im * xv.im) / d, (yv.im * xv.re - yv.re * xv.im) / d};
    };
    for (crd::usize p = 0; p + 1 < np; ++p)
    {
        const crd::usize a = pilot_idx[p], b = pilot_idx[p + 1];
        const Complex<T> ha = hpilot(p), hb = hpilot(p + 1);
        for (crd::usize k = a; k <= b; ++k)
        {
            const T t = (b > a) ? static_cast<T>(k - a) / static_cast<T>(b - a) : T(0);
            hf[k] = Complex<T>{ha.re + (hb.re - ha.re) * t, ha.im + (hb.im - ha.im) * t};
        }
    }
    for (crd::usize k = 0; k < pilot_idx[0]; ++k) // flat-hold before the first / after the last pilot
    {
        hf[k] = hpilot(0);
    }
    for (crd::usize k = pilot_idx[np - 1]; k < hf.size(); ++k)
    {
        hf[k] = hpilot(np - 1);
    }
}

// Zero-forcing per-subcarrier equalization: x̂[k] = y[k] / H[k].
template <typename T>
void zf_equalize(crd::containers::ConstSpan<Complex<T>> y, crd::containers::ConstSpan<Complex<T>> hf,
                 crd::containers::Span<Complex<T>> out) noexcept
{
    for (crd::usize k = 0; k < y.size(); ++k)
    {
        const T d = hf[k].re * hf[k].re + hf[k].im * hf[k].im + static_cast<T>(1e-30);
        out[k] = Complex<T>{(y[k].re * hf[k].re + y[k].im * hf[k].im) / d,
                            (y[k].im * hf[k].re - y[k].re * hf[k].im) / d};
    }
}

} // namespace crd::hesap::comms
