#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-spatial — kd_range_aabb (Phase 3.1.7 v5a).
//
// AABB-window range search: collect every point inside `box`. Output is
// APPENDED to `out` — caller controls capacity + lifetime; the query
// allocates nothing.
//
// Inclusive bounds: a point with `box.min[i] <= p[i] <= box.max[i]` for every
// axis hits. Order: tree-DFS order — deterministic given a fixed tree but
// unspecified w.r.t. payload index (the lex-tuple builder yields *coordinate*-
// ordered leaves, not payload-ordered). Sort `out` post-call if a specific
// order is needed.
//
// Query tolerates a non-finite query AABB — finite-vs-NaN comparisons are
// false, so a NaN-laden box returns no hits. Empty box (`min > max` on any
// axis) returns no hits.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/geometry/spatial/kd_tree.hpp>
#include <crd/math/vec.hpp>

namespace crd::geometry::spatial
{
using crd::geometry::primitives::AABB3;
using crd::math::MathScalar;

template <MathScalar T>
void kd_range_aabb(const KdTree<T>&                  tree,
                    crd::containers::ConstSpan<crd::math::Vec3<T>> points,
                    const AABB3<T>&                    box,
                    crd::containers::Array<crd::u32>&  out) noexcept;

extern template void kd_range_aabb<crd::f32>(const KdTree<crd::f32>&,
                                               crd::containers::ConstSpan<crd::math::Vec3<crd::f32>>,
                                               const AABB3<crd::f32>&,
                                               crd::containers::Array<crd::u32>&) noexcept;
extern template void kd_range_aabb<crd::f64>(const KdTree<crd::f64>&,
                                               crd::containers::ConstSpan<crd::math::Vec3<crd::f64>>,
                                               const AABB3<crd::f64>&,
                                               crd::containers::Array<crd::u32>&) noexcept;

} // namespace crd::geometry::spatial
