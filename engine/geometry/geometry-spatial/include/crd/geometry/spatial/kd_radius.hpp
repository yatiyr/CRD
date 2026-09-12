#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-spatial — kd_radius (Phase 3.1.7 v5a).
//
// Radius search: collect every point within `radius` of `query` (Euclidean).
// Output is APPENDED to `out` — caller controls capacity + lifetime; the query
// allocates nothing.
//
// Order: tree-DFS order — deterministic given a fixed tree but unspecified
// w.r.t. payload index (the lex-tuple builder yields *coordinate*-ordered
// leaves, not payload-ordered). If callers need a specific order (payload-
// ascending, distance-ascending, etc.), sort `out` post-call. Reproducibility
// across runs holds — same tree + same query = same emission sequence.
//
// The query tolerates a non-finite query point (returns no hits — every
// finite-vs-NaN comparison is false, so the AABB-prune kills every subtree
// at the root).
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/spatial/kd_tree.hpp>
#include <crd/math/vec.hpp>

namespace crd::geometry::spatial
{
using crd::math::MathScalar;
using crd::math::Vec3;

template <MathScalar T> struct KdRadiusHit
{
    crd::u32 payload{};         // original input index
    T        distance_squared{0};
};

template <MathScalar T>
void kd_radius(const KdTree<T>&                       tree,
                crd::containers::ConstSpan<Vec3<T>>    points,
                const Vec3<T>&                          query,
                T                                       radius,
                crd::containers::Array<KdRadiusHit<T>>& out) noexcept;

extern template void kd_radius<crd::f32>(const KdTree<crd::f32>&,
                                           crd::containers::ConstSpan<Vec3<crd::f32>>,
                                           const Vec3<crd::f32>&, crd::f32,
                                           crd::containers::Array<KdRadiusHit<crd::f32>>&) noexcept;
extern template void kd_radius<crd::f64>(const KdTree<crd::f64>&,
                                           crd::containers::ConstSpan<Vec3<crd::f64>>,
                                           const Vec3<crd::f64>&, crd::f64,
                                           crd::containers::Array<KdRadiusHit<crd::f64>>&) noexcept;

} // namespace crd::geometry::spatial
