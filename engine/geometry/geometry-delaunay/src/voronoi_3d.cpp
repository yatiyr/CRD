// ---------------------------------------------------------------------------
// crd-geometry-delaunay — v8d-3d 3D Voronoi cells extraction.
//
// See voronoi_3d.hpp for the algorithm contract. This TU owns:
//   - Half-face sort-and-scan to reconstruct Delaunay tet face adjacency.
//   - Half-edge sort-and-scan to reconstruct Delaunay edge -> tet fan map.
//   - Per-site cell walk: enumerate incident edges, walk each edge's tet
//     fan to collect face vertices (D98 — edge-fan walk direction; D99 —
//     CCW from neighbor_site side; D101 — unbounded face = any null
//     opposite-tet during the fan walk).
//   - ConvexHullView helper that converts a bounded cell's DCEL to a
//     vertices + face-planes + face-vertex-offsets bundle.
//
// Pinned design decisions D98-D101 (carryover for ADR-0076 §23 at v8-close):
//
//   D98. **Edge-fan walk direction**: from a tet with edge (s, n) at local
//        indices (si, ni), the 2 fan-faces (faces containing BOTH s and
//        n) are at local indices `{0,1,2,3} \ {si, ni}`. The 2 off-axis
//        faces contain only one of (s, n). Walk one direction by stepping
//        through the lower-index fan-face first; if that closes the fan
//        we're done (bounded face), else walk the OTHER fan-face direction
//        from the start tet and splice (unbounded face).
//
//   D99. **Face vertex CCW from neighbor_site side**: the face normal
//        points from `site` TOWARD `neighbor` — outward from THIS cell.
//        Vertex order verified via the cross-product sum (Newell's normal
//        formula) sign-aligned with `(neighbor - site)`. If the natural
//        walk produces the opposite winding, reverse before emit.
//
//   D100. **circumcenter_3d lifted to f64** (mirror of D95 for 3D). Already
//         in `crd-geometry-primitives::circumcenter.hpp`.
//
//   D101. **Unbounded face detection**: any null opposite-tet encountered
//         during the edge-fan walk makes the face unbounded. Cell.is_bounded
//         = !any face.is_unbounded.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/delaunay/delaunay_3d.hpp>
#include <crd/geometry/delaunay/voronoi_3d.hpp>
#include <crd/geometry/primitives/circumcenter.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <limits>
#include <utility>

namespace crd::geometry::delaunay
{

namespace
{

constexpr crd::u32 kNullNbr = std::numeric_limits<crd::u32>::max();

// Face local index = vertex it's opposite to (matches v8c's convention).
// face_vertices_table[i] is the 3-vertex local-index triple of face i:
//   face[0] = {1, 2, 3} (opposite v0; outward-CCW order in v8c)
//   face[1] = {0, 2, 3}
//   face[2] = {0, 1, 3}
//   face[3] = {0, 1, 2}
// (For face-vertex enumeration here we don't need v8c's CCW order; we just
// need to extract the 3 vertices NOT at the opposite index.)
constexpr crd::u32 kFaceVerts[4][3] = {
    {1U, 2U, 3U}, {0U, 2U, 3U}, {0U, 1U, 3U}, {0U, 1U, 2U},
};

// Edge local index → endpoint local indices (the 6 edges of a tet).
constexpr crd::u32 kEdgeVerts[6][2] = {
    {0U, 1U}, {0U, 2U}, {0U, 3U}, {1U, 2U}, {1U, 3U}, {2U, 3U},
};

inline VoronoiStatus3 propagate_status(DelaunayStatus3 s) noexcept
{
    switch (s)
    {
        case DelaunayStatus3::Ok:                return VoronoiStatus3::Ok;
        case DelaunayStatus3::TooFewPoints:      return VoronoiStatus3::TooFewPoints;
        case DelaunayStatus3::NonFiniteInput:    return VoronoiStatus3::NonFiniteInput;
        case DelaunayStatus3::DuplicatePoint:    return VoronoiStatus3::DuplicatePoint;
        case DelaunayStatus3::Coplanar:          return VoronoiStatus3::Coplanar;
        case DelaunayStatus3::InternalInvariant: return VoronoiStatus3::InternalInvariant;
    }
    return VoronoiStatus3::InternalInvariant;
}

// Sort 3 u32s ascending in place via fixed comparisons.
inline void sort3(crd::u32& a, crd::u32& b, crd::u32& c) noexcept
{
    if (a > b) { std::swap(a, b); }
    if (b > c) { std::swap(b, c); }
    if (a > b) { std::swap(a, b); }
}

// Rebuild Delaunay tet face adjacency from `tet_indices`. For T tets, emits
// 4T half-faces keyed by sorted-vertex-triple, sorts lex, and pairs
// consecutive entries with matching keys.
//
// Output `tet_nbrs[4*t + k]` = adjacent tet across face k of tet t, or
// kNullNbr if hull boundary. Face k = face opposite vertex k (matches v8c).
void build_tet_face_adjacency(const crd::containers::Array<crd::u32>& tet_indices,
                                crd::u32                                  tet_count,
                                crd::memory::IAllocator*                  alloc,
                                crd::containers::Array<crd::u32>&         out_nbrs)
{
    out_nbrs.resize(static_cast<crd::usize>(tet_count) * 4U, kNullNbr);

    struct HalfFace
    {
        crd::u32 v0; // sorted asc
        crd::u32 v1;
        crd::u32 v2;
        crd::u32 tet;
        crd::u32 k;  // local face index
    };
    crd::containers::Array<HalfFace> hfs(alloc);
    hfs.reserve(static_cast<crd::usize>(tet_count) * 4U);
    for (crd::u32 t = 0; t < tet_count; ++t)
    {
        for (crd::u32 k = 0; k < 4U; ++k)
        {
            HalfFace hf{};
            hf.v0 = tet_indices[4U * t + kFaceVerts[k][0]];
            hf.v1 = tet_indices[4U * t + kFaceVerts[k][1]];
            hf.v2 = tet_indices[4U * t + kFaceVerts[k][2]];
            sort3(hf.v0, hf.v1, hf.v2);
            hf.tet = t;
            hf.k   = k;
            hfs.push_back(hf);
        }
    }
    crd::containers::sort(hfs.data(), hfs.data() + hfs.size(),
                          [](const HalfFace& l, const HalfFace& r) noexcept {
                              if (l.v0 != r.v0) { return l.v0 < r.v0; }
                              if (l.v1 != r.v1) { return l.v1 < r.v1; }
                              if (l.v2 != r.v2) { return l.v2 < r.v2; }
                              if (l.tet != r.tet) { return l.tet < r.tet; }
                              return l.k < r.k;
                          });
    for (crd::u32 i = 0; i + 1U < hfs.size(); ++i)
    {
        const auto& a = hfs[i];
        const auto& b = hfs[i + 1U];
        if (a.v0 == b.v0 && a.v1 == b.v1 && a.v2 == b.v2)
        {
            out_nbrs[4U * a.tet + a.k] = b.tet;
            out_nbrs[4U * b.tet + b.k] = a.tet;
            ++i; // consume pair
        }
    }
}

// Build Delaunay edge → tet fan index. Output is:
//   - edges_sorted: array of HalfEdge records sorted lex by (vmin, vmax, tet, k)
//   - edge_starts: for each edge (vmin, vmax), the start offset in edges_sorted
//                   (built as a side index via {min, max} -> start_idx pairs)
// Each edge appears once per tet that contains it (6T total records).
struct HalfEdge
{
    crd::u32 vmin;
    crd::u32 vmax;
    crd::u32 tet;
    crd::u32 edge_idx;
};

void build_edge_fans(const crd::containers::Array<crd::u32>& tet_indices,
                      crd::u32                                  tet_count,
                      crd::containers::Array<HalfEdge>&         out_edges)
{
    out_edges.reserve(static_cast<crd::usize>(tet_count) * 6U);
    for (crd::u32 t = 0; t < tet_count; ++t)
    {
        for (crd::u32 e = 0; e < 6U; ++e)
        {
            crd::u32 a = tet_indices[4U * t + kEdgeVerts[e][0]];
            crd::u32 b = tet_indices[4U * t + kEdgeVerts[e][1]];
            HalfEdge he{};
            he.vmin     = a < b ? a : b;
            he.vmax     = a < b ? b : a;
            he.tet      = t;
            he.edge_idx = e;
            out_edges.push_back(he);
        }
    }
    crd::containers::sort(out_edges.data(), out_edges.data() + out_edges.size(),
                          [](const HalfEdge& l, const HalfEdge& r) noexcept {
                              if (l.vmin != r.vmin) { return l.vmin < r.vmin; }
                              if (l.vmax != r.vmax) { return l.vmax < r.vmax; }
                              if (l.tet  != r.tet)  { return l.tet  < r.tet;  }
                              return l.edge_idx < r.edge_idx;
                          });
}

inline crd::u32 local_index_of_3d(const crd::containers::Array<crd::u32>& tet_indices,
                                   crd::u32 t, crd::u32 site) noexcept
{
    for (crd::u32 k = 0; k < 4U; ++k)
    {
        if (tet_indices[4U * t + k] == site) { return k; }
    }
    return 4U;
}

// In tet `to`, find which local face index points BACK at tet `from`. Used
// when walking the edge fan: after stepping from `from` to `to` across some
// face, identify the face of `to` we came through.
inline crd::u32 entered_face_of(const crd::containers::Array<crd::u32>& tet_nbrs,
                                 crd::u32 to, crd::u32 from) noexcept
{
    for (crd::u32 k = 0; k < 4U; ++k)
    {
        if (tet_nbrs[4U * to + k] == from) { return k; }
    }
    return 4U;
}

// Walk the tet fan around the Delaunay edge (s, n) starting from a known
// tet `start_t`. Collect tet ids in CCW order around the edge axis. Returns
// (was_closed, fan_tets_in_order) — fan_tets is the ordered list of tets,
// already spliced if unbounded.
struct EdgeFanResult
{
    crd::containers::Array<crd::u32> fan_tets;
    bool                              closed = true;
    bool                              error  = false;

    explicit EdgeFanResult(crd::memory::IAllocator* alloc) : fan_tets(alloc) {}
};

EdgeFanResult walk_edge_fan(crd::u32                                  s,
                              crd::u32                                  n,
                              crd::u32                                  start_t,
                              const crd::containers::Array<crd::u32>&   tet_indices,
                              const crd::containers::Array<crd::u32>&   tet_nbrs,
                              crd::memory::IAllocator*                  alloc)
{
    EdgeFanResult res{alloc};
    res.fan_tets.push_back(start_t);

    const crd::u32 si_start = local_index_of_3d(tet_indices, start_t, s);
    const crd::u32 ni_start = local_index_of_3d(tet_indices, start_t, n);
    if (si_start >= 4U || ni_start >= 4U) { res.error = true; return res; }

    // The 2 fan-faces of start_t are the 2 indices in {0,1,2,3} \ {si, ni}.
    crd::u32 face_a = 4U;
    crd::u32 face_b = 4U;
    for (crd::u32 k = 0; k < 4U; ++k)
    {
        if (k == si_start || k == ni_start) { continue; }
        if (face_a == 4U) { face_a = k; } else { face_b = k; }
    }
    if (face_b == 4U) { res.error = true; return res; }

    // Forward walk: cross face_a first.
    bool fwd_closed = false;
    crd::u32 prev = start_t;
    crd::u32 cur  = tet_nbrs[4U * start_t + face_a];
    while (cur != kNullNbr)
    {
        if (cur == start_t) { fwd_closed = true; break; }
        res.fan_tets.push_back(cur);
        // In cur, find the OTHER fan-face (the one not pointing back at prev).
        const crd::u32 si_cur = local_index_of_3d(tet_indices, cur, s);
        const crd::u32 ni_cur = local_index_of_3d(tet_indices, cur, n);
        if (si_cur >= 4U || ni_cur >= 4U) { res.error = true; return res; }
        const crd::u32 entered = entered_face_of(tet_nbrs, cur, prev);
        if (entered >= 4U) { res.error = true; return res; }
        // Two fan-faces of cur: indices in {0,1,2,3} \ {si_cur, ni_cur}.
        // Exit via the one that isn't `entered`.
        crd::u32 exit_face = 4U;
        for (crd::u32 k = 0; k < 4U; ++k)
        {
            if (k == si_cur || k == ni_cur || k == entered) { continue; }
            exit_face = k;
            break;
        }
        if (exit_face >= 4U) { res.error = true; return res; }
        prev = cur;
        cur  = tet_nbrs[4U * cur + exit_face];
    }

    if (fwd_closed)
    {
        res.closed = true;
        return res;
    }

    // Forward walk hit a null — unbounded fan. Walk backward from start_t.
    crd::containers::Array<crd::u32> backward(alloc);
    prev = start_t;
    cur  = tet_nbrs[4U * start_t + face_b];
    while (cur != kNullNbr)
    {
        if (cur == start_t)
        {
            // Shouldn't happen if forward already hit null — defensive.
            break;
        }
        backward.push_back(cur);
        const crd::u32 si_cur = local_index_of_3d(tet_indices, cur, s);
        const crd::u32 ni_cur = local_index_of_3d(tet_indices, cur, n);
        if (si_cur >= 4U || ni_cur >= 4U) { res.error = true; return res; }
        const crd::u32 entered = entered_face_of(tet_nbrs, cur, prev);
        if (entered >= 4U) { res.error = true; return res; }
        crd::u32 exit_face = 4U;
        for (crd::u32 k = 0; k < 4U; ++k)
        {
            if (k == si_cur || k == ni_cur || k == entered) { continue; }
            exit_face = k;
            break;
        }
        if (exit_face >= 4U) { res.error = true; return res; }
        prev = cur;
        cur  = tet_nbrs[4U * cur + exit_face];
    }

    // Splice: reverse(backward) + fan_tets (which currently = [start_t, fwd...]).
    crd::containers::Array<crd::u32> spliced(alloc);
    spliced.reserve(backward.size() + res.fan_tets.size());
    for (crd::u32 i = static_cast<crd::u32>(backward.size()); i > 0; --i)
    {
        spliced.push_back(backward[i - 1U]);
    }
    for (crd::u32 i = 0; i < res.fan_tets.size(); ++i)
    {
        spliced.push_back(res.fan_tets[i]);
    }
    res.fan_tets = std::move(spliced);
    res.closed   = false;
    return res;
}

} // anonymous namespace

template <crd::math::MathScalar T>
VoronoiResult3<T>
voronoi_3d(crd::containers::ConstSpan<crd::math::Vec3<T>> sites,
           crd::memory::IAllocator*                        alloc)
{
    VoronoiResult3<T> result{alloc};
    // n = site count.
    const crd::u32 n = static_cast<crd::u32>(sites.size());

    // Phase 1: run Delaunay.
    auto del = delaunay_3d<T>(sites, alloc);
    if (!del.ok())
    {
        result.status = propagate_status(del.status);
        return result;
    }
    const crd::u32 tet_count = del.tet_count;

    // Phase 2: rebuild tet face adjacency.
    crd::containers::Array<crd::u32> tet_nbrs(alloc);
    build_tet_face_adjacency(del.tet_indices, tet_count, alloc, tet_nbrs);

    // Phase 3: circumcentres.
    result.voronoi_vertices.resize(tet_count);
    for (crd::u32 t = 0; t < tet_count; ++t)
    {
        const crd::u32 ia = del.tet_indices[4U * t + 0U];
        const crd::u32 ib = del.tet_indices[4U * t + 1U];
        const crd::u32 ic = del.tet_indices[4U * t + 2U];
        const crd::u32 id = del.tet_indices[4U * t + 3U];
        result.voronoi_vertices[t] = crd::geometry::primitives::circumcenter_3d(
            sites[ia], sites[ib], sites[ic], sites[id]);
    }

    // Phase 4: edge → tet fan index. Sort half-edges by (vmin, vmax, tet, edge).
    crd::containers::Array<HalfEdge> edges(alloc);
    build_edge_fans(del.tet_indices, tet_count, edges);

    // Phase 5: per-site cell construction. Walk through `edges`, accumulating
    // contiguous runs with the same (vmin, vmax) — each run is one Delaunay
    // edge. For each edge incident to site s, walk the tet fan and emit a
    // face on cell s pointing at the other endpoint.
    result.cells.reserve(n);
    for (crd::u32 s = 0; s < n; ++s)
    {
        VoronoiCell3<T> cell{alloc};
        cell.site_index = s;
        result.cells.push_back(std::move(cell));
    }

    crd::u32 i = 0;
    while (i < edges.size())
    {
        const crd::u32 vmin = edges[i].vmin;
        const crd::u32 vmax = edges[i].vmax;
        const crd::u32 run_start = i;
        while (i < edges.size() && edges[i].vmin == vmin && edges[i].vmax == vmax)
        {
            ++i;
        }
        // run_start..i is the set of tets containing edge (vmin, vmax).
        // Skip if vmin >= n (super-tet edges shouldn't appear here since
        // delaunay_3d strips them, but defensive).
        if (vmin >= n || vmax >= n) { continue; }

        // The starting tet for the fan walk: lowest tet id in the run
        // (deterministic). Take from edges[run_start] which is the smallest
        // tet (lex-sorted).
        const crd::u32 start_t = edges[run_start].tet;

        // Build the face for site vmin (neighbor = vmax) AND for site vmax
        // (neighbor = vmin). Both share the same fan ring of circumcentres,
        // but their face vertex ordering differs (CCW from each side).
        EdgeFanResult fan = walk_edge_fan(vmin, vmax, start_t, del.tet_indices,
                                            tet_nbrs, alloc);
        if (fan.error)
        {
            result.status = VoronoiStatus3::InternalInvariant;
            return result;
        }

        // Compute face for site vmin: neighbor_site_index = vmax.
        // The natural fan ordering may be CCW around (vmax - vmin) axis or
        // around (vmin - vmax) axis depending on walk direction. We test
        // via the cross-product sum and reverse if needed to make the face
        // normal align with (vmax - vmin) for site vmin's cell, and
        // (vmin - vmax) for site vmax's cell.
        const auto& p_min = sites[vmin];
        const auto& p_max = sites[vmax];
        const T axis_x = p_max.x - p_min.x;
        const T axis_y = p_max.y - p_min.y;
        const T axis_z = p_max.z - p_min.z;

        // Compute average normal from triangle fan of fan_tets' circumcentres.
        const auto& v0 = result.voronoi_vertices[fan.fan_tets[0]];
        T sum_nx = static_cast<T>(0);
        T sum_ny = static_cast<T>(0);
        T sum_nz = static_cast<T>(0);
        for (crd::u32 fi = 1; fi + 1U < fan.fan_tets.size(); ++fi)
        {
            const auto& vi = result.voronoi_vertices[fan.fan_tets[fi]];
            const auto& vj = result.voronoi_vertices[fan.fan_tets[fi + 1U]];
            const T ax = vi.x - v0.x;
            const T ay = vi.y - v0.y;
            const T az = vi.z - v0.z;
            const T bx = vj.x - v0.x;
            const T by = vj.y - v0.y;
            const T bz = vj.z - v0.z;
            sum_nx += ay * bz - az * by;
            sum_ny += az * bx - ax * bz;
            sum_nz += ax * by - ay * bx;
        }
        const T dot_with_axis = sum_nx * axis_x + sum_ny * axis_y + sum_nz * axis_z;
        // dot > 0: fan normal aligns with (vmax - vmin) → CCW from vmin's POV.
        // For site vmin's face: we want normal pointing TOWARD vmax (outward
        // from vmin's cell). dot > 0 means current order satisfies this.
        // For site vmax's face: we want normal pointing TOWARD vmin (outward
        // from vmax's cell). Current order has normal toward vmax; reverse.

        VoronoiFace3<T> face_for_vmin{alloc};
        face_for_vmin.neighbor_site_index = vmax;
        face_for_vmin.is_unbounded        = !fan.closed;
        face_for_vmin.vertex_indices.reserve(fan.fan_tets.size());

        VoronoiFace3<T> face_for_vmax{alloc};
        face_for_vmax.neighbor_site_index = vmin;
        face_for_vmax.is_unbounded        = !fan.closed;
        face_for_vmax.vertex_indices.reserve(fan.fan_tets.size());

        if (dot_with_axis > static_cast<T>(0))
        {
            // Forward order = CCW from vmin side.
            for (crd::u32 fi = 0; fi < fan.fan_tets.size(); ++fi)
            {
                face_for_vmin.vertex_indices.push_back(fan.fan_tets[fi]);
            }
            // Reverse for vmax side.
            for (crd::u32 fi = static_cast<crd::u32>(fan.fan_tets.size()); fi > 0; --fi)
            {
                face_for_vmax.vertex_indices.push_back(fan.fan_tets[fi - 1U]);
            }
        }
        else
        {
            // Reverse order = CCW from vmin side.
            for (crd::u32 fi = static_cast<crd::u32>(fan.fan_tets.size()); fi > 0; --fi)
            {
                face_for_vmin.vertex_indices.push_back(fan.fan_tets[fi - 1U]);
            }
            for (crd::u32 fi = 0; fi < fan.fan_tets.size(); ++fi)
            {
                face_for_vmax.vertex_indices.push_back(fan.fan_tets[fi]);
            }
        }

        result.cells[vmin].faces.push_back(std::move(face_for_vmin));
        result.cells[vmax].faces.push_back(std::move(face_for_vmax));
    }

    // is_bounded propagation: cell unbounded iff any face is_unbounded.
    for (auto& cell : result.cells)
    {
        cell.is_bounded = true;
        for (const auto& face : cell.faces)
        {
            if (face.is_unbounded) { cell.is_bounded = false; break; }
        }
    }

    result.status = VoronoiStatus3::Ok;
    return result;
}

template <crd::math::MathScalar T>
VoronoiCellHull3<T>
convex_hull_for_cell(const VoronoiResult3<T>&                          result,
                      crd::containers::ConstSpan<crd::math::Vec3<T>>    sites,
                      crd::u32                                          cell_index,
                      crd::memory::IAllocator*                          alloc)
{
    VoronoiCellHull3<T> hull{alloc};

    if (cell_index >= result.cells.size()) { return hull; }
    const auto& cell = result.cells[cell_index];
    if (!cell.is_bounded) { return hull; }
    if (cell.faces.size() < 4U) { return hull; } // a bounded polyhedron needs >= 4 faces

    const auto& site = sites[cell.site_index];

    // Collect unique vertex indices used by any face; build a remapping
    // from global voronoi_vertex_index to local hull vertex index.
    crd::containers::Array<crd::u32> global_to_local(alloc);
    global_to_local.resize(result.voronoi_vertices.size(), kNullNbr);
    for (const auto& face : cell.faces)
    {
        for (crd::u32 vi : face.vertex_indices)
        {
            if (global_to_local[vi] == kNullNbr)
            {
                global_to_local[vi] = static_cast<crd::u32>(hull.vertices.size());
                hull.vertices.push_back(result.voronoi_vertices[vi]);
            }
        }
    }

    // Emit faces: one Plane per face, vertex_indices (remapped to local) +
    // offsets prefix-sum.
    hull.face_vertex_offsets.push_back(0U);
    for (const auto& face : cell.faces)
    {
        const auto& nbr = sites[face.neighbor_site_index];
        // Plane normal: points from site TOWARD neighbor (= outward from
        // this cell). Plane passes through midpoint of site<->neighbor.
        T nx = nbr.x - site.x;
        T ny = nbr.y - site.y;
        T nz = nbr.z - site.z;
        // Normalize.
        const T len2 = nx * nx + ny * ny + nz * nz;
        if (len2 <= static_cast<T>(0))
        {
            // Degenerate (shouldn't happen — Delaunay rejects duplicates).
            hull.faces.push_back(crd::geometry::primitives::Plane<T>{});
            hull.face_vertex_offsets.push_back(static_cast<crd::u32>(hull.face_vertex_indices.size()));
            continue;
        }
        // Normalize via f64 sqrt for stability on large coords.
        const T inv_len = static_cast<T>(1.0 / crd::math::sqrt(static_cast<double>(len2)));
        nx *= inv_len;
        ny *= inv_len;
        nz *= inv_len;
        const T mx = (site.x + nbr.x) * static_cast<T>(0.5);
        const T my = (site.y + nbr.y) * static_cast<T>(0.5);
        const T mz = (site.z + nbr.z) * static_cast<T>(0.5);
        const T d  = -(nx * mx + ny * my + nz * mz);
        hull.faces.push_back(crd::geometry::primitives::Plane<T>{crd::math::Vec3<T>{nx, ny, nz}, d});

        for (crd::u32 vi : face.vertex_indices)
        {
            hull.face_vertex_indices.push_back(global_to_local[vi]);
        }
        hull.face_vertex_offsets.push_back(static_cast<crd::u32>(hull.face_vertex_indices.size()));
    }

    return hull;
}

// Explicit instantiations.
template VoronoiResult3<crd::f32>
voronoi_3d<crd::f32>(crd::containers::ConstSpan<crd::math::Vec3<crd::f32>>,
                      crd::memory::IAllocator*);
template VoronoiResult3<crd::f64>
voronoi_3d<crd::f64>(crd::containers::ConstSpan<crd::math::Vec3<crd::f64>>,
                      crd::memory::IAllocator*);

template VoronoiCellHull3<crd::f32>
convex_hull_for_cell<crd::f32>(const VoronoiResult3<crd::f32>&,
                                  crd::containers::ConstSpan<crd::math::Vec3<crd::f32>>,
                                  crd::u32, crd::memory::IAllocator*);
template VoronoiCellHull3<crd::f64>
convex_hull_for_cell<crd::f64>(const VoronoiResult3<crd::f64>&,
                                  crd::containers::ConstSpan<crd::math::Vec3<crd::f64>>,
                                  crd::u32, crd::memory::IAllocator*);

} // namespace crd::geometry::delaunay
