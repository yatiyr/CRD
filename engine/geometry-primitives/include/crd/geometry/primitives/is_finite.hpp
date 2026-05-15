#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-primitives — finiteness predicates + the NaN/Inf contract
// (Phase 3.1.7 v1h, ADR-0076 §15).
//
// `is_finite(x)` is true iff every component of `x` is a finite IEEE value
// (no NaN, no ±∞). Defined for `Vec2`/`Vec3` and every primitive shape type.
//
// THE CONTRACT (ADR-0076 §15):
//   * QUERIES TOLERATE. A query (`raycast` / `overlap` / `closest_point` /
//     `contains` / a `signed_distance` / an `intersects`) given a NaN/∞
//     primitive must not invoke UB — it returns "no hit" / "not contained" /
//     a NaN result, never a crash. The IEEE comparison semantics the whole
//     intersection corpus is built on (`a < b` is false when either is NaN;
//     `min`/`max` ordering drops a ±∞ slab axis) already give this; the
//     finiteness predicates here are *not* needed on the query path and must
//     not be sprinkled there.
//   * BUILDERS REJECT (IN DEBUG). An accelerator builder that ingests a corpus
//     it is about to permute / partition / hash (`bvh_build`,
//     `bvh_build_parallel`, `DynamicBvh::insert`/`update`, `bvh4_collapse`)
//     `CRD_ASSERT(all_finite(...))` on the *caller-supplied* data first — a
//     non-finite input there is a caller bug, and a NaN centroid silently
//     corrupts a SAH split / a tree rotation in ways that are miserable to
//     debug downstream. In release the assert compiles away and the builder
//     produces a valid-but-possibly-degenerate structure (still no UB).
//
//   NOT SUBJECT TO THE CONTRACT: internal sentinels. `aabb_empty() =
//   {min = +∞, max = −∞}` is a *deliberate* non-finite value the builders use
//   as the identity for AABB union — `is_finite(aabb_empty())` is `false` by
//   design and that is correct. Finiteness asserts go on *inputs*, never on
//   accumulators.
// ---------------------------------------------------------------------------

#include <crd/containers/span.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/scalar.hpp>

namespace crd::geometry::primitives
{
using crd::math::MathScalar;
using crd::math::MathValue;

// ---- Vectors ---------------------------------------------------------------

template <MathScalar T> [[nodiscard]] inline bool is_finite(const Vec2<T>& v) noexcept
{
    return crd::math::is_finite(v.x) && crd::math::is_finite(v.y);
}
template <MathScalar T> [[nodiscard]] inline bool is_finite(const Vec3<T>& v) noexcept
{
    return crd::math::is_finite(v.x) && crd::math::is_finite(v.y) && crd::math::is_finite(v.z);
}

// ---- 3D primitives ---------------------------------------------------------

template <MathScalar T> [[nodiscard]] inline bool is_finite(const Line3<T>& x) noexcept
{
    return is_finite(x.point) && is_finite(x.direction);
}
template <MathScalar T> [[nodiscard]] inline bool is_finite(const Segment3<T>& x) noexcept
{
    return is_finite(x.a) && is_finite(x.b);
}
template <MathScalar T> [[nodiscard]] inline bool is_finite(const Ray3<T>& x) noexcept
{
    return is_finite(x.origin) && is_finite(x.direction);
}
template <MathScalar T> [[nodiscard]] inline bool is_finite(const Plane<T>& x) noexcept
{
    return is_finite(x.normal) && crd::math::is_finite(x.d);
}
template <MathScalar T> [[nodiscard]] inline bool is_finite(const Sphere<T>& x) noexcept
{
    return is_finite(x.center) && crd::math::is_finite(x.radius);
}
template <MathScalar T> [[nodiscard]] inline bool is_finite(const AABB3<T>& x) noexcept
{
    return is_finite(x.min) && is_finite(x.max);
}
template <MathScalar T> [[nodiscard]] inline bool is_finite(const OBB3<T>& x) noexcept
{
    return is_finite(x.center) && is_finite(x.half_extents) && is_finite(x.orientation.c0) &&
           is_finite(x.orientation.c1) && is_finite(x.orientation.c2);
}
template <MathScalar T> [[nodiscard]] inline bool is_finite(const Capsule3<T>& x) noexcept
{
    return is_finite(x.a) && is_finite(x.b) && crd::math::is_finite(x.radius);
}
template <MathScalar T> [[nodiscard]] inline bool is_finite(const Cylinder3<T>& x) noexcept
{
    return is_finite(x.a) && is_finite(x.b) && crd::math::is_finite(x.radius);
}
template <MathScalar T> [[nodiscard]] inline bool is_finite(const Triangle3<T>& x) noexcept
{
    return is_finite(x.a) && is_finite(x.b) && is_finite(x.c);
}
template <MathScalar T> [[nodiscard]] inline bool is_finite(const Tetrahedron<T>& x) noexcept
{
    return is_finite(x.a) && is_finite(x.b) && is_finite(x.c) && is_finite(x.d);
}
template <MathScalar T> [[nodiscard]] inline bool is_finite(const Frustum<T>& x) noexcept
{
    for (const Plane<T>& p : x.planes)
    {
        if (!is_finite(p))
        {
            return false;
        }
    }
    return true;
}
template <MathScalar T> [[nodiscard]] inline bool is_finite(const ConvexHullView<T>& x) noexcept
{
    for (const Vec3<T>& v : x.vertices)
    {
        if (!is_finite(v))
        {
            return false;
        }
    }
    for (const Plane<T>& f : x.faces)
    {
        if (!is_finite(f))
        {
            return false;
        }
    }
    return true;
}

// ---- 2D primitives ---------------------------------------------------------

template <MathScalar T> [[nodiscard]] inline bool is_finite(const Line2<T>& x) noexcept
{
    return is_finite(x.point) && is_finite(x.direction);
}
template <MathScalar T> [[nodiscard]] inline bool is_finite(const Segment2<T>& x) noexcept
{
    return is_finite(x.a) && is_finite(x.b);
}
template <MathScalar T> [[nodiscard]] inline bool is_finite(const Ray2<T>& x) noexcept
{
    return is_finite(x.origin) && is_finite(x.direction);
}
template <MathScalar T> [[nodiscard]] inline bool is_finite(const AABB2<T>& x) noexcept
{
    return is_finite(x.min) && is_finite(x.max);
}
template <MathScalar T> [[nodiscard]] inline bool is_finite(const OBB2<T>& x) noexcept
{
    return is_finite(x.center) && is_finite(x.half_extents) && is_finite(x.orientation.c0) &&
           is_finite(x.orientation.c1);
}
template <MathScalar T> [[nodiscard]] inline bool is_finite(const Circle<T>& x) noexcept
{
    return is_finite(x.center) && crd::math::is_finite(x.radius);
}
template <MathScalar T> [[nodiscard]] inline bool is_finite(const Capsule2<T>& x) noexcept
{
    return is_finite(x.a) && is_finite(x.b) && crd::math::is_finite(x.radius);
}
template <MathScalar T> [[nodiscard]] inline bool is_finite(const Cylinder2<T>& x) noexcept
{
    return is_finite(x.a) && is_finite(x.b) && crd::math::is_finite(x.radius);
}
template <MathScalar T> [[nodiscard]] inline bool is_finite(const Triangle2<T>& x) noexcept
{
    return is_finite(x.a) && is_finite(x.b) && is_finite(x.c);
}

// ---- Span helper (the form the accelerator builders assert on) -------------

// True iff every element of `prims` is finite. The builder-reject side of the
// contract: `CRD_ASSERT(all_finite(prims))` at the top of `bvh_build` etc.
template <typename Primitive> [[nodiscard]] inline bool all_finite(crd::containers::ConstSpan<Primitive> prims) noexcept
{
    for (const Primitive& p : prims)
    {
        if (!is_finite(p))
        {
            return false;
        }
    }
    return true;
}

} // namespace crd::geometry::primitives
