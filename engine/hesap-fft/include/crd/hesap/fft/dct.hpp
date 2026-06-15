#pragma once

// dct.hpp — Phase 3.1.6 v10-f: Discrete Cosine / Sine transforms (DCT-II/III, DST-II/III), the workhorses of
// JPEG / audio / MFCC / spectral methods. Computed via Makhoul's O(N log N) reduction to ONE N-point complex
// FFT (the v10-b engine), NOT the O(N²) direct sum. Conventions match scipy norm=None exactly (every formula
// verified against scipy in scripts/dct_research.py — scipy is the correctness oracle). DCT-III is the
// inverse of DCT-II up to the 2N factor; DST-III the inverse of DST-II. N must be a power of two (≥ 2).
//
// Correctness gate = the DIRECT O(N²) DCT/DST sum (the analog of the FFT's brute-force-DFT gate) — NOT a
// forward∘inverse round-trip, which can cancel a sign error (the odeint-d4 trap). Lower-layer RAW (T = f32/f64,
// ADR-0078).
//
// Definitions (scipy norm=None):
//   DCT-II :  y[k] = 2 Σ_n x[n] cos(π(2n+1)k/(2N))
//   DCT-III:  y[k] = x[0] + 2 Σ_{n≥1} x[n] cos(πn(2k+1)/(2N))
//   DST-II :  y[k] = 2 Σ_n x[n] sin(π(2n+1)(k+1)/(2N))
//   DST-III:  y[k] = (-1)^k x[N-1] + 2 Σ_{n=0}^{N-2} x[n] sin(π(n+1)(2k+1)/(2N))

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/fft/fft.hpp>
#include <crd/hesap/fft/real_fft.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::fft
{

// DCT/DST plan over a size-N real signal (N a power of two). Owns one complex FftPlan + the half-angle phase
// table θ_k = πk/(2N), k=0..N (the Makhoul twiddles), shared by all four transforms => deterministic.
template <typename T> class DctPlan
{
public:
    DctPlan(crd::memory::IAllocator* alloc, crd::usize n)
        : m_n(n), m_fft(alloc, n), m_rfft(alloc, n >= 4 ? n : 4), m_buf(alloc), m_rbuf(alloc), m_half(alloc),
          m_cos(alloc), m_sin(alloc)
    {
        CRD_ASSERT(n >= 2 && (n & (n - 1)) == 0); // power of two ≥ 2 (so the reused FftPlan applies, N even)
        m_buf.resize(n);
        m_rbuf.resize(n);
        m_half.resize(n / 2 + 1);
        m_cos.resize(n + 1);
        m_sin.resize(n + 1);
        constexpr double pi = 3.14159265358979323846;
        for (crd::usize k = 0; k <= n; ++k)
        {
            const double th = pi * static_cast<double>(k) / (2.0 * static_cast<double>(n)); // θ_k = πk/2N
            m_cos[k] = static_cast<T>(std::cos(th));
            m_sin[k] = static_cast<T>(std::sin(th));
        }
    }

    [[nodiscard]] crd::usize size() const noexcept { return m_n; }

    // DCT-II: shuffle (even samples up, odd samples down) → FFT → 2 Re(e^{-iθ_k} W[k]). The shuffled sequence is
    // REAL, so a real FFT (half the work of a complex FFT — the v10-d engine) suffices: rfft gives W[0..N/2];
    // W[k>N/2] = conj(W[N-k]).
    void dct2(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const
    {
        const crd::usize n = m_n;
        CRD_ASSERT(x.size() == n && y.size() == n);
        T* w = m_rbuf.data();
        for (crd::usize k = 0; k < n / 2; ++k)
        {
            w[k] = x[2 * k];
            w[n - 1 - k] = x[2 * k + 1];
        }
        if (n < 4) // N=2: rfft needs N≥4; trivial complex path
        {
            Complex<T>* b = m_buf.data();
            b[0] = Complex<T>{w[0], static_cast<T>(0)};
            b[1] = Complex<T>{w[1], static_cast<T>(0)};
            m_fft.execute(crd::containers::Span<Complex<T>>(b, n), FftDirection::Forward);
            for (crd::usize k = 0; k < n; ++k)
            {
                y[k] = static_cast<T>(2) * (m_cos[k] * b[k].re + m_sin[k] * b[k].im);
            }
            return;
        }
        const crd::usize h = n / 2;
        m_rfft.rfft(crd::containers::ConstSpan<T>(w, n), crd::containers::Span<Complex<T>>(m_half.data(), h + 1));
        const Complex<T>* wh = m_half.data();
        for (crd::usize k = 0; k <= h; ++k) // W[k] = Wh[k]
        {
            y[k] = static_cast<T>(2) * (m_cos[k] * wh[k].re + m_sin[k] * wh[k].im);
        }
        for (crd::usize k = h + 1; k < n; ++k) // W[k] = conj(Wh[N-k]) => Re·cos − Im·sin (with conj flipping Im)
        {
            const Complex<T> wk = wh[n - k];
            y[k] = static_cast<T>(2) * (m_cos[k] * wk.re - m_sin[k] * wk.im);
        }
    }

    // DCT-III (inverse of DCT-II): build G[k]=½(x[k]-i x[N-k]), W=e^{iθ_k}G, IFFT (unnormalized), unshuffle, ×2.
    void dct3(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const
    {
        const crd::usize n = m_n;
        CRD_ASSERT(x.size() == n && y.size() == n);
        Complex<T>* b = m_buf.data();
        b[0] = Complex<T>{x[0] * static_cast<T>(0.5), static_cast<T>(0)};
        for (crd::usize k = 1; k < n; ++k)
        {
            const Complex<T> g{x[k] * static_cast<T>(0.5), -x[n - k] * static_cast<T>(0.5)};
            b[k] = Complex<T>{m_cos[k], m_sin[k]} * g; // e^{iθ_k} G[k]
        }
        m_fft.execute(crd::containers::Span<Complex<T>>(b, n), FftDirection::Inverse); // unnormalized
        for (crd::usize k = 0; k < n / 2; ++k)
        {
            y[2 * k] = static_cast<T>(2) * b[k].re;
            y[2 * k + 1] = static_cast<T>(2) * b[n - 1 - k].re;
        }
    }

    // DST-II: shuffle (odd samples reversed AND negated) → FFT → 2 sinθ_j·Wre − 2 cosθ_j·Wim, j=k+1, idx=j%N.
    // Real shuffled sequence ⇒ real FFT (half work, v10-d) with W[idx>N/2]=conj(Wh[N-idx]).
    void dst2(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const
    {
        const crd::usize n = m_n;
        CRD_ASSERT(x.size() == n && y.size() == n);
        T* w = m_rbuf.data();
        for (crd::usize k = 0; k < n / 2; ++k)
        {
            w[k] = x[2 * k];
            w[n - 1 - k] = -x[2 * k + 1];
        }
        if (n < 4) // N=2: rfft needs N≥4; trivial complex path
        {
            Complex<T>* b = m_buf.data();
            b[0] = Complex<T>{w[0], static_cast<T>(0)};
            b[1] = Complex<T>{w[1], static_cast<T>(0)};
            m_fft.execute(crd::containers::Span<Complex<T>>(b, n), FftDirection::Forward);
            for (crd::usize m = 0; m < n; ++m)
            {
                const crd::usize j = m + 1;
                const crd::usize idx = (j == n) ? 0 : j;
                y[m] = static_cast<T>(2) * m_sin[j] * b[idx].re - static_cast<T>(2) * m_cos[j] * b[idx].im;
            }
            return;
        }
        const crd::usize h = n / 2;
        m_rfft.rfft(crd::containers::ConstSpan<T>(w, n), crd::containers::Span<Complex<T>>(m_half.data(), h + 1));
        const Complex<T>* wh = m_half.data();
        for (crd::usize m = 0; m < n; ++m)
        {
            const crd::usize j = m + 1;              // θ_j, j = 1..N
            const crd::usize idx = (j == n) ? 0 : j; // W[(m+1) mod N]
            if (idx <= h)
            {
                y[m] = static_cast<T>(2) * m_sin[j] * wh[idx].re - static_cast<T>(2) * m_cos[j] * wh[idx].im;
            }
            else // W[idx] = conj(Wh[N-idx]) ⇒ Im flips sign
            {
                const Complex<T> wk = wh[n - idx];
                y[m] = static_cast<T>(2) * m_sin[j] * wk.re + static_cast<T>(2) * m_cos[j] * wk.im;
            }
        }
    }

    // DST-III (inverse of DST-II): W[0]=x[N-1]/2, W[j]=e^{iθ_j}(½x[N-j-1]-i½x[j-1]); IFFT, unshuffle (odd neg), ×2.
    void dst3(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const
    {
        const crd::usize n = m_n;
        CRD_ASSERT(x.size() == n && y.size() == n);
        Complex<T>* b = m_buf.data();
        b[0] = Complex<T>{x[n - 1] * static_cast<T>(0.5), static_cast<T>(0)};
        for (crd::usize j = 1; j < n; ++j)
        {
            const Complex<T> h{x[n - j - 1] * static_cast<T>(0.5), -x[j - 1] * static_cast<T>(0.5)};
            b[j] = Complex<T>{m_cos[j], m_sin[j]} * h; // e^{iθ_j} H[j]
        }
        m_fft.execute(crd::containers::Span<Complex<T>>(b, n), FftDirection::Inverse); // unnormalized
        for (crd::usize k = 0; k < n / 2; ++k)
        {
            y[2 * k] = static_cast<T>(2) * b[k].re;
            y[2 * k + 1] = static_cast<T>(-2) * b[n - 1 - k].re;
        }
    }

    // -------- direct O(N²) references (the GATE — exact, FFT-free) ----------------------------------------

    void direct_dct2(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const
    {
        const crd::usize n = m_n;
        constexpr double pi = 3.14159265358979323846;
        for (crd::usize k = 0; k < n; ++k)
        {
            double acc = 0.0;
            for (crd::usize nn = 0; nn < n; ++nn)
            {
                acc += static_cast<double>(x[nn]) *
                       std::cos(pi * static_cast<double>(2 * nn + 1) * static_cast<double>(k) / (2.0 * n));
            }
            y[k] = static_cast<T>(2.0 * acc);
        }
    }

    void direct_dct3(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const
    {
        const crd::usize n = m_n;
        constexpr double pi = 3.14159265358979323846;
        for (crd::usize k = 0; k < n; ++k)
        {
            double acc = 0.0;
            for (crd::usize nn = 1; nn < n; ++nn)
            {
                acc += static_cast<double>(x[nn]) *
                       std::cos(pi * static_cast<double>(nn) * static_cast<double>(2 * k + 1) / (2.0 * n));
            }
            y[k] = static_cast<T>(static_cast<double>(x[0]) + 2.0 * acc);
        }
    }

    void direct_dst2(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const
    {
        const crd::usize n = m_n;
        constexpr double pi = 3.14159265358979323846;
        for (crd::usize k = 0; k < n; ++k)
        {
            double acc = 0.0;
            for (crd::usize nn = 0; nn < n; ++nn)
            {
                acc += static_cast<double>(x[nn]) *
                       std::sin(pi * static_cast<double>(2 * nn + 1) * static_cast<double>(k + 1) / (2.0 * n));
            }
            y[k] = static_cast<T>(2.0 * acc);
        }
    }

    void direct_dst3(crd::containers::ConstSpan<T> x, crd::containers::Span<T> y) const
    {
        const crd::usize n = m_n;
        constexpr double pi = 3.14159265358979323846;
        for (crd::usize k = 0; k < n; ++k)
        {
            double acc = 0.0;
            for (crd::usize nn = 0; nn + 1 < n; ++nn)
            {
                acc += static_cast<double>(x[nn]) *
                       std::sin(pi * static_cast<double>(nn + 1) * static_cast<double>(2 * k + 1) / (2.0 * n));
            }
            const double sgn = (k % 2 == 0) ? 1.0 : -1.0;
            y[k] = static_cast<T>(sgn * static_cast<double>(x[n - 1]) + 2.0 * acc);
        }
    }

private:
    crd::usize m_n;
    mutable FftPlan<T> m_fft;                          // complex N-FFT (inverse transforms + N=2 forward)
    mutable RealFftPlan<T> m_rfft;                     // real N-FFT (forward dct2 — half the work; v10-d)
    mutable crd::containers::Array<Complex<T>> m_buf;  // N-point complex scratch
    mutable crd::containers::Array<T> m_rbuf;          // N-point real shuffle scratch (forward real path)
    mutable crd::containers::Array<Complex<T>> m_half; // half-spectrum N/2+1 (rfft output)
    crd::containers::Array<T> m_cos;                   // cos θ_k, k = 0..N
    crd::containers::Array<T> m_sin;                   // sin θ_k
};

} // namespace crd::hesap::fft
