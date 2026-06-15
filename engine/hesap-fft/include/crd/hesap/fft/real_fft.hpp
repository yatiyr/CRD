#pragma once

// real_fft.hpp — Phase 3.1.6 v10-d: real-input FFT (RFFT/IRFFT) via the half-size complex transform +
// Hermitian symmetry. A real n-point DFT has only n/2+1 independent outputs (X[n-k] = conj(X[k])), so we
// pack the n reals as n/2 complex, run ONE size-(n/2) complex FftPlan, and recombine — about half the work of a
// full complex FFT. Reuses the v10-b engine (deterministic plan). Normalization: forward unnormalized, irfft
// applies 1/n (round-trip exact).
// Lower-layer RAW (Complex<f32/f64>, ADR-0078).

#include <crd/hesap/fft/fft.hpp>

#include <cmath>

namespace crd::hesap::fft
{
// Real FFT plan: real[n] ↔ complex[n/2+1]. n must be a power of two ≥ 4 (so n/2 is a power of two for the
// reused complex FftPlan). The recombine twiddles W_n^k (k=0..n/2) are precomputed once and shared.
template <typename T> class RealFftPlan
{
public:
    RealFftPlan(crd::memory::IAllocator* alloc, crd::usize n)
        : m_n(n), m_half(alloc, n / 2), m_z(alloc), m_wn_re(alloc), m_wn_im(alloc)
    {
        CRD_ASSERT(n >= 4 && (n & (n - 1)) == 0); // power of two ≥ 4 ⇒ n/2 is a power of two
        const crd::usize h = n / 2;
        m_z.resize(h);
        m_wn_re.resize(h + 1);
        m_wn_im.resize(h + 1);
        constexpr double two_pi = 6.283185307179586476925286766559;
        for (crd::usize k = 0; k <= h; ++k)
        {
            const double ang = -two_pi * static_cast<double>(k) / static_cast<double>(n);
            m_wn_re[k] = static_cast<T>(std::cos(ang)); //  cos(2πk/n)
            m_wn_im[k] = static_cast<T>(std::sin(ang)); // -sin(2πk/n) ⇒ W_n^k = exp(-2πik/n)
        }
    }

    [[nodiscard]] crd::usize size() const noexcept { return m_n; }

    // Forward real FFT: real_in[n] → out[n/2+1] (out[0] and out[n/2] have zero imaginary part).
    void rfft(crd::containers::ConstSpan<T> real_in, crd::containers::Span<Complex<T>> out) const
    {
        const crd::usize h = m_n / 2;
        CRD_ASSERT(real_in.size() == m_n && out.size() == h + 1);
        Complex<T>* z = m_z.data();
        for (crd::usize j = 0; j < h; ++j) // pack: z[j] = x[2j] + i·x[2j+1]
        {
            z[j] = Complex<T>{real_in[2 * j], real_in[2 * j + 1]};
        }
        m_half.execute(crd::containers::Span<Complex<T>>(z, h), FftDirection::Forward); // Z = FFT_{n/2}(z)
        // Recombine: E[k]=(Z[k]+conj(Z[h-k]))/2, O[k]=(Z[k]-conj(Z[h-k]))/(2i), X[k]=E[k]+W_n^k·O[k].
        for (crd::usize k = 0; k <= h; ++k)
        {
            const Complex<T> zk = z[k % h];
            const Complex<T> znk = z[(h - k) % h];
            const T er = (zk.re + znk.re) * static_cast<T>(0.5);
            const T ei = (zk.im - znk.im) * static_cast<T>(0.5);
            const T orr = (zk.im + znk.im) * static_cast<T>(0.5); // O = (Z[k]-conj Z[h-k])/(2i)
            const T oii = (znk.re - zk.re) * static_cast<T>(0.5);
            const T wr = m_wn_re[k];
            const T wi = m_wn_im[k];
            out[k] = Complex<T>{er + (wr * orr - wi * oii), ei + (wr * oii + wi * orr)};
        }
    }

    // Inverse real FFT: in[n/2+1] (Hermitian half-spectrum) → real_out[n], normalized (irfft(rfft(x)) == x).
    void irfft(crd::containers::ConstSpan<Complex<T>> in, crd::containers::Span<T> real_out) const
    {
        const crd::usize h = m_n / 2;
        CRD_ASSERT(in.size() == h + 1 && real_out.size() == m_n);
        Complex<T>* z = m_z.data();
        for (crd::usize k = 0; k < h; ++k)
        {
            const Complex<T> xk = in[k];
            const Complex<T> xhk = in[h - k]; // k=0 ⇒ in[h]; k≥1 ⇒ in[h-k] ∈ [1,h-1]
            const T er = (xk.re + xhk.re) * static_cast<T>(0.5);
            const T ei = (xk.im - xhk.im) * static_cast<T>(0.5); // E[k] = (X[k]+conj X[h-k])/2
            const T dr = (xk.re - xhk.re) * static_cast<T>(0.5);
            const T di = (xk.im + xhk.im) * static_cast<T>(0.5); // D = (X[k]-conj X[h-k])/2
            const T wr = m_wn_re[k];
            const T wi = m_wn_im[k];         // W_n^{-k} = conj(W_n^k) = (wr, -wi)
            const T orr = wr * dr + wi * di; // O[k] = W_n^{-k}·D
            const T oii = wr * di - wi * dr;
            z[k] = Complex<T>{er - oii, ei + orr}; // Z[k] = E[k] + i·O[k]
        }
        m_half.execute(crd::containers::Span<Complex<T>>(z, h), FftDirection::Inverse); // = h·z (unnormalized)
        const T inv = static_cast<T>(1) / static_cast<T>(h);
        for (crd::usize j = 0; j < h; ++j)
        {
            real_out[2 * j] = z[j].re * inv;
            real_out[2 * j + 1] = z[j].im * inv;
        }
    }

private:
    crd::usize m_n;
    mutable FftPlan<T> m_half; // size n/2 complex transform (its scratch is reused)
    mutable crd::containers::Array<Complex<T>> m_z;
    crd::containers::Array<T> m_wn_re; // W_n^k, k = 0..n/2
    crd::containers::Array<T> m_wn_im;
};
} // namespace crd::hesap::fft
