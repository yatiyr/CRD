#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-delaunay — v8d-2d 2D Voronoi diagram extraction.
//
// Geometric dual of v8a/v8b Delaunay triangulation. For an input point set
// (sites), produces:
//   - One Voronoi cell per input site, comprising the locus of points
//     closer to that site than to any other input site.
//   - Voronoi vertices = Delaunay triangle circumcentres.
//   - Voronoi edges = perpendicular bisector segments between adjacent
//     circumcentres (interior) or rays to infinity (boundary).
//
// **Algorithm**:
//   1. Run `delaunay_2d(sites)` to get triangle indices.
//   2. Reconstruct Delaunay triangle adjacency via sort-and-scan over the
//      3T half-edges `(min(u,v), max(u,v), tri_id, opposite_local_index)`.
//      Pairs of consecutive entries with same (min, max) are interior
//      edges; singletons are convex hull edges.
//   3. Compute circumcentre of every Delaunay triangle (in `f64` precision
//      regardless of `T` per D95 — `crd::geometry::primitives::circumcenter_2d`).
//   4. For each input site, walk around it via Delaunay neighbour info,
//      collecting incident triangle ids in CCW order. The corresponding
//      circumcentres are the cell's Voronoi vertices.
//   5. Cells on the convex hull are unbounded — the walk hits a null
//      neighbour. We walk both CCW and CW from the start tri to capture
//      both ends, then record the outgoing ray directions as perpendiculars
//      to the bounding Delaunay hull edges (sign-checked via dot product
//      against the opposite vertex, D96).
//
// **Output convention (D97)**:
//   - `voronoi_vertices[t]` = circumcentre of Delaunay triangle `t`.
//   - `cells[i].vertex_indices` = CCW-ordered triangle ids around site `i`.
//   - `cells[i].is_bounded = true` iff every walk step found a valid
//     neighbour (closed polygon).
//   - For unbounded cells, `first_ray_dir` is the direction of the
//     incoming ray (from infinity TO `vertex_indices.front()`), and
//     `last_ray_dir` is the direction of the outgoing ray (FROM
//     `vertex_indices.back()` TO infinity). Both rays are perpendicular
//     to the corresponding Delaunay hull edges, pointing AWAY from the
//     cell interior.
//
// **Determinism contract** (ADR-0063 + ADR-0076 §4 pin #11):
//   - Delaunay output is deterministic (per v8a).
//   - Half-edge sort uses lex-tuple `(min, max, tri_id, opp_local_idx)`.
//   - Cell start tri = lowest-id incident triangle (deterministic pick).
//   - Walk direction prefers CCW from the start tri's local site index.
//
// **Robustness contract** (ADR-0076 §15):
//   - Diagnostic statuses (`TooFewPoints` / `NonFiniteInput` /
//     `DuplicatePoint`) propagated from `delaunay_2d`.
//   - Circumcentre computed in `f64` to avoid f32 overflow on large coords
//     (D95).
//   - `InternalInvariant` returned on any walk-step inconsistency
//     (defense-in-depth — should not trip with Stage D `incircle`).
//
// **Two-layer typing** (ADR-0078 §5 D34): raw `<MathScalar T>` body; typed
// `Vec2<Length32>` wrappers in `voronoi_2d_typed.hpp` ship at slice close
// on first typed consumer.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::delaunay
{

enum class VoronoiStatus2 : crd::u8
{
    Ok                = 0,
    TooFewPoints      = 1, // < 3 input sites
    NonFiniteInput    = 2,
    DuplicatePoint    = 3,
    InternalInvariant = 4,
};

template <crd::math::MathScalar T>
struct VoronoiCell
{
    crd::u32                          site_index = 0;  // index into input sites
    crd::containers::Array<crd::u32>  vertex_indices;   // CCW ids into voronoi_vertices
    bool                              is_bounded = true;
    crd::math::Vec2<T>                first_ray_dir{};  // valid iff !is_bounded
    crd::math::Vec2<T>                last_ray_dir{};   // valid iff !is_bounded

    explicit VoronoiCell(crd::memory::IAllocator* alloc) : vertex_indices(alloc) {}
};

template <crd::math::MathScalar T>
struct VoronoiResult2
{
    crd::containers::Array<crd::math::Vec2<T>> voronoi_vertices; // circumcentres
    crd::containers::Array<VoronoiCell<T>>     cells;             // one per input site
    VoronoiStatus2                              status = VoronoiStatus2::Ok;

    explicit VoronoiResult2(crd::memory::IAllocator* alloc)
      : voronoi_vertices(alloc), cells(alloc) {}

    [[nodiscard]] bool ok() const noexcept { return status == VoronoiStatus2::Ok; }
};

// Entry point. Builds the 2D Voronoi diagram of `sites` on `alloc`.
template <crd::math::MathScalar T>
[[nodiscard]] VoronoiResult2<T>
voronoi_2d(crd::containers::ConstSpan<crd::math::Vec2<T>> sites,
           crd::memory::IAllocator*                        alloc);

} // namespace crd::geometry::delaunay
