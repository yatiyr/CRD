#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-wavelet v11w-d — continuous wavelet transform.
//
//   ContinuousWavelet   mexh, morl, cmor{B}-{C}, gaus{P} (1-8), cgau{P} (1-8),
//                       shan{B}-{C}, fbsp{M}-{B}-{C}, paul{M}. ψ matches pywt
//                       exactly (gaus/cgau L2-normalized; paul is analytic —
//                       pywt has no paul, gated self-contained).
//   cwt                 W(a, b) over a vector of scales via PyWavelets' method
//                       (integrate ψ → resample per scale → convolve → diff →
//                       centre-trim). FFT convolution; the data spectrum is
//                       computed once and SHARED across scales; the scales are
//                       independent ⇒ MULTI-THREADED batched (per-job plans,
//                       serial-order writes ⇒ bit-identical across {1..16}).
//   central_frequency   FFT-peak of ψ (pywt's exact algorithm) ⇒ scale↔freq.
//   cwt_ridge           per-time argmax-scale ridge.
//
// Gate (ADR-0093, transform = spec-compliance): coefficients vs pywt.cwt + the
// central frequencies vs pywt + a tone localizes to scale ≈ fc/f + run-twice
// bit-identical + the {1,4,16}-thread bit-identity moat.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/fft/fft.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <numbers>

namespace crd::hesap::wavelet
{

enum class ContinuousWaveletKind : crd::u8
{
    Mexh, // Mexican hat / Ricker
    Morl, // real Morlet
    Cmor, // complex Morlet (B, C)
    Gaus, // Gaussian derivative, order P (real)
    Cgau, // complex Gaussian derivative, order P
    Shan, // complex Shannon (B, C)
    Fbsp, // frequency B-spline (M, B, C)
    Paul  // Paul, order M (complex; not in pywt — analytic)
};

// A continuous wavelet descriptor (parameters only; ψ is sampled by detail::sample_psi).
struct ContinuousWavelet
{
    ContinuousWaveletKind kind = ContinuousWaveletKind::Mexh;
    double lower = -8.0;
    double upper = 8.0;
    // Lower-layer numerical-kernel parameters (raw f64 per ADR-0078): normalized cycles/sample shape constants.
    double bandwidth = 1.0; // crd-lint-allow-untagged-physical (cmor/shan/fbsp B, lower-layer kernel)
    double center = 1.0;    // cmor/shan/fbsp C (modulation)
    int order = 0;          // gaus/cgau P, fbsp/paul M
    bool complex_valued = false;
    bool l2_normalize = false; // gaus/cgau normalize ψ to unit L2 (matches pywt's analytic constant)
};

namespace detail
{
template <typename T> [[nodiscard]] inline Complex<double> cmul(Complex<double> a, Complex<double> b) noexcept
{
    return Complex<double>{a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
}
[[nodiscard]] inline double np_sinc(double x) noexcept // numpy sinc: sin(pi x)/(pi x), 1 at 0
{
    if (x == 0.0)
    {
        return 1.0;
    }
    const double px = std::numbers::pi_v<double> * x;
    return crd::math::sin(px) / px;
}

// Build the real polynomial coefficients (ascending) of d^P/dx^P e^{-x^2} via D_{k+1}=D_k' - 2x·D_k, D_0=1.
inline int gauss_deriv_coeffs(int p, double* c /*size >= p+1*/) noexcept
{
    double cur[16] = {1.0};
    int n = 1;
    for (int s = 0; s < p; ++s)
    {
        double nxt[18] = {0.0};
        for (int k = 1; k < n; ++k) // derivative: k·c_k x^{k-1}
        {
            nxt[k - 1] += static_cast<double>(k) * cur[k];
        }
        for (int k = 0; k < n; ++k) // -2x·c
        {
            nxt[k + 1] += -2.0 * cur[k];
        }
        n += 1;
        for (int k = 0; k < n; ++k)
        {
            cur[k] = nxt[k];
        }
    }
    for (int k = 0; k < n; ++k)
    {
        c[k] = cur[k];
    }
    return n;
}

// Complex polynomial coefficients (ascending) of d^P/dx^P [e^{-x^2-ix}] via D_{k+1}=D_k' + (-2x - i)·D_k, D_0=1.
inline int cgauss_deriv_coeffs(int p, Complex<double>* c /*size >= p+1*/) noexcept
{
    Complex<double> cur[16] = {Complex<double>{1.0, 0.0}};
    int n = 1;
    for (int s = 0; s < p; ++s)
    {
        Complex<double> nxt[18] = {};
        for (int k = 1; k < n; ++k)
        {
            nxt[k - 1].re += static_cast<double>(k) * cur[k].re;
            nxt[k - 1].im += static_cast<double>(k) * cur[k].im;
        }
        for (int k = 0; k < n; ++k) // (-2x - i)·c : -2·shift-up  + (-i)·same
        {
            nxt[k + 1].re += -2.0 * cur[k].re;
            nxt[k + 1].im += -2.0 * cur[k].im;
            nxt[k].re += cur[k].im;  // -i·c : (re+i·im)·(-i) = im - i·re
            nxt[k].im += -cur[k].re;
        }
        n += 1;
        for (int k = 0; k < n; ++k)
        {
            cur[k] = nxt[k];
        }
    }
    for (int k = 0; k < n; ++k)
    {
        c[k] = cur[k];
    }
    return n;
}

// Sample ψ on the grid linspace(lower, upper, grid_n) into out (Complex<double>); L2-normalize if requested.
inline void sample_psi(const ContinuousWavelet& w, crd::usize grid_n, Complex<double>* out) noexcept
{
    const double pi = std::numbers::pi_v<double>;
    const double step = (w.upper - w.lower) / static_cast<double>(grid_n - 1);
    double gc[18];
    Complex<double> cc[18];
    int gn = 0, cn = 0;
    double gsign = 1.0;
    if (w.kind == ContinuousWaveletKind::Gaus)
    {
        gn = gauss_deriv_coeffs(w.order, gc);
        gsign = ((w.order / 2) & 1) ? -1.0 : 1.0; // (-1)^floor(P/2)
    }
    else if (w.kind == ContinuousWaveletKind::Cgau)
    {
        cn = cgauss_deriv_coeffs(w.order, cc);
    }
    for (crd::usize k = 0; k < grid_n; ++k)
    {
        const double t = w.lower + static_cast<double>(k) * step;
        Complex<double> v{0.0, 0.0};
        switch (w.kind)
        {
        case ContinuousWaveletKind::Mexh:
        {
            const double c = 2.0 / (crd::math::sqrt(3.0) * crd::math::pow(pi, 0.25));
            v = Complex<double>{c * (1.0 - t * t) * crd::math::exp(-t * t / 2.0), 0.0};
            break;
        }
        case ContinuousWaveletKind::Morl:
            v = Complex<double>{crd::math::exp(-t * t / 2.0) * crd::math::cos(5.0 * t), 0.0};
            break;
        case ContinuousWaveletKind::Cmor:
        {
            const double amp = crd::math::pow(pi * w.bandwidth, -0.5) * crd::math::exp(-t * t / w.bandwidth);
            const double ph = 2.0 * pi * w.center * t;
            v = Complex<double>{amp * crd::math::cos(ph), amp * crd::math::sin(ph)};
            break;
        }
        case ContinuousWaveletKind::Gaus:
        {
            double poly = 0.0, tp = 1.0;
            for (int j = 0; j < gn; ++j)
            {
                poly += gc[j] * tp;
                tp *= t;
            }
            v = Complex<double>{gsign * poly * crd::math::exp(-t * t), 0.0};
            break;
        }
        case ContinuousWaveletKind::Cgau:
        {
            Complex<double> poly{0.0, 0.0};
            double tp = 1.0;
            for (int j = 0; j < cn; ++j)
            {
                poly.re += cc[j].re * tp;
                poly.im += cc[j].im * tp;
                tp *= t;
            }
            const double e = crd::math::exp(-t * t);
            const Complex<double> g{e * crd::math::cos(t), -e * crd::math::sin(t)}; // e^{-x^2-ix}
            v = cmul<double>(poly, g);
            break;
        }
        case ContinuousWaveletKind::Shan:
        {
            const double amp = crd::math::sqrt(w.bandwidth) * np_sinc(w.bandwidth * t);
            const double ph = 2.0 * pi * w.center * t;
            v = Complex<double>{amp * crd::math::cos(ph), amp * crd::math::sin(ph)};
            break;
        }
        case ContinuousWaveletKind::Fbsp:
        {
            const double sc = np_sinc(w.bandwidth * t / static_cast<double>(w.order));
            const double amp = crd::math::sqrt(w.bandwidth) * crd::math::pow(sc, w.order);
            const double ph = 2.0 * pi * w.center * t;
            v = Complex<double>{amp * crd::math::cos(ph), amp * crd::math::sin(ph)};
            break;
        }
        case ContinuousWaveletKind::Paul:
        {
            // ψ_m(t) = (2^m i^m m!)/sqrt(π (2m)!) · (1 - i t)^{-(m+1)}
            const int m = w.order;
            double fact_m = 1.0, fact_2m = 1.0;
            for (int j = 2; j <= m; ++j)
            {
                fact_m *= j;
            }
            for (int j = 2; j <= 2 * m; ++j)
            {
                fact_2m *= j;
            }
            const double norm = crd::math::pow(2.0, m) * fact_m / crd::math::sqrt(pi * fact_2m);
            Complex<double> im_m{1.0, 0.0}; // i^m
            for (int j = 0; j < m; ++j)
            {
                im_m = Complex<double>{-im_m.im, im_m.re};
            }
            // (1 - i t)^{-(m+1)} = exp(-(m+1)·log(1 - i t))
            const double re1 = 1.0, im1 = -t;
            const double r = crd::math::sqrt(re1 * re1 + im1 * im1);
            const double th = crd::math::atan2(im1, re1);
            const double mag = crd::math::pow(r, -(m + 1.0));
            const double ang = -(m + 1.0) * th;
            const Complex<double> pw{mag * crd::math::cos(ang), mag * crd::math::sin(ang)};
            v = cmul<double>(Complex<double>{norm * im_m.re, norm * im_m.im}, pw);
            break;
        }
        }
        out[k] = v;
    }
    if (w.l2_normalize)
    {
        double nrm2 = 0.0;
        for (crd::usize k = 0; k < grid_n; ++k)
        {
            nrm2 += (out[k].re * out[k].re + out[k].im * out[k].im) * step;
        }
        const double inv = 1.0 / crd::math::sqrt(nrm2);
        for (crd::usize k = 0; k < grid_n; ++k)
        {
            out[k].re *= inv;
            out[k].im *= inv;
        }
    }
}

// non-negative decimal parse (advances pos).
[[nodiscard]] inline double parse_double(crd::containers::StringView sv, crd::usize& pos) noexcept
{
    double sign = 1.0;
    if (pos < sv.size() && sv[pos] == '-')
    {
        sign = -1.0;
        ++pos;
    }
    double v = 0.0;
    while (pos < sv.size() && sv[pos] >= '0' && sv[pos] <= '9')
    {
        v = v * 10.0 + static_cast<double>(sv[pos] - '0');
        ++pos;
    }
    if (pos < sv.size() && sv[pos] == '.')
    {
        ++pos;
        double frac = 0.1;
        while (pos < sv.size() && sv[pos] >= '0' && sv[pos] <= '9')
        {
            v += static_cast<double>(sv[pos] - '0') * frac;
            frac *= 0.1;
            ++pos;
        }
    }
    return sign * v;
}
} // namespace detail

// Build a ContinuousWavelet from a pywt-style name:
//   "mexh", "morl", "cmorB-C", "gausP", "cgauP", "shanB-C", "fbspM-B-C", "paulM".
[[nodiscard]] inline ContinuousWavelet continuous_wavelet(crd::containers::StringView name) noexcept
{
    ContinuousWavelet w;
    auto starts = [&](const char* p) {
        crd::containers::StringView s(p);
        return name.size() >= s.size() && name.substr(0, s.size()) == s;
    };
    if (name == "mexh")
    {
        w.kind = ContinuousWaveletKind::Mexh;
        w.lower = -8.0;
        w.upper = 8.0;
    }
    else if (name == "morl")
    {
        w.kind = ContinuousWaveletKind::Morl;
        w.lower = -8.0;
        w.upper = 8.0;
    }
    else if (starts("cmor"))
    {
        w.kind = ContinuousWaveletKind::Cmor;
        w.lower = -8.0;
        w.upper = 8.0;
        crd::usize pos = 4;
        w.bandwidth = detail::parse_double(name, pos);
        if (pos < name.size() && name[pos] == '-')
        {
            ++pos;
        }
        w.center = detail::parse_double(name, pos);
        w.complex_valued = true;
    }
    else if (starts("cgau"))
    {
        w.kind = ContinuousWaveletKind::Cgau;
        w.lower = -5.0;
        w.upper = 5.0;
        crd::usize pos = 4;
        w.order = static_cast<int>(detail::parse_double(name, pos));
        w.complex_valued = true;
        w.l2_normalize = true;
    }
    else if (starts("gaus"))
    {
        w.kind = ContinuousWaveletKind::Gaus;
        w.lower = -5.0;
        w.upper = 5.0;
        crd::usize pos = 4;
        w.order = static_cast<int>(detail::parse_double(name, pos));
        w.l2_normalize = true;
    }
    else if (starts("shan"))
    {
        w.kind = ContinuousWaveletKind::Shan;
        w.lower = -20.0;
        w.upper = 20.0;
        crd::usize pos = 4;
        w.bandwidth = detail::parse_double(name, pos);
        if (pos < name.size() && name[pos] == '-')
        {
            ++pos;
        }
        w.center = detail::parse_double(name, pos);
        w.complex_valued = true;
    }
    else if (starts("fbsp"))
    {
        w.kind = ContinuousWaveletKind::Fbsp;
        w.lower = -20.0;
        w.upper = 20.0;
        crd::usize pos = 4;
        w.order = static_cast<int>(detail::parse_double(name, pos)); // M
        if (pos < name.size() && name[pos] == '-')
        {
            ++pos;
        }
        w.bandwidth = detail::parse_double(name, pos); // B
        if (pos < name.size() && name[pos] == '-')
        {
            ++pos;
        }
        w.center = detail::parse_double(name, pos); // C
        w.complex_valued = true;
    }
    else if (starts("paul"))
    {
        w.kind = ContinuousWaveletKind::Paul;
        w.lower = -12.0;
        w.upper = 12.0;
        crd::usize pos = 4;
        w.order = (pos < name.size()) ? static_cast<int>(detail::parse_double(name, pos)) : 4;
        w.complex_valued = true;
    }
    return w;
}

// Central frequency via the FFT-peak of ψ (pywt's exact algorithm). precision = 12 ⇒ 2^12-point ψ grid.
[[nodiscard]] inline double central_frequency(crd::memory::IAllocator* alloc, const ContinuousWavelet& w,
                                              int precision = 12)
{
    const crd::usize n = crd::usize{1} << static_cast<crd::usize>(precision);
    const double domain = w.upper - w.lower;
    crd::containers::Array<Complex<double>> psi(alloc);
    psi.resize(n);
    detail::sample_psi(w, n, psi.data());
    fft::FftPlan<double> plan(alloc, n);
    plan.execute(crd::containers::Span<Complex<double>>(psi.data(), n), fft::FftDirection::Forward);
    crd::usize bin = 1;
    double best = -1.0;
    for (crd::usize k = 1; k < n; ++k)
    {
        const double m = psi[k].re * psi[k].re + psi[k].im * psi[k].im;
        if (m > best)
        {
            best = m;
            bin = k;
        }
    }
    crd::isize index = static_cast<crd::isize>(bin) + 1;
    if (static_cast<double>(index) > static_cast<double>(n) / 2.0)
    {
        index = static_cast<crd::isize>(n) - index + 2;
    }
    return static_cast<double>(index - 1) / domain;
}

[[nodiscard]] inline double scale_to_frequency(crd::memory::IAllocator* alloc, const ContinuousWavelet& w,
                                               double scale, double sampling_period = 1.0, int precision = 12)
{
    return central_frequency(alloc, w, precision) / (scale * sampling_period);
}

template <typename T> struct CwtResult
{
    crd::containers::Array<Complex<T>> coeffs; // num_scales * n, row-major
    crd::containers::Array<T> frequencies;     // num_scales
    crd::usize num_scales = 0;
    crd::usize n = 0;
    [[nodiscard]] const Complex<T>* row(crd::usize s) const noexcept { return coeffs.data() + s * n; }
};

// CWT of x at the given scales, faithful to pywt.cwt (FFT convolution; data spectrum shared across scales).
// The scales are independent ⇒ multi-threaded batched: each job owns its plan + buffers, writes its own rows ⇒
// the matrix is BIT-IDENTICAL across thread counts (the moat pywt/MATLAB lack). precision = 12 (pywt default).
template <typename T>
[[nodiscard]] CwtResult<T> cwt(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                               crd::containers::ConstSpan<T> scales, const ContinuousWavelet& w,
                               double sampling_period = 1.0, int precision = 12)
{
    const crd::usize n = x.size();
    const crd::usize ns = scales.size();
    const crd::usize grid_n = crd::usize{1} << static_cast<crd::usize>(precision);
    const double step = (w.upper - w.lower) / static_cast<double>(grid_n - 1);
    const double xrange = w.upper - w.lower;

    crd::containers::Array<Complex<double>> int_psi(alloc);
    int_psi.resize(grid_n);
    detail::sample_psi(w, grid_n, int_psi.data());
    Complex<double> acc{0.0, 0.0}; // int_psi = cumsum(ψ)·step  (conj for complex, per pywt)
    for (crd::usize k = 0; k < grid_n; ++k)
    {
        acc = Complex<double>{acc.re + int_psi[k].re, acc.im + int_psi[k].im};
        int_psi[k] = w.complex_valued ? Complex<double>{acc.re * step, -acc.im * step}
                                      : Complex<double>{acc.re * step, acc.im * step};
    }

    crd::usize kmax = 1;
    for (crd::usize s = 0; s < ns; ++s)
    {
        const double a = static_cast<double>(scales[s]);
        const crd::usize count_raw = static_cast<crd::usize>(crd::math::ceil(a * xrange + 1.0));
        crd::usize kk = 0;
        for (crd::usize m = 0; m < count_raw; ++m)
        {
            if (static_cast<crd::usize>(static_cast<double>(m) / (a * step)) >= grid_n)
            {
                break;
            }
            ++kk;
        }
        if (kk > kmax)
        {
            kmax = kk;
        }
    }
    crd::usize lfft = 1;
    while (lfft < n + kmax)
    {
        lfft <<= 1;
    }

    // data spectrum (shared, computed once).
    fft::FftPlan<T> dplan(alloc, lfft);
    crd::containers::Array<Complex<T>> data_freq(alloc);
    data_freq.resize(lfft);
    for (crd::usize i = 0; i < lfft; ++i)
    {
        data_freq[i] = (i < n) ? Complex<T>{x[i], T(0)} : Complex<T>{T(0), T(0)};
    }
    dplan.execute(crd::containers::Span<Complex<T>>(data_freq.data(), lfft), fft::FftDirection::Forward);

    CwtResult<T> out;
    out.n = n;
    out.num_scales = ns;
    out.coeffs.resize(ns * n);
    out.frequencies.resize(ns);
    const double fc = central_frequency(alloc, w, precision);
    for (crd::usize s = 0; s < ns; ++s)
    {
        out.frequencies[s] = static_cast<T>(fc / (static_cast<double>(scales[s]) * sampling_period));
    }
    const T inv_lfft = T(1) / static_cast<T>(lfft);

    // per-job FFT plans + scratch, built serially (the Welch pattern).
    const crd::u32 nw = crd::jobs::num_workers();
    const crd::u32 njobs = (ns < 4 || nw <= 1) ? 1U : ((nw < static_cast<crd::u32>(ns)) ? nw : static_cast<crd::u32>(ns));
    crd::containers::Array<fft::FftPlan<T>*> plans(alloc);
    crd::containers::Array<Complex<T>> kbuf(alloc), wbuf(alloc);
    plans.resize(njobs);
    kbuf.resize(static_cast<crd::usize>(njobs) * lfft);
    wbuf.resize(static_cast<crd::usize>(njobs) * lfft);
    for (crd::u32 j = 0; j < njobs; ++j)
    {
        auto* p = static_cast<fft::FftPlan<T>*>(alloc->allocate(sizeof(fft::FftPlan<T>), alignof(fft::FftPlan<T>)));
        ::new (static_cast<void*>(p)) fft::FftPlan<T>(alloc, lfft);
        plans[j] = p;
    }

    auto do_scale = [&](crd::u32 job, crd::usize s) {
        const double a = static_cast<double>(scales[s]);
        Complex<T>* kb = kbuf.data() + static_cast<crd::usize>(job) * lfft;
        Complex<T>* wb = wbuf.data() + static_cast<crd::usize>(job) * lfft;
        const crd::usize count_raw = static_cast<crd::usize>(crd::math::ceil(a * xrange + 1.0));
        crd::usize kk = 0;
        for (crd::usize m = 0; m < count_raw; ++m) // kernel = int_psi[j] reversed, j=floor(m/(a·step))
        {
            const crd::usize j = static_cast<crd::usize>(static_cast<double>(m) / (a * step));
            if (j >= grid_n)
            {
                break;
            }
            ++kk;
        }
        if (kk < 2)
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                out.coeffs[s * n + i] = Complex<T>{T(0), T(0)};
            }
            return;
        }
        for (crd::usize i = 0; i < lfft; ++i)
        {
            if (i < kk)
            {
                const crd::usize m = kk - 1 - i; // reversed
                const Complex<double> v = int_psi[static_cast<crd::usize>(static_cast<double>(m) / (a * step))];
                kb[i] = Complex<T>{static_cast<T>(v.re), static_cast<T>(v.im)};
            }
            else
            {
                kb[i] = Complex<T>{T(0), T(0)};
            }
        }
        plans[job]->execute(crd::containers::Span<Complex<T>>(kb, lfft), fft::FftDirection::Forward);
        for (crd::usize i = 0; i < lfft; ++i)
        {
            const Complex<T> d = data_freq[i];
            wb[i] = Complex<T>{d.re * kb[i].re - d.im * kb[i].im, d.re * kb[i].im + d.im * kb[i].re};
        }
        plans[job]->execute(crd::containers::Span<Complex<T>>(wb, lfft), fft::FftDirection::Inverse);
        const T scale_amp = -static_cast<T>(crd::math::sqrt(a));
        const crd::usize off = (kk - 2) / 2;
        for (crd::usize i = 0; i < n; ++i)
        {
            const crd::usize t = off + i;
            out.coeffs[s * n + i] = Complex<T>{scale_amp * (wb[t + 1].re - wb[t].re) * inv_lfft,
                                               scale_amp * (wb[t + 1].im - wb[t].im) * inv_lfft};
        }
    };

    if (njobs <= 1)
    {
        for (crd::usize s = 0; s < ns; ++s)
        {
            do_scale(0, s);
        }
    }
    else
    {
        crd::jobs::Counter* c = crd::jobs::parallel_for(njobs, njobs, [&](crd::u32 jb, crd::u32 je) {
            for (crd::u32 job = jb; job < je; ++job)
            {
                const crd::usize s0 = static_cast<crd::usize>(job) * ns / njobs;
                const crd::usize s1 = static_cast<crd::usize>(job + 1) * ns / njobs;
                for (crd::usize s = s0; s < s1; ++s)
                {
                    do_scale(job, s);
                }
            }
        });
        crd::jobs::wait(c);
    }
    for (crd::u32 j = 0; j < njobs; ++j)
    {
        plans[j]->~FftPlan<T>();
        alloc->deallocate(plans[j]);
    }
    return out;
}

// Ridge: for each time index, the scale that maximizes |W(a,b)|.
template <typename T>
[[nodiscard]] crd::containers::Array<crd::usize> cwt_ridge(crd::memory::IAllocator* alloc, const CwtResult<T>& r)
{
    crd::containers::Array<crd::usize> ridge(alloc);
    ridge.resize(r.n);
    for (crd::usize b = 0; b < r.n; ++b)
    {
        crd::usize best = 0;
        T bestmag = T(-1);
        for (crd::usize s = 0; s < r.num_scales; ++s)
        {
            const Complex<T> c = r.coeffs[s * r.n + b];
            const T mag = c.re * c.re + c.im * c.im;
            if (mag > bestmag)
            {
                bestmag = mag;
                best = s;
            }
        }
        ridge[b] = best;
    }
    return ridge;
}

} // namespace crd::hesap::wavelet
