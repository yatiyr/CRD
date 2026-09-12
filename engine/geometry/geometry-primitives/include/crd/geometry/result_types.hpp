#pragma once

// ---------------------------------------------------------------------------
// crd-geometry — unified query result types (Phase 3.1.7 v1i-a, ADR-0076 §16
// pin #2).
//
// `RayHit<Payload>` and `ClosestPointResult<Payload>` are the canonical, *typed*
// result types every geometry-query backend returns. The payload type names what
// the hit is — `u32` for a BVH leaf index, a `{tri_index, u, v}` struct for a
// mesh raycast (`crd-geometry-mesh` v4), an entity id for a scene raycast.
// Backends provide their own concrete return type by aliasing:
//   * `using BvhRayHit       = RayHit<u32>;`             (crd-geometry-bvh)
//   * `using BvhClosestPoint = ClosestPointResult<u32>;` (crd-geometry-bvh)
//   * `using MeshRayHit      = RayHit<MeshHitPayload>;`  (crd-geometry-mesh v4)
// — the alias documents the payload semantics at the call site without dragging
// in a fat over-generic variant.
//
// Field layout is fixed (the ADR pin): `RayHit{t, payload}`,
// `ClosestPointResult{point, distance_squared, payload}`. New backends must use
// the same field order so aliases are layout-compatible.
//
// This header is leaf-substrate: it depends only on `crd::f32` and
// `crd::math::Vec3` — no backend headers. Backend headers include it and add
// their aliases; the facade `crd/geometry/queries.hpp` includes it alongside
// every backend header to expose the unified overload set.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/math/vec.hpp>

namespace crd::geometry
{
// Nearest-hit raycast result. `t` is the ray parameter at the hit; `payload`
// names the hit (typically a primitive / leaf / entity index — its meaning is
// fixed by the backend alias). A miss is signalled by `std::nullopt`, not by an
// in-band sentinel.
template <typename Payload> struct RayHit
{
    crd::f32 t{0.0F};
    Payload payload{};
};

// Closest-point query result. `point` is the point on the named primitive (or
// its AABB, for broadphase forms); `distance_squared` is the squared distance
// from the query point. Squared throughout — callers `sqrt` only when needed.
template <typename Payload> struct ClosestPointResult
{
    crd::math::Vec3<crd::f32> point{};
    crd::f32 distance_squared{0.0F};
    Payload payload{};
};

} // namespace crd::geometry
