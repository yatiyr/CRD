#pragma once

// detail/jvp_rules.hpp — Phase 3.1.6 v15-b: the forward-mode DERIVATIVE RULES, each recurrence written ONCE.
// A first-order JVP rule for a unary f is just the local slope f'(v) evaluated at the value v; every carrier
// (Dual<T>, Jet<T,N>, JetPackD<N>, and the v15-g Taylor jets) then scales its own tangent(s) by that slope. Keeping
// the slopes here — not duplicated per carrier — is the ADR-0097 "generate each recurrence once" rule: one audited
// home for the math + the NaN/inf hardening (Ceres-faithful `pow`, `sqrt`-at-0, negative-base cases).
//
// These are PURE-VALUE helpers on the scalar T (they never see a tangent), so they are trivially reused by the
// Taylor recurrence (v15-g) and by the 3-oracle gate. All routes go through crd::math (the Cerid Math Mandate),
// so the slopes are deterministic and ≤ a few ulp. Binary rules (atan2, hypot, pow(x,y)) combine TWO tangents and
// so live at the carrier; their scalar pieces (the partial slopes) are provided here.

#include <crd/core/types.hpp>

#include <crd/math/cmath.hpp>

namespace crd::hesap::autodiff::forward::detail
{

// ln(2), ln(10) — exact double constants (deterministic; used by the base-2/base-10 exp/log slopes).
template <typename T>
inline constexpr T ln2_v = static_cast<T>(0.69314718055994530941723212145818L);
template <typename T>
inline constexpr T ln10_v = static_cast<T>(2.30258509299404568401799145468436L);

// ---- Unary slopes f'(v) (the JVP recurrence for each cmath function) ----------------------------------------
template <typename T> [[nodiscard]] inline T d_sqrt(T s) noexcept { return static_cast<T>(1) / (static_cast<T>(2) * s); } // s = sqrt(v)
template <typename T> [[nodiscard]] inline T d_rsqrt(T v, T r) noexcept { return static_cast<T>(-0.5) * r / v; } // r = rsqrt(v); d = -1/2 v^-3/2
template <typename T> [[nodiscard]] inline T d_cbrt(T c) noexcept { return static_cast<T>(1) / (static_cast<T>(3) * c * c); } // c = cbrt(v)

template <typename T> [[nodiscard]] inline T d_exp2(T v) noexcept { return crd::math::exp2(v) * ln2_v<T>; }
template <typename T> [[nodiscard]] inline T d_exp10(T v) noexcept { return crd::math::exp10(v) * ln10_v<T>; }
template <typename T> [[nodiscard]] inline T d_expm1(T v) noexcept { return crd::math::exp(v); }           // d/dv (e^v - 1) = e^v
template <typename T> [[nodiscard]] inline T d_log2(T v) noexcept { return static_cast<T>(1) / (v * ln2_v<T>); }
template <typename T> [[nodiscard]] inline T d_log10(T v) noexcept { return static_cast<T>(1) / (v * ln10_v<T>); }
template <typename T> [[nodiscard]] inline T d_log1p(T v) noexcept { return static_cast<T>(1) / (static_cast<T>(1) + v); }

template <typename T> [[nodiscard]] inline T d_asin(T v) noexcept { return static_cast<T>(1) / crd::math::sqrt(static_cast<T>(1) - v * v); }
template <typename T> [[nodiscard]] inline T d_acos(T v) noexcept { return static_cast<T>(-1) / crd::math::sqrt(static_cast<T>(1) - v * v); }
template <typename T> [[nodiscard]] inline T d_atan(T v) noexcept { return static_cast<T>(1) / (static_cast<T>(1) + v * v); }

template <typename T> [[nodiscard]] inline T d_sinh(T v) noexcept { return crd::math::cosh(v); }
template <typename T> [[nodiscard]] inline T d_cosh(T v) noexcept { return crd::math::sinh(v); }
template <typename T> [[nodiscard]] inline T d_asinh(T v) noexcept { return static_cast<T>(1) / crd::math::sqrt(v * v + static_cast<T>(1)); }
template <typename T> [[nodiscard]] inline T d_acosh(T v) noexcept { return static_cast<T>(1) / crd::math::sqrt(v * v - static_cast<T>(1)); } // v > 1
template <typename T> [[nodiscard]] inline T d_atanh(T v) noexcept { return static_cast<T>(1) / (static_cast<T>(1) - v * v); }              // |v| < 1

// ---- Binary partial slopes -----------------------------------------------------------------------------------
// atan2(y, x): ∂/∂y = x/(x²+y²), ∂/∂x = -y/(x²+y²).
template <typename T> [[nodiscard]] inline T atan2_dy(T y, T x) noexcept { return x / (x * x + y * y); }
template <typename T> [[nodiscard]] inline T atan2_dx(T y, T x) noexcept { return -y / (x * x + y * y); }
// hypot(x, y): ∂/∂x = x/h, ∂/∂y = y/h  (h = hypot(x,y)).

// ---- Hardened pow (Ceres-faithful edge conventions) ---------------------------------------------------------
// Constant exponent p. Returns {value, d/dx}. x^0 ≡ 1 with slope 0 (the natural formula p·x^(p-1) would give the
// indeterminate 0·∞ = NaN at x=0). Every other edge (x=0 with p≥1 → finite; 0<p<1 → +∞; x<0 non-integer → NaN)
// falls out of crd::math::pow correctly, so we defer to it.
template <typename T>
struct PowConst
{
    T value;
    T dbase;
};
// Ceres' exact branchless slope p·x^(p-1). This is deliberately NOT special-cased: at the genuine (x=0, p=0)
// singularity the slope 0·∞ = NaN, which MATCHES Ceres (`ceres::pow(f, scalar)` = g·pow(f.a, g-1)); every other
// edge is well-defined (x=0,p>1 → 0; p=1 → 1; 0<p<1 → +∞; x<0 integer p → real; x<0 non-integer → NaN). Branchless
// = nothing for MSVC /O2 to miscompile (an earlier x==0 branch let /O2 mis-select the not-taken 0/0=NaN path).
template <typename T>
[[nodiscard]] inline PowConst<T> pow_const(T x, T p) noexcept
{
    return {crd::math::pow(x, p), p * crd::math::pow(x, p - static_cast<T>(1))};
}

// Dual exponent: z = x^y. Returns {value, ∂z/∂x, ∂z/∂y} following Ceres jet.h's pow(f,g) case table:
//   x > 0                    : z = x^y; ∂x = y·x^(y-1); ∂y = z·ln x                       (holomorphic case)
//   x == 0, y > 1            : z = 0;   ∂x = 0;         ∂y = 0                             (0 with a flat approach)
//   x == 0, y == 1           : z = 0;   ∂x = 1;         ∂y = 0                             (identity)
//   x == 0, 0 < y < 1        : z = 0;   ∂x = +∞;        ∂y = 0
//   x == 0, y <= 0           : z = +∞/NaN;              (crd::math::pow drives value)     ∂ = NaN
//   x < 0, y integer         : z = x^y; ∂x = y·x^(y-1); ∂y = NaN   (ln of a negative base is undefined)
//   x < 0, y non-integer     : z = NaN; ∂ = NaN
template <typename T>
struct PowDual
{
    T value;
    T dbase; // ∂z/∂x
    T dexp;  // ∂z/∂y
};
template <typename T>
[[nodiscard]] inline PowDual<T> pow_dual(T x, T y) noexcept
{
    if (x > static_cast<T>(0))
    {
        const T z = crd::math::pow(x, y);
        return {z, y * crd::math::pow(x, y - static_cast<T>(1)), z * crd::math::log(x)};
    }
    if (x == static_cast<T>(0))
    {
        if (y > static_cast<T>(1))
        {
            return {static_cast<T>(0), static_cast<T>(0), static_cast<T>(0)};
        }
        if (y == static_cast<T>(1))
        {
            return {static_cast<T>(0), static_cast<T>(1), static_cast<T>(0)};
        }
        // 0 < y < 1  → slope +∞ ;  y ≤ 0 → value +∞/NaN, slope NaN. Both come out of pow(0, y-1).
        const T z  = crd::math::pow(x, y);
        const T db = y * crd::math::pow(x, y - static_cast<T>(1));
        return {z, db, static_cast<T>(0)};
    }
    // x < 0
    const T fl = crd::math::floor(y);
    if (y == fl) // integer exponent: value + base-slope defined; exponent-slope undefined (ln of negative)
    {
        const T z = crd::math::pow(x, y);
        return {z, y * crd::math::pow(x, y - static_cast<T>(1)), crd::math::log(x)}; // log(x<0) → NaN, as intended
    }
    const T nan = crd::math::log(x); // NaN
    return {nan, nan, nan};
}

} // namespace crd::hesap::autodiff::forward::detail
