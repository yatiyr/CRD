#pragma once

// crd-hesap-interp v13-c (part 2) — stable polynomial + rational interpolation.
//
//   BarycentricInterpolant  — Lagrange in the 2nd barycentric form (Berrut-Trefethen 2004). Backward-stable, O(n)/eval;
//                             conditioning is set by the node distribution, so on Chebyshev nodes it does NOT suffer
//                             the Runge blow-up that naive monomial/Vandermonde interpolation does. ★ stable poly.
//   NewtonInterpolant       — divided-difference table + Horner eval. The SAME interpolating polynomial as barycentric;
//                             ideal for incremental node addition.
//   FloaterHormannInterpolant — barycentric RATIONAL (Floater-Hormann 2007). ★ POLE-FREE on ℝ for any distinct nodes;
//                             reproduces polynomials of degree ≤ d (blend degree, default 3). The equispaced-data answer.
//
// Gated ≤1e-10 vs scipy.interpolate.{BarycentricInterpolator, FloaterHormannInterpolator}; Newton cross-checks
// barycentric (same polynomial) + exact-polynomial reproduction. The barycentric/FH eval is invariant to a common
// weight scaling, so the capacity scaling below is for numerical range only.

#include <crd/hesap/interp/piecewise.hpp>

namespace crd::hesap::interp
{

namespace detail
{
template <Real T>
[[nodiscard]] InterpStatus validate_nodes(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y) noexcept
{
    const crd::usize n = x.size();
    if (n < 1 || y.size() != n)
    {
        return InterpStatus::BadInput;
    }
    for (crd::usize i = 0; i < n; ++i)
    {
        if (!is_finite(x[i]) || !is_finite(y[i]))
        {
            return InterpStatus::BadInput;
        }
    }
    for (crd::usize i = 0; i + 1 < n; ++i)
    {
        if (!(x[i] < x[i + 1]))
        {
            return InterpStatus::NotIncreasing;
        }
    }
    return InterpStatus::Ok;
}
} // namespace detail

template <Real T>
class BarycentricInterpolant
{
public:
    explicit BarycentricInterpolant(crd::memory::IAllocator* alloc) noexcept : m_w(alloc) {}

    [[nodiscard]] InterpStatus build(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y)
    {
        const InterpStatus st = detail::validate_nodes(x, y);
        if (st != InterpStatus::Ok)
        {
            return st;
        }
        const crd::usize n = x.size();
        m_x = x;
        m_y = y;
        m_w.resize(n);
        const T cap = (n > 1) ? (x[n - 1] - x[0]) / static_cast<T>(4) : static_cast<T>(1); // capacity scaling
        for (crd::usize j = 0; j < n; ++j)
        {
            T w = static_cast<T>(1);
            for (crd::usize k = 0; k < n; ++k)
            {
                if (k != j)
                {
                    w *= cap / (x[j] - x[k]);
                }
            }
            m_w[j] = w;
        }
        return InterpStatus::Ok;
    }

    [[nodiscard]] T eval(T xq) const noexcept
    {
        const crd::usize n = m_x.size();
        T num = static_cast<T>(0);
        T den = static_cast<T>(0);
        for (crd::usize j = 0; j < n; ++j)
        {
            const T dx = xq - m_x[j];
            if (dx == static_cast<T>(0)) // exact at a node
            {
                return m_y[j];
            }
            const T t = m_w[j] / dx;
            num += t * m_y[j];
            den += t;
        }
        return num / den;
    }

private:
    crd::containers::ConstSpan<T> m_x{};
    crd::containers::ConstSpan<T> m_y{};
    crd::containers::Array<T> m_w;
};

template <Real T>
class NewtonInterpolant
{
public:
    explicit NewtonInterpolant(crd::memory::IAllocator* alloc) noexcept : m_c(alloc) {}

    [[nodiscard]] InterpStatus build(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y)
    {
        const InterpStatus st = detail::validate_nodes(x, y);
        if (st != InterpStatus::Ok)
        {
            return st;
        }
        const crd::usize n = x.size();
        m_x = x;
        m_c.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            m_c[i] = y[i];
        }
        for (crd::usize k = 1; k < n; ++k) // divided-difference table
        {
            for (crd::usize i = n - 1; i >= k; --i)
            {
                m_c[i] = (m_c[i] - m_c[i - 1]) / (x[i] - x[i - k]);
                if (i == k) // unsigned guard (i never goes below k)
                {
                    break;
                }
            }
        }
        return InterpStatus::Ok;
    }

    [[nodiscard]] T eval(T xq) const noexcept
    {
        const crd::usize n = m_x.size();
        T p = m_c[n - 1];
        for (crd::usize k = n - 1; k-- > 0;) // Newton-Horner
        {
            p = p * (xq - m_x[k]) + m_c[k];
        }
        return p;
    }

private:
    crd::containers::ConstSpan<T> m_x{};
    crd::containers::Array<T> m_c;
};

template <Real T>
class FloaterHormannInterpolant
{
public:
    explicit FloaterHormannInterpolant(crd::memory::IAllocator* alloc) noexcept : m_w(alloc) {}

    [[nodiscard]] InterpStatus build(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y, crd::usize d = 3)
    {
        const InterpStatus st = detail::validate_nodes(x, y);
        if (st != InterpStatus::Ok)
        {
            return st;
        }
        const crd::usize n = x.size();
        if (d >= n)
        {
            d = n - 1;
        }
        m_x = x;
        m_y = y;
        m_w.resize(n);
        for (crd::usize k = 0; k < n; ++k) // Floater-Hormann weights: Σ_{i∈J_k} (-1)^i ∏_{j=i,j≠k}^{i+d} 1/(x_k−x_j)
        {
            const crd::usize ilo = (k >= d) ? k - d : 0;
            const crd::usize ihi = (k < n - 1 - d) ? k : (n - 1 - d);
            T wk = static_cast<T>(0);
            for (crd::usize i = ilo; i <= ihi; ++i)
            {
                T prod = static_cast<T>(1);
                for (crd::usize j = i; j <= i + d; ++j)
                {
                    if (j != k)
                    {
                        prod *= static_cast<T>(1) / (x[k] - x[j]);
                    }
                }
                wk += ((i & 1u) ? static_cast<T>(-1) : static_cast<T>(1)) * prod;
            }
            m_w[k] = wk;
        }
        return InterpStatus::Ok;
    }

    [[nodiscard]] T eval(T xq) const noexcept
    {
        const crd::usize n = m_x.size();
        T num = static_cast<T>(0);
        T den = static_cast<T>(0);
        for (crd::usize k = 0; k < n; ++k)
        {
            const T dx = xq - m_x[k];
            if (dx == static_cast<T>(0))
            {
                return m_y[k];
            }
            const T t = m_w[k] / dx;
            num += t * m_y[k];
            den += t;
        }
        return num / den; // pole-free: the denominator never vanishes on ℝ
    }

private:
    crd::containers::ConstSpan<T> m_x{};
    crd::containers::ConstSpan<T> m_y{};
    crd::containers::Array<T> m_w;
};

} // namespace crd::hesap::interp
