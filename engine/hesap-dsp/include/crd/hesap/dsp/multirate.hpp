#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-k — multirate: polyphase resampling.
//
//   upfirdn        upsample by `up`, FIR-filter with h, downsample by `down`
//                  (scipy.signal.upfirdn, mode='constant' cval=0). Polyphase:
//                  each output touches ~len(h)/up taps (no zero-stuffing work).
//   resample_poly  rational resampling with a Kaiser-windowed anti-alias FIR
//                  (scipy.signal.resample_poly) — the DAW/SDR sample-rate hot path.
//   decimate       FIR zero-phase decimation by q (scipy.signal.decimate, ftype='fir').
//
// The two-layer LOWER kernel (the upfirdn polyphase inner loop is the hot path).
// HOT-PATH ⇒ benchmarked vs scipy + MATLAB + liquid-dsp. Gate: vs scipy ~1e-9
// + the run-twice determinism moat (single-thread, fixed tap order). f64.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dsp/fir.hpp>          // firwin
#include <crd/hesap/dsp/windows.hpp>      // kaiser / hamming
#include <crd/hesap/fft/bluestein.hpp>    // resample (any-size FFT)
#include <crd/memory/allocator.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>

namespace crd::hesap::dsp
{

namespace detail
{
// scipy _output_len: outputs of upfirdn = ceil(((n-1)*up + len_h)/down).
[[nodiscard]] inline crd::usize upfirdn_out_len(crd::usize len_h, crd::usize n_in, crd::usize up, crd::usize down) noexcept
{
    if (n_in == 0 || len_h == 0)
    {
        return 0;
    }
    return ((n_in - 1) * up + len_h - 1) / down + 1;
}
} // namespace detail

// upfirdn: upsample(up) -> FIR(h) -> downsample(down), zero-padded. Polyphase with a REVERSED per-phase tap bank:
// each output is a contiguous, branch-free dot product over the valid input range (no per-tap bounds checks).
template <typename T>
[[nodiscard]] crd::containers::Array<T> upfirdn(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> h,
                                                crd::containers::ConstSpan<T> x, crd::usize up, crd::usize down)
{
    crd::containers::Array<T> out(alloc);
    const crd::usize n = x.size(), lh = h.size();
    if (n == 0 || lh == 0)
    {
        return out;
    }
    const crd::usize out_len = detail::upfirdn_out_len(lh, n, up, down);
    out.resize(out_len);
    // Polyphase bank, each branch REVERSED so the dot product walks x ascending (cache + auto-vectorizable):
    // branch p has taps h[p], h[p+up], ...; reversed ⇒ rbank[p][j] = h[p + (blen[p]-1-j)*up].
    const crd::usize maxblen = (lh + up - 1) / up;
    crd::containers::Array<T> rbank(alloc);
    crd::containers::Array<crd::usize> blen(alloc);
    rbank.resize(up * maxblen);
    blen.resize(up);
    for (crd::usize p = 0; p < up; ++p)
    {
        crd::usize m = 0;
        for (crd::usize k = p; k < lh; k += up)
        {
            ++m;
        }
        blen[p] = m;
        for (crd::usize j = 0; j < m; ++j)
        {
            rbank[p * maxblen + j] = h[p + (m - 1 - j) * up]; // reversed
        }
    }
    for (crd::usize i = 0; i < out_len; ++i)
    {
        const crd::usize t = i * down;
        const crd::usize p = t % up;
        const long long base = static_cast<long long>(t / up); // x index for the FIRST (highest) tap of this branch
        const crd::usize bl = blen[p];
        const T* rb = &rbank[p * maxblen];
        // reversed: x index for rb[j] is start + j, start = base - (bl-1). valid j: start+j in [0, n-1], j in [0,bl-1].
        const long long start = base - static_cast<long long>(bl - 1);
        long long j0 = (start < 0) ? -start : 0;
        long long j1 = static_cast<long long>(bl) - 1;
        if (start + j1 > static_cast<long long>(n) - 1)
        {
            j1 = static_cast<long long>(n) - 1 - start;
        }
        T acc = T(0);
        const crd::usize xoff = static_cast<crd::usize>(start + j0);
        for (long long j = j0; j <= j1; ++j)
        {
            acc += rb[j] * x[xoff + static_cast<crd::usize>(j - j0)];
        }
        out[i] = acc;
    }
    return out;
}

namespace detail
{
// resample_poly core given an explicit (unpadded) FIR h and half_len (scipy's padding + trim arithmetic).
template <typename T>
[[nodiscard]] crd::containers::Array<T> resample_poly_h(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                                        crd::usize up, crd::usize down, crd::containers::ConstSpan<T> h,
                                                        crd::usize half_len)
{
    crd::containers::Array<T> out(alloc);
    const crd::usize n_in = x.size();
    crd::usize n_out = n_in * up;
    n_out = n_out / down + ((n_out % down) ? 1 : 0);
    const crd::usize lh = h.size();
    const crd::usize n_pre_pad = down - (half_len % down);
    const crd::usize n_pre_remove = (half_len + n_pre_pad) / down;
    crd::usize n_post_pad = 0;
    while (detail::upfirdn_out_len(lh + n_pre_pad + n_post_pad, n_in, up, down) < n_out + n_pre_remove)
    {
        ++n_post_pad;
    }
    crd::containers::Array<T> hp(alloc);
    hp.resize(n_pre_pad + lh + n_post_pad);
    for (crd::usize i = 0; i < hp.size(); ++i)
    {
        hp[i] = T(0);
    }
    for (crd::usize i = 0; i < lh; ++i)
    {
        hp[n_pre_pad + i] = h[i];
    }
    const auto y = upfirdn<T>(alloc, crd::containers::ConstSpan<T>(hp.data(), hp.size()), x, up, down);
    out.resize(n_out);
    for (crd::usize i = 0; i < n_out; ++i)
    {
        out[i] = (n_pre_remove + i < y.size()) ? y[n_pre_remove + i] : T(0);
    }
    return out;
}
} // namespace detail

// resample_poly: rational resample by up/down with a Kaiser(beta=5) anti-alias FIR (scipy default).
template <typename T>
[[nodiscard]] crd::containers::Array<T> resample_poly(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                                      crd::usize up, crd::usize down)
{
    const crd::usize g = std::gcd(up, down);
    up /= g;
    down /= g;
    crd::containers::Array<T> out(alloc);
    if (up == 1 && down == 1)
    {
        out.resize(x.size());
        for (crd::usize i = 0; i < x.size(); ++i)
        {
            out[i] = x[i];
        }
        return out;
    }
    const crd::usize max_rate = std::max(up, down);
    const T f_c = T(1) / static_cast<T>(max_rate);
    const crd::usize half_len = 10 * max_rate;
    const crd::usize numtaps = 2 * half_len + 1;
    const auto win = kaiser<T>(alloc, numtaps, 5.0, true);
    const T cutoff[1] = {f_c};
    auto h = firwin<T>(alloc, numtaps, crd::containers::ConstSpan<T>(cutoff, 1),
                       crd::containers::ConstSpan<T>(win.data(), numtaps), true);
    for (crd::usize i = 0; i < h.size(); ++i)
    {
        h[i] *= static_cast<T>(up);
    }
    return detail::resample_poly_h<T>(alloc, x, up, down, crd::containers::ConstSpan<T>(h.data(), h.size()), half_len);
}

// decimate (FIR, zero-phase): scipy.signal.decimate(x, q, ftype='fir'). Hamming-windowed FIR + resample_poly(1, q).
template <typename T>
[[nodiscard]] crd::containers::Array<T> decimate(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                                 crd::usize q)
{
    const crd::usize half_len = 10 * q;
    const crd::usize numtaps = 2 * half_len + 1;
    const auto win = hamming<T>(alloc, numtaps, true);
    const T cutoff[1] = {T(1) / static_cast<T>(q)};
    const auto h = firwin<T>(alloc, numtaps, crd::containers::ConstSpan<T>(cutoff, 1),
                             crd::containers::ConstSpan<T>(win.data(), numtaps), true);
    return detail::resample_poly_h<T>(alloc, x, 1, q, crd::containers::ConstSpan<T>(h.data(), h.size()), half_len);
}

// interp: integer upsample by q with an anti-imaging lowpass (= resample_poly(x, q, 1)).
template <typename T>
[[nodiscard]] crd::containers::Array<T> interp(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                               crd::usize q)
{
    return resample_poly<T>(alloc, x, q, 1);
}

// resample: FFT-based resample of x to `num` samples (scipy.signal.resample) — band-limited interpolation.
template <typename T>
[[nodiscard]] crd::containers::Array<T> resample(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                                 crd::usize num)
{
    const crd::usize n = x.size();
    crd::containers::Array<T> out(alloc);
    if (n == 0 || num == 0)
    {
        return out;
    }
    crd::containers::Array<Complex<T>> spec(alloc);
    spec.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        spec[i] = Complex<T>{x[i], T(0)};
    }
    fft::BluesteinPlan<T> fwd(alloc, n);
    fwd.execute(crd::containers::Span<Complex<T>>(spec.data(), n), fft::FftDirection::Forward);
    crd::containers::Array<Complex<T>> y(alloc);
    y.resize(num);
    for (crd::usize i = 0; i < num; ++i)
    {
        y[i] = Complex<T>{T(0), T(0)};
    }
    const crd::usize nmin = (n < num) ? n : num;
    const crd::usize half = nmin / 2;
    for (crd::usize i = 0; i <= half && i < num && i < n; ++i) // positive frequencies
    {
        y[i] = spec[i];
    }
    for (crd::usize i = 1; i <= half; ++i) // negative frequencies (mirror)
    {
        y[num - i] = spec[n - i];
    }
    if (nmin % 2 == 0 && half < num && half < n) // split the shared Nyquist bin
    {
        y[half] = Complex<T>{spec[half].re * T(0.5), spec[half].im * T(0.5)};
        y[num - half] = Complex<T>{spec[half].re * T(0.5), -spec[half].im * T(0.5)};
    }
    fft::BluesteinPlan<T> inv(alloc, num);
    inv.execute(crd::containers::Span<Complex<T>>(y.data(), num), fft::FftDirection::Inverse); // 1/num normalized
    const T scale = static_cast<T>(num) / static_cast<T>(n); // preserve amplitude
    out.resize(num);
    for (crd::usize i = 0; i < num; ++i)
    {
        out[i] = y[i].re * scale;
    }
    return out;
}

// half_band: a half-band lowpass FIR (cutoff fs/4) — every even-index tap is 0 except the centre (h[centre]=0.5).
// numtaps must be 4k+3 (length odd, (numtaps-1)/2 odd) for the half-band structure. Hamming-windowed sinc + zeroing.
template <typename T>
[[nodiscard]] crd::containers::Array<T> half_band(crd::memory::IAllocator* alloc, crd::usize numtaps)
{
    const T pi = static_cast<T>(std::numbers::pi_v<double>);
    crd::containers::Array<T> h(alloc);
    h.resize(numtaps);
    const long long c = static_cast<long long>(numtaps - 1) / 2;
    const auto win = hamming<T>(alloc, numtaps, true);
    for (crd::usize i = 0; i < numtaps; ++i)
    {
        const long long mm = static_cast<long long>(i) - c;
        T s;
        if (mm == 0)
        {
            s = T(0.5); // sinc(0)·0.5 cutoff
        }
        else
        {
            const T xm = static_cast<T>(mm);
            s = std::sin(pi * xm / T(2)) / (pi * xm); // ideal half-band impulse (cutoff 0.5 Nyquist)
        }
        h[i] = s * win[i];
    }
    for (crd::usize i = 0; i < numtaps; ++i) // enforce the half-band zeros: even offsets from centre vanish
    {
        if (((static_cast<long long>(i) - c) % 2) == 0 && (static_cast<long long>(i) - c) != 0)
        {
            h[i] = T(0);
        }
    }
    return h;
}

// CIC decimation filter: N integrator stages → decimate by R → N comb stages (differential delay M). DC gain (RM)^N.
template <typename T>
[[nodiscard]] crd::containers::Array<T> cic_decimate(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                                     crd::usize r, crd::usize stages, crd::usize m = 1)
{
    const crd::usize n = x.size();
    crd::containers::Array<T> acc(alloc);
    acc.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        acc[i] = x[i];
    }
    for (crd::usize s = 0; s < stages; ++s) // integrators: running sum
    {
        T run = T(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            run += acc[i];
            acc[i] = run;
        }
    }
    crd::containers::Array<T> dec(alloc); // decimate by R
    const crd::usize nd = n / r;
    dec.resize(nd);
    for (crd::usize i = 0; i < nd; ++i)
    {
        dec[i] = acc[i * r];
    }
    for (crd::usize s = 0; s < stages; ++s) // combs: y[i] - y[i-M]
    {
        for (crd::usize i = nd; i-- > 0;)
        {
            dec[i] = (i >= m) ? (dec[i] - dec[i - m]) : dec[i];
        }
    }
    return dec;
}

// Farrow fractional-delay interpolator (cubic Lagrange): y[n] ≈ x[n - mu], mu ∈ [0,1]. The Farrow structure
// (polynomial in mu) is the resampling-with-arbitrary-rate workhorse; here the fixed-delay form.
template <typename T>
[[nodiscard]] crd::containers::Array<T> farrow_delay(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                                     T mu)
{
    const crd::usize n = x.size();
    crd::containers::Array<T> out(alloc);
    out.resize(n);
    // cubic Lagrange weights for the sample at index n-mu, taps {n+1, n, n-1, n-2}.
    const T h_m1 = -mu * (mu - T(1)) * (mu - T(2)) / T(6);
    const T h_0 = (mu + T(1)) * (mu - T(1)) * (mu - T(2)) / T(2);
    const T h_1 = -(mu + T(1)) * mu * (mu - T(2)) / T(2);
    const T h_2 = (mu + T(1)) * mu * (mu - T(1)) / T(6);
    auto at = [&](long long i) -> T { return (i >= 0 && i < static_cast<long long>(n)) ? x[static_cast<crd::usize>(i)] : T(0); };
    for (crd::usize i = 0; i < n; ++i)
    {
        const long long k = static_cast<long long>(i);
        out[i] = h_m1 * at(k + 1) + h_0 * at(k) + h_1 * at(k - 1) + h_2 * at(k - 2);
    }
    return out;
}

} // namespace crd::hesap::dsp
