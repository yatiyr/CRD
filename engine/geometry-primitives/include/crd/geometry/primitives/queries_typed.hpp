#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-primitives — typed Quantity-overload queries
// (Phase 3.1.7.5 v0d-2 / ADR-0078 §4 D27).
//
// The geometry algorithms in `primitives.hpp` / `closest_point.hpp` /
// `intersect.hpp` stay templated on `MathScalar T` (raw f32/f64) because
// internal arithmetic (`static_cast<T>(0)`, `std::numeric_limits<T>::min()`,
// `sqrt(T)`, every branch on dimensionless comparison) is precision-tier
// concerned, not dimension concerned.
//
// This header adds **Quantity-overload wrappers** so consumers carrying
// `Sphere<Length32>`, `Vec3<Length32>` etc. (typed surfaces in eylem /
// scene / ECS) call the SAME function name and get TYPED results back.
// Pattern: strip the Dim tag at the wrapper boundary, call the raw
// algorithm, re-tag the result.
//
// API surface coverage (v0d-2): closest_point / distance / distance_squared
// for Sphere / Capsule3 / Triangle3 / Plane / Segment3 / AABB3 / OBB3.
// Raycast / intersection typed overloads follow if a consumer surfaces.
//
// Pattern (per ADR-0078 §3 D22 SIMD-boundary principle): the typed surface
// lives ONE LAYER ABOVE the algorithm. Algorithms see raw scalars; the
// dim tag rides at the API boundary. Zero runtime cost — `to_raw_vec` /
// `from_raw_vec` are constexpr; `.value` accessors compile away.

#include <crd/geometry/primitives/closest_point.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/vec.hpp>
#include <crd/units/quantity_aliases.hpp>

namespace crd::geometry::primitives
{
using crd::math::from_raw_vec;
using crd::math::to_raw_vec;
using crd::math::Vec3;

namespace detail_typed
{
// Generic strip helpers — read typed primitive, build the raw twin used by
// the algorithm. Each one is one-liner; inlined fully.

template <typename D, typename T>
[[nodiscard]] constexpr Sphere<T> strip(const Sphere<crd::units::Quantity<D, T>>& s) noexcept
{
    return Sphere<T>{to_raw_vec(s.center), s.radius.value};
}

template <typename D, typename T>
[[nodiscard]] constexpr Capsule3<T> strip(const Capsule3<crd::units::Quantity<D, T>>& c) noexcept
{
    return Capsule3<T>{to_raw_vec(c.a), to_raw_vec(c.b), c.radius.value};
}

template <typename D, typename T>
[[nodiscard]] constexpr Cylinder3<T> strip(const Cylinder3<crd::units::Quantity<D, T>>& c) noexcept
{
    return Cylinder3<T>{to_raw_vec(c.a), to_raw_vec(c.b), c.radius.value};
}

template <typename D, typename T>
[[nodiscard]] constexpr AABB3<T> strip(const AABB3<crd::units::Quantity<D, T>>& b) noexcept
{
    return AABB3<T>{to_raw_vec(b.min), to_raw_vec(b.max)};
}

template <typename D, typename T>
[[nodiscard]] constexpr OBB3<T> strip(const OBB3<crd::units::Quantity<D, T>>& b) noexcept
{
    // Half-extents carry Length too; orientation is dimensionless (rotation matrix).
    return OBB3<T>{to_raw_vec(b.center), to_raw_vec(b.half_extents), b.orientation};
}

template <typename D, typename T>
[[nodiscard]] constexpr Triangle3<T> strip(const Triangle3<crd::units::Quantity<D, T>>& t) noexcept
{
    return Triangle3<T>{to_raw_vec(t.a), to_raw_vec(t.b), to_raw_vec(t.c)};
}

template <typename D, typename T>
[[nodiscard]] constexpr Plane<T> strip(const Plane<crd::units::Quantity<D, T>>& p) noexcept
{
    // Plane normal carries the same template Dim as the rest of the
    // template-uniform Plane<T>, but semantically represents a dimensionless
    // unit direction. Strip via to_raw_vec at the boundary.
    return Plane<T>{to_raw_vec(p.normal), p.d.value};
}

template <typename D, typename T>
[[nodiscard]] constexpr Segment3<T> strip(const Segment3<crd::units::Quantity<D, T>>& s) noexcept
{
    return Segment3<T>{to_raw_vec(s.a), to_raw_vec(s.b)};
}

template <typename D, typename T>
[[nodiscard]] constexpr Ray3<T> strip(const Ray3<crd::units::Quantity<D, T>>& r) noexcept
{
    // Ray direction is dimensionless (unit vector or arbitrary direction);
    // origin carries Length. To keep the existing Ray3 signature shape, we
    // strip both fields — caller already supplied a dimensionless direction
    // as Vec3<f32> in idiomatic usage.
    return Ray3<T>{to_raw_vec(r.origin), to_raw_vec(r.direction)};
}

} // namespace detail_typed

// ---------------------------------------------------------------------------
// closest_point overloads
// ---------------------------------------------------------------------------

template <typename D, typename T>
[[nodiscard]] inline Vec3<crd::units::Quantity<D, T>>
closest_point(const Sphere<crd::units::Quantity<D, T>>& sphere,
              const Vec3<crd::units::Quantity<D, T>>& p) noexcept
{
    return from_raw_vec<D>(closest_point(detail_typed::strip(sphere), to_raw_vec(p)));
}

template <typename D, typename T>
[[nodiscard]] inline Vec3<crd::units::Quantity<D, T>>
closest_point(const Capsule3<crd::units::Quantity<D, T>>& cap,
              const Vec3<crd::units::Quantity<D, T>>& p) noexcept
{
    return from_raw_vec<D>(closest_point(detail_typed::strip(cap), to_raw_vec(p)));
}

template <typename D, typename T>
[[nodiscard]] inline Vec3<crd::units::Quantity<D, T>>
closest_point(const Cylinder3<crd::units::Quantity<D, T>>& cyl,
              const Vec3<crd::units::Quantity<D, T>>& p) noexcept
{
    return from_raw_vec<D>(closest_point(detail_typed::strip(cyl), to_raw_vec(p)));
}

template <typename D, typename T>
[[nodiscard]] inline Vec3<crd::units::Quantity<D, T>>
closest_point(const AABB3<crd::units::Quantity<D, T>>& box,
              const Vec3<crd::units::Quantity<D, T>>& p) noexcept
{
    return from_raw_vec<D>(closest_point(detail_typed::strip(box), to_raw_vec(p)));
}

template <typename D, typename T>
[[nodiscard]] inline Vec3<crd::units::Quantity<D, T>>
closest_point(const OBB3<crd::units::Quantity<D, T>>& box,
              const Vec3<crd::units::Quantity<D, T>>& p) noexcept
{
    return from_raw_vec<D>(closest_point(detail_typed::strip(box), to_raw_vec(p)));
}

template <typename D, typename T>
[[nodiscard]] inline Vec3<crd::units::Quantity<D, T>>
closest_point(const Triangle3<crd::units::Quantity<D, T>>& tri,
              const Vec3<crd::units::Quantity<D, T>>& p) noexcept
{
    return from_raw_vec<D>(closest_point(detail_typed::strip(tri), to_raw_vec(p)));
}

template <typename D, typename T>
[[nodiscard]] inline Vec3<crd::units::Quantity<D, T>>
closest_point(const Plane<crd::units::Quantity<D, T>>& plane,
              const Vec3<crd::units::Quantity<D, T>>& p) noexcept
{
    return from_raw_vec<D>(closest_point(detail_typed::strip(plane), to_raw_vec(p)));
}

template <typename D, typename T>
[[nodiscard]] inline Vec3<crd::units::Quantity<D, T>>
closest_point(const Segment3<crd::units::Quantity<D, T>>& seg,
              const Vec3<crd::units::Quantity<D, T>>& p) noexcept
{
    return from_raw_vec<D>(closest_point(detail_typed::strip(seg), to_raw_vec(p)));
}

// ---------------------------------------------------------------------------
// distance overloads — return Quantity<D, T> (the Length of the closest-point delta)
// ---------------------------------------------------------------------------

template <typename D, typename T, typename Shape>
[[nodiscard]] inline crd::units::Quantity<D, T>
distance_to_typed(const Shape& raw_shape, const Vec3<T>& raw_p) noexcept
{
    return crd::units::Quantity<D, T>{distance(raw_shape, raw_p)};
}

template <typename D, typename T>
[[nodiscard]] inline crd::units::Quantity<D, T>
distance(const Sphere<crd::units::Quantity<D, T>>& sphere,
         const Vec3<crd::units::Quantity<D, T>>& p) noexcept
{
    return distance_to_typed<D, T>(detail_typed::strip(sphere), to_raw_vec(p));
}

template <typename D, typename T>
[[nodiscard]] inline crd::units::Quantity<D, T>
distance(const Capsule3<crd::units::Quantity<D, T>>& cap,
         const Vec3<crd::units::Quantity<D, T>>& p) noexcept
{
    return distance_to_typed<D, T>(detail_typed::strip(cap), to_raw_vec(p));
}

template <typename D, typename T>
[[nodiscard]] inline crd::units::Quantity<D, T>
distance(const AABB3<crd::units::Quantity<D, T>>& box,
         const Vec3<crd::units::Quantity<D, T>>& p) noexcept
{
    return distance_to_typed<D, T>(detail_typed::strip(box), to_raw_vec(p));
}

template <typename D, typename T>
[[nodiscard]] inline crd::units::Quantity<D, T>
distance(const Triangle3<crd::units::Quantity<D, T>>& tri,
         const Vec3<crd::units::Quantity<D, T>>& p) noexcept
{
    return distance_to_typed<D, T>(detail_typed::strip(tri), to_raw_vec(p));
}

template <typename D, typename T>
[[nodiscard]] inline crd::units::Quantity<D, T>
distance(const Plane<crd::units::Quantity<D, T>>& plane,
         const Vec3<crd::units::Quantity<D, T>>& p) noexcept
{
    return distance_to_typed<D, T>(detail_typed::strip(plane), to_raw_vec(p));
}

// ---------------------------------------------------------------------------
// distance_squared overloads — return Quantity<DimMul<D, D>, T> (Area when D = Length)
// ---------------------------------------------------------------------------

template <typename D, typename T, typename Shape>
[[nodiscard]] inline crd::units::Quantity<crd::units::DimMul<D, D>, T>
distance_squared_to_typed(const Shape& raw_shape, const Vec3<T>& raw_p) noexcept
{
    return crd::units::Quantity<crd::units::DimMul<D, D>, T>{distance_squared(raw_shape, raw_p)};
}

template <typename D, typename T>
[[nodiscard]] inline crd::units::Quantity<crd::units::DimMul<D, D>, T>
distance_squared(const Sphere<crd::units::Quantity<D, T>>& sphere,
                 const Vec3<crd::units::Quantity<D, T>>& p) noexcept
{
    return distance_squared_to_typed<D, T>(detail_typed::strip(sphere), to_raw_vec(p));
}

template <typename D, typename T>
[[nodiscard]] inline crd::units::Quantity<crd::units::DimMul<D, D>, T>
distance_squared(const Capsule3<crd::units::Quantity<D, T>>& cap,
                 const Vec3<crd::units::Quantity<D, T>>& p) noexcept
{
    return distance_squared_to_typed<D, T>(detail_typed::strip(cap), to_raw_vec(p));
}

template <typename D, typename T>
[[nodiscard]] inline crd::units::Quantity<crd::units::DimMul<D, D>, T>
distance_squared(const AABB3<crd::units::Quantity<D, T>>& box,
                 const Vec3<crd::units::Quantity<D, T>>& p) noexcept
{
    return distance_squared_to_typed<D, T>(detail_typed::strip(box), to_raw_vec(p));
}

template <typename D, typename T>
[[nodiscard]] inline crd::units::Quantity<crd::units::DimMul<D, D>, T>
distance_squared(const Triangle3<crd::units::Quantity<D, T>>& tri,
                 const Vec3<crd::units::Quantity<D, T>>& p) noexcept
{
    return distance_squared_to_typed<D, T>(detail_typed::strip(tri), to_raw_vec(p));
}

} // namespace crd::geometry::primitives
