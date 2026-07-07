#pragma once

// dual.hpp — Phase 3.1.6 v15-a: Dual<T> — the forward-mode automatic-differentiation scalar (the CANONICAL home).
// A dual number carries a value v and ONE directional derivative d; arithmetic propagates the chain rule exactly
// (no truncation, no subtractive cancellation — unlike finite differences). Seeding x_i with d=1 (others d=0) and
// evaluating a scalar-generic functor f yields the EXACT ∂f/∂x_i in the result's derivative part. ADR-0097.
//
// MIGRATION (v15-a, ADR-0097 §2): this type was shipped by v7-b inside crd-hesap-opt (opt/dual.hpp). It now lives
// here — the framework owns the canonical Dual. `crd-hesap-opt` re-exports every name (`opt/dual.hpp` is a thin
// shim: `using autodiff::forward::Dual; using autodiff::forward::sin; …`) so every existing opt consumer compiles
// unchanged — ZERO REGRESSIONS is the v15-a gate. The math below is a VERBATIM move (do NOT change the sqrt/pow/abs
// conventions here — hardening the NaN/inf edges is v15-b's job, with new tests; shifting a convention now would
// break the opt suite). New in v15-a: min/max/select (bottom of file).
//
// SCALAR-GENERIC FUNCTOR CONTRACT: a functor written `template <class S> S operator()(ConstSpan<S> x) const`
// instantiates on both T (the real value path) and Dual<T> (the derivative path). It should use the arithmetic
// operators (which work for both) and, for transcendentals, the unqualified-call-with-`using crd::math::sin;` idiom so
// ADL finds crd::math::sin for T and crd::hesap::autodiff::forward::sin for Dual<T> (the standard autodiff idiom).
// The SAME functor also instantiates on std::complex<T> (the v13 complex-step oracle) — one functor, three oracles.

#include <crd/core/types.hpp>

#include <crd/hesap/autodiff/detail/jvp_rules.hpp>
#include <crd/math/cmath.hpp>

namespace crd::hesap::autodiff::forward
{

// Forward-mode dual number: value `v` + derivative `d` (one tangent direction). Trivially copyable.
template <typename T>
struct Dual
{
    T v; // value      f(x)
    T d; // derivative f'(x) along the seeded direction

    constexpr Dual() noexcept : v(static_cast<T>(0)), d(static_cast<T>(0)) {}
    // Implicit from a constant: a plain value has zero derivative (so `2.0 * dual` etc. just works).
    constexpr Dual(T value) noexcept : v(value), d(static_cast<T>(0)) {} // NOLINT(google-explicit-constructor)
    constexpr Dual(T value, T deriv) noexcept : v(value), d(deriv) {}

    [[nodiscard]] constexpr Dual operator+(const Dual& b) const noexcept { return Dual{v + b.v, d + b.d}; }
    [[nodiscard]] constexpr Dual operator-(const Dual& b) const noexcept { return Dual{v - b.v, d - b.d}; }
    [[nodiscard]] constexpr Dual operator-() const noexcept { return Dual{-v, -d}; }

    // Product rule: (a·b)' = a'·b + a·b'.
    [[nodiscard]] constexpr Dual operator*(const Dual& b) const noexcept
    {
        return Dual{v * b.v, d * b.v + v * b.d};
    }

    // Quotient rule: (a/b)' = (a'·b − a·b') / b².
    [[nodiscard]] constexpr Dual operator/(const Dual& b) const noexcept
    {
        const T inv = static_cast<T>(1) / b.v;
        return Dual{v * inv, (d - (v * inv) * b.d) * inv};
    }

    constexpr Dual& operator+=(const Dual& b) noexcept
    {
        *this = *this + b;
        return *this;
    }
    constexpr Dual& operator-=(const Dual& b) noexcept
    {
        *this = *this - b;
        return *this;
    }
    constexpr Dual& operator*=(const Dual& b) noexcept
    {
        *this = *this * b;
        return *this;
    }
    constexpr Dual& operator/=(const Dual& b) noexcept
    {
        *this = *this / b;
        return *this;
    }

    // Comparisons act on the VALUE (so functor branching `if (x > 0)` chooses a branch by the point, not the
    // tangent — the standard forward-AD convention).
    [[nodiscard]] constexpr bool operator==(const Dual& b) const noexcept { return v == b.v; }
    [[nodiscard]] constexpr bool operator!=(const Dual& b) const noexcept { return v != b.v; }
    [[nodiscard]] constexpr bool operator<(const Dual& b) const noexcept { return v < b.v; }
    [[nodiscard]] constexpr bool operator<=(const Dual& b) const noexcept { return v <= b.v; }
    [[nodiscard]] constexpr bool operator>(const Dual& b) const noexcept { return v > b.v; }
    [[nodiscard]] constexpr bool operator>=(const Dual& b) const noexcept { return v >= b.v; }
};

// ---- Transcendentals (ADL-found for Dual<T>; chain rule baked in) ------------------------------------------

// sin/cos both need sin(v) AND cos(v) (value + derivative), so ONE fused `crd::math::sincos` (a single shared
// range reduction) replaces two separate calls — the forward-AD perf lever: Ceres/libm call sin and cos
// separately (two reductions); we do one. Latency-bound AD chains win here.
template <typename T>
[[nodiscard]] inline Dual<T> sin(const Dual<T>& x) noexcept
{
    T s = static_cast<T>(0);
    T c = static_cast<T>(0);
    crd::math::sincos(x.v, s, c);
    return Dual<T>{s, x.d * c};
}

template <typename T>
[[nodiscard]] inline Dual<T> cos(const Dual<T>& x) noexcept
{
    T s = static_cast<T>(0);
    T c = static_cast<T>(0);
    crd::math::sincos(x.v, s, c);
    return Dual<T>{c, -x.d * s};
}

template <typename T>
[[nodiscard]] inline Dual<T> tan(const Dual<T>& x) noexcept
{
    const T c = crd::math::cos(x.v);
    return Dual<T>{crd::math::tan(x.v), x.d / (c * c)}; // sec²(x)·x'
}

template <typename T>
[[nodiscard]] inline Dual<T> exp(const Dual<T>& x) noexcept
{
    const T e = crd::math::exp(x.v);
    return Dual<T>{e, x.d * e};
}

template <typename T>
[[nodiscard]] inline Dual<T> log(const Dual<T>& x) noexcept
{
    return Dual<T>{crd::math::log(x.v), x.d / x.v};
}

template <typename T>
[[nodiscard]] inline Dual<T> sqrt(const Dual<T>& x) noexcept
{
    const T s = crd::math::sqrt(x.v);
    return Dual<T>{s, x.d / (static_cast<T>(2) * s)};
}

template <typename T>
[[nodiscard]] inline Dual<T> tanh(const Dual<T>& x) noexcept
{
    const T t = crd::math::tanh(x.v);
    return Dual<T>{t, x.d * (static_cast<T>(1) - t * t)}; // sech²(x)·x'
}

template <typename T>
[[nodiscard]] inline Dual<T> abs(const Dual<T>& x) noexcept
{
    // Subgradient at 0 takes the +1 branch (convention); fine for optimization use.
    return x.v >= static_cast<T>(0) ? x : Dual<T>{-x.v, -x.d};
}

// pow, constant exponent — hardened at x=0 / x<0 (detail::pow_const; x^0 ≡ 1 slope 0).
template <typename T>
[[nodiscard]] inline Dual<T> pow(const Dual<T>& x, T p) noexcept
{
    const auto r = detail::pow_const(x.v, p);
    return Dual<T>{r.value, x.d * r.dbase};
}

// pow, dual exponent — Ceres-faithful edge table (detail::pow_dual): x>0 holomorphic; x=0 by the exponent's sign;
// x<0 integer exponent keeps the base-slope (exponent-slope NaN); x<0 non-integer NaN.
template <typename T>
[[nodiscard]] inline Dual<T> pow(const Dual<T>& x, const Dual<T>& y) noexcept
{
    const auto r = detail::pow_dual(x.v, y.v);
    return Dual<T>{r.value, x.d * r.dbase + y.d * r.dexp};
}

// ---- v15-b: the full crd::math JVP surface (each slope from detail::jvp_rules — written once) ----------------
// (macro pastes FN as the defined name AND crd::math::FN — genuinely not expressible as a template function)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define CRD_AD_DUAL_UNARY(FN, SLOPE)                                                                                    \
    template <typename T>                                                                                              \
    [[nodiscard]] inline Dual<T> FN(const Dual<T>& x) noexcept                                                         \
    {                                                                                                                 \
        return Dual<T>{crd::math::FN(x.v), x.d * (SLOPE)};                                                            \
    }
CRD_AD_DUAL_UNARY(asin, detail::d_asin(x.v))
CRD_AD_DUAL_UNARY(acos, detail::d_acos(x.v))
CRD_AD_DUAL_UNARY(atan, detail::d_atan(x.v))
CRD_AD_DUAL_UNARY(sinh, detail::d_sinh(x.v))
CRD_AD_DUAL_UNARY(cosh, detail::d_cosh(x.v))
CRD_AD_DUAL_UNARY(asinh, detail::d_asinh(x.v))
CRD_AD_DUAL_UNARY(acosh, detail::d_acosh(x.v))
CRD_AD_DUAL_UNARY(atanh, detail::d_atanh(x.v))
CRD_AD_DUAL_UNARY(exp2, detail::d_exp2(x.v))
CRD_AD_DUAL_UNARY(exp10, detail::d_exp10(x.v))
CRD_AD_DUAL_UNARY(expm1, detail::d_expm1(x.v))
CRD_AD_DUAL_UNARY(log2, detail::d_log2(x.v))
CRD_AD_DUAL_UNARY(log10, detail::d_log10(x.v))
CRD_AD_DUAL_UNARY(log1p, detail::d_log1p(x.v))
#undef CRD_AD_DUAL_UNARY

// cbrt / rsqrt reuse the computed value in their slope (cheaper than recomputing).
template <typename T>
[[nodiscard]] inline Dual<T> cbrt(const Dual<T>& x) noexcept
{
    const T c = crd::math::cbrt(x.v);
    return Dual<T>{c, x.d * detail::d_cbrt(c)};
}
template <typename T>
[[nodiscard]] inline Dual<T> rsqrt(const Dual<T>& x) noexcept
{
    const T r = crd::math::rsqrt(x.v);
    return Dual<T>{r, x.d * detail::d_rsqrt(x.v, r)};
}

// Binary rules (combine two tangents).
template <typename T>
[[nodiscard]] inline Dual<T> atan2(const Dual<T>& y, const Dual<T>& x) noexcept
{
    return Dual<T>{crd::math::atan2(y.v, x.v), y.d * detail::atan2_dy(y.v, x.v) + x.d * detail::atan2_dx(y.v, x.v)};
}
template <typename T>
[[nodiscard]] inline Dual<T> hypot(const Dual<T>& x, const Dual<T>& y) noexcept
{
    const T h = crd::math::hypot(x.v, y.v);
    return Dual<T>{h, (x.v * x.d + y.v * y.d) / h}; // ∂/∂x = x/h, ∂/∂y = y/h
}

// ---- Mixed scalar/dual operators (so `2.0 * x`, `x + 1.0` read naturally in generic code) ------------------

template <typename T>
[[nodiscard]] constexpr Dual<T> operator+(T s, const Dual<T>& x) noexcept
{
    return Dual<T>{s + x.v, x.d};
}
template <typename T>
[[nodiscard]] constexpr Dual<T> operator+(const Dual<T>& x, T s) noexcept
{
    return Dual<T>{x.v + s, x.d};
}
template <typename T>
[[nodiscard]] constexpr Dual<T> operator-(T s, const Dual<T>& x) noexcept
{
    return Dual<T>{s - x.v, -x.d};
}
template <typename T>
[[nodiscard]] constexpr Dual<T> operator-(const Dual<T>& x, T s) noexcept
{
    return Dual<T>{x.v - s, x.d};
}
template <typename T>
[[nodiscard]] constexpr Dual<T> operator*(T s, const Dual<T>& x) noexcept
{
    return Dual<T>{s * x.v, s * x.d};
}
template <typename T>
[[nodiscard]] constexpr Dual<T> operator*(const Dual<T>& x, T s) noexcept
{
    return Dual<T>{x.v * s, x.d * s};
}
template <typename T>
[[nodiscard]] constexpr Dual<T> operator/(const Dual<T>& x, T s) noexcept
{
    const T inv = static_cast<T>(1) / s;
    return Dual<T>{x.v * inv, x.d * inv};
}
template <typename T>
[[nodiscard]] constexpr Dual<T> operator/(T s, const Dual<T>& x) noexcept
{
    const T inv = static_cast<T>(1) / x.v;
    return Dual<T>{s * inv, -(s * inv) * inv * x.d}; // (s/x)' = −s·x'/x²
}

// ---- select / min / max (v15-a NEW) ------------------------------------------------------------------------
//
// A comparison drops the derivative (returns bool on the value); these three carry the ACTIVE BRANCH's derivative.
// Tie convention (documented sub-gradient): min/max return the FIRST argument on an exact value tie (`a` for a==b),
// so the result is deterministic and lane-order-independent. (Ceres instead AVERAGES the two jets on a tie — an
// equally valid subgradient midpoint; we take the active-branch form the v15 slice contract names. If a future
// consumer needs the averaged subgradient, add an explicit `min_avg`/`max_avg` — do not change this default.)

template <typename T>
[[nodiscard]] constexpr Dual<T> select(bool cond, const Dual<T>& a, const Dual<T>& b) noexcept
{
    return cond ? a : b; // carries the taken branch's value AND derivative
}

template <typename T>
[[nodiscard]] constexpr Dual<T> min(const Dual<T>& a, const Dual<T>& b) noexcept
{
    return a.v <= b.v ? a : b; // tie (a.v == b.v) -> a
}

template <typename T>
[[nodiscard]] constexpr Dual<T> max(const Dual<T>& a, const Dual<T>& b) noexcept
{
    return a.v >= b.v ? a : b; // tie (a.v == b.v) -> a
}

} // namespace crd::hesap::autodiff::forward
