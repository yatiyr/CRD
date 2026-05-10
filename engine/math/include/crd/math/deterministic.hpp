// crd::math::deterministic — Cephes-style polynomial transcendentals that
// produce bit-exact identical f32 results across MSVC / clang-cl / GCC and
// across x64 / ARM64 platforms. Phase 3.1 v0c.
//
// Why this exists: ADR-0063 §2 bans std::sin / std::cos / std::atan2 /
// std::exp / std::log / std::pow inside engine/eylem/** (and later
// engine/hesap/**) because the C runtime's libm differs across platforms
// (Microsoft CRT vs glibc vs Apple vecLib vs Bionic, etc.) — the same f32
// input can return different f32 outputs, breaking the eylem v9b 9-config
// replay-hash CI matrix.
//
// The replacements here are pure adds + multiplies + bit-tricks (the
// determinism contract enforces no fast-math / no FMA contraction / no x87
// 80-bit intermediates), so the same input produces the same f32 bit
// pattern on every backend. The accuracy target is ~3 ulps for trig,
// ~5 ulps for exp/log — about 1-2 ulps tighter than libc on average.
//
// Coefficients are Cephes-derived (Stephen Moshier, public domain) with
// minor tweaks for f32 precision; see deterministic.cpp for the
// per-function citation.
//
// SCOPE — what's IN v0c:
//     sin, cos, tan, asin, acos, atan, atan2,
//     exp, exp2, log, log2, log10, pow,
//     floor, ceil, trunc, round, fmod
//
// SCOPE — what's NOT in v0c (see docs/debt.md, "Phase 3.1 v0c" entry):
//     - f64 overloads          → crd-hesap v0a
//     - Vec4f/Vec8f overloads  → eylem v1+ when batched-angle work surfaces
//     - sinh / cosh / tanh     → reserved (no consumer in eylem v1-v6)
//     - erf / gamma / bessel   → crd-hesap-stats v13
//     - expm1 / log1p          → reserved

#pragma once

#include <crd/core/types.hpp>

namespace crd::math::deterministic
{
using crd::f32;
using crd::f64;

// ---- constants (compile-time; same value on every platform) ---------------

inline constexpr f32 pi      = 3.14159265358979323846F;
inline constexpr f32 tau     = 6.28318530717958647693F;  // 2*pi
inline constexpr f32 pi_2    = 1.57079632679489661923F;  // pi/2
inline constexpr f32 pi_4    = 0.78539816339744830962F;  // pi/4
inline constexpr f32 inv_pi  = 0.31830988618379067154F;  // 1/pi
inline constexpr f32 inv_2pi = 0.15915494309189533577F;  // 1/(2*pi)

inline constexpr f32 e         = 2.71828182845904523536F;
inline constexpr f32 ln2       = 0.69314718055994530942F;
inline constexpr f32 inv_ln2   = 1.44269504088896340736F;  // 1/ln(2) = log2(e)
inline constexpr f32 ln10      = 2.30258509299404568402F;
inline constexpr f32 inv_ln10  = 0.43429448190325182765F;  // 1/ln(10) = log10(e)

// ---- trig -----------------------------------------------------------------

[[nodiscard]] f32 sin(f32 x) noexcept;
[[nodiscard]] f32 cos(f32 x) noexcept;
[[nodiscard]] f32 tan(f32 x) noexcept;

[[nodiscard]] f32 asin(f32 x) noexcept;   // input clamped to [-1, 1]
[[nodiscard]] f32 acos(f32 x) noexcept;   // input clamped to [-1, 1]
[[nodiscard]] f32 atan(f32 x) noexcept;
[[nodiscard]] f32 atan2(f32 y, f32 x) noexcept;

// ---- exp / log / pow ------------------------------------------------------

[[nodiscard]] f32 exp(f32 x) noexcept;     // returns +inf on overflow, +0 on underflow
[[nodiscard]] f32 exp2(f32 x) noexcept;    // 2^x
[[nodiscard]] f32 log(f32 x) noexcept;     // natural log; -inf at 0, NaN for x<0
[[nodiscard]] f32 log2(f32 x) noexcept;
[[nodiscard]] f32 log10(f32 x) noexcept;
[[nodiscard]] f32 pow(f32 base, f32 exponent) noexcept;

// Cancellation-resistant near-zero variants (debt paydown for v0c).
//   expm1(x) = exp(x) - 1, accurate when |x| is small
//   log1p(x) = log(1 + x), accurate when |x| is small
[[nodiscard]] f32 expm1(f32 x) noexcept;
[[nodiscard]] f32 log1p(f32 x) noexcept;

// ---- hyperbolic (debt paydown for v0c) ------------------------------------

[[nodiscard]] f32 sinh(f32 x) noexcept;    // (e^x - e^-x) / 2
[[nodiscard]] f32 cosh(f32 x) noexcept;    // (e^x + e^-x) / 2
[[nodiscard]] f32 tanh(f32 x) noexcept;    // sinh / cosh; saturates to ±1 at large |x|

// ---- IEEE-correct rounding (deterministic by hardware contract) -----------
//
// Wrappers exist for namespace consistency (so all transcendentals route
// through one place), not because the underlying ops are non-deterministic.
// IEEE-754 mandates correct rounding for these.

[[nodiscard]] f32 floor(f32 x) noexcept;
[[nodiscard]] f32 ceil(f32 x) noexcept;
[[nodiscard]] f32 trunc(f32 x) noexcept;
[[nodiscard]] f32 round(f32 x) noexcept;   // round-to-nearest-ties-away-from-zero (matches std::roundf)
[[nodiscard]] f32 fmod(f32 x, f32 y) noexcept;

// ---- absolute value + sign helpers (also routed for consistency) ----------

[[nodiscard]] f32 abs(f32 x) noexcept;
[[nodiscard]] f32 copysign(f32 magnitude, f32 sign_src) noexcept;

// ---- special functions (debt paydown for v0c) -----------------------------
//
// Basic special functions widely used in error analysis, numerical
// integration, statistics, and physics. Cephes-derived implementations.
// Reserved for crd-hesap-stats v13: Bessel functions (J/Y/I/K),
// orthogonal polynomials (Legendre/Hermite/Chebyshev), polygamma, and
// hypergeometric — those are statistics-module concerns, not basic math.

[[nodiscard]] f32 erf(f32 x) noexcept;       // 2/√π ∫₀ˣ e^-t² dt; range [-1, 1]
[[nodiscard]] f32 erfc(f32 x) noexcept;      // 1 - erf(x); avoids cancellation for large x
[[nodiscard]] f32 gamma(f32 x) noexcept;     // continuous factorial; gamma(n+1) = n!
[[nodiscard]] f32 lgamma(f32 x) noexcept;    // log|gamma(x)|; for huge values
[[nodiscard]] f32 beta(f32 a, f32 b) noexcept;  // gamma(a)*gamma(b)/gamma(a+b)

// ===========================================================================
// f64 overloads — same surface as f32, with Cephes-derived f64 coefficient
// tables. Bit-exact across MSVC / GCC / clang × x64 / ARM under the same
// ADR-0063 contract. (Debt paydown for v0c.)
// ===========================================================================

// f64 constants (compile-time, same value on every platform)
inline constexpr f64 pi64      = 3.14159265358979323846;
inline constexpr f64 tau64     = 6.28318530717958647693;
inline constexpr f64 pi_2_64   = 1.57079632679489661923;
inline constexpr f64 pi_4_64   = 0.78539816339744830962;
inline constexpr f64 inv_pi64  = 0.31830988618379067154;
inline constexpr f64 e64       = 2.71828182845904523536;
inline constexpr f64 ln2_64    = 0.69314718055994530942;
inline constexpr f64 inv_ln2_64 = 1.44269504088896340736;
inline constexpr f64 ln10_64   = 2.30258509299404568402;
inline constexpr f64 inv_ln10_64 = 0.43429448190325182765;

// Trig
[[nodiscard]] f64 sin(f64 x) noexcept;
[[nodiscard]] f64 cos(f64 x) noexcept;
[[nodiscard]] f64 tan(f64 x) noexcept;
[[nodiscard]] f64 asin(f64 x) noexcept;
[[nodiscard]] f64 acos(f64 x) noexcept;
[[nodiscard]] f64 atan(f64 x) noexcept;
[[nodiscard]] f64 atan2(f64 y, f64 x) noexcept;

// Exp / log / pow
[[nodiscard]] f64 exp(f64 x) noexcept;
[[nodiscard]] f64 exp2(f64 x) noexcept;
[[nodiscard]] f64 log(f64 x) noexcept;
[[nodiscard]] f64 log2(f64 x) noexcept;
[[nodiscard]] f64 log10(f64 x) noexcept;
[[nodiscard]] f64 pow(f64 base, f64 exponent) noexcept;
[[nodiscard]] f64 expm1(f64 x) noexcept;
[[nodiscard]] f64 log1p(f64 x) noexcept;

// Hyperbolic
[[nodiscard]] f64 sinh(f64 x) noexcept;
[[nodiscard]] f64 cosh(f64 x) noexcept;
[[nodiscard]] f64 tanh(f64 x) noexcept;

// Rounding (IEEE-correct hardware ops)
[[nodiscard]] f64 floor(f64 x) noexcept;
[[nodiscard]] f64 ceil(f64 x) noexcept;
[[nodiscard]] f64 trunc(f64 x) noexcept;
[[nodiscard]] f64 round(f64 x) noexcept;
[[nodiscard]] f64 fmod(f64 x, f64 y) noexcept;
[[nodiscard]] f64 abs(f64 x) noexcept;
[[nodiscard]] f64 copysign(f64 magnitude, f64 sign_src) noexcept;

// f64 special functions
[[nodiscard]] f64 erf(f64 x) noexcept;
[[nodiscard]] f64 erfc(f64 x) noexcept;
[[nodiscard]] f64 gamma(f64 x) noexcept;
[[nodiscard]] f64 lgamma(f64 x) noexcept;
[[nodiscard]] f64 beta(f64 a, f64 b) noexcept;

}  // namespace crd::math::deterministic

// ===========================================================================
// SIMD-batched overloads — Vec4f / Vec8f (debt paydown for v0c).
//
// API surface for batched physics/animation paths. Current impl is
// per-lane scalar loop (correct + bit-exact lane-wise vs scalar
// determinism, but no SIMD speedup). True branchless SIMD using the same
// Cephes coefficients lands when eylem v1+ surfaces a measured demand for
// the throughput — at that point we add ~20 helpers (Vec4f bitwise +
// int convert + truncate) and rewrite. The API is stable; only the
// implementation changes.
// ===========================================================================

#include <crd/math/simd/vec4f.hpp>
#include <crd/math/simd/vec8f.hpp>

namespace crd::math::deterministic
{
[[nodiscard]] crd::math::simd::Vec4f sin(crd::math::simd::Vec4f x) noexcept;
[[nodiscard]] crd::math::simd::Vec4f cos(crd::math::simd::Vec4f x) noexcept;
[[nodiscard]] crd::math::simd::Vec4f exp(crd::math::simd::Vec4f x) noexcept;
[[nodiscard]] crd::math::simd::Vec4f log(crd::math::simd::Vec4f x) noexcept;

[[nodiscard]] crd::math::simd::Vec8f sin(crd::math::simd::Vec8f x) noexcept;
[[nodiscard]] crd::math::simd::Vec8f cos(crd::math::simd::Vec8f x) noexcept;
[[nodiscard]] crd::math::simd::Vec8f exp(crd::math::simd::Vec8f x) noexcept;
[[nodiscard]] crd::math::simd::Vec8f log(crd::math::simd::Vec8f x) noexcept;
}  // namespace crd::math::deterministic
