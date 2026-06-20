#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-j — convolution + correlation (direct + FFT-based).
//
//   convolve    — direct O(n*m) linear convolution (bit-exact; best for small).
//   fftconvolve — FFT-based O(N log N) over crd-hesap-fft (the v10 engine that
//                 beats PocketFFT) — the perf battleground vs scipy.fftconvolve.
//   correlate   — cross-correlation (convolution with the time-reversed kernel).
//
// FFT conv: pad to nfft = next-pow2(n+m-1), FFT a and b, multiply, inverse FFT,
// take the real part, /nfft (the engine's inverse is unnormalized). Gate: direct
// bit-exact; fftconvolve == direct to FFT roundoff (~1e-10) AND vs scipy. f64.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/fft/fft.hpp>
#include <crd/hesap/fft/real_fft.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::dsp
{

// direct linear convolution (mode='full'): out[k] = Σ_j a[j] b[k-j], length n+m-1. Bit-exact.
template <typename T>
[[nodiscard]] crd::containers::Array<T> convolve(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> a,
                                                 crd::containers::ConstSpan<T> b)
{
    crd::containers::Array<T> out(alloc);
    if (a.size() == 0 || b.size() == 0)
    {
        return out;
    }
    const crd::usize n = a.size() + b.size() - 1;
    out.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        out[i] = T(0);
    }
    for (crd::usize i = 0; i < a.size(); ++i)
    {
        for (crd::usize j = 0; j < b.size(); ++j)
        {
            out[i + j] += a[i] * b[j];
        }
    }
    return out;
}

// FFT-based linear convolution (mode='full'), length n+m-1. Over the crd-hesap-fft engine.
template <typename T>
[[nodiscard]] crd::containers::Array<T> fftconvolve(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> a,
                                                    crd::containers::ConstSpan<T> b)
{
    crd::containers::Array<T> out(alloc);
    if (a.size() == 0 || b.size() == 0)
    {
        return out;
    }
    const crd::usize n = a.size() + b.size() - 1;
    crd::usize nfft = 4; // RealFftPlan requires a pow-2 >= 4
    while (nfft < n)
    {
        nfft <<= 1;
    }
    // REAL FFT path (half the work of a complex FFT — the scipy fftconvolve uses rfft too): rfft a + b,
    // multiply the Hermitian half-spectra, irfft (already normalized).
    const crd::usize half = nfft / 2 + 1;
    crd::containers::Array<T> pa(alloc), pb(alloc), real_out(alloc);
    pa.resize(nfft);
    pb.resize(nfft);
    real_out.resize(nfft);
    for (crd::usize i = 0; i < nfft; ++i)
    {
        pa[i] = (i < a.size()) ? a[i] : T(0);
        pb[i] = (i < b.size()) ? b[i] : T(0);
    }
    crd::containers::Array<Complex<T>> fa(alloc), fb(alloc);
    fa.resize(half);
    fb.resize(half);
    fft::RealFftPlan<T> plan(alloc, nfft);
    plan.rfft(crd::containers::ConstSpan<T>(pa.data(), nfft), crd::containers::Span<Complex<T>>(fa.data(), half));
    plan.rfft(crd::containers::ConstSpan<T>(pb.data(), nfft), crd::containers::Span<Complex<T>>(fb.data(), half));
    for (crd::usize i = 0; i < half; ++i) // pointwise product of the half-spectra
    {
        const T re = fa[i].re * fb[i].re - fa[i].im * fb[i].im;
        const T im = fa[i].re * fb[i].im + fa[i].im * fb[i].re;
        fa[i] = Complex<T>{re, im};
    }
    plan.irfft(crd::containers::ConstSpan<Complex<T>>(fa.data(), half),
               crd::containers::Span<T>(real_out.data(), nfft)); // normalized
    out.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        out[i] = real_out[i];
    }
    return out;
}

// FftConvolver — a REUSABLE FFT-convolution engine: builds the FFT plan + scratch ONCE, then convolves many
// signals of the same padded size with NO per-call plan rebuild. This is the realistic hot path (repeated
// convolution / streaming filtering): the v10 deterministic-plan twiddle precompute (~28 ms at 2^21) is paid
// once, not every call — the single-shot `fftconvolve` above pays it each time (fair only for a cold call).
// Construct with the two operand lengths; `convolve(a, b)` requires len(a)==na, len(b)==nb.
template <typename T> class FftConvolver
{
public:
    FftConvolver(crd::memory::IAllocator* alloc, crd::usize na, crd::usize nb)
        : m_alloc(alloc), m_na(na), m_nb(nb), m_n(na + nb - 1), m_pa(alloc), m_pb(alloc), m_ro(alloc), m_fa(alloc),
          m_fb(alloc), m_plan(nullptr)
    {
        m_nfft = 4;
        while (m_nfft < m_n)
        {
            m_nfft <<= 1;
        }
        m_half = m_nfft / 2 + 1;
        m_pa.resize(m_nfft);
        m_pb.resize(m_nfft);
        m_ro.resize(m_nfft);
        m_fa.resize(m_half);
        m_fb.resize(m_half);
        m_plan = static_cast<fft::RealFftPlan<T>*>(
            m_alloc->allocate(sizeof(fft::RealFftPlan<T>), alignof(fft::RealFftPlan<T>)));
        ::new (static_cast<void*>(m_plan)) fft::RealFftPlan<T>(m_alloc, m_nfft); // twiddle precompute ONCE
    }
    ~FftConvolver()
    {
        if (m_plan != nullptr)
        {
            m_plan->~RealFftPlan();
            m_alloc->deallocate(m_plan);
        }
    }
    FftConvolver(const FftConvolver&) = delete;
    FftConvolver& operator=(const FftConvolver&) = delete;

    [[nodiscard]] crd::containers::Array<T> convolve(crd::containers::ConstSpan<T> a, crd::containers::ConstSpan<T> b)
    {
        CRD_ASSERT(a.size() == m_na && b.size() == m_nb);
        for (crd::usize i = 0; i < m_nfft; ++i)
        {
            m_pa[i] = (i < m_na) ? a[i] : T(0);
            m_pb[i] = (i < m_nb) ? b[i] : T(0);
        }
        m_plan->rfft(crd::containers::ConstSpan<T>(m_pa.data(), m_nfft),
                     crd::containers::Span<Complex<T>>(m_fa.data(), m_half));
        m_plan->rfft(crd::containers::ConstSpan<T>(m_pb.data(), m_nfft),
                     crd::containers::Span<Complex<T>>(m_fb.data(), m_half));
        for (crd::usize i = 0; i < m_half; ++i)
        {
            const T re = m_fa[i].re * m_fb[i].re - m_fa[i].im * m_fb[i].im;
            const T im = m_fa[i].re * m_fb[i].im + m_fa[i].im * m_fb[i].re;
            m_fa[i] = Complex<T>{re, im};
        }
        m_plan->irfft(crd::containers::ConstSpan<Complex<T>>(m_fa.data(), m_half),
                      crd::containers::Span<T>(m_ro.data(), m_nfft));
        crd::containers::Array<T> out(m_alloc);
        out.resize(m_n);
        for (crd::usize i = 0; i < m_n; ++i)
        {
            out[i] = m_ro[i];
        }
        return out;
    }

private:
    crd::memory::IAllocator* m_alloc;
    crd::usize m_na, m_nb, m_n, m_nfft = 0, m_half = 0;
    crd::containers::Array<T> m_pa, m_pb, m_ro;
    crd::containers::Array<Complex<T>> m_fa, m_fb;
    fft::RealFftPlan<T>* m_plan;
};

// Overlap-add FFT convolution (scipy.signal.oaconvolve): blocks the longer signal, FFT-convolves each block with
// the (shorter) `b`, and overlap-adds. Same result as fftconvolve/direct; cache-friendly for long×short. mode='full'.
template <typename T>
[[nodiscard]] crd::containers::Array<T> oaconvolve(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> a,
                                                   crd::containers::ConstSpan<T> b)
{
    crd::containers::Array<T> out(alloc);
    if (a.size() == 0 || b.size() == 0)
    {
        return out;
    }
    const crd::usize m = b.size();
    const crd::usize n = a.size() + m - 1;
    out.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        out[i] = T(0);
    }
    // block size L ≈ 4× the filter length (rounded so the per-block FFT stays small); each block convolved w/ b.
    crd::usize L = 1;
    while (L < 4 * m)
    {
        L <<= 1;
    }
    FftConvolver<T> conv(alloc, L, m);
    crd::containers::Array<T> blk(alloc);
    blk.resize(L);
    for (crd::usize off = 0; off < a.size(); off += L)
    {
        const crd::usize len = (off + L <= a.size()) ? L : (a.size() - off);
        for (crd::usize i = 0; i < L; ++i)
        {
            blk[i] = (i < len) ? a[off + i] : T(0); // zero-pad the last partial block to L
        }
        const auto bc = conv.convolve(crd::containers::ConstSpan<T>(blk.data(), L), b); // length L+m-1
        for (crd::usize i = 0; i < bc.size(); ++i)
        {
            if (off + i < n)
            {
                out[off + i] += bc[i];
            }
        }
    }
    return out;
}

// Polynomial deconvolution (scipy.signal.deconvolve): signal = convolve(divisor, quotient) + remainder. Long
// division. Returns the quotient (length n-m+1); `remainder` (length n) receives the residual.
template <typename T>
[[nodiscard]] crd::containers::Array<T> deconvolve(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> signal,
                                                   crd::containers::ConstSpan<T> divisor,
                                                   crd::containers::Array<T>& remainder)
{
    const crd::usize n = signal.size();
    const crd::usize m = divisor.size();
    crd::containers::Array<T> quot(alloc);
    remainder.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        remainder[i] = signal[i];
    }
    if (m == 0 || m > n)
    {
        return quot; // empty quotient; remainder == signal
    }
    const crd::usize qlen = n - m + 1;
    quot.resize(qlen);
    for (crd::usize i = 0; i < qlen; ++i)
    {
        const T q = remainder[i] / divisor[0];
        quot[i] = q;
        for (crd::usize j = 0; j < m; ++j)
        {
            remainder[i + j] -= q * divisor[j];
        }
    }
    return quot;
}

// cross-correlation (scipy.signal.correlate, mode='full'): correlate(a,b)[k] = Σ a[n] b[n - k + m - 1].
// Equivalent to convolve(a, reverse(b)). Length n+m-1.
template <typename T>
[[nodiscard]] crd::containers::Array<T> correlate(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> a,
                                                  crd::containers::ConstSpan<T> b)
{
    crd::containers::Array<T> br(alloc);
    br.resize(b.size());
    for (crd::usize i = 0; i < b.size(); ++i)
    {
        br[i] = b[b.size() - 1 - i];
    }
    return convolve<T>(alloc, a, crd::containers::ConstSpan<T>(br.data(), br.size()));
}

// Matched filter: the optimal detector for a known `templ` in noise — correlate the signal with the template
// (the output peak marks the best alignment / detection lag). = cross-correlation.
template <typename T>
[[nodiscard]] crd::containers::Array<T> matched_filter(crd::memory::IAllocator* alloc,
                                                       crd::containers::ConstSpan<T> signal,
                                                       crd::containers::ConstSpan<T> templ)
{
    return correlate<T>(alloc, signal, templ);
}

} // namespace crd::hesap::dsp
