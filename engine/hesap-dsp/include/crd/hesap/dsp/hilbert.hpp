#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-l — Hilbert transform / analytic signal.
//
//   hilbert      analytic signal x_a = x + j·H{x} via the FFT (scipy.signal.hilbert):
//                X = FFT(x); zero the negative frequencies, double the positive;
//                inverse FFT. Re(x_a) == x; Im(x_a) == the Hilbert transform.
//   envelope     |x_a| — the instantaneous amplitude (AM demodulation).
//   instantaneous_phase  unwrap(arg(x_a)).
//   instantaneous_frequency  d/dt of the unwrapped phase /(2π)·fs.
//
// Over the crd-hesap-fft engine (BluesteinPlan ⇒ ANY length matches scipy, not
// just powers of two). Gate: real+imag vs scipy ~1e-10 + the analytic property
// (Re == x) + a chirp's instantaneous frequency recovering the linear sweep.
// f64 lower-layer raw scalars.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/fft/bluestein.hpp>
#include <crd/hesap/fft/fft.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <numbers>

namespace crd::hesap::dsp
{

// Analytic signal of a real input (scipy.signal.hilbert). Returns x + j·H{x}.
template <typename T>
[[nodiscard]] crd::containers::Array<Complex<T>> hilbert(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x)
{
    const crd::usize n = x.size();
    crd::containers::Array<Complex<T>> a(alloc);
    a.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        a[i] = Complex<T>{x[i], T(0)};
    }
    if (n == 0)
    {
        return a;
    }
    // multiply by the step h: keep DC (and Nyquist for even n), double positive freqs, zero negatives.
    auto apply_h = [&]()
    {
        for (crd::usize k = 0; k < n; ++k)
        {
            T hk;
            if (n % 2 == 0)
            {
                hk = (k == 0 || k == n / 2) ? T(1) : (k < n / 2 ? T(2) : T(0));
            }
            else
            {
                hk = (k == 0) ? T(1) : (k <= (n - 1) / 2 ? T(2) : T(0));
            }
            a[k].re *= hk;
            a[k].im *= hk;
        }
    };
    const crd::containers::Span<Complex<T>> span(a.data(), n);
    if ((n & (n - 1)) == 0) // power of two ⇒ the direct FftPlan (Bluestein pads to 2n ⇒ ~2x the work)
    {
        const fft::FftPlan<T> plan(alloc, n);
        plan.execute(span, fft::FftDirection::Forward);
        apply_h();
        plan.execute(span, fft::FftDirection::Inverse); // UNNORMALIZED ⇒ scale by 1/n below
        const T inv = T(1) / static_cast<T>(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            a[i].re *= inv;
            a[i].im *= inv;
        }
    }
    else // arbitrary length ⇒ Bluestein (its inverse is already 1/n normalized)
    {
        const fft::BluesteinPlan<T> plan(alloc, n);
        plan.execute(span, fft::FftDirection::Forward);
        apply_h();
        plan.execute(span, fft::FftDirection::Inverse);
    }
    return a;
}

// Plan-cached analytic-signal transformer for the streaming / repeated-block case (envelope detection on a fixed
// block size). Builds the FFT plan ONCE — the v11-j FftConvolver lesson: the one-shot hilbert() rebuilds the full
// twiddle table every call. Power-of-two block size only (the streaming common case); the free hilbert() handles
// any length. The cached plan ⇒ Cerid's FFT execute (beats PocketFFT) is the realistic hot-loop comparison.
template <typename T> class HilbertTransformer
{
public:
    HilbertTransformer(crd::memory::IAllocator* alloc, crd::usize n) : m_n(n), m_plan(alloc, n) {}

    // Analytic signal of x (length n) into out (length n). Reuses the cached plan.
    void transform(crd::containers::ConstSpan<T> x, crd::containers::Span<Complex<T>> out) const
    {
        const crd::usize n = m_n;
        for (crd::usize i = 0; i < n; ++i)
        {
            out[i] = Complex<T>{x[i], T(0)};
        }
        m_plan.execute(out, fft::FftDirection::Forward);
        for (crd::usize k = 0; k < n; ++k)
        {
            const T hk = (k == 0 || k == n / 2) ? T(1) : (k < n / 2 ? T(2) : T(0));
            out[k].re *= hk;
            out[k].im *= hk;
        }
        m_plan.execute(out, fft::FftDirection::Inverse);
        const T inv = T(1) / static_cast<T>(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            out[i].re *= inv;
            out[i].im *= inv;
        }
    }

private:
    crd::usize m_n;
    fft::FftPlan<T> m_plan;
};

// Envelope (instantaneous amplitude) = |analytic signal|.
template <typename T>
[[nodiscard]] crd::containers::Array<T> envelope(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x)
{
    const auto a = hilbert<T>(alloc, x);
    crd::containers::Array<T> e(alloc);
    e.resize(a.size());
    for (crd::usize i = 0; i < a.size(); ++i)
    {
        e[i] = std::hypot(a[i].re, a[i].im);
    }
    return e;
}

namespace detail
{
// numpy.unwrap (discont=pi): correct phase jumps > pi to their 2π complement, in place.
template <typename T> void unwrap(T* p, crd::usize n) noexcept
{
    const T two_pi = static_cast<T>(2.0 * std::numbers::pi_v<double>);
    const T pi = static_cast<T>(std::numbers::pi_v<double>);
    T cumulative = T(0);
    T prev_raw = (n > 0) ? p[0] : T(0);
    for (crd::usize k = 1; k < n; ++k)
    {
        const T raw = p[k];
        const T dd = raw - prev_raw;
        T ddmod = std::fmod(dd + pi, two_pi);
        if (ddmod < T(0))
        {
            ddmod += two_pi;
        }
        ddmod -= pi;
        if (ddmod == -pi && dd > T(0))
        {
            ddmod = pi;
        }
        T correct = ddmod - dd;
        if (std::abs(dd) < pi)
        {
            correct = T(0);
        }
        cumulative += correct;
        prev_raw = raw;
        p[k] = raw + cumulative;
    }
}
} // namespace detail

// Instantaneous phase = unwrap(arg(analytic signal)).
template <typename T>
[[nodiscard]] crd::containers::Array<T> instantaneous_phase(crd::memory::IAllocator* alloc,
                                                            crd::containers::ConstSpan<T> x)
{
    const auto a = hilbert<T>(alloc, x);
    crd::containers::Array<T> ph(alloc);
    ph.resize(a.size());
    for (crd::usize i = 0; i < a.size(); ++i)
    {
        ph[i] = std::atan2(a[i].im, a[i].re);
    }
    detail::unwrap<T>(ph.data(), ph.size());
    return ph;
}

// Instantaneous frequency = d(unwrapped phase)/dt /(2π) · fs. Length n-1.
template <typename T>
[[nodiscard]] crd::containers::Array<T> instantaneous_frequency(crd::memory::IAllocator* alloc,
                                                                crd::containers::ConstSpan<T> x, T fs)
{
    const auto ph = instantaneous_phase<T>(alloc, x);
    crd::containers::Array<T> f(alloc);
    if (ph.size() < 2)
    {
        return f;
    }
    const T two_pi = static_cast<T>(2.0 * std::numbers::pi_v<double>);
    f.resize(ph.size() - 1);
    for (crd::usize i = 0; i + 1 < ph.size(); ++i)
    {
        f[i] = (ph[i + 1] - ph[i]) / two_pi * fs;
    }
    return f;
}

namespace detail
{
// the hilbert2 "single-orthant" frequency step (scipy.signal.hilbert2): k0 = (L+1)/2; double 1..k0-1, ZERO k0..L-1.
// Differs from the 1D hilbert step in that the even-L Nyquist bin (L/2 ≥ k0) is zeroed, not kept.
template <typename T> [[nodiscard]] inline T hilbert2_step(crd::usize k, crd::usize l) noexcept
{
    const crd::usize k0 = (l + 1) / 2;
    return (k == 0) ? T(1) : (k < k0 ? T(2) : T(0));
}
} // namespace detail

// hilbert2: the 2D analytic signal of a real r×c image (row-major), scipy.signal.hilbert2. The 2D FFT is multiplied
// by the outer product of the two 1D Hilbert steps, then inverse-transformed (the (+,+) single-quadrant analytic
// signal — Re is NOT the input, unlike 1D). Power-of-two dimensions (FftPlan); transforms via a fresh per-line plan.
template <typename T>
[[nodiscard]] crd::containers::Array<Complex<T>> hilbert2(crd::memory::IAllocator* alloc,
                                                          crd::containers::ConstSpan<T> x, crd::usize r, crd::usize c)
{
    crd::containers::Array<Complex<T>> xf(alloc);
    xf.resize(r * c);
    for (crd::usize i = 0; i < r * c; ++i)
    {
        xf[i] = Complex<T>{x[i], T(0)};
    }
    if (r == 0 || c == 0)
    {
        return xf;
    }
    const bool rpow2 = (r & (r - 1)) == 0, cpow2 = (c & (c - 1)) == 0;
    crd::containers::Array<Complex<T>> buf(alloc);
    buf.resize((r > c) ? r : c);
    // transform a single line of length `len` (one fresh plan per call — robust); inverse is renormalized to 1/len.
    auto line = [&](crd::usize len, bool p2, fft::FftDirection dir)
    {
        if (p2)
        {
            const fft::FftPlan<T> plan(alloc, len);
            plan.execute(crd::containers::Span<Complex<T>>(buf.data(), len), dir);
            if (dir == fft::FftDirection::Inverse)
            {
                const T inv = T(1) / static_cast<T>(len);
                for (crd::usize k = 0; k < len; ++k)
                {
                    buf[k].re *= inv;
                    buf[k].im *= inv;
                }
            }
        }
        else
        {
            const fft::BluesteinPlan<T> plan(alloc, len); // inverse already 1/len normalized
            plan.execute(crd::containers::Span<Complex<T>>(buf.data(), len), dir);
        }
    };
    auto row_xform = [&](fft::FftDirection dir)
    {
        for (crd::usize row = 0; row < r; ++row)
        {
            for (crd::usize j = 0; j < c; ++j)
            {
                buf[j] = xf[row * c + j];
            }
            line(c, cpow2, dir);
            for (crd::usize j = 0; j < c; ++j)
            {
                xf[row * c + j] = buf[j];
            }
        }
    };
    auto col_xform = [&](fft::FftDirection dir)
    {
        for (crd::usize col = 0; col < c; ++col)
        {
            for (crd::usize i = 0; i < r; ++i)
            {
                buf[i] = xf[i * c + col];
            }
            line(r, rpow2, dir);
            for (crd::usize i = 0; i < r; ++i)
            {
                xf[i * c + col] = buf[i];
            }
        }
    };
    row_xform(fft::FftDirection::Forward);
    col_xform(fft::FftDirection::Forward);
    for (crd::usize i = 0; i < r; ++i) // multiply by the outer-product step h1[i]·h2[j]
    {
        const T h1 = detail::hilbert2_step<T>(i, r);
        for (crd::usize j = 0; j < c; ++j)
        {
            const T h = h1 * detail::hilbert2_step<T>(j, c);
            xf[i * c + j].re *= h;
            xf[i * c + j].im *= h;
        }
    }
    col_xform(fft::FftDirection::Inverse);
    row_xform(fft::FftDirection::Inverse);
    return xf;
}

} // namespace crd::hesap::dsp
