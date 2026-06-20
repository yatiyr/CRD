#pragma once

// nufft.hpp — Phase 3.1.6 v10-g: Non-Uniform FFT (Greengard-Lee), type-1 (nonuniform->uniform "spread") and
// type-2 (uniform->nonuniform "interp"), 1D, complex f32/f64. Kernel parameters follow the FINUFFT reference
// (Barnett-Magland-af Klinteberg) so the construction is directly comparable: the ES kernel ("exponential of
// semicircle", phi(z)=exp(beta(sqrt(1-z^2)-1))), upsample factor sigma=2, kernel half-width w tied to the
// requested tolerance. The fine-grid FFT reuses the v10-b FftPlan; a NUFFT is GRIDDING-bound, not
// FFT-kernel-bound, so the spreader is where the time goes.
//
// DETERMINISM (and the one genuine correctness hazard in the whole FFT cluster): type-1 SCATTERS each source
// onto w neighbouring fine-grid cells WITH ACCUMULATION. Serial accumulation in point order is a fixed reduction
// order => run-twice bit-identical TODAY (verified). This is NOT (yet) a {1..16} parallel moat: the parallel
// NUFFT is not built. When it IS jobs-parallelized (a later sub-slice) it must bin-sort points + give each
// output subgrid exactly one owner + keep a fixed traversal order — NEVER atomic-add into shared cells, which
// would make the float accumulation order thread-count-dependent and break bit-identity. That design preserves
// determinism (FINUFFT does not promise cross-thread bit-identity); the claim here is the serial reproducibility
// we actually have. Type-2 INTERPOLATES read-only from the grid => naturally deterministic.
//
// Sign convention (FINUFFT): isign = +1 => exp(+i k x_j); isign = -1 => exp(-i k x_j).
//   type-1: f_k = sum_j c_j exp(isign * i k x_j),  k = -N/2 .. N/2-1   (N output modes)
//   type-2: c_j = sum_k f_k exp(isign * i k x_j),  for each nonuniform x_j
// Correctness gate = the DIRECT nonuniform DFT (NOT round-trip — type1∘type2 can cancel spreading errors the
// same way IFFT∘FFT hides twiddle-sign bugs; the odeint-d4 / brute-force-DFT instinct applies verbatim).
// Lower-layer RAW (Complex<f32/f64>, ADR-0078).

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/fft/fft.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <cstring>

#ifdef CRD_NUFFT_PROFILE
#include <chrono>
#include <cstdio>
#endif

namespace crd::hesap::fft
{

// Spreading-kernel / accuracy options. Defaults track FINUFFT's upsampfac=2.0 path.
struct NufftOpts
{
    double tol = 1e-9;  // requested accuracy => kernel half-width w
    double sigma = 2.0; // upsample factor (fine grid n ~ sigma * N, rounded UP to a power of two)
};

// 1D NUFFT plan: N uniform modes (k=-N/2..N/2-1) <-> M nonuniform points x_j. The kernel weights are
// precomputed once per point set (set_points) and SHARED by type-1 and type-2 (they are transposes), so
// repeated transforms on the same geometry pay the spread-weight cost once. n (fine grid) is a power of two
// so the reused FftPlan applies. Constraints: N even, N >= 2.
template <typename T> class NufftPlan
{
public:
    NufftPlan(crd::memory::IAllocator* alloc, crd::usize n_modes, crd::usize n_points, const NufftOpts& opts = {})
        : m_alloc(alloc), m_nmodes(n_modes), m_npts(n_points), m_n(fine_size(n_modes, opts.sigma)),
          m_fft(alloc, fine_size(n_modes, opts.sigma)), m_dk(alloc), m_x(alloc), m_i1(alloc), m_kw(alloc),
          m_grid(alloc), m_have_points(false)
    {
        CRD_ASSERT(n_modes >= 2 && (n_modes % 2) == 0); // central band -N/2..N/2-1 extracted from the fine grid
        // Kernel half-width from tolerance, clamped; beta = 2.30 w @ sigma=2. The +2 (vs FINUFFT's +1) makes the
        // ACHIEVED error meet the requested tol with margin — our ES kernel runs ~3x looser than FINUFFT's tuned
        // beta at equal width, so we spend one extra width to actually deliver eps (a library must meet its tol).
        const double digits = -std::log10(opts.tol > 0.0 ? opts.tol : 1e-16);
        crd::i64 w = static_cast<crd::i64>(std::ceil(digits)) + 2;
        w = w < 2 ? 2 : (w > kMaxWidth ? kMaxWidth : w);
        m_w = static_cast<crd::usize>(w);
        m_beta = 2.30 * static_cast<double>(m_w); // ES kernel beta for sigma=2.0
        m_grid.resize(m_n);
        build_deconv_table();
    }

    [[nodiscard]] crd::usize num_modes() const noexcept { return m_nmodes; }
    [[nodiscard]] crd::usize num_points() const noexcept { return m_npts; }
    [[nodiscard]] crd::usize fine_size() const noexcept { return m_n; }
    [[nodiscard]] crd::usize kernel_width() const noexcept { return m_w; }

    // Bind the nonuniform points x_j (radians; folded into [0, 2*pi)). Precomputes, per point, the base fine-
    // grid index i1 and the w ES-kernel weights. O(M*w), done once; type-1/type-2 then reuse the weights.
    void set_points(crd::containers::ConstSpan<T> x)
    {
        CRD_ASSERT(x.size() == m_npts);
        m_x.resize(m_npts);
        m_i1.resize(m_npts);
        m_kw.resize(m_npts * m_w);
        const double n = static_cast<double>(m_n);
        const double half_w = 0.5 * static_cast<double>(m_w);
        constexpr double two_pi = 6.283185307179586476925286766559;
        for (crd::usize j = 0; j < m_npts; ++j)
        {
            double xj = static_cast<double>(x[j]);
            xj -= two_pi * std::floor(xj / two_pi); // fold into [0, 2*pi)
            m_x[j] = static_cast<T>(xj);
            const double xi = xj * n / two_pi; // fractional fine-grid coordinate in [0, n)
            const crd::i64 i1 = static_cast<crd::i64>(std::ceil(xi - half_w));
            m_i1[j] = i1;
            T* kw = m_kw.data() + j * m_w;
            for (crd::usize l = 0; l < m_w; ++l)
            {
                const double z = static_cast<double>(i1 + static_cast<crd::i64>(l)) - xi; // in [-w/2, w/2)
                kw[l] = static_cast<T>(es_kernel(2.0 * z / static_cast<double>(m_w)));
            }
        }
        m_have_points = true;
    }

    // Type-1: nonuniform strengths c[M] -> uniform modes f[N] (k = -N/2..N/2-1, mode k at out index k+N/2).
    // Spread (deterministic, point-order accumulation) -> fine FFT -> deconvolve + extract central band.
    void type1(crd::containers::ConstSpan<Complex<T>> c, crd::containers::Span<Complex<T>> f, int isign) const
    {
        CRD_ASSERT(m_have_points && c.size() == m_npts && f.size() == m_nmodes);
        Complex<T>* g = m_grid.data();
        std::memset(static_cast<void*>(g), 0, m_n * sizeof(Complex<T>)); // all-zero bytes == (0,0); spread accumulates into it
#ifdef CRD_NUFFT_PROFILE
        const auto t_spread0 = std::chrono::steady_clock::now();
#endif
        // SPREAD — the moat-critical scatter. Serial, point-order accumulation = fixed reduction order.
        const crd::i64 n_i = static_cast<crd::i64>(m_n);
        const crd::i64 w_i = static_cast<crd::i64>(m_w);
        for (crd::usize j = 0; j < m_npts; ++j) // point-order accumulation = fixed reduction order (the moat)
        {
            const Complex<T> cj = c[j];
            const crd::i64 i1 = m_i1[j];
            const T* kw = m_kw.data() + j * m_w;
            if (i1 >= 0 && i1 + w_i <= n_i) // fast path: the w cells are contiguous + in-range (no wrap, no modulo)
            {
                Complex<T>* gp = g + i1;
                for (crd::usize l = 0; l < m_w; ++l)
                {
                    const T wgt = kw[l];
                    gp[l].re += cj.re * wgt;
                    gp[l].im += cj.im * wgt;
                }
            }
            else // boundary point: wrap each cell (rare — only within w of the grid ends)
            {
                for (crd::usize l = 0; l < m_w; ++l)
                {
                    const crd::usize gi = wrap_index(i1 + static_cast<crd::i64>(l));
                    const T wgt = kw[l];
                    g[gi].re += cj.re * wgt;
                    g[gi].im += cj.im * wgt;
                }
            }
        }
        // FINUFFT sign: isign=+1 uses exp(+i...) => the INVERSE-direction FFT here (forward = exp(-i...)).
        const FftDirection dir = (isign >= 0) ? FftDirection::Inverse : FftDirection::Forward;
#ifdef CRD_NUFFT_PROFILE
        const auto t_fft0 = std::chrono::steady_clock::now();
#endif
        m_fft.execute(crd::containers::Span<Complex<T>>(g, m_n), dir);
#ifdef CRD_NUFFT_PROFILE
        const auto t_deconv0 = std::chrono::steady_clock::now();
#endif
        // Deconvolve + extract: f_k = d_k * FFT(grid)_{k mod n}, k=-N/2..N/2-1. The mode->grid map is two
        // contiguous blocks (k<0 wraps to the grid top, k>=0 to the bottom) => no modulo.
        const crd::usize half = m_nmodes / 2;
        const crd::usize top = m_n - half;
        for (crd::usize idx = 0; idx < half; ++idx) // k = -N/2..-1  =>  gi = n - N/2 + idx
        {
            const T d = m_dk[idx];
            f[idx] = Complex<T>{g[top + idx].re * d, g[top + idx].im * d};
        }
        for (crd::usize idx = half; idx < m_nmodes; ++idx) // k = 0..N/2-1  =>  gi = idx - N/2
        {
            const T d = m_dk[idx];
            const crd::usize gi = idx - half;
            f[idx] = Complex<T>{g[gi].re * d, g[gi].im * d};
        }
#ifdef CRD_NUFFT_PROFILE
        const auto t_end = std::chrono::steady_clock::now();
        auto us = [](auto a, auto b)
        {
            return std::chrono::duration<double, std::micro>(b - a).count();
        };
        std::fprintf(stderr, "[nufft-prof t1 n=%zu m=%zu] zero+spread=%.1fus fft=%.1fus deconv=%.1fus\n", m_n, m_npts,
                     us(t_spread0, t_fft0), us(t_fft0, t_deconv0), us(t_deconv0, t_end));
#endif
    }

    // Type-2: uniform modes f[N] -> nonuniform values c[M]. Deconvolve + zero-pad -> fine inverse FFT ->
    // interpolate (read-only gather, naturally deterministic).
    void type2(crd::containers::ConstSpan<Complex<T>> f, crd::containers::Span<Complex<T>> c, int isign) const
    {
        CRD_ASSERT(m_have_points && f.size() == m_nmodes && c.size() == m_npts);
        Complex<T>* g = m_grid.data();
        std::memset(static_cast<void*>(g), 0, m_n * sizeof(Complex<T>)); // zero the fine grid; deconvolved modes scattered below
        const crd::usize half = m_nmodes / 2;
        const crd::usize top = m_n - half;
        for (crd::usize idx = 0; idx < half; ++idx) // k = -N/2..-1  =>  gi = n - N/2 + idx (grid top)
        {
            const T d = m_dk[idx];
            g[top + idx] = Complex<T>{f[idx].re * d, f[idx].im * d};
        }
        for (crd::usize idx = half; idx < m_nmodes; ++idx) // k = 0..N/2-1  =>  gi = idx - N/2 (grid bottom)
        {
            const T d = m_dk[idx];
            g[idx - half] = Complex<T>{f[idx].re * d, f[idx].im * d};
        }
        // Same sign mapping as type-1: isign=+1 => exp(+i...) grid synthesis.
        const FftDirection dir = (isign >= 0) ? FftDirection::Inverse : FftDirection::Forward;
        m_fft.execute(crd::containers::Span<Complex<T>>(g, m_n), dir);
        const crd::i64 n_i = static_cast<crd::i64>(m_n);
        const crd::i64 w_i = static_cast<crd::i64>(m_w);
        for (crd::usize j = 0; j < m_npts; ++j)
        {
            const crd::i64 i1 = m_i1[j];
            const T* kw = m_kw.data() + j * m_w;
            T re = static_cast<T>(0);
            T im = static_cast<T>(0);
            if (i1 >= 0 && i1 + w_i <= n_i) // fast path: contiguous in-range gather (no wrap, no modulo)
            {
                const Complex<T>* gp = g + i1;
                // 4 independent accumulator lanes break the serial reduction dependency chain (each `re +=`
                // otherwise waits a full FMA latency on the prior add) — the interp's gather is a reduction,
                // unlike the spread's independent scatter writes. Deterministic run-to-run (fixed order).
                T re0 = re, re1 = 0, re2 = 0, re3 = 0;
                T im0 = im, im1 = 0, im2 = 0, im3 = 0;
                crd::usize l = 0;
                for (; l + 4 <= m_w; l += 4)
                {
                    re0 += gp[l].re * kw[l];
                    im0 += gp[l].im * kw[l];
                    re1 += gp[l + 1].re * kw[l + 1];
                    im1 += gp[l + 1].im * kw[l + 1];
                    re2 += gp[l + 2].re * kw[l + 2];
                    im2 += gp[l + 2].im * kw[l + 2];
                    re3 += gp[l + 3].re * kw[l + 3];
                    im3 += gp[l + 3].im * kw[l + 3];
                }
                for (; l < m_w; ++l) // tail (w not a multiple of 4)
                {
                    re0 += gp[l].re * kw[l];
                    im0 += gp[l].im * kw[l];
                }
                re = (re0 + re1) + (re2 + re3);
                im = (im0 + im1) + (im2 + im3);
            }
            else
            {
                for (crd::usize l = 0; l < m_w; ++l)
                {
                    const crd::usize gi = wrap_index(i1 + static_cast<crd::i64>(l));
                    const T wgt = kw[l];
                    re += g[gi].re * wgt;
                    im += g[gi].im * wgt;
                }
            }
            c[j] = Complex<T>{re, im};
        }
    }

    // -------- direct O(N*M) references (the CORRECTNESS GATE — exact, no kernel) -------------------------

    // Direct type-1: f_k = sum_j c_j exp(isign*i*k*x_j). Cross-checks type1() to the kernel's tolerance.
    void direct_type1(crd::containers::ConstSpan<Complex<T>> c, crd::containers::Span<Complex<T>> f, int isign) const
    {
        CRD_ASSERT(m_have_points && c.size() == m_npts && f.size() == m_nmodes);
        const double s = (isign >= 0) ? 1.0 : -1.0;
        const crd::usize half = m_nmodes / 2;
        for (crd::usize idx = 0; idx < m_nmodes; ++idx)
        {
            const double k = static_cast<double>(static_cast<crd::i64>(idx) - static_cast<crd::i64>(half));
            double re = 0.0;
            double im = 0.0;
            for (crd::usize j = 0; j < m_npts; ++j)
            {
                const double ang = s * k * static_cast<double>(m_x[j]);
                const double cr = std::cos(ang);
                const double ci = std::sin(ang);
                re += static_cast<double>(c[j].re) * cr - static_cast<double>(c[j].im) * ci;
                im += static_cast<double>(c[j].re) * ci + static_cast<double>(c[j].im) * cr;
            }
            f[idx] = Complex<T>{static_cast<T>(re), static_cast<T>(im)};
        }
    }

    // Direct type-2: c_j = sum_k f_k exp(isign*i*k*x_j).
    void direct_type2(crd::containers::ConstSpan<Complex<T>> f, crd::containers::Span<Complex<T>> c, int isign) const
    {
        CRD_ASSERT(m_have_points && f.size() == m_nmodes && c.size() == m_npts);
        const double s = (isign >= 0) ? 1.0 : -1.0;
        const crd::usize half = m_nmodes / 2;
        for (crd::usize j = 0; j < m_npts; ++j)
        {
            const double xj = static_cast<double>(m_x[j]);
            double re = 0.0;
            double im = 0.0;
            for (crd::usize idx = 0; idx < m_nmodes; ++idx)
            {
                const double k = static_cast<double>(static_cast<crd::i64>(idx) - static_cast<crd::i64>(half));
                const double ang = s * k * xj;
                const double cr = std::cos(ang);
                const double ci = std::sin(ang);
                re += static_cast<double>(f[idx].re) * cr - static_cast<double>(f[idx].im) * ci;
                im += static_cast<double>(f[idx].re) * ci + static_cast<double>(f[idx].im) * cr;
            }
            c[j] = Complex<T>{static_cast<T>(re), static_cast<T>(im)};
        }
    }

private:
    static constexpr crd::i64 kMaxWidth = 16;

    [[nodiscard]] static crd::usize fine_size(crd::usize n_modes, double sigma) noexcept
    {
        const double want = sigma * static_cast<double>(n_modes);
        crd::usize n = 1;
        while (static_cast<double>(n) < want)
        {
            n <<= 1; // round UP to a power of two for the reused FftPlan
        }
        return n;
    }

    // ES kernel on the canonical argument t = 2z/w in [-1, 1]; phi(t) = exp(beta*(sqrt(1-t^2) - 1)).
    [[nodiscard]] double es_kernel(double t) const noexcept
    {
        const double s = 1.0 - t * t;
        if (s <= 0.0)
        {
            return std::exp(-m_beta); // |t| >= 1 endpoint: kernel ~ exp(-beta), tiny
        }
        return std::exp(m_beta * (std::sqrt(s) - 1.0));
    }

    // Deconvolution table d_k = 1 / K(k), K(k) = (w/2) integral_{-1}^{1} phi(t) cos(pi*w*k*t/n) dt, via
    // Gauss-Legendre. K(k) is real + even; computed in f64, narrowed to T. (Same role as the FFT twiddle table:
    // precomputed once, shared => deterministic.)
    void build_deconv_table()
    {
        const crd::usize half = m_nmodes / 2;
        m_dk.resize(m_nmodes);
        // GL nodes/weights on [-1,1]; q generous (smooth integrand bar a mild endpoint sqrt-singularity).
        const crd::usize q = static_cast<crd::usize>(2 * m_w + 16);
        crd::containers::Array<double> nodes(m_alloc);
        crd::containers::Array<double> wts(m_alloc);
        nodes.resize(q);
        wts.resize(q);
        gauss_legendre(q, nodes.data(), wts.data());
        const double pwn = 3.14159265358979323846 * static_cast<double>(m_w) / static_cast<double>(m_n);
        for (crd::usize idx = 0; idx < m_nmodes; ++idx)
        {
            const double k = static_cast<double>(static_cast<crd::i64>(idx) - static_cast<crd::i64>(half));
            double acc = 0.0;
            for (crd::usize i = 0; i < q; ++i)
            {
                const double t = nodes[i];
                acc += wts[i] * es_kernel(t) * std::cos(pwn * k * t);
            }
            const double kk = 0.5 * static_cast<double>(m_w) * acc;
            m_dk[idx] = static_cast<T>(1.0 / kk);
        }
    }

    // Gauss-Legendre nodes/weights on [-1,1] via Newton iteration on the Legendre polynomial (standard).
    static void gauss_legendre(crd::usize n, double* x, double* w) noexcept
    {
        constexpr double pi = 3.14159265358979323846;
        const crd::usize m = (n + 1) / 2;
        for (crd::usize i = 0; i < m; ++i)
        {
            double z = std::cos(pi * (static_cast<double>(i) + 0.75) / (static_cast<double>(n) + 0.5));
            double z1 = 0.0;
            double pp = 0.0;
            for (int it = 0; it < 100; ++it)
            {
                double p1 = 1.0;
                double p2 = 0.0;
                for (crd::usize jj = 0; jj < n; ++jj) // Legendre recurrence at z
                {
                    const double p3 = p2;
                    p2 = p1;
                    p1 = ((2.0 * static_cast<double>(jj) + 1.0) * z * p2 - static_cast<double>(jj) * p3) /
                         (static_cast<double>(jj) + 1.0);
                }
                pp = static_cast<double>(n) * (z * p1 - p2) / (z * z - 1.0);
                z1 = z;
                z = z1 - p1 / pp;
                if (std::abs(z - z1) < 1e-15)
                {
                    break;
                }
            }
            x[i] = -z;
            x[n - 1 - i] = z;
            const double ww = 2.0 / ((1.0 - z * z) * pp * pp);
            w[i] = ww;
            w[n - 1 - i] = ww;
        }
    }

    [[nodiscard]] crd::usize wrap_index(crd::i64 g) const noexcept
    {
        const crd::i64 n = static_cast<crd::i64>(m_n);
        crd::i64 r = g % n;
        if (r < 0)
        {
            r += n;
        }
        return static_cast<crd::usize>(r);
    }

    crd::memory::IAllocator* m_alloc;
    crd::usize m_nmodes;
    crd::usize m_npts;
    crd::usize m_n;                                    // fine grid size (power of two)
    crd::usize m_w;                                    // kernel half-width (grid points)
    double m_beta;                                     // ES kernel shape
    mutable FftPlan<T> m_fft;                          // size m_n complex transform (scratch reused)
    crd::containers::Array<T> m_dk;                    // deconvolution factors, one per output mode
    crd::containers::Array<T> m_x;                     // folded points (radians)
    crd::containers::Array<crd::i64> m_i1;             // per-point base fine-grid index
    crd::containers::Array<T> m_kw;                    // per-point kernel weights (M * w), shared by type-1/type-2
    mutable crd::containers::Array<Complex<T>> m_grid; // fine grid scratch
    bool m_have_points;
};

} // namespace crd::hesap::fft
