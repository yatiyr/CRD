#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-spatial — unified query facade for v5 backends (Phase 3.1.7
// v5-queries-extension; ADR-0076 §16 pin #1 + §20).
//
// `raycast` / `overlap` / `radius` / `nearest_n` / `find_overlapping_pairs`
// as a single set of free-function overloads in `crd::geometry`, resolved
// at compile time over the five v5 backends. Coexists with the BVH facade
// in `crd/geometry/queries.hpp` (`crd-geometry-bvh`) — both sets live in
// the same `crd::geometry` namespace, so users including BOTH headers get
// one unified ADL set covering primitives + BVH + v5 spatial backends.
//
// **Compile-time overload polymorphism** (NOT virtual) per ADR-0076 §16
// pin #1. Zero overhead — the overloads forward to the native per-backend
// methods.
//
// ── Support matrix ────────────────────────────────────────────────────────
//
//                  overlap   raycast   radius   nearest_n   find_pairs
//   ──────────────────────────────────────────────────────────────────────
//   KdTree          --        --        ✓        ✓           --
//   LooseOctree     ✓         ✓         --       --          --
//   RTree           ✓         ✓         --       ✓           --
//   SpatialHash     ✓         ✓         ✓        --          ✓
//   UniformGrid     ✓         ✓         ✓        --          ✓
//   ──────────────────────────────────────────────────────────────────────
//   BvhTree         ✓         ✓         --       closest_pt  --
//   Bvh4Tree        ✓         ✓         --       closest_pt  --
//   DynamicBvh      ✓ (fat)   ✓ (fat)   --       closest_pt  ✓
//
// `--` = NOT exposed via the facade because the backend doesn't naturally
// support that query. Don't fake what doesn't fit. KdTree's natural ops
// are k-NN + radius over a point cloud (no AABBs stored); LooseOctree +
// RTree are AABB indices with no per-point radius (consumer can do
// AABB-overlap-then-filter at the call site if needed).
//
// Result types differ across backends; the overload set is type-resolved
// by the FIRST argument (the tree). Callers pass the matching output
// container:
//   * `overlap`     → `Array<u32>`                       (4 AABB backends)
//   * `raycast`     → `optional<RayHit<u32>>`            (4 AABB backends)
//   * `radius`      → `Array<KdRadiusHit<T>>`            (KdTree)
//                  / `Array<u32>`                        (SpatialHash, UniformGrid)
//   * `nearest_n`   → `Array<KdNeighbor<T>>`             (KdTree)
//                  / `Array<RTree<T>::Neighbor>`         (RTree)
//   * `find_overlapping_pairs` → `Array<SpatialHashPair>` / `Array<UniformGridPair>`
//
// Callback forms (`template <typename Fn>`) preserve the streaming-
// iteration shape from the native APIs — callback receives the natural
// per-backend payload type (typically `u32`, or `KdRadiusHit<T>` /
// `KdNeighbor<T>` for kd-tree).
//
// ── Scratch overloads ─────────────────────────────────────────────────────
//
// The facade exposes CONVENIENCE overloads only — single-thread API. Users
// who need parallel-query thread-safety call the native scratch-taking
// overload directly on the backend (`tree.overlap(q, scratch, out)`).
// Rationale: the facade is for ergonomic per-call dispatch; the scratch
// pattern is a backend-specific concept that doesn't generalise (KdTree /
// LooseOctree / RTree don't need scratch at all — their queries are
// naturally const-safe). Per `feedback_spatial_substrate_thread_safety.md`.
//
// ── Determinism + thread-safety ───────────────────────────────────────────
//
// Forwards-only — each overload preserves the backend's documented
// determinism + thread-safety contract. The facade adds no state.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/geometry/result_types.hpp>
#include <crd/geometry/spatial/kd_nearest_n.hpp>
#include <crd/geometry/spatial/kd_radius.hpp>
#include <crd/geometry/spatial/kd_tree.hpp>
#include <crd/geometry/spatial/loose_octree.hpp>
#include <crd/geometry/spatial/rtree.hpp>
#include <crd/geometry/spatial/spatial_hash.hpp>
#include <crd/geometry/spatial/uniform_grid.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

#include <limits>
#include <optional>

namespace crd::geometry
{

// =============================================================================
// overlap (AABB query) — LooseOctree / RTree / SpatialHash / UniformGrid
// =============================================================================

template <typename Fn, crd::math::MathScalar T>
inline void overlap(const spatial::LooseOctree<T>& tree, const primitives::AABB3<T>& q, Fn&& on_hit)
{
    tree.overlap(q, static_cast<Fn&&>(on_hit));
}
template <crd::math::MathScalar T>
inline void overlap(const spatial::LooseOctree<T>& tree, const primitives::AABB3<T>& q,
                     crd::containers::Array<crd::u32>& out)
{
    tree.overlap(q, out);
}

template <typename Fn, crd::math::MathScalar T>
inline void overlap(const spatial::RTree<T>& tree, const primitives::AABB3<T>& q, Fn&& on_hit)
{
    tree.overlap(q, static_cast<Fn&&>(on_hit));
}
template <crd::math::MathScalar T>
inline void overlap(const spatial::RTree<T>& tree, const primitives::AABB3<T>& q,
                     crd::containers::Array<crd::u32>& out)
{
    tree.overlap(q, out);
}

template <typename Fn, crd::math::MathScalar T>
inline void overlap(const spatial::SpatialHash<T>& tree, const primitives::AABB3<T>& q, Fn&& on_hit)
{
    tree.overlap(q, static_cast<Fn&&>(on_hit));
}
template <crd::math::MathScalar T>
inline void overlap(const spatial::SpatialHash<T>& tree, const primitives::AABB3<T>& q,
                     crd::containers::Array<crd::u32>& out)
{
    tree.overlap(q, out);
}

template <typename Fn, crd::math::MathScalar T>
inline void overlap(const spatial::UniformGrid<T>& tree, const primitives::AABB3<T>& q, Fn&& on_hit)
{
    tree.overlap(q, static_cast<Fn&&>(on_hit));
}
template <crd::math::MathScalar T>
inline void overlap(const spatial::UniformGrid<T>& tree, const primitives::AABB3<T>& q,
                     crd::containers::Array<crd::u32>& out)
{
    tree.overlap(q, out);
}

// =============================================================================
// raycast — LooseOctree / RTree / SpatialHash / UniformGrid
// =============================================================================

template <crd::math::MathScalar T>
[[nodiscard]] inline std::optional<crd::geometry::RayHit<crd::u32>>
raycast(const spatial::LooseOctree<T>& tree, const primitives::Ray3<T>& ray,
         T tmax = std::numeric_limits<T>::infinity()) noexcept
{
    return tree.raycast(ray, tmax);
}

template <crd::math::MathScalar T>
[[nodiscard]] inline std::optional<crd::geometry::RayHit<crd::u32>>
raycast(const spatial::RTree<T>& tree, const primitives::Ray3<T>& ray,
         T tmax = std::numeric_limits<T>::infinity()) noexcept
{
    return tree.raycast(ray, tmax);
}

template <crd::math::MathScalar T>
[[nodiscard]] inline std::optional<crd::geometry::RayHit<crd::u32>>
raycast(const spatial::SpatialHash<T>& tree, const primitives::Ray3<T>& ray,
         T tmax = std::numeric_limits<T>::infinity()) noexcept
{
    return tree.raycast(ray, tmax);
}

template <crd::math::MathScalar T>
[[nodiscard]] inline std::optional<crd::geometry::RayHit<crd::u32>>
raycast(const spatial::UniformGrid<T>& tree, const primitives::Ray3<T>& ray,
         T tmax = std::numeric_limits<T>::infinity()) noexcept
{
    return tree.raycast(ray, tmax);
}

// =============================================================================
// radius (sphere query) — KdTree / SpatialHash / UniformGrid
// =============================================================================

// KdTree — point cloud. Returns Array<KdRadiusHit<T>> (payload + dist²).
// kd-tree is non-owning over its point span; user passes points alongside.
template <crd::math::MathScalar T>
inline void radius(const spatial::KdTree<T>&                                tree,
                    crd::containers::ConstSpan<crd::math::Vec3<T>>          points,
                    const crd::math::Vec3<T>&                                query,
                    T                                                        r,
                    crd::containers::Array<spatial::KdRadiusHit<T>>&         out)
{
    spatial::kd_radius<T>(tree, points, query, r, out);
}

// SpatialHash — AABB index. Returns Array<u32>.
template <typename Fn, crd::math::MathScalar T>
inline void radius(const spatial::SpatialHash<T>& tree, const crd::math::Vec3<T>& q, T r, Fn&& on_hit)
{
    tree.radius(q, r, static_cast<Fn&&>(on_hit));
}
template <crd::math::MathScalar T>
inline void radius(const spatial::SpatialHash<T>& tree, const crd::math::Vec3<T>& q, T r,
                    crd::containers::Array<crd::u32>& out)
{
    tree.radius(q, r, out);
}

// UniformGrid — AABB index. Returns Array<u32>.
template <typename Fn, crd::math::MathScalar T>
inline void radius(const spatial::UniformGrid<T>& tree, const crd::math::Vec3<T>& q, T r, Fn&& on_hit)
{
    tree.radius(q, r, static_cast<Fn&&>(on_hit));
}
template <crd::math::MathScalar T>
inline void radius(const spatial::UniformGrid<T>& tree, const crd::math::Vec3<T>& q, T r,
                    crd::containers::Array<crd::u32>& out)
{
    tree.radius(q, r, out);
}

// =============================================================================
// nearest_n (k-NN) — KdTree / RTree
// =============================================================================

template <crd::math::MathScalar T>
inline void nearest_n(const spatial::KdTree<T>&                       tree,
                       crd::containers::ConstSpan<crd::math::Vec3<T>> points,
                       const crd::math::Vec3<T>&                       query,
                       crd::usize                                       k,
                       crd::containers::Array<spatial::KdNeighbor<T>>& out)
{
    spatial::kd_nearest_n<T>(tree, points, query, k, out);
}

template <crd::math::MathScalar T>
inline void nearest_n(const spatial::RTree<T>&                                       tree,
                       const crd::math::Vec3<T>&                                     query,
                       crd::usize                                                     k,
                       crd::containers::Array<typename spatial::RTree<T>::Neighbor>& out)
{
    tree.nearest_n(query, k, out);
}

// =============================================================================
// find_overlapping_pairs — SpatialHash / UniformGrid
// =============================================================================
//
// (DynamicBvh's version lives in `crd/geometry/queries.hpp` — both facades
// coexist by name; the overload set resolves per-tree-type.)

template <crd::math::MathScalar T>
inline void find_overlapping_pairs(const spatial::SpatialHash<T>&                    tree,
                                    crd::containers::Array<spatial::SpatialHashPair>& out)
{
    tree.find_overlapping_pairs(out);
}

template <crd::math::MathScalar T>
inline void find_overlapping_pairs(const spatial::UniformGrid<T>&                     tree,
                                    crd::containers::Array<spatial::UniformGridPair>& out)
{
    tree.find_overlapping_pairs(out);
}

} // namespace crd::geometry
