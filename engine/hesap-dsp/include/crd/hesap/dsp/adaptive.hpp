#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp v11-t — adaptive filters (the streaming LOWER layer).
//
//   LmsFilter      least-mean-squares: w += mu·e·x
//   NlmsFilter     normalized LMS: w += mu/(eps+‖x‖²)·e·x  (step-size robust)
//   SignLmsFilter  sign-LMS: w += mu·sign(e)·x  (cheap, robust to outliers)
//   RlsFilter      recursive least squares (exponential forgetting λ) — O(m²)/sample
//   wiener_hopf    the batch optimal Wiener solution R w = p (normal equations)
//
// Each streaming filter is an allocation-free stateful kernel (`step(x, d)` →
// output, updating the weights) — the system-ID / equalization / echo-cancel
// hot loop, fixed per-sample FP order ⇒ run-twice bit-deterministic (the moat).
// Gate: known-plant recovery (the adaptive filter converges to a planted FIR) +
// Wiener-Hopf vs the normal-equation solution. HOT-PATH ⇒ benchmarked vs
// liquid-dsp + MATLAB. Lower-layer raw scalars.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::dsp
{

namespace detail
{
// Shift a length-m delay line one sample (x[0] = newest) and insert the new sample.
template <typename T> inline void push_delay(T* x, crd::usize m, T xnew) noexcept
{
    for (crd::usize i = m - 1; i > 0; --i)
    {
        x[i] = x[i - 1];
    }
    x[0] = xnew;
}
template <typename T> [[nodiscard]] inline T dot(const T* a, const T* b, crd::usize m) noexcept
{
    T s = T(0);
    for (crd::usize i = 0; i < m; ++i)
    {
        s += a[i] * b[i];
    }
    return s;
}
} // namespace detail

template <typename T> class LmsFilter
{
public:
    LmsFilter(crd::memory::IAllocator* alloc, crd::usize m, T mu) : m_w(alloc), m_x(alloc), m_m(m), m_mu(mu)
    {
        m_w.resize(m);
        m_x.resize(m);
        for (crd::usize i = 0; i < m; ++i)
        {
            m_w[i] = T(0);
            m_x[i] = T(0);
        }
    }
    // Process one (input, desired) sample: returns the filter output, then adapts the weights toward `d`.
    T step(T x, T d)
    {
        detail::push_delay<T>(m_x.data(), m_m, x);
        const T y = detail::dot<T>(m_w.data(), m_x.data(), m_m);
        const T e = d - y;
        const T g = m_mu * e;
        for (crd::usize i = 0; i < m_m; ++i)
        {
            m_w[i] += g * m_x[i];
        }
        return y;
    }
    [[nodiscard]] crd::containers::ConstSpan<T> weights() const { return {m_w.data(), m_m}; }

private:
    crd::containers::Array<T> m_w, m_x;
    crd::usize m_m;
    T m_mu;
};

template <typename T> class NlmsFilter
{
public:
    NlmsFilter(crd::memory::IAllocator* alloc, crd::usize m, T mu, T eps = static_cast<T>(1e-6))
        : m_w(alloc), m_x(alloc), m_m(m), m_mu(mu), m_eps(eps)
    {
        m_w.resize(m);
        m_x.resize(m);
        for (crd::usize i = 0; i < m; ++i)
        {
            m_w[i] = T(0);
            m_x[i] = T(0);
        }
    }
    T step(T x, T d)
    {
        detail::push_delay<T>(m_x.data(), m_m, x);
        const T y = detail::dot<T>(m_w.data(), m_x.data(), m_m);
        const T e = d - y;
        const T power = detail::dot<T>(m_x.data(), m_x.data(), m_m);
        const T g = m_mu * e / (m_eps + power);
        for (crd::usize i = 0; i < m_m; ++i)
        {
            m_w[i] += g * m_x[i];
        }
        return y;
    }
    [[nodiscard]] crd::containers::ConstSpan<T> weights() const { return {m_w.data(), m_m}; }

private:
    crd::containers::Array<T> m_w, m_x;
    crd::usize m_m;
    T m_mu, m_eps;
};

template <typename T> class SignLmsFilter
{
public:
    SignLmsFilter(crd::memory::IAllocator* alloc, crd::usize m, T mu) : m_w(alloc), m_x(alloc), m_m(m), m_mu(mu)
    {
        m_w.resize(m);
        m_x.resize(m);
        for (crd::usize i = 0; i < m; ++i)
        {
            m_w[i] = T(0);
            m_x[i] = T(0);
        }
    }
    T step(T x, T d)
    {
        detail::push_delay<T>(m_x.data(), m_m, x);
        const T y = detail::dot<T>(m_w.data(), m_x.data(), m_m);
        const T e = d - y;
        const T s = (e > T(0)) ? T(1) : ((e < T(0)) ? T(-1) : T(0));
        const T g = m_mu * s;
        for (crd::usize i = 0; i < m_m; ++i)
        {
            m_w[i] += g * m_x[i];
        }
        return y;
    }
    [[nodiscard]] crd::containers::ConstSpan<T> weights() const { return {m_w.data(), m_m}; }

private:
    crd::containers::Array<T> m_w, m_x;
    crd::usize m_m;
    T m_mu;
};

template <typename T> class RlsFilter
{
public:
    RlsFilter(crd::memory::IAllocator* alloc, crd::usize m, T lambda, T delta = static_cast<T>(100))
        : m_w(alloc), m_x(alloc), m_p(alloc), m_pi(alloc), m_k(alloc), m_m(m), m_lambda(lambda)
    {
        m_w.resize(m);
        m_x.resize(m);
        m_pi.resize(m);
        m_k.resize(m);
        m_p.resize(m * m);
        for (crd::usize i = 0; i < m; ++i)
        {
            m_w[i] = T(0);
            m_x[i] = T(0);
        }
        for (crd::usize i = 0; i < m * m; ++i)
        {
            m_p[i] = T(0);
        }
        for (crd::usize i = 0; i < m; ++i)
        {
            m_p[i * m + i] = delta; // P0 = delta·I (large ⇒ fast initial adaptation)
        }
    }
    T step(T x, T d)
    {
        detail::push_delay<T>(m_x.data(), m_m, x);
        const T* xb = m_x.data();
        T* pi = m_pi.data();
        // pi = P · x
        for (crd::usize i = 0; i < m_m; ++i)
        {
            pi[i] = detail::dot<T>(&m_p[i * m_m], xb, m_m);
        }
        const T denom = m_lambda + detail::dot<T>(xb, pi, m_m);
        T* k = m_k.data();
        for (crd::usize i = 0; i < m_m; ++i)
        {
            k[i] = pi[i] / denom; // gain
        }
        const T y = detail::dot<T>(m_w.data(), xb, m_m);
        const T e = d - y;
        for (crd::usize i = 0; i < m_m; ++i)
        {
            m_w[i] += k[i] * e;
        }
        // P = (P - k·piᵀ) / lambda
        const T inv_l = T(1) / m_lambda;
        for (crd::usize i = 0; i < m_m; ++i)
        {
            for (crd::usize j = 0; j < m_m; ++j)
            {
                m_p[i * m_m + j] = (m_p[i * m_m + j] - k[i] * pi[j]) * inv_l;
            }
        }
        return y;
    }
    [[nodiscard]] crd::containers::ConstSpan<T> weights() const { return {m_w.data(), m_m}; }

private:
    crd::containers::Array<T> m_w, m_x, m_p, m_pi, m_k;
    crd::usize m_m;
    T m_lambda;
};

// Affine-projection algorithm (APA), projection order p_order: reuses the last p_order regressors ⇒ faster
// convergence than NLMS (which is APA with p_order=1) on correlated input. Per sample: a p_order×p_order solve.
template <typename T> class ApFilter
{
public:
    ApFilter(crd::memory::IAllocator* alloc, crd::usize m, crd::usize p_order, T mu, T delta = static_cast<T>(1e-3))
        : m_w(alloc), m_x(alloc), m_u(alloc), m_d(alloc), m_g(alloc), m_e(alloc), m_a(alloc), m_m(m), m_p(p_order),
          m_mu(mu), m_delta(delta)
    {
        m_w.resize(m);
        m_x.resize(m);
        m_u.resize(m * p_order); // p_order most-recent regressors, column j = regressor at lag j (m-vector)
        m_d.resize(p_order);     // the p_order most-recent desired samples
        m_g.resize(p_order);
        m_e.resize(p_order);
        m_a.resize(p_order * p_order);
        for (crd::usize i = 0; i < m; ++i)
        {
            m_w[i] = T(0);
            m_x[i] = T(0);
        }
        for (crd::usize i = 0; i < m * p_order; ++i)
        {
            m_u[i] = T(0);
        }
        for (crd::usize i = 0; i < p_order; ++i)
        {
            m_d[i] = T(0);
        }
    }
    T step(T x, T d)
    {
        detail::push_delay<T>(m_x.data(), m_m, x);
        // shift the regressor / desired history (column 0 = newest).
        for (crd::usize j = m_p - 1; j > 0; --j)
        {
            for (crd::usize i = 0; i < m_m; ++i)
            {
                m_u[j * m_m + i] = m_u[(j - 1) * m_m + i];
            }
            m_d[j] = m_d[j - 1];
        }
        for (crd::usize i = 0; i < m_m; ++i)
        {
            m_u[i] = m_x[i];
        }
        m_d[0] = d;
        const T y = detail::dot<T>(m_w.data(), m_x.data(), m_m);
        // e[k] = d[k] - u_kᵀ w  (the a-priori error over the p_order recent regressors).
        for (crd::usize k = 0; k < m_p; ++k)
        {
            m_e[k] = m_d[k] - detail::dot<T>(&m_u[k * m_m], m_w.data(), m_m);
        }
        // A = UᵀU + δI  (p_order×p_order); solve A g = e; then w += mu·U·g.
        for (crd::usize i = 0; i < m_p; ++i)
        {
            for (crd::usize j = 0; j < m_p; ++j)
            {
                T s = detail::dot<T>(&m_u[i * m_m], &m_u[j * m_m], m_m);
                if (i == j)
                {
                    s += m_delta;
                }
                m_a[i * m_p + j] = s;
            }
            m_g[i] = m_e[i];
        }
        for (crd::usize c = 0; c < m_p; ++c) // Gaussian elimination (p_order small, SPD ⇒ no pivot needed)
        {
            const T piv = m_a[c * m_p + c];
            for (crd::usize i = c + 1; i < m_p; ++i)
            {
                const T f = m_a[i * m_p + c] / piv;
                for (crd::usize j = c; j < m_p; ++j)
                {
                    m_a[i * m_p + j] -= f * m_a[c * m_p + j];
                }
                m_g[i] -= f * m_g[c];
            }
        }
        for (crd::usize ii = m_p; ii-- > 0;)
        {
            T s = m_g[ii];
            for (crd::usize j = ii + 1; j < m_p; ++j)
            {
                s -= m_a[ii * m_p + j] * m_g[j];
            }
            m_g[ii] = s / m_a[ii * m_p + ii];
        }
        for (crd::usize i = 0; i < m_m; ++i) // w += mu · U · g
        {
            T upd = T(0);
            for (crd::usize k = 0; k < m_p; ++k)
            {
                upd += m_u[k * m_m + i] * m_g[k];
            }
            m_w[i] += m_mu * upd;
        }
        return y;
    }
    [[nodiscard]] crd::containers::ConstSpan<T> weights() const { return {m_w.data(), m_m}; }

private:
    crd::containers::Array<T> m_w, m_x, m_u, m_d, m_g, m_e, m_a;
    crd::usize m_m, m_p;
    T m_mu, m_delta;
};

// Wiener-Hopf: the batch optimal length-m FIR (normal equations R w = p) for input x → desired d.
// R = autocorrelation Toeplitz, p = cross-correlation. Solved by Gaussian elimination (m small).
template <typename T>
[[nodiscard]] crd::containers::Array<T> wiener_hopf(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<T> x,
                                                    crd::containers::ConstSpan<T> d, crd::usize m)
{
    const crd::usize n = x.size();
    crd::containers::Array<T> r(alloc), p(alloc), a(alloc), w(alloc);
    r.resize(m);
    p.resize(m);
    a.resize(m * m);
    w.resize(m);
    for (crd::usize k = 0; k < m; ++k)
    {
        T rr = T(0), pp = T(0);
        for (crd::usize i = k; i < n; ++i)
        {
            rr += x[i] * x[i - k];
            pp += d[i] * x[i - k];
        }
        r[k] = rr;
        p[k] = pp;
    }
    for (crd::usize i = 0; i < m; ++i) // Toeplitz R from autocorrelation r
    {
        for (crd::usize j = 0; j < m; ++j)
        {
            a[i * m + j] = r[(i > j) ? (i - j) : (j - i)];
        }
        w[i] = p[i];
    }
    // Gaussian elimination with partial pivoting (a·w = p, solve in place).
    for (crd::usize c = 0; c < m; ++c)
    {
        crd::usize piv = c;
        for (crd::usize i = c + 1; i < m; ++i)
        {
            if (std::abs(a[i * m + c]) > std::abs(a[piv * m + c]))
            {
                piv = i;
            }
        }
        if (piv != c)
        {
            for (crd::usize j = 0; j < m; ++j)
            {
                std::swap(a[c * m + j], a[piv * m + j]);
            }
            std::swap(w[c], w[piv]);
        }
        const T d0 = a[c * m + c];
        for (crd::usize i = c + 1; i < m; ++i)
        {
            const T f = a[i * m + c] / d0;
            for (crd::usize j = c; j < m; ++j)
            {
                a[i * m + j] -= f * a[c * m + j];
            }
            w[i] -= f * w[c];
        }
    }
    for (crd::usize ii = m; ii-- > 0;)
    {
        T s = w[ii];
        for (crd::usize j = ii + 1; j < m; ++j)
        {
            s -= a[ii * m + j] * w[j];
        }
        w[ii] = s / a[ii * m + ii];
    }
    return w;
}

} // namespace crd::hesap::dsp
