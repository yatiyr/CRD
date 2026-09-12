#pragma once

// taylor.hpp — Phase 3.1.6 v15-g: TAYLOR-MODE automatic differentiation. A TaylorJet<T,K> carries the length-(K+1)
// array of NORMALIZED Taylor coefficients a[k] = f^(k)(t₀)/k! (normalized to dodge k! overflow; recover the raw
// derivative as k!·a[k]). Seeding a variable = {x, 1, 0, …} generalizes Dual{v,d} to all orders. Evaluating a
// scalar-generic functor on a seeded TaylorJet yields the whole order-K truncated series in O(K²) — the crush over
// repeated first-order AD (O(2^K) nested) or finite differences (K evals, catastrophic cancellation at high order).
//
// One MASTER recurrence (Griewank-Walther Ch.13) drives every function y=φ(f): with w = φ'(f) (a series you have),
//   y_k = (1/k)·Σ_{i=0}^{k−1} (k−i)·f_{k−i}·w_i.
// The (k−i)/k weight is folded INSIDE the accumulation (overflow-guard). The Cauchy product Σ f_i g_{k−i} is the
// O(K²) core; NO FFT multiply (worse for the decaying coefficients of the K≈20–30 regime). ADR-0097.

#include <crd/core/types.hpp>

#include <crd/math/cmath.hpp>

namespace crd::hesap::autodiff::forward
{

template <typename T, int K>
struct TaylorJet
{
    static constexpr int kOrder = K;
    T                    a[K + 1]; // normalized coefficients a[k] = f^(k)/k!

    constexpr TaylorJet() noexcept : a{} {}
    constexpr TaylorJet(T c) noexcept : a{} { a[0] = c; } // NOLINT — a constant series {c,0,…}
    [[nodiscard]] static constexpr TaylorJet var(T x) noexcept  // a variable seeded at x: {x,1,0,…}
    {
        TaylorJet r;
        r.a[0] = x;
        if constexpr (K >= 1) { r.a[1] = static_cast<T>(1); }
        return r;
    }
    [[nodiscard]] T derivative(int k) const noexcept // raw k-th derivative = k!·a[k]
    {
        T f = static_cast<T>(1);
        for (int i = 2; i <= k; ++i) { f *= static_cast<T>(i); }
        return f * a[k];
    }
};

template <typename T, int K>
[[nodiscard]] constexpr TaylorJet<T, K> operator+(const TaylorJet<T, K>& f, const TaylorJet<T, K>& g) noexcept
{
    TaylorJet<T, K> r;
    for (int k = 0; k <= K; ++k) { r.a[k] = f.a[k] + g.a[k]; }
    return r;
}
template <typename T, int K>
[[nodiscard]] constexpr TaylorJet<T, K> operator-(const TaylorJet<T, K>& f, const TaylorJet<T, K>& g) noexcept
{
    TaylorJet<T, K> r;
    for (int k = 0; k <= K; ++k) { r.a[k] = f.a[k] - g.a[k]; }
    return r;
}
template <typename T, int K>
[[nodiscard]] constexpr TaylorJet<T, K> operator-(const TaylorJet<T, K>& f) noexcept
{
    TaylorJet<T, K> r;
    for (int k = 0; k <= K; ++k) { r.a[k] = -f.a[k]; }
    return r;
}
// Cauchy product — (f·g)_k = Σ_{i=0}^{k} f_i·g_{k−i}.
template <typename T, int K>
[[nodiscard]] constexpr TaylorJet<T, K> operator*(const TaylorJet<T, K>& f, const TaylorJet<T, K>& g) noexcept
{
    TaylorJet<T, K> r;
    for (int k = 0; k <= K; ++k)
    {
        T s = static_cast<T>(0);
        for (int i = 0; i <= k; ++i) { s += f.a[i] * g.a[k - i]; }
        r.a[k] = s;
    }
    return r;
}
// (f/g)_k = (1/g_0)[ f_k − Σ_{i=0}^{k−1} (f/g)_i·g_{k−i} ].
template <typename T, int K>
[[nodiscard]] constexpr TaylorJet<T, K> operator/(const TaylorJet<T, K>& f, const TaylorJet<T, K>& g) noexcept
{
    TaylorJet<T, K> r;
    const T         inv = static_cast<T>(1) / g.a[0];
    for (int k = 0; k <= K; ++k)
    {
        T s = f.a[k];
        for (int i = 0; i < k; ++i) { s -= r.a[i] * g.a[k - i]; }
        r.a[k] = s * inv;
    }
    return r;
}

// mixed scalar
template <typename T, int K>
[[nodiscard]] constexpr TaylorJet<T, K> operator+(T s, const TaylorJet<T, K>& f) noexcept { TaylorJet<T, K> r = f; r.a[0] = s + f.a[0]; return r; }
template <typename T, int K>
[[nodiscard]] constexpr TaylorJet<T, K> operator+(const TaylorJet<T, K>& f, T s) noexcept { return s + f; }
template <typename T, int K>
[[nodiscard]] constexpr TaylorJet<T, K> operator-(const TaylorJet<T, K>& f, T s) noexcept { TaylorJet<T, K> r = f; r.a[0] = f.a[0] - s; return r; }
template <typename T, int K>
[[nodiscard]] constexpr TaylorJet<T, K> operator-(T s, const TaylorJet<T, K>& f) noexcept { TaylorJet<T, K> r = -f; r.a[0] = s - f.a[0]; return r; }
template <typename T, int K>
[[nodiscard]] constexpr TaylorJet<T, K> operator*(T s, const TaylorJet<T, K>& f) noexcept
{
    TaylorJet<T, K> r;
    for (int k = 0; k <= K; ++k) { r.a[k] = s * f.a[k]; }
    return r;
}
template <typename T, int K>
[[nodiscard]] constexpr TaylorJet<T, K> operator*(const TaylorJet<T, K>& f, T s) noexcept { return s * f; }

// exp: w = e itself ⇒ e_0=exp(f_0); e_k=(1/k)Σ_{i=0}^{k−1}(k−i)f_{k−i}·e_i.
template <typename T, int K>
[[nodiscard]] inline TaylorJet<T, K> exp(const TaylorJet<T, K>& f) noexcept
{
    TaylorJet<T, K> e;
    e.a[0] = crd::math::exp(f.a[0]);
    for (int k = 1; k <= K; ++k)
    {
        T s = static_cast<T>(0);
        for (int i = 0; i < k; ++i) { s += static_cast<T>(k - i) * f.a[k - i] * e.a[i]; }
        e.a[k] = s / static_cast<T>(k);
    }
    return e;
}
// log: l_0=log(f_0); l_k=(1/f_0)[ f_k − (1/k)Σ_{i=1}^{k−1} i·f_{k−i}·l_i ].
template <typename T, int K>
[[nodiscard]] inline TaylorJet<T, K> log(const TaylorJet<T, K>& f) noexcept
{
    TaylorJet<T, K> l;
    const T         inv = static_cast<T>(1) / f.a[0];
    l.a[0]              = crd::math::log(f.a[0]);
    for (int k = 1; k <= K; ++k)
    {
        T s = static_cast<T>(0);
        for (int i = 1; i < k; ++i) { s += static_cast<T>(i) * f.a[k - i] * l.a[i]; }
        l.a[k] = (f.a[k] - s / static_cast<T>(k)) * inv;
    }
    return l;
}
// sqrt: s_0=√f_0; s_k=(1/(2s_0))[ f_k − Σ_{i=1}^{k−1} s_i·s_{k−i} ].
template <typename T, int K>
[[nodiscard]] inline TaylorJet<T, K> sqrt(const TaylorJet<T, K>& f) noexcept
{
    TaylorJet<T, K> s;
    s.a[0]          = crd::math::sqrt(f.a[0]);
    const T inv2 = static_cast<T>(1) / (static_cast<T>(2) * s.a[0]);
    for (int k = 1; k <= K; ++k)
    {
        T acc = f.a[k];
        for (int i = 1; i < k; ++i) { acc -= s.a[i] * s.a[k - i]; }
        s.a[k] = acc * inv2;
    }
    return s;
}
// sin & cos coupled (master rule, w_sin=cos, w_cos=−sin).
template <typename T, int K>
inline void sincos(const TaylorJet<T, K>& f, TaylorJet<T, K>& s, TaylorJet<T, K>& c) noexcept
{
    crd::math::sincos(f.a[0], s.a[0], c.a[0]);
    for (int k = 1; k <= K; ++k)
    {
        T ss = static_cast<T>(0);
        T cc = static_cast<T>(0);
        for (int i = 0; i < k; ++i)
        {
            const T w = static_cast<T>(k - i) * f.a[k - i];
            ss += w * c.a[i];
            cc += w * s.a[i];
        }
        s.a[k] = ss / static_cast<T>(k);
        c.a[k] = -cc / static_cast<T>(k);
    }
}
template <typename T, int K>
[[nodiscard]] inline TaylorJet<T, K> sin(const TaylorJet<T, K>& f) noexcept
{
    TaylorJet<T, K> s;
    TaylorJet<T, K> c;
    sincos(f, s, c);
    return s;
}
template <typename T, int K>
[[nodiscard]] inline TaylorJet<T, K> cos(const TaylorJet<T, K>& f) noexcept
{
    TaylorJet<T, K> s;
    TaylorJet<T, K> c;
    sincos(f, s, c);
    return c;
}
// tanh: w = 1 − t² (t = tanh f); t_k=(1/k)Σ(k−i)f_{k−i}w_i, w_k = −(t·t)_k for k≥1, w_0 = 1−t_0².
template <typename T, int K>
[[nodiscard]] inline TaylorJet<T, K> tanh(const TaylorJet<T, K>& f) noexcept
{
    TaylorJet<T, K> t;
    TaylorJet<T, K> w;
    t.a[0] = crd::math::tanh(f.a[0]);
    w.a[0] = static_cast<T>(1) - t.a[0] * t.a[0];
    for (int k = 1; k <= K; ++k)
    {
        T s = static_cast<T>(0);
        for (int i = 0; i < k; ++i) { s += static_cast<T>(k - i) * f.a[k - i] * w.a[i]; }
        t.a[k] = s / static_cast<T>(k);
        T q = static_cast<T>(0); // (t·t)_k
        for (int i = 0; i <= k; ++i) { q += t.a[i] * t.a[k - i]; }
        w.a[k] = -q;
    }
    return t;
}
// pow(f, α): p_0=f_0^α; p_k=(1/(k·f_0))Σ_{i=0}^{k−1}(α(k−i)−i)·f_{k−i}·p_i.
template <typename T, int K>
[[nodiscard]] inline TaylorJet<T, K> pow(const TaylorJet<T, K>& f, T alpha) noexcept
{
    TaylorJet<T, K> p;
    p.a[0]        = crd::math::pow(f.a[0], alpha);
    const T inv0 = static_cast<T>(1) / f.a[0];
    for (int k = 1; k <= K; ++k)
    {
        T s = static_cast<T>(0);
        for (int i = 0; i < k; ++i) { s += (alpha * static_cast<T>(k - i) - static_cast<T>(i)) * f.a[k - i] * p.a[i]; }
        p.a[k] = s * inv0 / static_cast<T>(k);
    }
    return p;
}

} // namespace crd::hesap::autodiff::forward
