#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-m — non-parametric spectral estimation (Welch PSD).
//
// Welch's method: split the signal into overlapping windowed segments, take the
// periodogram of each, and AVERAGE. The per-segment FFTs are INDEPENDENT — the
// natural place for a MULTI-THREADED FFT (the advisor's clean target, vs the
// delicate single long×long four-step). Each job owns its own RealFftPlan
// (built serially up front — no thread-safe alloc needed), executes in parallel,
// and the FINAL AVERAGE is a SERIAL fixed-order reduction ⇒ the PSD is
// BIT-IDENTICAL across {1..N} thread counts (the determinism moat — an edge MKL
// /FFTW don't give). Faithful scipy.signal.welch (hann, density, onesided,
// detrend='constant'). Lower-layer raw scalars.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dsp/windows.hpp>
#include <crd/hesap/fft/real_fft.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocator.hpp>

#include <new>

namespace crd::hesap::dsp
{

// The shared MULTI-THREADED parallel-batched-FFT core: computes the per-segment one-sided density periodograms
// (nseg × nfreq, row-major) of `x`. Welch averages these; the spectrogram returns them. Each job owns a
// RealFftPlan (built serially up-front) and writes its own segment rows ⇒ the matrix is BIT-IDENTICAL across
// thread counts. Returns the pgram Array; sets nseg/nfreq/hop. PERIODIC hann (scipy fftbins), detrend='constant'.
template <typename T>
[[nodiscard]] crd::containers::Array<T> segment_periodograms(crd::memory::IAllocator* alloc,
                                                             crd::containers::ConstSpan<T> x, T fs, crd::usize nperseg,
                                                             crd::usize noverlap, crd::usize& nseg, crd::usize& nfreq)
{
    const crd::usize hop = nperseg - noverlap;
    nfreq = nperseg / 2 + 1;
    nseg = (x.size() < nperseg) ? 0 : (x.size() - noverlap) / hop;
    crd::containers::Array<T> pgram(alloc);
    if (nseg == 0)
    {
        return pgram;
    }
    const auto win = hann<T>(alloc, nperseg, /*sym=*/false);
    T winsum2 = T(0);
    for (crd::usize i = 0; i < nperseg; ++i)
    {
        winsum2 += win[i] * win[i];
    }
    const T scale = T(1) / (fs * winsum2);
    pgram.resize(nseg * nfreq);

    const crd::u32 nw = crd::jobs::num_workers();
    const crd::u32 njobs = (nw < static_cast<crd::u32>(nseg)) ? nw : static_cast<crd::u32>(nseg);

    // pre-build njobs plans + per-job scratch SERIALLY (no allocation inside the parallel region).
    crd::containers::Array<fft::RealFftPlan<T>*> plans(alloc);
    crd::containers::Array<T> segbuf(alloc);            // njobs × nperseg
    crd::containers::Array<Complex<T>> specbuf(alloc);  // njobs × nfreq
    const crd::u32 npl = (njobs < 1U) ? 1U : njobs;
    plans.resize(npl);
    segbuf.resize(static_cast<crd::usize>(npl) * nperseg);
    specbuf.resize(static_cast<crd::usize>(npl) * nfreq);
    for (crd::u32 j = 0; j < npl; ++j)
    {
        auto* p = static_cast<fft::RealFftPlan<T>*>(
            alloc->allocate(sizeof(fft::RealFftPlan<T>), alignof(fft::RealFftPlan<T>)));
        ::new (static_cast<void*>(p)) fft::RealFftPlan<T>(alloc, nperseg);
        plans[j] = p;
    }

    // one segment's periodogram: detrend (subtract mean) + window → rfft → scaled |·|² (one-sided doubling).
    auto do_segment = [&](crd::u32 jobidx, crd::usize seg)
    {
        T* sb = segbuf.data() + static_cast<crd::usize>(jobidx) * nperseg;
        Complex<T>* sp = specbuf.data() + static_cast<crd::usize>(jobidx) * nfreq;
        const crd::usize start = seg * hop;
        T mean = T(0);
        for (crd::usize i = 0; i < nperseg; ++i)
        {
            mean += x[start + i];
        }
        mean /= static_cast<T>(nperseg);
        for (crd::usize i = 0; i < nperseg; ++i)
        {
            sb[i] = (x[start + i] - mean) * win[i]; // detrend='constant' + window
        }
        plans[jobidx]->rfft(crd::containers::ConstSpan<T>(sb, nperseg), crd::containers::Span<Complex<T>>(sp, nfreq));
        T* row = pgram.data() + seg * nfreq;
        for (crd::usize k = 0; k < nfreq; ++k)
        {
            T p = scale * (sp[k].re * sp[k].re + sp[k].im * sp[k].im);
            if (k != 0 && k != nfreq - 1) // one-sided: double all but DC + Nyquist
            {
                p *= T(2);
            }
            row[k] = p;
        }
    };

    if (nw <= 1 || nseg < 4)
    {
        for (crd::usize s = 0; s < nseg; ++s)
        {
            do_segment(0, s);
        }
    }
    else
    {
        crd::jobs::Counter* c = crd::jobs::parallel_for(
            njobs, njobs,
            [&](crd::u32 jb, crd::u32 je)
            {
                for (crd::u32 job = jb; job < je; ++job)
                {
                    const crd::usize s0 = static_cast<crd::usize>(job) * nseg / njobs;
                    const crd::usize s1 = static_cast<crd::usize>(job + 1) * nseg / njobs;
                    for (crd::usize s = s0; s < s1; ++s)
                    {
                        do_segment(job, s);
                    }
                }
            });
        crd::jobs::wait(c);
    }

    for (crd::u32 j = 0; j < npl; ++j)
    {
        plans[j]->~RealFftPlan();
        alloc->deallocate(plans[j]);
    }
    return pgram;
}

// Welch power spectral density (scipy.signal.welch): average the per-segment periodograms. `noverlap` default
// nperseg/2. The SERIAL fixed-order average keeps the PSD bit-identical across thread counts (the moat).
template <typename T>
[[nodiscard]] crd::containers::Array<T> welch_psd(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                                  T fs, crd::usize nperseg, crd::usize noverlap = 0)
{
    if (noverlap == 0)
    {
        noverlap = nperseg / 2;
    }
    crd::usize nseg = 0;
    crd::usize nfreq = 0;
    const auto pgram = segment_periodograms<T>(alloc, x, fs, nperseg, noverlap, nseg, nfreq);
    crd::containers::Array<T> psd(alloc);
    psd.resize(nperseg / 2 + 1);
    for (crd::usize k = 0; k < psd.size(); ++k)
    {
        psd[k] = T(0);
    }
    if (nseg == 0)
    {
        return psd;
    }
    for (crd::usize s = 0; s < nseg; ++s) // serial fixed-order average ⇒ bit-identical across thread counts
    {
        const T* row = pgram.data() + s * nfreq;
        for (crd::usize k = 0; k < nfreq; ++k)
        {
            psd[k] += row[k];
        }
    }
    for (crd::usize k = 0; k < nfreq; ++k)
    {
        psd[k] /= static_cast<T>(nseg);
    }
    return psd;
}

// Spectrogram (scipy.signal.spectrogram, mode='psd'): the per-segment periodograms WITHOUT averaging — a
// time-frequency map. Returns the [nseg × nfreq] matrix (row-major: row s = frame s). `out_nseg`/`out_nfreq`
// receive the dimensions. Same parallel-batched-FFT core as Welch ⇒ bit-identical across thread counts.
template <typename T>
[[nodiscard]] crd::containers::Array<T> spectrogram(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                                    T fs, crd::usize nperseg, crd::usize noverlap, crd::usize& out_nseg,
                                                    crd::usize& out_nfreq)
{
    return segment_periodograms<T>(alloc, x, fs, nperseg, noverlap, out_nseg, out_nfreq);
}

// Periodogram (scipy.signal.periodogram): single-segment PSD, BOXCAR (rectangular) window, density, one-sided.
// = |rfft(x)|² / (fs·N), doubled except DC+Nyquist.
template <typename T>
[[nodiscard]] crd::containers::Array<T> periodogram(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                                    T fs)
{
    const crd::usize n = x.size();
    const crd::usize nfreq = n / 2 + 1;
    crd::containers::Array<T> psd(alloc);
    psd.resize(nfreq);
    const T scale = T(1) / (fs * static_cast<T>(n));
    fft::RealFftPlan<T> plan(alloc, n);
    crd::containers::Array<T> xc(alloc);
    crd::containers::Array<Complex<T>> spec(alloc);
    xc.resize(n);
    spec.resize(nfreq);
    T mean = T(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        mean += x[i];
    }
    mean /= static_cast<T>(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        xc[i] = x[i] - mean; // detrend='constant' (scipy default)
    }
    plan.rfft(crd::containers::ConstSpan<T>(xc.data(), n), crd::containers::Span<Complex<T>>(spec.data(), nfreq));
    for (crd::usize k = 0; k < nfreq; ++k)
    {
        T p = scale * (spec[k].re * spec[k].re + spec[k].im * spec[k].im);
        if (k != 0 && k != nfreq - 1)
        {
            p *= T(2);
        }
        psd[k] = p;
    }
    return psd;
}

// Cross-spectral density (scipy.signal.csd): like Welch but X·conj(Y), the COMPLEX cross-spectrum averaged over
// segments. hann/density/onesided/detrend-constant. coherence = |Pxy|²/(Pxx·Pyy). Serial (correctness-gated).
template <typename T>
[[nodiscard]] crd::containers::Array<Complex<T>> csd(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                                     crd::containers::ConstSpan<T> y, T fs, crd::usize nperseg,
                                                     crd::usize noverlap = 0)
{
    if (noverlap == 0)
    {
        noverlap = nperseg / 2;
    }
    const crd::usize hop = nperseg - noverlap;
    const crd::usize nfreq = nperseg / 2 + 1;
    const crd::usize nseg = (x.size() < nperseg) ? 0 : (x.size() - noverlap) / hop;
    crd::containers::Array<Complex<T>> pxy(alloc);
    pxy.resize(nfreq);
    for (crd::usize k = 0; k < nfreq; ++k)
    {
        pxy[k] = Complex<T>{T(0), T(0)};
    }
    if (nseg == 0)
    {
        return pxy;
    }
    const auto win = hann<T>(alloc, nperseg, /*sym=*/false);
    T winsum2 = T(0);
    for (crd::usize i = 0; i < nperseg; ++i)
    {
        winsum2 += win[i] * win[i];
    }
    const T scale = T(1) / (fs * winsum2);
    fft::RealFftPlan<T> plan(alloc, nperseg);
    crd::containers::Array<T> sx(alloc), sy(alloc);
    crd::containers::Array<Complex<T>> fx(alloc), fy(alloc);
    sx.resize(nperseg);
    sy.resize(nperseg);
    fx.resize(nfreq);
    fy.resize(nfreq);
    for (crd::usize s = 0; s < nseg; ++s)
    {
        const crd::usize start = s * hop;
        T mx = T(0), my = T(0);
        for (crd::usize i = 0; i < nperseg; ++i)
        {
            mx += x[start + i];
            my += y[start + i];
        }
        mx /= static_cast<T>(nperseg);
        my /= static_cast<T>(nperseg);
        for (crd::usize i = 0; i < nperseg; ++i)
        {
            sx[i] = (x[start + i] - mx) * win[i];
            sy[i] = (y[start + i] - my) * win[i];
        }
        plan.rfft(crd::containers::ConstSpan<T>(sx.data(), nperseg), crd::containers::Span<Complex<T>>(fx.data(), nfreq));
        plan.rfft(crd::containers::ConstSpan<T>(sy.data(), nperseg), crd::containers::Span<Complex<T>>(fy.data(), nfreq));
        for (crd::usize k = 0; k < nfreq; ++k)
        {
            // conj(X) · Y · scale (scipy.signal.csd convention), one-sided doubled.
            T re = (fx[k].re * fy[k].re + fx[k].im * fy[k].im) * scale;
            T im = (fx[k].re * fy[k].im - fx[k].im * fy[k].re) * scale;
            if (k != 0 && k != nfreq - 1)
            {
                re *= T(2);
                im *= T(2);
            }
            pxy[k].re += re;
            pxy[k].im += im;
        }
    }
    for (crd::usize k = 0; k < nfreq; ++k)
    {
        pxy[k].re /= static_cast<T>(nseg);
        pxy[k].im /= static_cast<T>(nseg);
    }
    return pxy;
}

// Magnitude-squared coherence (scipy.signal.coherence): Cxy = |Pxy|² / (Pxx · Pyy), in [0,1].
template <typename T>
[[nodiscard]] crd::containers::Array<T> coherence(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                                  crd::containers::ConstSpan<T> y, T fs, crd::usize nperseg,
                                                  crd::usize noverlap = 0)
{
    const auto pxy = csd<T>(alloc, x, y, fs, nperseg, noverlap);
    const auto pxx = welch_psd<T>(alloc, x, fs, nperseg, noverlap);
    const auto pyy = welch_psd<T>(alloc, y, fs, nperseg, noverlap);
    crd::containers::Array<T> cxy(alloc);
    cxy.resize(pxy.size());
    for (crd::usize k = 0; k < pxy.size(); ++k)
    {
        const T num = pxy[k].re * pxy[k].re + pxy[k].im * pxy[k].im;
        const T den = pxx[k] * pyy[k];
        cxy[k] = (den > static_cast<T>(1e-300)) ? (num / den) : T(0);
    }
    return cxy;
}

// Short-Time Fourier Transform: the COMPLEX spectra of overlapping windowed (hann) segments — frame s =
// rfft(x[s·hop : s·hop+nperseg] · win), kept complex (no scaling, no detrend). Returns the [nframes × nfreq]
// row-major complex matrix; sets nframes/nfreq. Multi-threaded (per-job plans) ⇒ bit-identical across threads.
// Pairs with istft for perfect reconstruction (COLA hann + 50% overlap).
template <typename T>
[[nodiscard]] crd::containers::Array<Complex<T>> stft(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                                      crd::usize nperseg, crd::usize noverlap, crd::usize& nframes,
                                                      crd::usize& nfreq)
{
    const crd::usize hop = nperseg - noverlap;
    nfreq = nperseg / 2 + 1;
    nframes = (x.size() < nperseg) ? 0 : (x.size() - noverlap) / hop;
    crd::containers::Array<Complex<T>> out(alloc);
    if (nframes == 0)
    {
        return out;
    }
    const auto win = hann<T>(alloc, nperseg, /*sym=*/false);
    out.resize(nframes * nfreq);

    const crd::u32 nw = crd::jobs::num_workers();
    const crd::u32 njobs = (nw < static_cast<crd::u32>(nframes)) ? nw : static_cast<crd::u32>(nframes);
    const crd::u32 npl = (njobs < 1U) ? 1U : njobs;
    crd::containers::Array<fft::RealFftPlan<T>*> plans(alloc);
    crd::containers::Array<T> segbuf(alloc);
    plans.resize(npl);
    segbuf.resize(static_cast<crd::usize>(npl) * nperseg);
    for (crd::u32 j = 0; j < npl; ++j)
    {
        auto* p = static_cast<fft::RealFftPlan<T>*>(
            alloc->allocate(sizeof(fft::RealFftPlan<T>), alignof(fft::RealFftPlan<T>)));
        ::new (static_cast<void*>(p)) fft::RealFftPlan<T>(alloc, nperseg);
        plans[j] = p;
    }
    auto do_frame = [&](crd::u32 jobidx, crd::usize frame)
    {
        T* sb = segbuf.data() + static_cast<crd::usize>(jobidx) * nperseg;
        const crd::usize start = frame * hop;
        for (crd::usize i = 0; i < nperseg; ++i)
        {
            sb[i] = x[start + i] * win[i];
        }
        plans[jobidx]->rfft(crd::containers::ConstSpan<T>(sb, nperseg),
                            crd::containers::Span<Complex<T>>(out.data() + frame * nfreq, nfreq));
    };
    if (nw <= 1 || nframes < 4)
    {
        for (crd::usize f = 0; f < nframes; ++f)
        {
            do_frame(0, f);
        }
    }
    else
    {
        crd::jobs::Counter* c = crd::jobs::parallel_for(
            njobs, njobs,
            [&](crd::u32 jb, crd::u32 je)
            {
                for (crd::u32 job = jb; job < je; ++job)
                {
                    const crd::usize f0 = static_cast<crd::usize>(job) * nframes / njobs;
                    const crd::usize f1 = static_cast<crd::usize>(job + 1) * nframes / njobs;
                    for (crd::usize f = f0; f < f1; ++f)
                    {
                        do_frame(job, f);
                    }
                }
            });
        crd::jobs::wait(c);
    }
    for (crd::u32 j = 0; j < npl; ++j)
    {
        plans[j]->~RealFftPlan();
        alloc->deallocate(plans[j]);
    }
    return out;
}

// Inverse STFT: overlap-add reconstruction with COLA normalization. irfft each frame → synthesis-windowed (hann)
// → overlap-add, divided by the overlapped Σ(win²). With hann + 50% overlap this reconstructs x exactly (interior),
// the perfect-reconstruction property. Returns the signal of length (nframes-1)·hop + nperseg. Serial (deterministic).
template <typename T>
[[nodiscard]] crd::containers::Array<T> istft(crd::memory::IAllocator* alloc,
                                              crd::containers::ConstSpan<Complex<T>> spectra, crd::usize nframes,
                                              crd::usize nfreq, crd::usize nperseg, crd::usize noverlap)
{
    const crd::usize hop = nperseg - noverlap;
    const crd::usize n = (nframes == 0) ? 0 : (nframes - 1) * hop + nperseg;
    crd::containers::Array<T> out(alloc);
    crd::containers::Array<T> wsum(alloc);
    out.resize(n);
    wsum.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        out[i] = T(0);
        wsum[i] = T(0);
    }
    if (nframes == 0)
    {
        return out;
    }
    const auto win = hann<T>(alloc, nperseg, /*sym=*/false);
    fft::RealFftPlan<T> plan(alloc, nperseg);
    crd::containers::Array<T> seg(alloc);
    crd::containers::Array<Complex<T>> spec(alloc);
    seg.resize(nperseg);
    spec.resize(nfreq);
    for (crd::usize f = 0; f < nframes; ++f)
    {
        for (crd::usize k = 0; k < nfreq; ++k)
        {
            spec[k] = spectra[f * nfreq + k];
        }
        plan.irfft(crd::containers::ConstSpan<Complex<T>>(spec.data(), nfreq),
                   crd::containers::Span<T>(seg.data(), nperseg));
        const crd::usize start = f * hop;
        for (crd::usize i = 0; i < nperseg; ++i)
        {
            out[start + i] += seg[i] * win[i];   // synthesis window
            wsum[start + i] += win[i] * win[i];  // COLA normalization denominator
        }
    }
    for (crd::usize i = 0; i < n; ++i)
    {
        if (wsum[i] > static_cast<T>(1e-12))
        {
            out[i] /= wsum[i];
        }
    }
    return out;
}

} // namespace crd::hesap::dsp
