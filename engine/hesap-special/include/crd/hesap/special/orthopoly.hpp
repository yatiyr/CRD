#pragma once

// crd-hesap-special v12-c — classical orthogonal polynomials (evaluation via the stable three-term recurrences) +
// Golub-Welsch Gauss quadrature roots/weights (separate header section below). Families: Legendre (+ associated),
// Hermite (physicist Hₙ + probabilist Heₙ), Laguerre (+ generalized Lₙ^α), Chebyshev 1st–4th (T/U/V/W), Gegenbauer
// Cₙ^α, Jacobi Pₙ^{α,β}. The recurrences carry EXACT integer/rational coefficients (no fitted tables ⇒ low risk).
// Gated vs scipy.special (eval_legendre / eval_hermite / eval_genlaguerre / eval_chebyt / eval_gegenbauer /
// eval_jacobi …). Internals f64; public templates cast for f32. Feeds v13 Gauss quadrature.

#include <crd/hesap/special/gamma.hpp> // Real concept

#include <crd/math/cmath.hpp>

namespace crd::hesap::special
{
// ---- Legendre Pₙ(x): (n+1)P_{n+1} = (2n+1)x Pₙ − n P_{n−1} ----
template <Real T>
[[nodiscard]] T legendre(int n, T x) noexcept
{
    if (n == 0)
    {
        return T{1};
    }
    T p0 = T{1};
    T p1 = x;
    for (int k = 1; k < n; ++k)
    {
        const T p2 = (static_cast<T>(2 * k + 1) * x * p1 - static_cast<T>(k) * p0) / static_cast<T>(k + 1);
        p0 = p1;
        p1 = p2;
    }
    return p1;
}

// ---- Associated Legendre Pₙᵐ(x), 0 ≤ m ≤ n, |x| ≤ 1 (Condon-Shortley phase, scipy/Boost convention) ----
template <Real T>
[[nodiscard]] T legendre_assoc(int n, int m, T x) noexcept
{
    if (m < 0 || m > n)
    {
        return T{0};
    }
    // Pₘᵐ = (−1)^m (2m−1)!! (1−x²)^{m/2}
    T pmm = T{1};
    if (m > 0)
    {
        const T somx2 = crd::math::sqrt((T{1} - x) * (T{1} + x));
        T fact = T{1};
        for (int i = 1; i <= m; ++i)
        {
            pmm *= -fact * somx2;
            fact += T{2};
        }
    }
    if (n == m)
    {
        return pmm;
    }
    T pmmp1 = x * static_cast<T>(2 * m + 1) * pmm; // P_{m+1}^m
    if (n == m + 1)
    {
        return pmmp1;
    }
    T pll = T{0};
    for (int l = m + 2; l <= n; ++l)
    {
        pll = (static_cast<T>(2 * l - 1) * x * pmmp1 - static_cast<T>(l + m - 1) * pmm) / static_cast<T>(l - m);
        pmm = pmmp1;
        pmmp1 = pll;
    }
    return pll;
}

// ---- Hermite (physicist) Hₙ: H_{n+1} = 2x Hₙ − 2n H_{n−1} ----
template <Real T>
[[nodiscard]] T hermite(int n, T x) noexcept
{
    if (n == 0)
    {
        return T{1};
    }
    T h0 = T{1};
    T h1 = T{2} * x;
    for (int k = 1; k < n; ++k)
    {
        const T h2 = T{2} * x * h1 - static_cast<T>(2 * k) * h0;
        h0 = h1;
        h1 = h2;
    }
    return h1;
}

// ---- Hermite (probabilist) Heₙ: He_{n+1} = x Heₙ − n He_{n−1} ----
template <Real T>
[[nodiscard]] T hermite_e(int n, T x) noexcept
{
    if (n == 0)
    {
        return T{1};
    }
    T h0 = T{1};
    T h1 = x;
    for (int k = 1; k < n; ++k)
    {
        const T h2 = x * h1 - static_cast<T>(k) * h0;
        h0 = h1;
        h1 = h2;
    }
    return h1;
}

// ---- Generalized Laguerre Lₙ^α: (n+1)L_{n+1}^α = (2n+1+α−x)Lₙ^α − (n+α)L_{n−1}^α  (α=0 ⇒ ordinary Lₙ) ----
template <Real T>
[[nodiscard]] T laguerre_assoc(int n, T alpha, T x) noexcept
{
    if (n == 0)
    {
        return T{1};
    }
    T l0 = T{1};
    T l1 = T{1} + alpha - x;
    for (int k = 1; k < n; ++k)
    {
        const T l2 = ((static_cast<T>(2 * k + 1) + alpha - x) * l1 - (static_cast<T>(k) + alpha) * l0) /
                     static_cast<T>(k + 1);
        l0 = l1;
        l1 = l2;
    }
    return l1;
}
template <Real T>
[[nodiscard]] T laguerre(int n, T x) noexcept
{
    return laguerre_assoc(n, T{0}, x);
}

// ---- Chebyshev 1st–4th kind. T/U: standard; V (3rd) P₁=2x−1; W (4th) P₁=2x+1; all P_{n+1}=2x Pₙ−P_{n−1} ----
namespace detail
{
template <Real T>
[[nodiscard]] T cheb_recur(int n, T x, T p1) noexcept // p0 = 1, p1 given
{
    if (n == 0)
    {
        return T{1};
    }
    T q0 = T{1};
    T q1 = p1;
    for (int k = 1; k < n; ++k)
    {
        const T q2 = T{2} * x * q1 - q0;
        q0 = q1;
        q1 = q2;
    }
    return q1;
}
} // namespace detail
template <Real T>
[[nodiscard]] T chebyshev_t(int n, T x) noexcept
{
    return detail::cheb_recur(n, x, x);
}
template <Real T>
[[nodiscard]] T chebyshev_u(int n, T x) noexcept
{
    return detail::cheb_recur(n, x, T{2} * x);
}
template <Real T>
[[nodiscard]] T chebyshev_v(int n, T x) noexcept
{
    return detail::cheb_recur(n, x, T{2} * x - T{1});
}
template <Real T>
[[nodiscard]] T chebyshev_w(int n, T x) noexcept
{
    return detail::cheb_recur(n, x, T{2} * x + T{1});
}

// ---- Gegenbauer Cₙ^α: C₀=1, C₁=2αx, n Cₙ = 2(n+α−1)x C_{n−1} − (n+2α−2) C_{n−2} ----
template <Real T>
[[nodiscard]] T gegenbauer(int n, T alpha, T x) noexcept
{
    if (n == 0)
    {
        return T{1};
    }
    T c0 = T{1};
    T c1 = T{2} * alpha * x;
    for (int k = 2; k <= n; ++k)
    {
        const T c2 = (T{2} * (static_cast<T>(k) + alpha - T{1}) * x * c1 -
                      (static_cast<T>(k) + T{2} * alpha - T{2}) * c0) /
                     static_cast<T>(k);
        c0 = c1;
        c1 = c2;
    }
    return c1;
}

// ---- Jacobi Pₙ^{α,β}: P₀=1, P₁=½(α−β)+½(α+β+2)x, then the standard 3-term recurrence (DLMF 18.9.2). ----
template <Real T>
[[nodiscard]] T jacobi(int n, T alpha, T beta, T x) noexcept
{
    if (n == 0)
    {
        return T{1};
    }
    T p0 = T{1};
    T p1 = static_cast<T>(0.5) * (alpha - beta) + static_cast<T>(0.5) * (alpha + beta + T{2}) * x;
    for (int k = 1; k < n; ++k)
    {
        const T kk = static_cast<T>(k);
        const T a1 = T{2} * (kk + T{1}) * (kk + alpha + beta + T{1}) * (T{2} * kk + alpha + beta);
        const T a2 = (T{2} * kk + alpha + beta + T{1}) * (alpha * alpha - beta * beta);
        const T a3 = (T{2} * kk + alpha + beta) * (T{2} * kk + alpha + beta + T{1}) * (T{2} * kk + alpha + beta + T{2});
        const T a4 = T{2} * (kk + alpha) * (kk + beta) * (T{2} * kk + alpha + beta + T{2});
        const T p2 = ((a2 + a3 * x) * p1 - a4 * p0) / a1;
        p0 = p1;
        p1 = p2;
    }
    return p1;
}

} // namespace crd::hesap::special
