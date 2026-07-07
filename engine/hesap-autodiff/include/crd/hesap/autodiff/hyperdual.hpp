#pragma once

// hyperdual.hpp — Phase 3.1.6 v15-c: exact SECOND-order forward AD (Fike & Alonso 2011, AIAA-2011-886). A hyper-dual
// number carries a value plus two first-order tangents and ONE mixed second-order part, over the algebra
// ε1² = ε2² = (ε1ε2)² = 0, ε1ε2 = ε2ε1 ≠ 0:
//     x = f0 + f1·ε1 + f2·ε2 + f12·ε1ε2.
// Evaluating a scalar-generic functor on a hyper-dual yields f, ∂f/∂ε1, ∂f/∂ε2 and ∂²f/∂ε1∂ε2 EXACTLY — no step,
// no subtractive cancellation (the fatal flaw of FD-of-FD, which loses ~half the digits in f''). ADR-0097.
//
// A FLAT 4-slot POD beats nested Dual<Dual<T>> (which carries 2^K slots for K-th order and re-derives the chain
// each level); the flat product/chain rules below are hand-written once. Drivers (bottom): a single Hessian entry
// H_ij (seed ε1=e_i, ε2=e_j, read f12), and — the opt lever — the curvature vᵀHv in ONE pass (seed BOTH ε with v).
// The full symmetric Hessian is n(n+1)/2 such passes; the cheap *vector* HVP is forward-over-reverse (v16-e).
//
// Rules go through crd::math (deterministic); first slopes reuse detail::jvp_rules, second slopes are here.

#include <crd/core/types.hpp>

#include <crd/hesap/autodiff/detail/jvp_rules.hpp>
#include <crd/math/cmath.hpp>

namespace crd::hesap::autodiff::forward
{

template <typename T>
struct HyperDual
{
    T f0;  // value            f(x)
    T f1;  // ∂/∂ε1            (first tangent, direction 1)
    T f2;  // ∂/∂ε2            (first tangent, direction 2)
    T f12; // ∂²/∂ε1∂ε2        (mixed second-order)

    constexpr HyperDual() noexcept : f0(T(0)), f1(T(0)), f2(T(0)), f12(T(0)) {}
    // NOLINTNEXTLINE(google-explicit-constructor) — a constant lifts with zero tangents
    constexpr HyperDual(T value) noexcept : f0(value), f1(T(0)), f2(T(0)), f12(T(0)) {}
    constexpr HyperDual(T value, T d1, T d2, T d12) noexcept : f0(value), f1(d1), f2(d2), f12(d12) {}

    [[nodiscard]] constexpr HyperDual operator-() const noexcept { return {-f0, -f1, -f2, -f12}; }
    [[nodiscard]] constexpr HyperDual operator+(const HyperDual& b) const noexcept
    {
        return {f0 + b.f0, f1 + b.f1, f2 + b.f2, f12 + b.f12};
    }
    [[nodiscard]] constexpr HyperDual operator-(const HyperDual& b) const noexcept
    {
        return {f0 - b.f0, f1 - b.f1, f2 - b.f2, f12 - b.f12};
    }
    // Product over the hyper-dual algebra: the f12 slot gathers BOTH cross terms f1·b.f2 + f2·b.f1.
    [[nodiscard]] constexpr HyperDual operator*(const HyperDual& b) const noexcept
    {
        return {f0 * b.f0, f0 * b.f1 + f1 * b.f0, f0 * b.f2 + f2 * b.f0,
                f0 * b.f12 + f1 * b.f2 + f2 * b.f1 + f12 * b.f0};
    }
    [[nodiscard]] constexpr HyperDual operator/(const HyperDual& b) const noexcept; // = *this * recip(b)

    constexpr HyperDual& operator+=(const HyperDual& b) noexcept { return *this = *this + b; }
    constexpr HyperDual& operator-=(const HyperDual& b) noexcept { return *this = *this - b; }
    constexpr HyperDual& operator*=(const HyperDual& b) noexcept { return *this = *this * b; }
    constexpr HyperDual& operator/=(const HyperDual& b) noexcept { return *this = *this / b; }

    // Comparisons on the value (standard forward-AD convention).
    [[nodiscard]] constexpr bool operator==(const HyperDual& b) const noexcept { return f0 == b.f0; }
    [[nodiscard]] constexpr bool operator!=(const HyperDual& b) const noexcept { return f0 != b.f0; }
    [[nodiscard]] constexpr bool operator<(const HyperDual& b) const noexcept { return f0 < b.f0; }
    [[nodiscard]] constexpr bool operator<=(const HyperDual& b) const noexcept { return f0 <= b.f0; }
    [[nodiscard]] constexpr bool operator>(const HyperDual& b) const noexcept { return f0 > b.f0; }
    [[nodiscard]] constexpr bool operator>=(const HyperDual& b) const noexcept { return f0 >= b.f0; }
};

// Chain rule for a unary g: given g=g(f0), g1=g'(f0), g2=g''(f0), lift through the hyper-dual.
//   result.f12 = g''·(f1·f2) + g'·f12   (the Faà-di-Bruno second-order term).
template <typename T>
[[nodiscard]] constexpr HyperDual<T> hd_chain(const HyperDual<T>& x, T g, T g1, T g2) noexcept
{
    return {g, g1 * x.f1, g1 * x.f2, g2 * x.f1 * x.f2 + g1 * x.f12};
}

template <typename T>
[[nodiscard]] inline HyperDual<T> recip(const HyperDual<T>& x) noexcept
{
    const T inv = T(1) / x.f0;
    return hd_chain(x, inv, -inv * inv, T(2) * inv * inv * inv); // g=1/x, g'=-1/x², g''=2/x³
}
template <typename T>
[[nodiscard]] constexpr HyperDual<T> HyperDual<T>::operator/(const HyperDual& b) const noexcept
{
    return *this * recip(b);
}

// ---- Transcendentals (g, g', g'') --------------------------------------------------------------------------
template <typename T>
[[nodiscard]] inline HyperDual<T> exp(const HyperDual<T>& x) noexcept
{
    const T e = crd::math::exp(x.f0);
    return hd_chain(x, e, e, e);
}
template <typename T>
[[nodiscard]] inline HyperDual<T> log(const HyperDual<T>& x) noexcept
{
    const T inv = T(1) / x.f0;
    return hd_chain(x, crd::math::log(x.f0), inv, -inv * inv);
}
template <typename T>
[[nodiscard]] inline HyperDual<T> sin(const HyperDual<T>& x) noexcept
{
    T s = T(0);
    T c = T(0);
    crd::math::sincos(x.f0, s, c);
    return hd_chain(x, s, c, -s);
}
template <typename T>
[[nodiscard]] inline HyperDual<T> cos(const HyperDual<T>& x) noexcept
{
    T s = T(0);
    T c = T(0);
    crd::math::sincos(x.f0, s, c);
    return hd_chain(x, c, -s, -c);
}
template <typename T>
[[nodiscard]] inline HyperDual<T> tan(const HyperDual<T>& x) noexcept
{
    const T t   = crd::math::tan(x.f0);
    const T sec = T(1) + t * t; // sec² = 1 + tan²
    return hd_chain(x, t, sec, T(2) * t * sec);
}
template <typename T>
[[nodiscard]] inline HyperDual<T> sqrt(const HyperDual<T>& x) noexcept
{
    const T r  = crd::math::sqrt(x.f0);
    const T g1 = T(1) / (T(2) * r);
    return hd_chain(x, r, g1, -g1 / (T(2) * x.f0)); // g'' = -1/(4 x^{3/2}) = -g'/(2x)
}
template <typename T>
[[nodiscard]] inline HyperDual<T> tanh(const HyperDual<T>& x) noexcept
{
    const T t  = crd::math::tanh(x.f0);
    const T g1 = T(1) - t * t;
    return hd_chain(x, t, g1, T(-2) * t * g1); // g'' = -2 t (1 - t²)
}
template <typename T>
[[nodiscard]] inline HyperDual<T> sinh(const HyperDual<T>& x) noexcept
{
    return hd_chain(x, crd::math::sinh(x.f0), crd::math::cosh(x.f0), crd::math::sinh(x.f0));
}
template <typename T>
[[nodiscard]] inline HyperDual<T> cosh(const HyperDual<T>& x) noexcept
{
    return hd_chain(x, crd::math::cosh(x.f0), crd::math::sinh(x.f0), crd::math::cosh(x.f0));
}
template <typename T>
[[nodiscard]] inline HyperDual<T> asin(const HyperDual<T>& x) noexcept
{
    const T u  = T(1) - x.f0 * x.f0;
    const T g1 = T(1) / crd::math::sqrt(u);
    return hd_chain(x, crd::math::asin(x.f0), g1, x.f0 / (u * crd::math::sqrt(u))); // g'' = x/(1-x²)^{3/2}
}
template <typename T>
[[nodiscard]] inline HyperDual<T> atan(const HyperDual<T>& x) noexcept
{
    const T d = T(1) + x.f0 * x.f0;
    return hd_chain(x, crd::math::atan(x.f0), T(1) / d, T(-2) * x.f0 / (d * d)); // g'' = -2x/(1+x²)²
}
template <typename T>
[[nodiscard]] inline HyperDual<T> abs(const HyperDual<T>& x) noexcept
{
    return x.f0 >= T(0) ? x : -x; // g'' = 0 away from 0; subgradient +1 at 0 (matches Dual/Jet)
}
template <typename T>
[[nodiscard]] inline HyperDual<T> pow(const HyperDual<T>& x, T p) noexcept
{
    const T g  = crd::math::pow(x.f0, p);
    const T g1 = p * crd::math::pow(x.f0, p - T(1));
    const T g2 = p * (p - T(1)) * crd::math::pow(x.f0, p - T(2));
    return hd_chain(x, g, g1, g2);
}

// ---- Mixed scalar/hyper-dual --------------------------------------------------------------------------------
template <typename T>
[[nodiscard]] constexpr HyperDual<T> operator+(T s, const HyperDual<T>& x) noexcept
{
    return {s + x.f0, x.f1, x.f2, x.f12};
}
template <typename T>
[[nodiscard]] constexpr HyperDual<T> operator+(const HyperDual<T>& x, T s) noexcept { return s + x; }
template <typename T>
[[nodiscard]] constexpr HyperDual<T> operator-(T s, const HyperDual<T>& x) noexcept
{
    return {s - x.f0, -x.f1, -x.f2, -x.f12};
}
template <typename T>
[[nodiscard]] constexpr HyperDual<T> operator-(const HyperDual<T>& x, T s) noexcept
{
    return {x.f0 - s, x.f1, x.f2, x.f12};
}
template <typename T>
[[nodiscard]] constexpr HyperDual<T> operator*(T s, const HyperDual<T>& x) noexcept
{
    return {s * x.f0, s * x.f1, s * x.f2, s * x.f12};
}
template <typename T>
[[nodiscard]] constexpr HyperDual<T> operator*(const HyperDual<T>& x, T s) noexcept { return s * x; }
template <typename T>
[[nodiscard]] constexpr HyperDual<T> operator/(const HyperDual<T>& x, T s) noexcept
{
    const T inv = T(1) / s;
    return {x.f0 * inv, x.f1 * inv, x.f2 * inv, x.f12 * inv};
}
template <typename T>
[[nodiscard]] inline HyperDual<T> operator/(T s, const HyperDual<T>& x) noexcept { return s * recip(x); }

template <typename T>
[[nodiscard]] constexpr HyperDual<T> select(bool cond, const HyperDual<T>& a, const HyperDual<T>& b) noexcept
{
    return cond ? a : b;
}
template <typename T>
[[nodiscard]] constexpr HyperDual<T> min(const HyperDual<T>& a, const HyperDual<T>& b) noexcept
{
    return a.f0 <= b.f0 ? a : b;
}
template <typename T>
[[nodiscard]] constexpr HyperDual<T> max(const HyperDual<T>& a, const HyperDual<T>& b) noexcept
{
    return a.f0 >= b.f0 ? a : b;
}

// ===================== exact second-order drivers =====================
// A single Hessian entry H_ij = ∂²f/∂x_i∂x_j: seed ε1 on input i, ε2 on input j, read f12 (ONE evaluation).
template <int N, class F>
[[nodiscard]] inline crd::f64 hessian_entry(const F& f, const crd::f64* x, int i, int j) noexcept
{
    HyperDual<crd::f64> hx[N];
    for (int k = 0; k < N; ++k)
    {
        hx[k] = HyperDual<crd::f64>{x[k], k == i ? 1.0 : 0.0, k == j ? 1.0 : 0.0, 0.0};
    }
    return f(hx, N).f12;
}

// The full SYMMETRIC Hessian into row-major H[N*N] (n(n+1)/2 evaluations; upper mirrored to lower).
template <int N, class F>
inline void hessian(const F& f, const crd::f64* x, crd::f64* h) noexcept
{
    for (int i = 0; i < N; ++i)
    {
        for (int j = 0; j <= i; ++j)
        {
            const crd::f64 hij = hessian_entry<N>(f, x, i, j);
            h[i * N + j]       = hij;
            h[j * N + i]       = hij;
        }
    }
}

// Curvature vᵀHv in ONE pass — the opt trust-region / Newton lever: seed BOTH ε with the SAME vector v, read f12.
// (f12 = Σ_ij v_i v_j ∂²f/∂x_i∂x_j = vᵀHv.) Also returns the directional gradient gᵀv via f1 if wanted.
template <int N, class F>
[[nodiscard]] inline crd::f64 curvature(const F& f, const crd::f64* x, const crd::f64* v) noexcept
{
    HyperDual<crd::f64> hx[N];
    for (int k = 0; k < N; ++k)
    {
        hx[k] = HyperDual<crd::f64>{x[k], v[k], v[k], 0.0};
    }
    return f(hx, N).f12;
}

} // namespace crd::hesap::autodiff::forward
