#pragma once

// multivariate.hpp — Phase 3.1.6 v12-k: multivariate distributions over the shipped Cholesky (SANITY rule 8 — reuse
// crd-hesap-dense's factor_cholesky; the stats→dense edge is acyclic, dense never references stats). Each distribution
// factors its covariance/scale ONCE in the ctor (amortised, like the v12-h lgamma lever) and copies the L factor into
// a flat raw Array<T> so the hot path (logpdf forward-substitution, rvs L·z) is pure raw f64 (ADR-0078 lower layer).
// Distributions: MultivariateNormal · MultivariateT · Dirichlet · Wishart · InverseWishart · LKJ · Multinomial.
// Gold-standard gated vs scipy.stats (+ the analytic LKJ p=2 marginal, which has no scipy). f32/f64 templated.

#include <crd/hesap/stats/distribution.hpp> // Real concept + detail::kLn2Pi/kPi/...
#include <crd/hesap/stats/samplers.hpp>     // gamma_dist / beta_dist / chi_squared / standard_normal / next_double

#include <crd/hesap/dense/cholesky.hpp>     // factor_cholesky + Cholesky<T,L>
#include <crd/hesap/dense/matrix_types.hpp> // Symmetric<T>
#include <crd/hesap/special/gamma.hpp>      // lgamma

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>

#include <crd/math/cmath.hpp>

namespace crd::hesap::stats
{
namespace detail
{
inline constexpr int kMvMaxDim = 128; // stack-scratch cap for the per-sample hot paths
template <Real T>
inline constexpr T kLnPi = static_cast<T>(1.1447298858494001741434273513530587); // kLn2/kPi/kLn2Pi come from distribution.hpp

// Factor a k×k SPD matrix (row-major span, lower triangle read) → flat row-major lower-triangular L (k·k, upper = 0)
// + logdet = Σ 2·ln L_ii, reusing crd-hesap-dense's blocked Cholesky. Returns false if not positive-definite.
template <Real T>
inline bool factor_spd(crd::memory::IAllocator* alloc, crd::containers::Span<const T> a, crd::usize k,
                       crd::containers::Array<T>& l_out, T& logdet)
{
    crd::hesap::dense::Symmetric<T> sym(alloc, k);
    for (crd::usize i = 0; i < k; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            sym.at(i, j) = a[i * k + j];
        }
    }
    crd::hesap::dense::Cholesky<T, crd::hesap::dense::Layout::RowMajor> chol(alloc, k); // RowMajor: the instantiated factor_cholesky
    crd::hesap::dense::factor_cholesky(chol, sym);
    if (chol.is_singular())
    {
        return false;
    }
    l_out.resize(k * k);
    for (crd::usize i = 0; i < k * k; ++i)
    {
        l_out[i] = static_cast<T>(0);
    }
    logdet = static_cast<T>(0);
    const auto& p = chol.packed();
    for (crd::usize i = 0; i < k; ++i)
    {
        for (crd::usize j = 0; j <= i; ++j)
        {
            const T v = p.at(i, j);
            l_out[i * k + j] = v;
            if (i == j)
            {
                logdet += static_cast<T>(2) * crd::math::log(v);
            }
        }
    }
    return true;
}

// Forward substitution L·y = b (L lower-triangular, flat row-major). Returns ‖y‖² (the Mahalanobis form when b=x−μ).
template <Real T>
inline T forward_quadform(const T* l, crd::usize k, const T* b, T* y) noexcept
{
    T q = static_cast<T>(0);
    for (crd::usize i = 0; i < k; ++i)
    {
        T s = b[i];
        for (crd::usize j = 0; j < i; ++j)
        {
            s -= l[i * k + j] * y[j];
        }
        y[i] = s / l[i * k + i];
        q += y[i] * y[i];
    }
    return q;
}

// Solve L·Lᵀ·x = b (full SPD solve via forward then back substitution), flat row-major lower-triangular L.
template <Real T>
inline void spd_solve(const T* l, crd::usize k, const T* b, T* x, T* scratch) noexcept
{
    for (crd::usize i = 0; i < k; ++i) // L·y = b
    {
        T s = b[i];
        for (crd::usize j = 0; j < i; ++j)
        {
            s -= l[i * k + j] * scratch[j];
        }
        scratch[i] = s / l[i * k + i];
    }
    for (crd::usize ii = k; ii-- > 0;) // Lᵀ·x = y
    {
        T s = scratch[ii];
        for (crd::usize j = ii + 1; j < k; ++j)
        {
            s -= l[j * k + ii] * x[j];
        }
        x[ii] = s / l[ii * k + ii];
    }
}

// ln Γ_p(a) — the multivariate log-gamma (Wishart / inverse-Wishart normaliser).
template <Real T>
inline T multigammaln(T a, crd::usize p) noexcept
{
    T s = static_cast<T>(p) * static_cast<T>(p - 1) * static_cast<T>(0.25) * kLnPi<T>;
    for (crd::usize j = 1; j <= p; ++j)
    {
        s += static_cast<T>(special::lgamma(static_cast<double>(a + static_cast<T>(1 - static_cast<int>(j)) * static_cast<T>(0.5))));
    }
    return s;
}
} // namespace detail

// ───────────────────────────── MultivariateNormal(μ, Σ) ─────────────────────────────
template <Real T>
class MultivariateNormal
{
public:
    using value_type = T;

    MultivariateNormal(crd::memory::IAllocator* alloc, crd::containers::Span<const T> mean,
                       crd::containers::Span<const T> cov)
        : m_k(mean.size()), m_mean(alloc), m_l(alloc)
    {
        CRD_ASSERT_MSG(m_k <= detail::kMvMaxDim, "MultivariateNormal: dim exceeds kMvMaxDim");
        CRD_ASSERT_MSG(cov.size() == m_k * m_k, "MultivariateNormal: cov must be k*k row-major");
        m_mean.resize(m_k);
        for (crd::usize i = 0; i < m_k; ++i)
        {
            m_mean[i] = mean[i];
        }
        T logdet = static_cast<T>(0);
        m_ok = detail::factor_spd<T>(alloc, cov, m_k, m_l, logdet);
        m_lognorm = static_cast<T>(-0.5) * (logdet + static_cast<T>(m_k) * detail::kLn2Pi<T>);
        m_logdet = logdet;
    }

    [[nodiscard]] crd::usize dim() const noexcept { return m_k; }
    [[nodiscard]] bool is_valid() const noexcept { return m_ok; }

    [[nodiscard]] T mahalanobis_sq(crd::containers::Span<const T> x) const noexcept
    {
        T y[detail::kMvMaxDim];
        T b[detail::kMvMaxDim];
        for (crd::usize i = 0; i < m_k; ++i)
        {
            b[i] = x[i] - m_mean[i];
        }
        return detail::forward_quadform<T>(m_l.data(), m_k, b, y);
    }

    [[nodiscard]] T logpdf(crd::containers::Span<const T> x) const noexcept
    {
        return m_lognorm - static_cast<T>(0.5) * mahalanobis_sq(x);
    }
    [[nodiscard]] T pdf(crd::containers::Span<const T> x) const noexcept { return crd::math::exp(logpdf(x)); }

    // v12-l: ∇_x log p = −Σ⁻¹(x−μ) — the HMC gradient, via the cached Cholesky (spd_solve = Lᵀ\(L\b)).
    // grad.size() must equal dim(). Pure raw f64 hot path (ADR-0078).
    void dlogpdf_dx(crd::containers::Span<const T> x, crd::containers::Span<T> grad) const noexcept
    {
        T b[detail::kMvMaxDim];
        T scratch[detail::kMvMaxDim];
        for (crd::usize i = 0; i < m_k; ++i)
        {
            b[i] = x[i] - m_mean[i];
        }
        detail::spd_solve<T>(m_l.data(), m_k, b, grad.data(), scratch);
        for (crd::usize i = 0; i < m_k; ++i)
        {
            grad[i] = -grad[i];
        }
    }

    [[nodiscard]] T entropy() const noexcept
    {
        return static_cast<T>(0.5) * static_cast<T>(m_k) * (static_cast<T>(1) + detail::kLn2Pi<T>) +
               static_cast<T>(0.5) * m_logdet;
    }

    template <BitGenerator G>
    void rvs(G& g, crd::containers::Span<T> out) const noexcept
    {
        T z[detail::kMvMaxDim];
        for (crd::usize i = 0; i < m_k; ++i)
        {
            z[i] = static_cast<T>(standard_normal(g));
        }
        for (crd::usize i = 0; i < m_k; ++i) // out = μ + L·z
        {
            T s = m_mean[i];
            for (crd::usize j = 0; j <= i; ++j)
            {
                s += m_l[i * m_k + j] * z[j];
            }
            out[i] = s;
        }
    }

private:
    crd::usize m_k;
    crd::containers::Array<T> m_mean;
    crd::containers::Array<T> m_l; // flat row-major lower-triangular Cholesky factor
    T m_logdet = static_cast<T>(0);
    T m_lognorm = static_cast<T>(0);
    bool m_ok = false;
};

// ───────────────────────────── MultivariateT(μ, Σ, ν) ─── scipy: shape matrix, not covariance ─────────────────────
template <Real T>
class MultivariateT
{
public:
    using value_type = T;

    MultivariateT(crd::memory::IAllocator* alloc, crd::containers::Span<const T> loc,
                  crd::containers::Span<const T> shape, T df)
        : m_k(loc.size()), m_df(df), m_loc(alloc), m_l(alloc)
    {
        CRD_ASSERT_MSG(m_k <= detail::kMvMaxDim, "MultivariateT: dim exceeds kMvMaxDim");
        m_loc.resize(m_k);
        for (crd::usize i = 0; i < m_k; ++i)
        {
            m_loc[i] = loc[i];
        }
        T logdet = static_cast<T>(0);
        m_ok = detail::factor_spd<T>(alloc, shape, m_k, m_l, logdet);
        const T half = static_cast<T>(0.5);
        const T p = static_cast<T>(m_k);
        m_lognorm = static_cast<T>(special::lgamma(static_cast<double>(half * (df + p)))) -
                    static_cast<T>(special::lgamma(static_cast<double>(half * df))) -
                    half * (p * crd::math::log(df * detail::kPi<T>) + logdet);
    }

    [[nodiscard]] crd::usize dim() const noexcept { return m_k; }
    [[nodiscard]] bool is_valid() const noexcept { return m_ok; }

    [[nodiscard]] T logpdf(crd::containers::Span<const T> x) const noexcept
    {
        T y[detail::kMvMaxDim];
        T b[detail::kMvMaxDim];
        for (crd::usize i = 0; i < m_k; ++i)
        {
            b[i] = x[i] - m_loc[i];
        }
        const T q = detail::forward_quadform<T>(m_l.data(), m_k, b, y);
        // log(1+q/ν): the argument ≥ 1 (q≥0) ⇒ no cancellation, so plain log matches log1p to f64 but is ~3× faster.
        return m_lognorm - static_cast<T>(0.5) * (m_df + static_cast<T>(m_k)) * crd::math::log(static_cast<T>(1) + q / m_df);
    }

    // v12-l: ∇_x log p = −(ν+k)/(ν+q)·Σ⁻¹(x−μ),  q = (x−μ)ᵀΣ⁻¹(x−μ) — the HMC gradient for the heavy-tailed prior.
    void dlogpdf_dx(crd::containers::Span<const T> x, crd::containers::Span<T> grad) const noexcept
    {
        T b[detail::kMvMaxDim];
        T scratch[detail::kMvMaxDim];
        for (crd::usize i = 0; i < m_k; ++i)
        {
            b[i] = x[i] - m_loc[i];
        }
        const T q = detail::forward_quadform<T>(m_l.data(), m_k, b, scratch);
        detail::spd_solve<T>(m_l.data(), m_k, b, grad.data(), scratch);
        const T c = -(m_df + static_cast<T>(m_k)) / (m_df + q);
        for (crd::usize i = 0; i < m_k; ++i)
        {
            grad[i] = c * grad[i];
        }
    }
    [[nodiscard]] T pdf(crd::containers::Span<const T> x) const noexcept { return crd::math::exp(logpdf(x)); }

    template <BitGenerator G>
    void rvs(G& g, crd::containers::Span<T> out) const noexcept
    {
        T z[detail::kMvMaxDim];
        for (crd::usize i = 0; i < m_k; ++i)
        {
            z[i] = static_cast<T>(standard_normal(g));
        }
        const T w = static_cast<T>(chi_squared(g, static_cast<double>(m_df))); // X = μ + L·z·√(ν/w)
        const T scale = crd::math::sqrt(m_df / w);
        for (crd::usize i = 0; i < m_k; ++i)
        {
            T s = static_cast<T>(0);
            for (crd::usize j = 0; j <= i; ++j)
            {
                s += m_l[i * m_k + j] * z[j];
            }
            out[i] = m_loc[i] + s * scale;
        }
    }

private:
    crd::usize m_k;
    T m_df;
    crd::containers::Array<T> m_loc;
    crd::containers::Array<T> m_l;
    T m_lognorm = static_cast<T>(0);
    bool m_ok = false;
};

// ───────────────────────────── Dirichlet(α) ─────────────────────────────
template <Real T>
class Dirichlet
{
public:
    using value_type = T;

    Dirichlet(crd::memory::IAllocator* alloc, crd::containers::Span<const T> alpha) : m_k(alpha.size()), m_alpha(alloc)
    {
        m_alpha.resize(m_k);
        m_a0 = static_cast<T>(0);
        T lg = static_cast<T>(0);
        for (crd::usize i = 0; i < m_k; ++i)
        {
            m_alpha[i] = alpha[i];
            m_a0 += alpha[i];
            lg += static_cast<T>(special::lgamma(static_cast<double>(alpha[i])));
        }
        m_lognorm = static_cast<T>(special::lgamma(static_cast<double>(m_a0))) - lg; // amortised normaliser
    }

    [[nodiscard]] crd::usize dim() const noexcept { return m_k; }

    [[nodiscard]] T logpdf(crd::containers::Span<const T> x) const noexcept
    {
        T s = m_lognorm;
        for (crd::usize i = 0; i < m_k; ++i)
        {
            s += (m_alpha[i] - static_cast<T>(1)) * crd::math::log(x[i]);
        }
        return s;
    }
    [[nodiscard]] T pdf(crd::containers::Span<const T> x) const noexcept { return crd::math::exp(logpdf(x)); }

    void mean(crd::containers::Span<T> out) const noexcept
    {
        for (crd::usize i = 0; i < m_k; ++i)
        {
            out[i] = m_alpha[i] / m_a0;
        }
    }

    template <BitGenerator G>
    void rvs(G& g, crd::containers::Span<T> out) const noexcept
    {
        T sum = static_cast<T>(0);
        for (crd::usize i = 0; i < m_k; ++i)
        {
            out[i] = static_cast<T>(gamma_dist(g, static_cast<double>(m_alpha[i]), 1.0));
            sum += out[i];
        }
        for (crd::usize i = 0; i < m_k; ++i)
        {
            out[i] /= sum;
        }
    }

private:
    crd::usize m_k;
    crd::containers::Array<T> m_alpha;
    T m_a0 = static_cast<T>(0);
    T m_lognorm = static_cast<T>(0);
};

// ───────────────────────────── Wishart(ν, V) ─── X is k×k SPD ─────────────────────────────
template <Real T>
class Wishart
{
public:
    using value_type = T;

    Wishart(crd::memory::IAllocator* alloc, T df, crd::containers::Span<const T> scale)
        : m_alloc(alloc), m_k(static_cast<crd::usize>(crd::math::lround(crd::math::sqrt(static_cast<double>(scale.size()))))),
          m_df(df), m_lv(alloc)
    {
        CRD_ASSERT_MSG(m_k <= detail::kMvMaxDim, "Wishart: dim exceeds kMvMaxDim");
        m_ok = detail::factor_spd<T>(alloc, scale, m_k, m_lv, m_logdet_v);
        const T half = static_cast<T>(0.5);
        const T p = static_cast<T>(m_k);
        m_lognorm = -(half * df * p) * detail::kLn2<T> - half * df * m_logdet_v -
                    detail::multigammaln<T>(half * df, m_k);
    }

    [[nodiscard]] crd::usize dim() const noexcept { return m_k; }
    [[nodiscard]] bool is_valid() const noexcept { return m_ok; }

    // logpdf at an SPD matrix X (k×k row-major). log f = (ν−k−1)/2·logdet X − ½·tr(V⁻¹X) + lognorm.
    [[nodiscard]] T logpdf(crd::containers::Span<const T> x) const noexcept
    {
        crd::containers::Array<T> lx(m_alloc);
        T logdet_x = static_cast<T>(0);
        if (!detail::factor_spd<T>(m_alloc, x, m_k, lx, logdet_x))
        {
            return -std::numeric_limits<T>::infinity();
        }
        const T tr = trace_solve(x);
        const T half = static_cast<T>(0.5);
        return (half * (m_df - static_cast<T>(m_k) - static_cast<T>(1))) * logdet_x - half * tr + m_lognorm;
    }
    [[nodiscard]] T pdf(crd::containers::Span<const T> x) const noexcept { return crd::math::exp(logpdf(x)); }

    // rvs via the Bartlett decomposition: A lower-triangular, A_ii = √χ²(ν−i+1) (1-indexed), A_{i>j} ~ N(0,1);
    // then W = (Lv·A)(Lv·A)ᵀ. Output W as k×k row-major.
    template <BitGenerator G>
    void rvs(G& g, crd::containers::Span<T> out) const noexcept
    {
        T a[detail::kMvMaxDim * detail::kMvMaxDim];
        bartlett(g, a);
        T m[detail::kMvMaxDim * detail::kMvMaxDim]; // M = Lv·A (lower-triangular)
        for (crd::usize i = 0; i < m_k; ++i)
        {
            for (crd::usize j = 0; j <= i; ++j)
            {
                T s = static_cast<T>(0);
                for (crd::usize t = j; t <= i; ++t)
                {
                    s += m_lv[i * m_k + t] * a[t * m_k + j];
                }
                m[i * m_k + j] = s;
            }
        }
        for (crd::usize i = 0; i < m_k; ++i) // W = M·Mᵀ
        {
            for (crd::usize j = 0; j < m_k; ++j)
            {
                T s = static_cast<T>(0);
                const crd::usize lim = i < j ? i : j;
                for (crd::usize t = 0; t <= lim; ++t)
                {
                    s += m[i * m_k + t] * m[j * m_k + t];
                }
                out[i * m_k + j] = s;
            }
        }
    }

    [[nodiscard]] T df() const noexcept { return m_df; }
    [[nodiscard]] const crd::containers::Array<T>& scale_factor() const noexcept { return m_lv; }

private:
    template <BitGenerator G>
    void bartlett(G& g, T* a) const noexcept
    {
        for (crd::usize i = 0; i < m_k * m_k; ++i)
        {
            a[i] = static_cast<T>(0);
        }
        for (crd::usize i = 0; i < m_k; ++i)
        {
            a[i * m_k + i] = crd::math::sqrt(static_cast<T>(chi_squared(g, static_cast<double>(m_df - static_cast<T>(i)))));
            for (crd::usize j = 0; j < i; ++j)
            {
                a[i * m_k + j] = static_cast<T>(standard_normal(g));
            }
        }
    }

    // tr(V⁻¹·X) = Σ_c (V⁻¹·X)_cc — solve V·m = X[:,c] column by column, accumulate m[c].
    [[nodiscard]] T trace_solve(crd::containers::Span<const T> x) const noexcept
    {
        T col[detail::kMvMaxDim];
        T sol[detail::kMvMaxDim];
        T scratch[detail::kMvMaxDim];
        T tr = static_cast<T>(0);
        for (crd::usize c = 0; c < m_k; ++c)
        {
            for (crd::usize r = 0; r < m_k; ++r)
            {
                col[r] = x[r * m_k + c];
            }
            detail::spd_solve<T>(m_lv.data(), m_k, col, sol, scratch);
            tr += sol[c];
        }
        return tr;
    }

    crd::memory::IAllocator* m_alloc;
    crd::usize m_k;
    T m_df;
    crd::containers::Array<T> m_lv; // Cholesky factor of the scale V
    T m_logdet_v = static_cast<T>(0);
    T m_lognorm = static_cast<T>(0);
    bool m_ok = false;
};

// ───────────────────────────── InverseWishart(ν, Ψ) ─── X is k×k SPD ─────────────────────────────
// scipy.stats.invwishart convention: log f = ν/2·logdet Ψ − (ν+k+1)/2·logdet X − ½·tr(Ψ·X⁻¹) − νk/2·ln2 − lnΓ_k(ν/2).
template <Real T>
class InverseWishart
{
public:
    using value_type = T;

    InverseWishart(crd::memory::IAllocator* alloc, T df, crd::containers::Span<const T> scale)
        : m_alloc(alloc), m_k(static_cast<crd::usize>(crd::math::lround(crd::math::sqrt(static_cast<double>(scale.size()))))),
          m_df(df), m_psi(alloc), m_lpsi(alloc)
    {
        CRD_ASSERT_MSG(m_k <= detail::kMvMaxDim, "InverseWishart: dim exceeds kMvMaxDim");
        m_psi.resize(m_k * m_k);
        for (crd::usize i = 0; i < m_k * m_k; ++i)
        {
            m_psi[i] = scale[i];
        }
        m_ok = detail::factor_spd<T>(alloc, scale, m_k, m_lpsi, m_logdet_psi);
        const T half = static_cast<T>(0.5);
        const T p = static_cast<T>(m_k);
        m_lognorm = half * df * m_logdet_psi - (half * df * p) * detail::kLn2<T> -
                    detail::multigammaln<T>(half * df, m_k);
    }

    [[nodiscard]] crd::usize dim() const noexcept { return m_k; }
    [[nodiscard]] bool is_valid() const noexcept { return m_ok; }

    [[nodiscard]] T logpdf(crd::containers::Span<const T> x) const noexcept
    {
        crd::containers::Array<T> lx(m_alloc);
        T logdet_x = static_cast<T>(0);
        if (!detail::factor_spd<T>(m_alloc, x, m_k, lx, logdet_x))
        {
            return -std::numeric_limits<T>::infinity();
        }
        const T tr = trace_psi_xinv(lx.data()); // tr(Ψ·X⁻¹), X⁻¹ via the chol of X
        const T half = static_cast<T>(0.5);
        return m_lognorm - half * (m_df + static_cast<T>(m_k) + static_cast<T>(1)) * logdet_x - half * tr;
    }
    [[nodiscard]] T pdf(crd::containers::Span<const T> x) const noexcept { return crd::math::exp(logpdf(x)); }

    // rvs: X = W⁻¹ where W ~ Wishart(ν, Ψ⁻¹). We sample via Bartlett on Ψ's factor then invert (small k).
    template <BitGenerator G>
    void rvs(G& g, crd::containers::Span<T> out) const noexcept
    {
        // Sample C ~ Wishart(ν, I) lower factor, then X = (Lψ⁻ᵀ · A)⁻¹·… — use: X = Lψ · A⁻ᵀ · A⁻¹ · Lψᵀ with
        // A the Bartlett factor of Wishart(ν, I). Equivalent: solve and form. We build via the standard route:
        // sample W = Wishart(ν, Ψ⁻¹) then invert. Here we directly form X = Lψ (A Aᵀ)⁻¹ Lψᵀ.
        T a[detail::kMvMaxDim * detail::kMvMaxDim];
        bartlett_identity(g, a); // A = Bartlett factor of Wishart(ν, I): A Aᵀ ~ Wishart(ν,I)
        // B = A⁻¹ (lower-triangular inverse)
        T binv[detail::kMvMaxDim * detail::kMvMaxDim];
        invert_lower(a, binv);
        // S = Bᵀ·B = (A Aᵀ)⁻¹ ; then X = Lψ · S · Lψᵀ
        T s[detail::kMvMaxDim * detail::kMvMaxDim];
        for (crd::usize i = 0; i < m_k; ++i)
        {
            for (crd::usize j = 0; j < m_k; ++j)
            {
                T acc = static_cast<T>(0);
                for (crd::usize t = (i > j ? i : j); t < m_k; ++t)
                {
                    acc += binv[t * m_k + i] * binv[t * m_k + j];
                }
                s[i * m_k + j] = acc;
            }
        }
        // M = Lψ · S
        T m[detail::kMvMaxDim * detail::kMvMaxDim];
        for (crd::usize i = 0; i < m_k; ++i)
        {
            for (crd::usize j = 0; j < m_k; ++j)
            {
                T acc = static_cast<T>(0);
                for (crd::usize t = 0; t <= i; ++t)
                {
                    acc += m_lpsi[i * m_k + t] * s[t * m_k + j];
                }
                m[i * m_k + j] = acc;
            }
        }
        // X = M · Lψᵀ
        for (crd::usize i = 0; i < m_k; ++i)
        {
            for (crd::usize j = 0; j < m_k; ++j)
            {
                T acc = static_cast<T>(0);
                for (crd::usize t = 0; t <= j; ++t)
                {
                    acc += m[i * m_k + t] * m_lpsi[j * m_k + t];
                }
                out[i * m_k + j] = acc;
            }
        }
    }

private:
    template <BitGenerator G>
    void bartlett_identity(G& g, T* a) const noexcept
    {
        for (crd::usize i = 0; i < m_k * m_k; ++i)
        {
            a[i] = static_cast<T>(0);
        }
        for (crd::usize i = 0; i < m_k; ++i)
        {
            a[i * m_k + i] = crd::math::sqrt(static_cast<T>(chi_squared(g, static_cast<double>(m_df - static_cast<T>(i)))));
            for (crd::usize j = 0; j < i; ++j)
            {
                a[i * m_k + j] = static_cast<T>(standard_normal(g));
            }
        }
    }
    void invert_lower(const T* a, T* inv) const noexcept // inverse of a lower-triangular matrix
    {
        for (crd::usize i = 0; i < m_k * m_k; ++i)
        {
            inv[i] = static_cast<T>(0);
        }
        for (crd::usize i = 0; i < m_k; ++i)
        {
            inv[i * m_k + i] = static_cast<T>(1) / a[i * m_k + i];
            for (crd::usize j = 0; j < i; ++j)
            {
                T s = static_cast<T>(0);
                for (crd::usize t = j; t < i; ++t)
                {
                    s += a[i * m_k + t] * inv[t * m_k + j];
                }
                inv[i * m_k + j] = -s / a[i * m_k + i];
            }
        }
    }
    [[nodiscard]] T trace_psi_xinv(const T* lx) const noexcept // tr(Ψ·X⁻¹), X = Lx·Lxᵀ
    {
        // X⁻¹ column c = spd_solve(Lx, e_c). tr(Ψ X⁻¹) = Σ_{r,c} Ψ_{rc} (X⁻¹)_{cr} = Σ_c (Ψ·X⁻¹)_{cc}.
        T ec[detail::kMvMaxDim];
        T xic[detail::kMvMaxDim];
        T scratch[detail::kMvMaxDim];
        T tr = static_cast<T>(0);
        for (crd::usize c = 0; c < m_k; ++c)
        {
            for (crd::usize r = 0; r < m_k; ++r)
            {
                ec[r] = (r == c) ? static_cast<T>(1) : static_cast<T>(0);
            }
            detail::spd_solve<T>(lx, m_k, ec, xic, scratch); // xic = X⁻¹ column c
            for (crd::usize r = 0; r < m_k; ++r)
            {
                tr += m_psi[c * m_k + r] * xic[r]; // (Ψ X⁻¹)_{cc} = Σ_r Ψ_{cr} (X⁻¹)_{rc}
            }
        }
        return tr;
    }

    crd::memory::IAllocator* m_alloc;
    crd::usize m_k;
    T m_df;
    crd::containers::Array<T> m_psi;  // the scale matrix Ψ (row-major)
    crd::containers::Array<T> m_lpsi; // Cholesky factor of Ψ
    T m_logdet_psi = static_cast<T>(0);
    T m_lognorm = static_cast<T>(0);
    bool m_ok = false;
};

// ───────────────────────────── LKJ correlation (η) ─── density ∝ det(R)^(η−1) ─────────────────────────────
// Normalising constant (Lewandowski-Kurowicka-Joe 2009, Thm 5; verified against the k=2 analytic marginal).
template <Real T>
class LKJ
{
public:
    using value_type = T;

    LKJ(T eta, crd::usize k) noexcept : m_k(k), m_eta(eta)
    {
        const T half = static_cast<T>(0.5);
        T c = static_cast<T>(0);
        for (crd::usize kk = 1; kk < k; ++kk)
        {
            const T km = static_cast<T>(k - kk); // (K−k)
            c += (static_cast<T>(2) * eta - static_cast<T>(2) + km) * km * detail::kLn2<T>;
            const T a = eta + half * static_cast<T>(static_cast<int>(k) - 1 - static_cast<int>(kk)); // η+(K−1−k)/2
            c += km * (static_cast<T>(2) * static_cast<T>(special::lgamma(static_cast<double>(a))) -
                       static_cast<T>(special::lgamma(static_cast<double>(static_cast<T>(2) * a))));
        }
        m_log_c = c; // log of the normalising constant Z; logpdf = (η−1)·logdet R − log Z
    }

    [[nodiscard]] crd::usize dim() const noexcept { return m_k; }
    [[nodiscard]] T log_norm_const() const noexcept { return m_log_c; }

    // logpdf at a correlation matrix R (k×k row-major, unit diagonal, SPD).
    [[nodiscard]] T logpdf(crd::memory::IAllocator* alloc, crd::containers::Span<const T> r) const noexcept
    {
        crd::containers::Array<T> lr(alloc);
        T logdet = static_cast<T>(0);
        if (!detail::factor_spd<T>(alloc, r, m_k, lr, logdet))
        {
            return -std::numeric_limits<T>::infinity();
        }
        return (m_eta - static_cast<T>(1)) * logdet - m_log_c;
    }

    // rvs via the onion method (Lewandowski-Kurowicka-Joe). Output R as k×k row-major correlation matrix.
    template <BitGenerator G>
    void rvs(G& g, crd::containers::Span<T> out) const noexcept
    {
        for (crd::usize i = 0; i < m_k * m_k; ++i)
        {
            out[i] = static_cast<T>(0);
        }
        out[0] = static_cast<T>(1);
        if (m_k == 1)
        {
            return;
        }
        T beta = m_eta + static_cast<T>(m_k - 2) * static_cast<T>(0.5);
        // first 2×2 correlation
        T r12 = static_cast<T>(2) * static_cast<T>(beta_dist(g, static_cast<double>(beta), static_cast<double>(beta))) -
                static_cast<T>(1);
        out[0 * m_k + 0] = static_cast<T>(1);
        out[1 * m_k + 1] = static_cast<T>(1);
        out[0 * m_k + 1] = r12;
        out[1 * m_k + 0] = r12;
        for (crd::usize kk = 2; kk < m_k; ++kk) // extend from kk×kk to (kk+1)×(kk+1)
        {
            beta -= static_cast<T>(0.5);
            const T y = static_cast<T>(beta_dist(g, 0.5 * static_cast<double>(kk), static_cast<double>(beta)));
            T u[detail::kMvMaxDim];
            T nrm = static_cast<T>(0);
            for (crd::usize i = 0; i < kk; ++i)
            {
                u[i] = static_cast<T>(standard_normal(g));
                nrm += u[i] * u[i];
            }
            const T inv = crd::math::sqrt(y) / crd::math::sqrt(nrm);
            for (crd::usize i = 0; i < kk; ++i)
            {
                u[i] *= inv; // w = √y · (u/‖u‖)
            }
            // L = chol of the current kk×kk principal block; z = L·w are the new correlations
            T lblk[detail::kMvMaxDim * detail::kMvMaxDim];
            chol_block(out, kk, lblk);
            for (crd::usize i = 0; i < kk; ++i)
            {
                T s = static_cast<T>(0);
                for (crd::usize j = 0; j <= i; ++j)
                {
                    s += lblk[i * kk + j] * u[j];
                }
                out[i * m_k + kk] = s;
                out[kk * m_k + i] = s;
            }
            out[kk * m_k + kk] = static_cast<T>(1);
        }
    }

private:
    void chol_block(crd::containers::Span<const T> r, crd::usize n, T* l) const noexcept
    {
        for (crd::usize i = 0; i < n * n; ++i)
        {
            l[i] = static_cast<T>(0);
        }
        for (crd::usize j = 0; j < n; ++j)
        {
            T d = r[j * m_k + j];
            for (crd::usize s = 0; s < j; ++s)
            {
                d -= l[j * n + s] * l[j * n + s];
            }
            const T ljj = crd::math::sqrt(d);
            l[j * n + j] = ljj;
            for (crd::usize i = j + 1; i < n; ++i)
            {
                T v = r[i * m_k + j];
                for (crd::usize s = 0; s < j; ++s)
                {
                    v -= l[i * n + s] * l[j * n + s];
                }
                l[i * n + j] = v / ljj;
            }
        }
    }

    crd::usize m_k;
    T m_eta;
    T m_log_c = static_cast<T>(0);
};

// ───────────────────────────── Multinomial(n, p) ─────────────────────────────
template <Real T>
class Multinomial
{
public:
    using value_type = T;

    Multinomial(crd::memory::IAllocator* alloc, crd::i64 n, crd::containers::Span<const T> p)
        : m_k(p.size()), m_n(n), m_p(alloc), m_logp(alloc)
    {
        m_p.resize(m_k);
        m_logp.resize(m_k);
        for (crd::usize i = 0; i < m_k; ++i)
        {
            m_p[i] = p[i];
            m_logp[i] = crd::math::log(p[i]);
        }
        m_lgn1 = static_cast<T>(special::lgamma(static_cast<double>(n) + 1.0));
    }

    [[nodiscard]] crd::usize dim() const noexcept { return m_k; }

    [[nodiscard]] T logpmf(crd::containers::Span<const T> x) const noexcept
    {
        T s = m_lgn1;
        for (crd::usize i = 0; i < m_k; ++i)
        {
            s += x[i] * m_logp[i] - static_cast<T>(special::lgamma(static_cast<double>(x[i]) + 1.0));
        }
        return s;
    }
    [[nodiscard]] T pmf(crd::containers::Span<const T> x) const noexcept { return crd::math::exp(logpmf(x)); }

    template <BitGenerator G>
    void rvs(G& g, crd::containers::Span<T> out) const noexcept
    {
        crd::i64 remaining = m_n; // conditional-binomial construction
        T premaining = static_cast<T>(1);
        for (crd::usize i = 0; i + 1 < m_k; ++i)
        {
            const double pi = premaining > static_cast<T>(0) ? static_cast<double>(m_p[i] / premaining) : 0.0;
            const crd::i64 xi = (remaining > 0) ? binomial(g, remaining, pi > 1.0 ? 1.0 : pi) : 0;
            out[i] = static_cast<T>(xi);
            remaining -= xi;
            premaining -= m_p[i];
        }
        out[m_k - 1] = static_cast<T>(remaining);
    }

private:
    crd::usize m_k;
    crd::i64 m_n;
    crd::containers::Array<T> m_p;
    crd::containers::Array<T> m_logp;
    T m_lgn1 = static_cast<T>(0);
};

} // namespace crd::hesap::stats
