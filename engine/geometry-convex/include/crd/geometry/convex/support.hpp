#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-convex — `ConvexShape` concept (Phase 3.1.7 v2a;
// ADR-0076 §4 pin #14, §16 pin #1).
//
// `SupportPoint<T>` + `k_invalid_vertex` + the four `support()` free-fn
// overloads (Sphere / OBB3 / Capsule3 / ConvexHullView) live in
// `crd::geometry::primitives` (alongside the shape types) so ADL finds them.
// This header re-exports `SupportPoint` / `k_invalid_vertex` into the
// `crd::geometry::convex` namespace for call-site readability and adds the
// generic `ConvexShape<S, T>` concept used by `gjk_distance`.
//
// **Why the substrate decisions live here, not in primitives** — the
// *contract* (concept) and the *driver* (GJK) belong to the convex layer;
// the *types* (`SupportPoint`) and per-shape implementations live in
// primitives because that's where ADL needs them. The split is identical
// in shape to how `crd-geometry-bvh::queries.hpp` aliases `BvhRayHit =
// RayHit<u32>` while the templated `RayHit<P>` lives in `result_types.hpp`.
//
// Three substrate decisions are LOCKED here, propagating through every
// later v2 slice:
//
//   (1) **C++20 `ConvexShape` concept + ADL `support()` free-fn overloads.**
//       New shapes (eylem's `Collider::ConvexHull`, future user-cooked
//       hulls) participate by writing one free function in their own
//       namespace — no virtual dispatch, no registry.
//
//   (2) **Local-frame shapes + two explicit transforms.** GJK works in
//       shape A's local frame (`T_BA = inv(xform_a) * xform_b` once);
//       support functions take `dir` already in the shape's local frame
//       and are pure axis-aligned math (~6-10 ops each). ~40% faster on
//       OBB pairs than world-frame driving.
//
//   (3) **`SupportPoint{point, vertex_idx}`.** vertex_idx drives GJK's
//       primary index-match termination (Box2D pattern — epsilon-free)
//       and EPA's polytope vertex de-duplication (O(1) integer-compare
//       in v2c).
// ---------------------------------------------------------------------------

#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

#include <concepts>

namespace crd::geometry::convex
{
// Re-export from primitives so call sites in this module's namespace can
// say `convex::SupportPoint<T>` etc.
using crd::geometry::primitives::k_invalid_vertex;
using crd::geometry::primitives::SupportPoint;
using crd::math::MathScalar;
using crd::math::Vec3;

// `ConvexShape<S, T>` — anything providing `support(shape, dir_local)
// -> SupportPoint<T>` via ADL. `T` is first (not the shape) so the same
// shape type can participate at multiple scalars (`f32` + `f64`) without
// a separate concept — v2i instantiates `gjk_distance<f64>` against the
// same hull types this concept names today.
template <typename S, typename T>
concept ConvexShape = MathScalar<T> && requires(const S& s, const Vec3<T>& dir) {
    { support(s, dir) } -> std::same_as<SupportPoint<T>>;
};

} // namespace crd::geometry::convex
