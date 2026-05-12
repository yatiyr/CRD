#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-primitives — the iq formulary: smooth-blend & domain operators
// (Phase 3.1.7 v0e). Inigo Quilez's canonical SDF combinators
// (iquilezles.org/articles/smin, /distfunctions). Two flavours:
//
//   * Value-domain ops — act on distance values `d` (a scalar `T`):
//       smin_poly / smin_cubic / smin_exp (+ smax_* counterparts), op_round
//       (inflate by r), op_onion (shell of half-thickness t), extrude_2d
//       (turn a 2D SDF + a z-coord into a 3D SDF). `k`/`t`/`r > 0` expected.
//   * Position-domain ops — act on a query point `p` (`Vec2`/`Vec3`/scalar),
//       returning the position to feed the underlying SDF:
//       domain_repeat / domain_mirror (tile / mirror-tile a unit cell),
//       domain_elongate (stretch a shape's domain by ±h), domain_twist /
//       domain_bend (the rotational warps).
//
// Determinism (ADR-0076 §4): `smin_exp` uses `crd::math::deterministic::exp2 /
// log2` and `domain_twist` / `domain_bend` use `crd::math::deterministic::sin /
// cos` — never the libm variants (the `crd-no-std-math-check` guard enforces
// this for `engine/geometry-primitives`). `std::floor` / `std::sqrt` are
// IEEE-exact and thus allowed. The polynomial smin/smax forms collapse to the
// exact min/max as `k → 0` (no discontinuity).
//
// These get a GLSL/HLSL twin emitted by `crd-geometry-shader-helpers` (v9e);
// the C++ scalar form here is the ULP-conformance reference for that.
// ---------------------------------------------------------------------------

#include <crd/math/deterministic.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

#include <cmath>
#include <limits>

namespace crd::geometry::primitives
{
using crd::math::MathScalar;
using crd::math::Vec2;
using crd::math::Vec3;

// ===========================================================================
// Smooth min / max — value-domain.
// ===========================================================================

// Quadratic polynomial smooth-min (iq) — C¹, blends within `k` of the crossover,
// returns exactly min(a,b) outside that band (and as k→0). `k > 0`.
template <MathScalar T> [[nodiscard]] constexpr T smin_poly(T a, T b, T k) noexcept
{
    const T kk = k * static_cast<T>(4);
    const T h = crd::math::max(kk - (a < b ? b - a : a - b), static_cast<T>(0)) / (kk + std::numeric_limits<T>::min());
    return crd::math::min(a, b) - h * h * kk * static_cast<T>(0.25);
}
template <MathScalar T> [[nodiscard]] constexpr T smax_poly(T a, T b, T k) noexcept
{
    const T kk = k * static_cast<T>(4);
    const T h = crd::math::max(kk - (a < b ? b - a : a - b), static_cast<T>(0)) / (kk + std::numeric_limits<T>::min());
    return crd::math::max(a, b) + h * h * kk * static_cast<T>(0.25);
}

// Cubic polynomial smooth-min (iq) — C², a touch smoother than the quadratic.
template <MathScalar T> [[nodiscard]] constexpr T smin_cubic(T a, T b, T k) noexcept
{
    const T kk = k * static_cast<T>(6);
    const T h = crd::math::max(kk - (a < b ? b - a : a - b), static_cast<T>(0)) / (kk + std::numeric_limits<T>::min());
    return crd::math::min(a, b) - h * h * h * kk * (static_cast<T>(1) / static_cast<T>(6));
}
template <MathScalar T> [[nodiscard]] constexpr T smax_cubic(T a, T b, T k) noexcept
{
    const T kk = k * static_cast<T>(6);
    const T h = crd::math::max(kk - (a < b ? b - a : a - b), static_cast<T>(0)) / (kk + std::numeric_limits<T>::min());
    return crd::math::max(a, b) + h * h * h * kk * (static_cast<T>(1) / static_cast<T>(6));
}

// Exponential smooth-min (iq) — *associative* (chaining N of them is order-free),
// but never returns exactly min(a,b) (always a hair below). `k > 0`.
template <MathScalar T> [[nodiscard]] inline T smin_exp(T a, T b, T k) noexcept
{
    const T r = crd::math::deterministic::exp2(-a / k) + crd::math::deterministic::exp2(-b / k);
    return -k * crd::math::deterministic::log2(r);
}
template <MathScalar T> [[nodiscard]] inline T smax_exp(T a, T b, T k) noexcept
{
    return -smin_exp(-a, -b, k);
}

// ===========================================================================
// Value-domain shape ops.
// ===========================================================================

// Inflate / round an SDF by `r` (a sphere of radius r swept over the surface).
template <MathScalar T> [[nodiscard]] constexpr T op_round(T d, T r) noexcept
{
    return d - r;
}
// Turn a solid SDF into a shell of half-thickness `t` (iq's onion).
template <MathScalar T> [[nodiscard]] constexpr T op_onion(T d, T t) noexcept
{
    return (d < static_cast<T>(0) ? -d : d) - t;
}

// Extrude a 2D SDF along z to half-height `h`: given `d2 = sdf2d(p.xy)` and
// `p_z`, returns the 3D SDF value (iq's opExtrusion).
template <MathScalar T> [[nodiscard]] inline T extrude_2d(T d2, T p_z, T h) noexcept
{
    const T wx = d2;
    const T wy = (p_z < static_cast<T>(0) ? -p_z : p_z) - h;
    const T inside = crd::math::min(crd::math::max(wx, wy), static_cast<T>(0));
    const T mx = crd::math::max(wx, static_cast<T>(0));
    const T my = crd::math::max(wy, static_cast<T>(0));
    return inside + static_cast<T>(std::sqrt(mx * mx + my * my));
}

// ===========================================================================
// Position-domain ops — repeat / mirror / elongate.
// ===========================================================================

namespace detail
{
// True mathematical modulo (result has the sign of `m`), via the IEEE-exact
// floor. `m > 0` expected.
template <MathScalar T> [[nodiscard]] inline T mod_floor(T x, T m) noexcept
{
    return x - m * static_cast<T>(std::floor(x / m));
}
} // namespace detail

// One-axis repeat: position within the `period`-sized cell, centered on 0.
template <MathScalar T> [[nodiscard]] inline T domain_repeat(T p, T period) noexcept
{
    return detail::mod_floor(p + static_cast<T>(0.5) * period, period) - static_cast<T>(0.5) * period;
}
template <MathScalar T> [[nodiscard]] inline Vec2<T> domain_repeat(const Vec2<T>& p, const Vec2<T>& period) noexcept
{
    return Vec2<T>(domain_repeat(p.x, period.x), domain_repeat(p.y, period.y));
}
template <MathScalar T> [[nodiscard]] inline Vec3<T> domain_repeat(const Vec3<T>& p, const Vec3<T>& period) noexcept
{
    return Vec3<T>(domain_repeat(p.x, period.x), domain_repeat(p.y, period.y), domain_repeat(p.z, period.z));
}

// One-axis mirror-repeat: cells alternate orientation (so neighbouring tiles
// meet edge-to-edge). Returns the position within the cell, centered on 0.
template <MathScalar T> [[nodiscard]] inline T domain_mirror(T p, T period) noexcept
{
    T q = detail::mod_floor(p, static_cast<T>(2) * period);
    if (q > period)
    {
        q = static_cast<T>(2) * period - q;
    }
    return q - static_cast<T>(0.5) * period;
}
template <MathScalar T> [[nodiscard]] inline Vec2<T> domain_mirror(const Vec2<T>& p, const Vec2<T>& period) noexcept
{
    return Vec2<T>(domain_mirror(p.x, period.x), domain_mirror(p.y, period.y));
}
template <MathScalar T> [[nodiscard]] inline Vec3<T> domain_mirror(const Vec3<T>& p, const Vec3<T>& period) noexcept
{
    return Vec3<T>(domain_mirror(p.x, period.x), domain_mirror(p.y, period.y), domain_mirror(p.z, period.z));
}

// Elongate a shape's domain by ±h along each axis (iq's simple opElongate): feed
// the result to the base SDF. Exact for the surface and the exterior; the
// interior is a hair off (a full correction needs the +max(...) term — add it at
// the call site if you need exact interior distances).
template <MathScalar T> [[nodiscard]] constexpr Vec2<T> domain_elongate(const Vec2<T>& p, const Vec2<T>& h) noexcept
{
    return Vec2<T>(p.x - crd::math::clamp(p.x, -h.x, h.x), p.y - crd::math::clamp(p.y, -h.y, h.y));
}
template <MathScalar T> [[nodiscard]] constexpr Vec3<T> domain_elongate(const Vec3<T>& p, const Vec3<T>& h) noexcept
{
    return Vec3<T>(p.x - crd::math::clamp(p.x, -h.x, h.x), p.y - crd::math::clamp(p.y, -h.y, h.y),
                   p.z - crd::math::clamp(p.z, -h.z, h.z));
}

// ===========================================================================
// Position-domain ops — rotational warps (use the deterministic trig).
// ===========================================================================

// Twist around the y-axis by an angle `k · p.y` (iq's opTwist).
template <MathScalar T> [[nodiscard]] inline Vec3<T> domain_twist(const Vec3<T>& p, T k) noexcept
{
    const T angle = k * p.y;
    const T c = crd::math::deterministic::cos(angle);
    const T s = crd::math::deterministic::sin(angle);
    return Vec3<T>(c * p.x - s * p.z, p.y, s * p.x + c * p.z);
}
// Cheap bend around the z-axis by an angle `k · p.x` (iq's opCheapBend).
template <MathScalar T> [[nodiscard]] inline Vec3<T> domain_bend(const Vec3<T>& p, T k) noexcept
{
    const T angle = k * p.x;
    const T c = crd::math::deterministic::cos(angle);
    const T s = crd::math::deterministic::sin(angle);
    return Vec3<T>(c * p.x - s * p.y, s * p.x + c * p.y, p.z);
}

} // namespace crd::geometry::primitives
