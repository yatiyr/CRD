#pragma once

// crd-hesap-interp v13-d (part 1) — spectral interpolation (Chebyshev + trigonometric/Fourier), over crd-hesap-fft.
//
//   ChebyshevInterpolant — interpolate at the 1st-kind Chebyshev nodes; coefficients via the DCT-II (the v10 fft
//     engine), eval by the backward-stable Clenshaw recurrence. ★ near-minimax, EXPONENTIAL convergence for analytic
//     f; the Chebfun approach. (N must be a power of two — the FFT grid.)
//   TrigInterpolant — band-limited trigonometric interpolation of equispaced PERIODIC data via the rFFT. EXACT for
//     band-limited signals; spectral convergence for smooth periodic f.
//
// Eval is transcendental-light + deterministic (Clenshaw is pure FMUL/FADD; the trig eval uses the deterministic
// crd::math cos/sin). Build rides the shipped FFT/DCT (its precomputed-twiddle determinism moat). Gated by
// exponential convergence to the analytic function + band-limited exactness; benched vs numpy/scipy.

#include <crd/hesap/complex.hpp>
#include <crd/hesap/fft/dct.hpp>
#include <crd/hesap/fft/real_fft.hpp>
#include <crd/hesap/interp/piecewise.hpp>
#include <crd/math/cmath.hpp>

namespace crd::hesap::interp
{

namespace detail
{
inline constexpr double kPi = 3.14159265358979323846;

template <Real T> [[nodiscard]] constexpr bool is_pow2(crd::usize n) noexcept
{
    return n >= 2 && (n & (n - 1)) == 0;
}
} // namespace detail

// The N 1st-kind Chebyshev nodes mapped to [lo,hi], in the DCT-II input order: out[j] = mid + halfw·cos(π(j+½)/N).
// Sample f here (in order), then pass the values to ChebyshevInterpolant::build.
template <Real T> void chebyshev_nodes(crd::usize n, T lo, T hi, crd::containers::Span<T> out) noexcept
{
    const T mid = (lo + hi) / static_cast<T>(2);
    const T halfw = (hi - lo) / static_cast<T>(2);
    for (crd::usize j = 0; j < n; ++j)
    {
        const T th = static_cast<T>(detail::kPi) * (static_cast<T>(j) + static_cast<T>(0.5)) / static_cast<T>(n);
        out[j] = mid + halfw * crd::math::cos(th);
    }
}

template <Real T> class ChebyshevInterpolant
{
public:
    explicit ChebyshevInterpolant(crd::memory::IAllocator* alloc) noexcept : m_alloc(alloc), m_a(alloc) {}

    [[nodiscard]] InterpStatus build(crd::containers::ConstSpan<T> y, T lo, T hi)
    {
        const crd::usize n = y.size();
        if (!detail::is_pow2<T>(n) || !(hi > lo))
        {
            return InterpStatus::BadInput;
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            if (!detail::is_finite(y[i]))
            {
                return InterpStatus::BadInput;
            }
        }
        m_lo = lo;
        m_hi = hi;
        m_a.resize(n);
        crd::hesap::fft::DctPlan<T> plan(m_alloc, n);
        crd::containers::Array<T> dctc(m_alloc);
        dctc.resize(n);
        plan.dct2(y, crd::containers::Span<T>{dctc.data(), n}); // dct2 = 2·(standard DCT-II)
        m_a[0] = dctc[0] / (static_cast<T>(2) * static_cast<T>(n));
        for (crd::usize j = 1; j < n; ++j)
        {
            m_a[j] = dctc[j] / static_cast<T>(n);
        }
        return InterpStatus::Ok;
    }

    [[nodiscard]] T eval(T x) const noexcept
    {
        const crd::usize n = m_a.size();
        const T xi = (static_cast<T>(2) * x - m_lo - m_hi) / (m_hi - m_lo); // map to [-1,1]
        T b1 = static_cast<T>(0);
        T b2 = static_cast<T>(0);
        for (crd::usize j = n - 1; j >= 1; --j) // Clenshaw (backward-stable)
        {
            const T b0 = static_cast<T>(2) * xi * b1 - b2 + m_a[j];
            b2 = b1;
            b1 = b0;
        }
        return m_a[0] + xi * b1 - b2;
    }

    // Batch eval (resampling): the Clenshaw recurrence is the OUTER loop (over coefficients), the point loop is INNER
    // and point-independent ⇒ auto-vectorizes (4-wide AVX2 for f64). `b1`/`b2` are caller scratch (length xq.size()).
    // Bit-identical to eval() (same FMUL/FADD order; -ffp-contract=off). Allocation-free, noexcept.
    void eval_batch(crd::containers::ConstSpan<T> xq, crd::containers::Span<T> out, crd::containers::Span<T> b1,
                    crd::containers::Span<T> b2) const noexcept
    {
        const crd::usize m = xq.size();
        const crd::usize n = m_a.size();
        for (crd::usize i = 0; i < m; ++i)
        {
            out[i] = (static_cast<T>(2) * xq[i] - m_lo - m_hi) / (m_hi - m_lo); // ξ — identical form to eval()
            b1[i] = static_cast<T>(0);
            b2[i] = static_cast<T>(0);
        }
        for (crd::usize j = n - 1; j >= 1; --j)
        {
            const T aj = m_a[j];
            for (crd::usize i = 0; i < m; ++i) // vectorizes (point-independent)
            {
                const T b0 = static_cast<T>(2) * out[i] * b1[i] - b2[i] + aj;
                b2[i] = b1[i];
                b1[i] = b0;
            }
        }
        const T a0 = m_a[0];
        for (crd::usize i = 0; i < m; ++i)
        {
            out[i] = a0 + out[i] * b1[i] - b2[i];
        }
    }

    [[nodiscard]] crd::containers::ConstSpan<T> coefficients() const noexcept
    {
        return crd::containers::ConstSpan<T>{m_a.data(), m_a.size()};
    }

private:
    crd::memory::IAllocator* m_alloc;
    T m_lo = static_cast<T>(0);
    T m_hi = static_cast<T>(1);
    crd::containers::Array<T> m_a;
};

template <Real T> class TrigInterpolant
{
public:
    explicit TrigInterpolant(crd::memory::IAllocator* alloc) noexcept : m_alloc(alloc), m_re(alloc), m_im(alloc) {}

    // y[k] = f at x_k = lo + (hi−lo)·k/N (periodic with period hi−lo), k=0..N−1. N power of two.
    [[nodiscard]] InterpStatus build(crd::containers::ConstSpan<T> y, T lo, T hi)
    {
        const crd::usize n = y.size();
        if (!detail::is_pow2<T>(n) || !(hi > lo))
        {
            return InterpStatus::BadInput;
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            if (!detail::is_finite(y[i]))
            {
                return InterpStatus::BadInput;
            }
        }
        m_lo = lo;
        m_period = hi - lo;
        m_n = n;
        const crd::usize h = n / 2;
        m_re.resize(h + 1);
        m_im.resize(h + 1);
        crd::hesap::fft::RealFftPlan<T> plan(m_alloc, n);
        crd::containers::Array<crd::hesap::Complex<T>> spec(m_alloc);
        spec.resize(h + 1);
        plan.rfft(y, crd::containers::Span<crd::hesap::Complex<T>>{spec.data(), h + 1});
        for (crd::usize j = 0; j <= h; ++j)
        {
            m_re[j] = spec[j].re;
            m_im[j] = spec[j].im;
        }
        return InterpStatus::Ok;
    }

    // p(x) = (1/N)[F₀ + 2 Σ_{j=1}^{N/2−1} Re(F_j e^{iωjx'}) + F_{N/2} cos(ω·(N/2)·x')], ω=2π/period, x'=x−lo.
    [[nodiscard]] T eval(T x) const noexcept
    {
        const crd::usize n = m_n;
        const crd::usize h = n / 2;
        const T omega = static_cast<T>(2) * static_cast<T>(detail::kPi) / m_period;
        const T xp = x - m_lo;
        T sum = m_re[0];
        for (crd::usize j = 1; j < h; ++j)
        {
            const T ang = static_cast<T>(j) * omega * xp;
            sum += static_cast<T>(2) * (m_re[j] * crd::math::cos(ang) - m_im[j] * crd::math::sin(ang));
        }
        sum += m_re[h] * crd::math::cos(static_cast<T>(h) * omega * xp); // Nyquist (F_{N/2} real)
        return sum / static_cast<T>(n);
    }

private:
    crd::memory::IAllocator* m_alloc;
    T m_lo = static_cast<T>(0);
    T m_period = static_cast<T>(1);
    crd::usize m_n = 0;
    crd::containers::Array<T> m_re;
    crd::containers::Array<T> m_im;
};

} // namespace crd::hesap::interp
