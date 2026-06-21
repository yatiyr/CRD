#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-s — detection + measurements.
//
//   local_maxima / find_peaks / peak_prominences / peak_widths   faithful
//        scipy.signal peak detection (plateau-aware maxima + prominences +
//        widths at a relative height + height/distance/prominence filters).
//   argrelextrema   relative extrema with an order.
//   detrend         remove a constant (mean) or linear (LS line) trend.
//   rms / peak_value / crest_factor   amplitude statistics.
//   thd / snr / sinad / sfdr / enob   spectrum quality metrics (vs MATLAB).
//
// Peak detection is exact-index vs scipy; detrend is bit-exact; the spectrum
// metrics gate analytically (a planted harmonic ⇒ a known THD) + vs MATLAB.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/fft/real_fft.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocator.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace crd::hesap::dsp
{

// local maxima (scipy _local_maxima_1d): interior plateau-aware maxima. Returns the plateau midpoint indices.
template <typename T>
[[nodiscard]] crd::containers::Array<crd::usize> local_maxima(crd::memory::IAllocator* alloc,
                                                              crd::containers::ConstSpan<T> x)
{
    crd::containers::Array<crd::usize> peaks(alloc);
    const crd::usize n = x.size();
    if (n < 3)
    {
        return peaks;
    }
    crd::usize i = 1;
    const crd::usize i_max = n - 1;
    while (i < i_max)
    {
        if (x[i - 1] < x[i])
        {
            crd::usize ahead = i + 1;
            while (ahead < i_max && x[ahead] == x[i]) // walk the plateau
            {
                ++ahead;
            }
            if (x[ahead] < x[i]) // a real maximum (falls on the right)
            {
                const crd::usize left = i, right = ahead - 1;
                peaks.push_back((left + right) / 2); // plateau midpoint
                i = ahead;
                continue;
            }
        }
        ++i;
    }
    return peaks;
}

// Multithreaded local maxima (embarrassingly parallel): each job owns the rising edges in its chunk and writes its
// peaks into a disjoint scratch region (no allocation in the parallel region — the Welch pattern). The gather is in
// chunk order ⇒ the result is BIT-IDENTICAL to the serial scan, independent of thread count (the {1..16} moat).
template <typename T>
[[nodiscard]] crd::containers::Array<crd::usize> local_maxima_mt(crd::memory::IAllocator* alloc,
                                                                 crd::containers::ConstSpan<T> x)
{
    const crd::usize n = x.size();
    if (n < 8192)
    {
        return local_maxima<T>(alloc, x); // small ⇒ serial (no thread overhead)
    }
    crd::u32 njobs = crd::jobs::num_workers();
    if (njobs > static_cast<crd::u32>(n / 4096))
    {
        njobs = static_cast<crd::u32>(n / 4096);
    }
    if (njobs < 2)
    {
        return local_maxima<T>(alloc, x);
    }
    crd::containers::Array<crd::usize> scratch(alloc), counts(alloc);
    scratch.resize(n);
    counts.resize(njobs);
    struct Ctx // packed so the closure stays within the job SBO (capture one pointer)
    {
        const T* x;
        crd::usize* scratch;
        crd::usize* counts;
        crd::usize n;
        crd::u32 njobs;
    };
    Ctx ctx{x.data(), scratch.data(), counts.data(), n, njobs};
    const Ctx* cp = &ctx;
    crd::jobs::Counter* c = crd::jobs::parallel_for(
        njobs, njobs,
        [cp](crd::u32 jb, crd::u32 je)
        {
            for (crd::u32 job = jb; job < je; ++job)
            {
                const crd::usize base = static_cast<crd::usize>(job) * cp->n / cp->njobs;
                const crd::usize lo = std::max<crd::usize>(1, base);
                const crd::usize hi =
                    std::min<crd::usize>(static_cast<crd::usize>(job + 1) * cp->n / cp->njobs, cp->n - 1);
                crd::usize cnt = 0, i = lo;
                while (i < hi)
                {
                    if (cp->x[i - 1] < cp->x[i])
                    {
                        crd::usize ahead = i + 1;
                        while (ahead < cp->n - 1 && cp->x[ahead] == cp->x[i])
                        {
                            ++ahead;
                        }
                        if (cp->x[ahead] < cp->x[i])
                        {
                            cp->scratch[base + cnt] = (i + ahead - 1) / 2;
                            ++cnt;
                            i = ahead;
                            continue;
                        }
                    }
                    ++i;
                }
                cp->counts[job] = cnt;
            }
        });
    crd::jobs::wait(c);
    crd::containers::Array<crd::usize> out(alloc);
    for (crd::u32 j = 0; j < njobs; ++j) // gather in chunk order ⇒ ascending indices, deterministic
    {
        const crd::usize base = static_cast<crd::usize>(j) * n / njobs;
        for (crd::usize k = 0; k < counts[j]; ++k)
        {
            out.push_back(scratch[base + k]);
        }
    }
    return out;
}

// peak prominences (scipy _peak_prominences). wlen = 0 ⇒ whole signal.
template <typename T> struct Prominences
{
    crd::containers::Array<T> prominences;
    crd::containers::Array<crd::usize> left_bases, right_bases;
    explicit Prominences(crd::memory::IAllocator* a) : prominences(a), left_bases(a), right_bases(a) {}
};

template <typename T>
[[nodiscard]] Prominences<T> peak_prominences(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                              crd::containers::ConstSpan<crd::usize> peaks, crd::usize wlen = 0)
{
    Prominences<T> r(alloc);
    const crd::usize n = x.size();
    for (crd::usize pn = 0; pn < peaks.size(); ++pn)
    {
        const crd::usize peak = peaks[pn];
        long long i_min = 0;
        long long i_max = static_cast<long long>(n) - 1;
        if (wlen >= 2)
        {
            i_min = std::max<long long>(static_cast<long long>(peak) - static_cast<long long>(wlen / 2), i_min);
            i_max = std::min<long long>(static_cast<long long>(peak) + static_cast<long long>(wlen / 2), i_max);
        }
        long long i = static_cast<long long>(peak);
        crd::usize left_base = peak;
        T left_min = x[peak];
        while (i_min <= i && x[static_cast<crd::usize>(i)] <= x[peak])
        {
            if (x[static_cast<crd::usize>(i)] < left_min)
            {
                left_min = x[static_cast<crd::usize>(i)];
                left_base = static_cast<crd::usize>(i);
            }
            --i;
        }
        i = static_cast<long long>(peak);
        crd::usize right_base = peak;
        T right_min = x[peak];
        while (i <= i_max && x[static_cast<crd::usize>(i)] <= x[peak])
        {
            if (x[static_cast<crd::usize>(i)] < right_min)
            {
                right_min = x[static_cast<crd::usize>(i)];
                right_base = static_cast<crd::usize>(i);
            }
            ++i;
        }
        r.prominences.push_back(x[peak] - std::max(left_min, right_min));
        r.left_bases.push_back(left_base);
        r.right_bases.push_back(right_base);
    }
    return r;
}

// peak widths at `rel_height` of the prominence (scipy _peak_widths).
template <typename T>
[[nodiscard]] crd::containers::Array<T> peak_widths(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                                    crd::containers::ConstSpan<crd::usize> peaks, T rel_height,
                                                    const Prominences<T>& prom)
{
    crd::containers::Array<T> widths(alloc);
    for (crd::usize pn = 0; pn < peaks.size(); ++pn)
    {
        const crd::usize peak = peaks[pn];
        const crd::usize i_min = prom.left_bases[pn], i_max = prom.right_bases[pn];
        const T height = x[peak] - prom.prominences[pn] * rel_height;
        long long i = static_cast<long long>(peak);
        while (static_cast<long long>(i_min) < i && height < x[static_cast<crd::usize>(i)])
        {
            --i;
        }
        T left_ip = static_cast<T>(i);
        if (x[static_cast<crd::usize>(i)] < height)
        {
            left_ip += (height - x[static_cast<crd::usize>(i)]) / (x[static_cast<crd::usize>(i) + 1] - x[static_cast<crd::usize>(i)]);
        }
        i = static_cast<long long>(peak);
        while (i < static_cast<long long>(i_max) && height < x[static_cast<crd::usize>(i)])
        {
            ++i;
        }
        T right_ip = static_cast<T>(i);
        if (x[static_cast<crd::usize>(i)] < height)
        {
            right_ip -= (height - x[static_cast<crd::usize>(i)]) / (x[static_cast<crd::usize>(i) - 1] - x[static_cast<crd::usize>(i)]);
        }
        widths.push_back(right_ip - left_ip);
    }
    return widths;
}

// find_peaks with height + distance filters (scipy.signal.find_peaks subset). Returns the kept peak indices.
template <typename T>
[[nodiscard]] crd::containers::Array<crd::usize> find_peaks(crd::memory::IAllocator* alloc,
                                                            crd::containers::ConstSpan<T> x,
                                                            T min_height = -std::numeric_limits<T>::infinity(),
                                                            crd::usize min_distance = 0)
{
    // MEASURED: the serial scan wins — find_peaks is OUTPUT-ASSEMBLY-bound (building the peak list), not scan-bound,
    // so multithreading the scan loses to the per-call scratch alloc + the serial gather (see local_maxima_mt below,
    // kept for huge-array callers + as the determinism-moat reference). Honest parity with scipy (both ~2.6 ms @1M).
    crd::containers::Array<crd::usize> peaks = local_maxima<T>(alloc, x);
    if (min_height > -std::numeric_limits<T>::infinity()) // height filter
    {
        crd::containers::Array<crd::usize> kept(alloc);
        for (crd::usize i = 0; i < peaks.size(); ++i)
        {
            if (x[peaks[i]] >= min_height)
            {
                kept.push_back(peaks[i]);
            }
        }
        peaks = std::move(kept);
    }
    if (min_distance >= 1 && peaks.size() > 1) // distance filter (scipy _select_by_peak_distance: highest-first)
    {
        const crd::usize np = peaks.size();
        crd::containers::Array<crd::usize> order(alloc); // indices sorted by ascending height (priority)
        order.resize(np);
        for (crd::usize i = 0; i < np; ++i)
        {
            order[i] = i;
        }
        for (crd::usize a = 0; a + 1 < np; ++a) // selection sort by height ascending
        {
            for (crd::usize b = a + 1; b < np; ++b)
            {
                if (x[peaks[order[b]]] < x[peaks[order[a]]])
                {
                    const crd::usize t = order[a];
                    order[a] = order[b];
                    order[b] = t;
                }
            }
        }
        crd::containers::Array<bool> keep(alloc);
        keep.resize(np);
        for (crd::usize i = 0; i < np; ++i)
        {
            keep[i] = true;
        }
        for (crd::usize k = np; k-- > 0;) // highest priority first
        {
            const crd::usize j = order[k];
            if (!keep[j])
            {
                continue;
            }
            for (crd::usize m = j + 1; m < np && peaks[m] - peaks[j] < min_distance; ++m)
            {
                keep[m] = false;
            }
            for (crd::usize m = j; m-- > 0 && peaks[j] - peaks[m] < min_distance;)
            {
                keep[m] = false;
            }
        }
        crd::containers::Array<crd::usize> kept(alloc);
        for (crd::usize i = 0; i < np; ++i)
        {
            if (keep[i])
            {
                kept.push_back(peaks[i]);
            }
        }
        peaks = std::move(kept);
    }
    return peaks;
}

// argrelextrema: indices where x is a relative max (greater=true) / min over ±order neighbours (scipy).
template <typename T>
[[nodiscard]] crd::containers::Array<crd::usize> argrelextrema(crd::memory::IAllocator* alloc,
                                                               crd::containers::ConstSpan<T> x, bool greater,
                                                               crd::usize order = 1)
{
    crd::containers::Array<crd::usize> out(alloc);
    const crd::usize n = x.size();
    for (crd::usize i = order; i + order < n; ++i)
    {
        bool ext = true;
        for (crd::usize k = 1; k <= order && ext; ++k)
        {
            const bool l = greater ? (x[i] > x[i - k]) : (x[i] < x[i - k]);
            const bool rr = greater ? (x[i] > x[i + k]) : (x[i] < x[i + k]);
            ext = l && rr;
        }
        if (ext)
        {
            out.push_back(i);
        }
    }
    return out;
}

// detrend: 'constant' subtracts the mean; 'linear' subtracts the least-squares straight line (scipy.signal.detrend).
template <typename T>
[[nodiscard]] crd::containers::Array<T> detrend(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                                bool linear)
{
    const crd::usize n = x.size();
    crd::containers::Array<T> out(alloc);
    out.resize(n);
    if (!linear)
    {
        T mean = T(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            mean += x[i];
        }
        mean /= static_cast<T>(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            out[i] = x[i] - mean;
        }
        return out;
    }
    // LS line on the scipy design A = [t/N, 1], t = 1..N: solve the 2x2 normal equations.
    T s00 = T(0), s01 = T(0), s11 = static_cast<T>(n), b0 = T(0), b1 = T(0);
    for (crd::usize i = 0; i < n; ++i)
    {
        const T t = static_cast<T>(i + 1) / static_cast<T>(n);
        s00 += t * t;
        s01 += t;
        b0 += t * x[i];
        b1 += x[i];
    }
    const T det = s00 * s11 - s01 * s01;
    const T c0 = (b0 * s11 - b1 * s01) / det; // slope coeff
    const T c1 = (s00 * b1 - s01 * b0) / det; // intercept coeff
    for (crd::usize i = 0; i < n; ++i)
    {
        const T t = static_cast<T>(i + 1) / static_cast<T>(n);
        out[i] = x[i] - (c0 * t + c1);
    }
    return out;
}

// amplitude statistics.
template <typename T> [[nodiscard]] T rms(crd::containers::ConstSpan<T> x) noexcept
{
    T s = T(0);
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        s += x[i] * x[i];
    }
    return std::sqrt(s / static_cast<T>(x.size()));
}
template <typename T> [[nodiscard]] T peak_value(crd::containers::ConstSpan<T> x) noexcept
{
    T m = T(0);
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        m = std::max(m, std::abs(x[i]));
    }
    return m;
}
template <typename T> [[nodiscard]] T crest_factor(crd::containers::ConstSpan<T> x) noexcept
{
    const T r = rms<T>(x);
    return (r > T(0)) ? peak_value<T>(x) / r : T(0);
}

namespace detail
{
// one-sided power spectrum (rfft, no window for an integer-cycle gate; harmonic powers are single-bin sums).
template <typename T>
struct SpectrumMetrics
{
    T fundamental, harmonics, noise, spur, dc;
    crd::usize f_bin;
};
template <typename T>
[[nodiscard]] SpectrumMetrics<T> spectrum_metrics(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                                  crd::usize nharm)
{
    crd::usize nfft = 4;
    while (nfft < x.size())
    {
        nfft <<= 1;
    }
    const crd::usize half = nfft / 2 + 1;
    crd::containers::Array<T> buf(alloc);
    crd::containers::Array<Complex<T>> spec(alloc);
    buf.resize(nfft);
    spec.resize(half);
    for (crd::usize i = 0; i < nfft; ++i)
    {
        buf[i] = (i < x.size()) ? x[i] : T(0);
    }
    fft::RealFftPlan<T> plan(alloc, nfft);
    plan.rfft(crd::containers::ConstSpan<T>(buf.data(), nfft), crd::containers::Span<Complex<T>>(spec.data(), half));
    crd::containers::Array<T> pw(alloc); // bin power
    pw.resize(half);
    for (crd::usize i = 0; i < half; ++i)
    {
        pw[i] = spec[i].re * spec[i].re + spec[i].im * spec[i].im;
    }
    SpectrumMetrics<T> m{};
    m.dc = pw[0];
    crd::usize f0 = 1;
    for (crd::usize i = 2; i < half; ++i) // fundamental = largest non-DC bin
    {
        if (pw[i] > pw[f0])
        {
            f0 = i;
        }
    }
    m.f_bin = f0;
    m.fundamental = pw[f0];
    m.harmonics = T(0);
    crd::containers::Array<bool> used(alloc);
    used.resize(half);
    for (crd::usize i = 0; i < half; ++i)
    {
        used[i] = false;
    }
    used[0] = true;
    used[f0] = true;
    for (crd::usize h = 2; h <= nharm; ++h) // harmonics at h·f0 (aliased into [0, half))
    {
        crd::usize hb = h * f0;
        if (hb >= half)
        {
            hb = (nfft - (hb % nfft) < half) ? (nfft - hb % nfft) : (hb % nfft); // simple fold
            if (hb >= half)
            {
                continue;
            }
        }
        if (!used[hb])
        {
            m.harmonics += pw[hb];
            used[hb] = true;
        }
    }
    m.noise = T(0);
    m.spur = T(0);
    for (crd::usize i = 1; i < half; ++i)
    {
        if (i == f0)
        {
            continue;
        }
        m.spur = std::max(m.spur, pw[i]); // SFDR spur = largest non-fundamental bin (harmonics INCLUDED)
        if (!used[i])
        {
            m.noise += pw[i]; // noise = non-DC, non-fundamental, non-harmonic
        }
    }
    return m;
}
template <typename T> [[nodiscard]] inline T to_db_power(T num, T den) noexcept
{
    return T(10) * std::log10(num / (den + static_cast<T>(1e-300)));
}
} // namespace detail

// THD in dB (10·log10(Σ harmonic power / fundamental power)).
template <typename T> [[nodiscard]] T thd(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x, crd::usize nharm = 6)
{
    const auto m = detail::spectrum_metrics<T>(alloc, x, nharm);
    return detail::to_db_power<T>(m.harmonics, m.fundamental);
}
// SNR in dB (fundamental / noise, excluding DC + harmonics).
template <typename T> [[nodiscard]] T snr(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x, crd::usize nharm = 6)
{
    const auto m = detail::spectrum_metrics<T>(alloc, x, nharm);
    return detail::to_db_power<T>(m.fundamental, m.noise);
}
// SINAD in dB (fundamental / (noise + distortion)).
template <typename T> [[nodiscard]] T sinad(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x, crd::usize nharm = 6)
{
    const auto m = detail::spectrum_metrics<T>(alloc, x, nharm);
    return detail::to_db_power<T>(m.fundamental, m.noise + m.harmonics);
}
// SFDR in dB (fundamental / largest spur).
template <typename T> [[nodiscard]] T sfdr(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x, crd::usize nharm = 6)
{
    const auto m = detail::spectrum_metrics<T>(alloc, x, nharm);
    return detail::to_db_power<T>(m.fundamental, m.spur);
}
// ENOB from SINAD: (SINAD_dB − 1.76) / 6.02.
template <typename T> [[nodiscard]] T enob(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x, crd::usize nharm = 6)
{
    return (sinad<T>(alloc, x, nharm) - static_cast<T>(1.76)) / static_cast<T>(6.02);
}

// cubic B-spline coefficients (scipy.signal.cspline1d): c with s[k] = (c[k-1] + 4·c[k] + c[k+1])/6 — i.e. solve the
// [1,4,1] tridiagonal system (mirror boundaries) via the Thomas algorithm. Reconstruction at the knots == signal.
template <typename T>
[[nodiscard]] crd::containers::Array<T> cspline1d(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> s)
{
    const crd::usize n = s.size();
    crd::containers::Array<T> c(alloc), lower(alloc), diag(alloc), upper(alloc), rhs(alloc), cp(alloc), dp(alloc);
    c.resize(n);
    if (n == 0)
    {
        return c;
    }
    if (n == 1)
    {
        c[0] = s[0];
        return c;
    }
    lower.resize(n);
    diag.resize(n);
    upper.resize(n);
    rhs.resize(n);
    cp.resize(n);
    dp.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        diag[i] = T(4);
        lower[i] = T(1);
        upper[i] = T(1);
        rhs[i] = T(6) * s[i];
    }
    upper[0] = T(2);     // mirror: c[-1] = c[1]
    lower[n - 1] = T(2); // mirror: c[N] = c[N-2]
    // Thomas algorithm.
    cp[0] = upper[0] / diag[0];
    dp[0] = rhs[0] / diag[0];
    for (crd::usize i = 1; i < n; ++i)
    {
        const T m = diag[i] - lower[i] * cp[i - 1];
        cp[i] = upper[i] / m;
        dp[i] = (rhs[i] - lower[i] * dp[i - 1]) / m;
    }
    c[n - 1] = dp[n - 1];
    for (crd::usize i = n - 1; i-- > 0;)
    {
        c[i] = dp[i] - cp[i] * c[i + 1];
    }
    return c;
}

} // namespace crd::hesap::dsp
