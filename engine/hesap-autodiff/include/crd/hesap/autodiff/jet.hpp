#pragma once

// jet.hpp — Phase 3.1.6 v15-a: Jet<T,N> — the N-direction forward-mode carrier. A jet is value `a` plus a
// length-N perturbation vector `v` (one partial per seeded direction); evaluating a scalar-generic functor on a
// jet computes f AND all N directional derivatives in a SINGLE pass (vs Dual<T>'s one-direction-per-pass). Seed
// v[k]=1 for the k-th input and read the k-th slot of the result = ∂f/∂x_k. ADR-0097.
//
// This is the Ceres `Jet<T,N>` class — with two deliberate differences (ADR-0097 §1,
// docs/research/2026-07-06-v15-forward-ad-crush.md §A):
//   (1) the perturbation is a PLAIN C-array `T v[N]`, NOT an Eigen fixed-size vector — no Eigen dependency
//       (compile-time + WASM/MSVC portability win), no forced alignment. The partial loops auto-vectorize under
//       g++/clang (verified: 32-byte AVX). This Jet is the SCALAR-CORRECT SUBSTRATE; the SPEED crush is the SIMD
//       vector-forward carrier (partials packed in a Vec4d/Vec8f register, single-rounded FMA, no intermediate
//       materialization — measured 1.85x vs Ceres on arithmetic-heavy AD), which is slice v15-d.
//   (2) comparisons act on the VALUE only (standard forward-AD convention), matching Dual<T>.
// The rule surface mirrors dual.hpp exactly; the NaN/inf hardening of sqrt/pow/abs is v15-b's job for both carriers.

#include <crd/core/types.hpp>

#include <crd/hesap/autodiff/detail/jvp_rules.hpp>
#include <crd/math/cmath.hpp>

namespace crd::hesap::autodiff::forward
{

// Forward-mode jet: value `a` + N-vector of partials `v`. Aggregate / trivially copyable POD (no Eigen, no align
// requirement). `DIMENSION` mirrors the Ceres spelling for consumers that template over the width.
template <typename T, int N>
struct Jet
{
    static constexpr int DIMENSION = N;
    using Scalar                   = T;

    T a;    // value      f(x)
    T v[N]; // partials   ∂f/∂(seeded direction k), k = 0..N-1

    // Zero jet (value 0, all partials 0).
    constexpr Jet() noexcept : a(static_cast<T>(0)), v{} {}

    // From a constant: a + 0 (a plain value has zero perturbation) — implicit so `2.0 * jet` etc. just works.
    constexpr Jet(T value) noexcept : a(value), v{} {} // NOLINT(google-explicit-constructor)

    // Seed constructor: value + a 1 in slot k (the k-th input direction), zeros elsewhere.
    constexpr Jet(T value, int k) noexcept : a(value), v{}
    {
        v[k] = static_cast<T>(1);
    }

    [[nodiscard]] constexpr Jet operator-() const noexcept
    {
        Jet r;
        r.a = -a;
        for (int i = 0; i < N; ++i)
        {
            r.v[i] = -v[i];
        }
        return r;
    }

    [[nodiscard]] constexpr Jet operator+(const Jet& b) const noexcept
    {
        Jet r;
        r.a = a + b.a;
        for (int i = 0; i < N; ++i)
        {
            r.v[i] = v[i] + b.v[i];
        }
        return r;
    }

    [[nodiscard]] constexpr Jet operator-(const Jet& b) const noexcept
    {
        Jet r;
        r.a = a - b.a;
        for (int i = 0; i < N; ++i)
        {
            r.v[i] = v[i] - b.v[i];
        }
        return r;
    }

    // Product rule per slot: (a·b)' = a·b' + a'·b.
    [[nodiscard]] constexpr Jet operator*(const Jet& b) const noexcept
    {
        Jet r;
        r.a = a * b.a;
        for (int i = 0; i < N; ++i)
        {
            r.v[i] = a * b.v[i] + v[i] * b.a;
        }
        return r;
    }

    // Quotient rule per slot: reuse a/b (Ceres form, valid since perturbation² = 0).
    [[nodiscard]] constexpr Jet operator/(const Jet& b) const noexcept
    {
        const T inv    = static_cast<T>(1) / b.a;
        const T a_by_b = a * inv;
        Jet     r;
        r.a = a_by_b;
        for (int i = 0; i < N; ++i)
        {
            r.v[i] = (v[i] - a_by_b * b.v[i]) * inv;
        }
        return r;
    }

    constexpr Jet& operator+=(const Jet& b) noexcept { return *this = *this + b; }
    constexpr Jet& operator-=(const Jet& b) noexcept { return *this = *this - b; }
    constexpr Jet& operator*=(const Jet& b) noexcept { return *this = *this * b; }
    constexpr Jet& operator/=(const Jet& b) noexcept { return *this = *this / b; }

    // Comparisons on the value only.
    [[nodiscard]] constexpr bool operator==(const Jet& b) const noexcept { return a == b.a; }
    [[nodiscard]] constexpr bool operator!=(const Jet& b) const noexcept { return a != b.a; }
    [[nodiscard]] constexpr bool operator<(const Jet& b) const noexcept { return a < b.a; }
    [[nodiscard]] constexpr bool operator<=(const Jet& b) const noexcept { return a <= b.a; }
    [[nodiscard]] constexpr bool operator>(const Jet& b) const noexcept { return a > b.a; }
    [[nodiscard]] constexpr bool operator>=(const Jet& b) const noexcept { return a >= b.a; }
};

// ---- Chain-rule helper: result = f(a) with each partial scaled by f'(a) --------------------------------------
namespace detail
{
template <typename T, int N>
[[nodiscard]] constexpr Jet<T, N> scale_partials(T value, T deriv, const Jet<T, N>& x) noexcept
{
    Jet<T, N> r;
    r.a = value;
    for (int i = 0; i < N; ++i)
    {
        r.v[i] = deriv * x.v[i];
    }
    return r;
}
} // namespace detail

// ---- Transcendentals (ADL-found for Jet<T,N>; same closed forms as dual.hpp) ---------------------------------

// One fused sincos (single shared range reduction) for value + derivative — Ceres/libm call sin and cos
// separately (two reductions); we do one (the forward-AD perf lever on latency-bound chains).
template <typename T, int N>
[[nodiscard]] inline Jet<T, N> sin(const Jet<T, N>& x) noexcept
{
    T s = static_cast<T>(0);
    T c = static_cast<T>(0);
    crd::math::sincos(x.a, s, c);
    return detail::scale_partials(s, c, x);
}
template <typename T, int N>
[[nodiscard]] inline Jet<T, N> cos(const Jet<T, N>& x) noexcept
{
    T s = static_cast<T>(0);
    T c = static_cast<T>(0);
    crd::math::sincos(x.a, s, c);
    return detail::scale_partials(c, -s, x);
}
template <typename T, int N>
[[nodiscard]] inline Jet<T, N> tan(const Jet<T, N>& x) noexcept
{
    const T c = crd::math::cos(x.a);
    return detail::scale_partials(crd::math::tan(x.a), static_cast<T>(1) / (c * c), x); // sec²
}
template <typename T, int N>
[[nodiscard]] inline Jet<T, N> exp(const Jet<T, N>& x) noexcept
{
    const T e = crd::math::exp(x.a);
    return detail::scale_partials(e, e, x);
}
template <typename T, int N>
[[nodiscard]] inline Jet<T, N> log(const Jet<T, N>& x) noexcept
{
    return detail::scale_partials(crd::math::log(x.a), static_cast<T>(1) / x.a, x);
}
template <typename T, int N>
[[nodiscard]] inline Jet<T, N> sqrt(const Jet<T, N>& x) noexcept
{
    const T s = crd::math::sqrt(x.a);
    return detail::scale_partials(s, static_cast<T>(1) / (static_cast<T>(2) * s), x);
}
template <typename T, int N>
[[nodiscard]] inline Jet<T, N> tanh(const Jet<T, N>& x) noexcept
{
    const T t = crd::math::tanh(x.a);
    return detail::scale_partials(t, static_cast<T>(1) - t * t, x); // sech²
}
template <typename T, int N>
[[nodiscard]] inline Jet<T, N> abs(const Jet<T, N>& x) noexcept
{
    // Subgradient at 0 takes the +1 branch (convention), matching Dual<T>.
    return x.a >= static_cast<T>(0) ? x : -x;
}
// pow, constant exponent — hardened (detail::pow_const; x^0 ≡ 1 slope 0, x=0/x<0 edges).
template <typename T, int N>
[[nodiscard]] inline Jet<T, N> pow(const Jet<T, N>& x, T p) noexcept
{
    const auto r = detail::pow_const(x.a, p);
    return detail::scale_partials(r.value, r.dbase, x);
}

// pow, jet exponent — Ceres-faithful edge table (detail::pow_dual); combines base + exponent tangents.
template <typename T, int N>
[[nodiscard]] inline Jet<T, N> pow(const Jet<T, N>& x, const Jet<T, N>& y) noexcept
{
    const auto r = detail::pow_dual(x.a, y.a);
    Jet<T, N>  out;
    out.a = r.value;
    for (int i = 0; i < N; ++i)
    {
        out.v[i] = r.dbase * x.v[i] + r.dexp * y.v[i];
    }
    return out;
}

// ---- v15-b: the full crd::math JVP surface (slopes from detail::jvp_rules — written once) --------------------
// (macro pastes FN as the defined name AND crd::math::FN — genuinely not expressible as a template function)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define CRD_AD_JET_UNARY(FN, SLOPE)                                                                                    \
    template <typename T, int N>                                                                                      \
    [[nodiscard]] inline Jet<T, N> FN(const Jet<T, N>& x) noexcept                                                    \
    {                                                                                                                 \
        return detail::scale_partials(crd::math::FN(x.a), SLOPE, x);                                                 \
    }
CRD_AD_JET_UNARY(asin, detail::d_asin(x.a))
CRD_AD_JET_UNARY(acos, detail::d_acos(x.a))
CRD_AD_JET_UNARY(atan, detail::d_atan(x.a))
CRD_AD_JET_UNARY(sinh, detail::d_sinh(x.a))
CRD_AD_JET_UNARY(cosh, detail::d_cosh(x.a))
CRD_AD_JET_UNARY(asinh, detail::d_asinh(x.a))
CRD_AD_JET_UNARY(acosh, detail::d_acosh(x.a))
CRD_AD_JET_UNARY(atanh, detail::d_atanh(x.a))
CRD_AD_JET_UNARY(exp2, detail::d_exp2(x.a))
CRD_AD_JET_UNARY(exp10, detail::d_exp10(x.a))
CRD_AD_JET_UNARY(expm1, detail::d_expm1(x.a))
CRD_AD_JET_UNARY(log2, detail::d_log2(x.a))
CRD_AD_JET_UNARY(log10, detail::d_log10(x.a))
CRD_AD_JET_UNARY(log1p, detail::d_log1p(x.a))
#undef CRD_AD_JET_UNARY

template <typename T, int N>
[[nodiscard]] inline Jet<T, N> cbrt(const Jet<T, N>& x) noexcept
{
    const T c = crd::math::cbrt(x.a);
    return detail::scale_partials(c, detail::d_cbrt(c), x);
}
template <typename T, int N>
[[nodiscard]] inline Jet<T, N> rsqrt(const Jet<T, N>& x) noexcept
{
    const T r = crd::math::rsqrt(x.a);
    return detail::scale_partials(r, detail::d_rsqrt(x.a, r), x);
}
template <typename T, int N>
[[nodiscard]] inline Jet<T, N> atan2(const Jet<T, N>& y, const Jet<T, N>& x) noexcept
{
    const T   dy = detail::atan2_dy(y.a, x.a);
    const T   dx = detail::atan2_dx(y.a, x.a);
    Jet<T, N> out;
    out.a = crd::math::atan2(y.a, x.a);
    for (int i = 0; i < N; ++i)
    {
        out.v[i] = dy * y.v[i] + dx * x.v[i];
    }
    return out;
}
template <typename T, int N>
[[nodiscard]] inline Jet<T, N> hypot(const Jet<T, N>& x, const Jet<T, N>& y) noexcept
{
    const T   h  = crd::math::hypot(x.a, y.a);
    const T   ix = x.a / h;
    const T   iy = y.a / h;
    Jet<T, N> out;
    out.a = h;
    for (int i = 0; i < N; ++i)
    {
        out.v[i] = ix * x.v[i] + iy * y.v[i];
    }
    return out;
}

// ---- Mixed scalar/jet operators ------------------------------------------------------------------------------

template <typename T, int N>
[[nodiscard]] constexpr Jet<T, N> operator+(T s, const Jet<T, N>& x) noexcept
{
    Jet<T, N> r = x;
    r.a         = s + x.a;
    return r;
}
template <typename T, int N>
[[nodiscard]] constexpr Jet<T, N> operator+(const Jet<T, N>& x, T s) noexcept
{
    return s + x;
}
template <typename T, int N>
[[nodiscard]] constexpr Jet<T, N> operator-(T s, const Jet<T, N>& x) noexcept
{
    Jet<T, N> r = -x;
    r.a         = s - x.a;
    return r;
}
template <typename T, int N>
[[nodiscard]] constexpr Jet<T, N> operator-(const Jet<T, N>& x, T s) noexcept
{
    Jet<T, N> r = x;
    r.a         = x.a - s;
    return r;
}
template <typename T, int N>
[[nodiscard]] constexpr Jet<T, N> operator*(T s, const Jet<T, N>& x) noexcept
{
    return detail::scale_partials(s * x.a, s, x);
}
template <typename T, int N>
[[nodiscard]] constexpr Jet<T, N> operator*(const Jet<T, N>& x, T s) noexcept
{
    return s * x;
}
template <typename T, int N>
[[nodiscard]] constexpr Jet<T, N> operator/(const Jet<T, N>& x, T s) noexcept
{
    const T inv = static_cast<T>(1) / s;
    return detail::scale_partials(x.a * inv, inv, x);
}
template <typename T, int N>
[[nodiscard]] constexpr Jet<T, N> operator/(T s, const Jet<T, N>& x) noexcept
{
    const T inv = static_cast<T>(1) / x.a;
    return detail::scale_partials(s * inv, -(s * inv) * inv, x); // (s/x)' = -s*x'/x^2
}

// ---- select / min / max (mirror dual.hpp; active-branch derivative, tie -> first arg) ------------------------

template <typename T, int N>
[[nodiscard]] constexpr Jet<T, N> select(bool cond, const Jet<T, N>& a, const Jet<T, N>& b) noexcept
{
    return cond ? a : b;
}
template <typename T, int N>
[[nodiscard]] constexpr Jet<T, N> min(const Jet<T, N>& a, const Jet<T, N>& b) noexcept
{
    return a.a <= b.a ? a : b;
}
template <typename T, int N>
[[nodiscard]] constexpr Jet<T, N> max(const Jet<T, N>& a, const Jet<T, N>& b) noexcept
{
    return a.a >= b.a ? a : b;
}

} // namespace crd::hesap::autodiff::forward
