#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-delaunay — v8d-3d 3D Voronoi cells extraction.
//
// Geometric dual of v8c 3D Bowyer-Watson tetrahedralisation. For an input
// site set, produces one Voronoi cell per input site, where each cell is
// the locus of points closer to that site than to any other input site.
// Cells are polyhedra (in 3D); their faces are perpendicular bisector
// polygons dual to Delaunay edges.
//
// **Algorithm**:
//   1. Run `delaunay_3d(sites)` to get tet indices.
//   2. Reconstruct Delaunay tet face adjacency via sort-and-scan over 4T
//      half-faces keyed `(sorted v0/v1/v2, tet_id, opp_local_idx)` — per
//      ADR-0076 §4 pin #11 lex-tuple determinism, no HashMap.
//   3. Compute circumcentre per Delaunay tet via
//      `crd::geometry::primitives::circumcenter_3d` (lifted to f64 per
//      D100 — coord^2 products overflow f32 above ~10^4).
//   4. Build Delaunay edge → tet fan index via sort-and-scan over 6T
//      half-edge records (each tet contributes 6 edges).
//   5. Per site, per incident Delaunay edge: walk the tet fan around the
//      edge axis via face-adjacency. Each fan-tet's circumcentre is one
//      face vertex; CCW order (D99 — from neighbor_site's side) gives a
//      consistent outward face normal. Null opposite-tet → face is
//      unbounded.
//   6. Cell.is_bounded = !any face.is_unbounded.
//
// **Output (D101)**:
//   - `voronoi_vertices[t]` = circumcentre of Delaunay tet `t`.
//   - `cells[i].site_index == i` (cells in input-site order).
//   - Each cell has one face per Delaunay edge incident to the site.
//   - Each face's `vertex_indices` are CCW from the NEIGHBOR side (so the
//     face normal points from site TOWARD neighbor — outward from this cell).
//
// **ConvexHullView helper**:
//   `convex_hull_view_for_cell(result, cell_index, alloc) -> ConvexHullView<T>`
//   converts a bounded cell's DCEL to a `ConvexHullView<T>` (vertices +
//   outward face planes). Returns an empty view if the cell is unbounded.
//   First-class consumer convenience for NNI / Worley / grain-structure
//   downstream code that wants the convex-hull form.
//
// **Determinism contract** (ADR-0063 + ADR-0076 §4 pin #11):
//   - Delaunay output is deterministic (per v8c).
//   - Half-face sort lex-tuple `(v_min, v_mid, v_max, tet_id, opp_local_idx)`.
//   - Half-edge sort lex-tuple `(v_min, v_max, tet_id, local_edge_idx)`.
//   - Face start-tet = lowest-id fan tet; deterministic walk direction.
//
// **Robustness contract** (ADR-0076 §15):
//   - Diagnostic statuses (`TooFewPoints` / `NonFiniteInput` /
//     `DuplicatePoint` / `Coplanar`) propagated from `delaunay_3d`.
//   - Circumcentre lifted to f64 (D100).
//   - `InternalInvariant` returned on any walk-step inconsistency
//     (defense-in-depth — should not trip with Stage D `insphere`).
//
// **Two-layer typing** (ADR-0078 §5 D34): raw `<MathScalar T>` body;
// typed `Vec3<Length32>` wrappers in `voronoi_3d_typed.hpp` ship at slice
// close on first typed consumer.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::delaunay
{

enum class VoronoiStatus3 : crd::u8
{
    Ok                = 0,
    TooFewPoints      = 1, // < 4 input sites
    NonFiniteInput    = 2,
    DuplicatePoint    = 3,
    Coplanar          = 4, // all N points coplanar
    InternalInvariant = 5,
};

template <crd::math::MathScalar T>
struct VoronoiFace3
{
    crd::u32                          neighbor_site_index = 0; // dual to Delaunay edge (site, neighbor)
    crd::containers::Array<crd::u32>  vertex_indices;           // CCW from neighbor_site side
    bool                              is_unbounded = false;     // polygon doesn't close

    explicit VoronoiFace3(crd::memory::IAllocator* alloc) : vertex_indices(alloc) {}
};

template <crd::math::MathScalar T>
struct VoronoiCell3
{
    crd::u32                                site_index = 0;
    crd::containers::Array<VoronoiFace3<T>> faces;
    bool                                    is_bounded = true;

    explicit VoronoiCell3(crd::memory::IAllocator* alloc) : faces(alloc) {}
};

template <crd::math::MathScalar T>
struct VoronoiResult3
{
    crd::containers::Array<crd::math::Vec3<T>> voronoi_vertices; // tet circumcentres
    crd::containers::Array<VoronoiCell3<T>>    cells;             // one per input site
    VoronoiStatus3                              status = VoronoiStatus3::Ok;

    explicit VoronoiResult3(crd::memory::IAllocator* alloc)
      : voronoi_vertices(alloc), cells(alloc) {}

    [[nodiscard]] bool ok() const noexcept { return status == VoronoiStatus3::Ok; }
};

// Owning convex-hull representation of a bounded Voronoi cell. Vertices +
// outward face planes + face-vertex offset table (prefix-sum); the data
// matches `ConvexHullView<T>`'s field layout. Allocated on the caller's
// allocator. Returned by `convex_hull_for_cell`. Empty (.faces is_empty)
// for unbounded cells.
template <crd::math::MathScalar T>
struct VoronoiCellHull3
{
    crd::containers::Array<crd::math::Vec3<T>>           vertices;
    crd::containers::Array<crd::geometry::primitives::Plane<T>> faces;
    crd::containers::Array<crd::u32>                       face_vertex_indices;
    crd::containers::Array<crd::u32>                       face_vertex_offsets; // prefix-sum (size = faces.size() + 1)

    explicit VoronoiCellHull3(crd::memory::IAllocator* alloc)
      : vertices(alloc), faces(alloc), face_vertex_indices(alloc), face_vertex_offsets(alloc) {}

    [[nodiscard]] bool empty() const noexcept { return faces.size() == 0U; }

    // Non-owning view over this storage. Lifetime = this object.
    [[nodiscard]] crd::geometry::primitives::ConvexHullView<T> view() const noexcept
    {
        crd::geometry::primitives::ConvexHullView<T> v{};
        v.vertices = crd::containers::ConstSpan<crd::math::Vec3<T>>{vertices.data(), vertices.size()};
        v.faces = crd::containers::ConstSpan<crd::geometry::primitives::Plane<T>>{faces.data(), faces.size()};
        v.face_vertex_indices = crd::containers::ConstSpan<crd::u32>{face_vertex_indices.data(), face_vertex_indices.size()};
        v.face_vertex_offsets = crd::containers::ConstSpan<crd::u32>{face_vertex_offsets.data(), face_vertex_offsets.size()};
        return v;
    }
};

// Entry point. Builds the 3D Voronoi diagram of `sites` on `alloc`.
template <crd::math::MathScalar T>
[[nodiscard]] VoronoiResult3<T>
voronoi_3d(crd::containers::ConstSpan<crd::math::Vec3<T>> sites,
           crd::memory::IAllocator*                        alloc);

// Convert one bounded Voronoi cell to a ConvexHullView-compatible form.
// Returns an empty `VoronoiCellHull3` if the cell is unbounded or
// `cell_index` is out of range.
template <crd::math::MathScalar T>
[[nodiscard]] VoronoiCellHull3<T>
convex_hull_for_cell(const VoronoiResult3<T>&                          result,
                      crd::containers::ConstSpan<crd::math::Vec3<T>>    sites,
                      crd::u32                                          cell_index,
                      crd::memory::IAllocator*                          alloc);

} // namespace crd::geometry::delaunay
