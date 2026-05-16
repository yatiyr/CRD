#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-spatial — kd_nearest_n (Phase 3.1.7 v5a).
//
// Branch-and-bound k-nearest-neighbour search. Worst case scans the whole
// tree; balanced-median build + widest-extent split gets average ~O(log N)
// for low-dimensional clouds.
//
// API: caller passes a *fixed-capacity* `out` array (its `capacity()` is the
// `k` of k-NN). The query maintains a max-heap-by-distance over `out`'s raw
// storage via `crd::containers::push_heap` / `pop_heap` (deterministic across
// MSVC / GCC / clang — same algorithms `crd-eylem` will use). Allocates
// nothing past `out`'s pre-reserved buffer.
//
// Result order: `out` is sorted ASCENDING by `distance_squared` after the
// query returns. Lowest-payload-index tiebreak on equal distance, per
// ADR-0076 §4 pin #11.
//
// Query tolerates a non-finite query point — every finite-vs-NaN comparison
// is false, so the AABB-prune kills every subtree at the root and `out`
// returns empty.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/spatial/kd_tree.hpp>
#include <crd/math/vec.hpp>

namespace crd::geometry::spatial
{
using crd::math::MathScalar;
using crd::math::Vec3;

template <MathScalar T> struct KdNeighbor
{
    crd::u32 payload{};        // original input index
    T        distance_squared{0};
};

// `k` is passed explicitly — `Array::reserve` is a "≥ n" hint and may
// allocate more, so `out.capacity()` is not a reliable upper bound. The
// result count is min(k, point_count); `out` is sorted ascending by
// distance_squared on return.
template <MathScalar T>
void kd_nearest_n(const KdTree<T>&                       tree,
                   crd::containers::ConstSpan<Vec3<T>>    points,
                   const Vec3<T>&                          query,
                   crd::usize                              k,
                   crd::containers::Array<KdNeighbor<T>>&  out) noexcept;

extern template void kd_nearest_n<crd::f32>(const KdTree<crd::f32>&,
                                              crd::containers::ConstSpan<Vec3<crd::f32>>,
                                              const Vec3<crd::f32>&, crd::usize,
                                              crd::containers::Array<KdNeighbor<crd::f32>>&) noexcept;
extern template void kd_nearest_n<crd::f64>(const KdTree<crd::f64>&,
                                              crd::containers::ConstSpan<Vec3<crd::f64>>,
                                              const Vec3<crd::f64>&, crd::usize,
                                              crd::containers::Array<KdNeighbor<crd::f64>>&) noexcept;

} // namespace crd::geometry::spatial
