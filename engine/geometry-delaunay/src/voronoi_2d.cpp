// ---------------------------------------------------------------------------
// crd-geometry-delaunay — v8d-2d 2D Voronoi diagram extraction.
//
// Algorithm in voronoi_2d.hpp comment header. This TU owns:
//   - Half-edge sort-and-scan to reconstruct Delaunay triangle adjacency
//     (avoiding HashMap per ADR-0076 §4 pin #11 lex-tuple determinism).
//   - Per-site walk via Delaunay neighbour info (no angular sort) to
//     produce CCW-ordered Voronoi cell vertex sequences.
//   - Unbounded-cell handling: walk both CCW and CW from start tri,
//     splice the two halves, compute ray directions as perpendicular to
//     the bounding hull edges (D96 — sign-checked via dot vs opposite
//     vertex).
//
// Pinned design decisions D95-D97:
//
//   D95. **Circumcentre lifted to `f64`** regardless of `T`. The formula
//        has products of order `coord^2` which overflow f32 above coord
//        magnitude ~10^4. Shipped as `crd::geometry::primitives::circumcenter_2d`
//        primitive (reusable by Ruppert v8g + future tet meshers).
//
//   D96. **Unbounded-cell ray direction = perpendicular to Delaunay hull
//        edge**, sign-checked via dot against `(midpoint - opposite_vertex)`
//        to point AWAY from the cell interior. NOT derived from
//        `circumcentre - midpoint` (which can be near-zero or wrong-side
//        for obtuse hull triangles where the circumcentre is outside).
//
//   D97. **Output structure**: `voronoi_vertices` indexed by Delaunay
//        triangle id (so a cell vertex_index IS a triangle id, dual-graph
//        natural). Cells in input-site order (so `cells[i].site_index == i`
//        always). CCW orientation pinned for bounded cells (signed_area > 0).
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/delaunay/delaunay_2d.hpp>
#include <crd/geometry/delaunay/voronoi_2d.hpp>
#include <crd/geometry/primitives/circumcenter.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include <limits>

namespace crd::geometry::delaunay
{

namespace
{

constexpr crd::u32 kNullNbr = std::numeric_limits<crd::u32>::max();

// Translate DelaunayStatus -> VoronoiStatus2.
inline VoronoiStatus2 propagate_status(DelaunayStatus s) noexcept
{
    switch (s)
    {
        case DelaunayStatus::Ok:                return VoronoiStatus2::Ok;
        case DelaunayStatus::TooFewPoints:      return VoronoiStatus2::TooFewPoints;
        case DelaunayStatus::NonFiniteInput:    return VoronoiStatus2::NonFiniteInput;
        case DelaunayStatus::DuplicatePoint:    return VoronoiStatus2::DuplicatePoint;
        case DelaunayStatus::InternalInvariant: return VoronoiStatus2::InternalInvariant;
    }
    return VoronoiStatus2::InternalInvariant;
}

// Rebuild Delaunay triangle adjacency from `triangle_indices` via half-edge
// sort-and-scan. For T triangles, emits 3T half-edges and sorts them lex by
// (min_endpoint, max_endpoint, tri_id). Pairs of consecutive entries with
// matching endpoints are interior edges; singletons are hull edges.
//
// Output `tri_neighbours[3*t + k]` = adjacent tri across edge k of tri t,
// or kNullNbr if hull edge. Convention matches v8a: edge k of tri t =
// (v[k], v[(k+1)%3]).
void build_tri_adjacency(const crd::containers::Array<crd::u32>& tri_indices,
                          crd::u32                                  tri_count,
                          crd::memory::IAllocator*                  alloc,
                          crd::containers::Array<crd::u32>&         out_nbrs)
{
    out_nbrs.resize(static_cast<crd::usize>(tri_count) * 3U, kNullNbr);

    struct HalfEdge
    {
        crd::u32 a;   // min endpoint
        crd::u32 b;   // max endpoint
        crd::u32 tri; // owning triangle id
        crd::u32 k;   // local edge index (0/1/2) in that triangle
    };
    crd::containers::Array<HalfEdge> hes(alloc);
    hes.reserve(static_cast<crd::usize>(tri_count) * 3U);
    for (crd::u32 t = 0; t < tri_count; ++t)
    {
        for (crd::u32 k = 0; k < 3U; ++k)
        {
            const crd::u32 u = tri_indices[3U * t + k];
            const crd::u32 v = tri_indices[3U * t + (k + 1U) % 3U];
            HalfEdge he{};
            he.a   = u < v ? u : v;
            he.b   = u < v ? v : u;
            he.tri = t;
            he.k   = k;
            hes.push_back(he);
        }
    }
    crd::containers::sort(hes.data(), hes.data() + hes.size(),
                          [](const HalfEdge& l, const HalfEdge& r) noexcept {
                              if (l.a != r.a) { return l.a < r.a; }
                              if (l.b != r.b) { return l.b < r.b; }
                              if (l.tri != r.tri) { return l.tri < r.tri; }
                              return l.k < r.k;
                          });
    for (crd::u32 i = 0; i + 1U < hes.size(); ++i)
    {
        const auto& h0 = hes[i];
        const auto& h1 = hes[i + 1U];
        if (h0.a == h1.a && h0.b == h1.b)
        {
            // Shared edge: cross-link the two tris.
            out_nbrs[3U * h0.tri + h0.k] = h1.tri;
            out_nbrs[3U * h1.tri + h1.k] = h0.tri;
            ++i; // consume the pair
        }
        // Singletons left at kNullNbr (hull edges).
    }
}

// Find local index `k` (0/1/2) such that tri_indices[3*t + k] == site.
// Returns 3 if site is not a vertex of tri t.
inline crd::u32 local_index_of(const crd::containers::Array<crd::u32>& tri_indices,
                                crd::u32 t, crd::u32 site) noexcept
{
    for (crd::u32 k = 0; k < 3U; ++k)
    {
        if (tri_indices[3U * t + k] == site) { return k; }
    }
    return 3U;
}

} // anonymous namespace

template <crd::math::MathScalar T>
VoronoiResult2<T>
voronoi_2d(crd::containers::ConstSpan<crd::math::Vec2<T>> sites,
           crd::memory::IAllocator*                        alloc)
{
    VoronoiResult2<T> result{alloc};
    const crd::u32 n = static_cast<crd::u32>(sites.size());

    // Phase 1: run Delaunay.
    auto del = delaunay_2d<T>(sites, alloc);
    if (!del.ok())
    {
        result.status = propagate_status(del.status);
        return result;
    }
    const crd::u32 tri_count = del.triangle_count;

    // Phase 2: rebuild Delaunay tri adjacency.
    crd::containers::Array<crd::u32> tri_nbrs(alloc);
    build_tri_adjacency(del.triangle_indices, tri_count, alloc, tri_nbrs);

    // Phase 3: compute circumcentre per Delaunay triangle. Indexed by tri id.
    result.voronoi_vertices.resize(tri_count);
    for (crd::u32 t = 0; t < tri_count; ++t)
    {
        const crd::u32 ia = del.triangle_indices[3U * t + 0U];
        const crd::u32 ib = del.triangle_indices[3U * t + 1U];
        const crd::u32 ic = del.triangle_indices[3U * t + 2U];
        result.voronoi_vertices[t] = crd::geometry::primitives::circumcenter_2d(
            sites[ia], sites[ib], sites[ic]);
    }

    // Phase 4: build site -> any incident triangle (lowest-id for determinism).
    crd::containers::Array<crd::u32> site_to_tri(alloc);
    site_to_tri.resize(n, kNullNbr);
    for (crd::u32 t = 0; t < tri_count; ++t)
    {
        for (crd::u32 k = 0; k < 3U; ++k)
        {
            const crd::u32 v = del.triangle_indices[3U * t + k];
            if (v < n && site_to_tri[v] == kNullNbr)
            {
                site_to_tri[v] = t;
            }
        }
    }

    // Phase 5: build cells. One per input site, in site-id order.
    result.cells.reserve(n);
    for (crd::u32 s = 0; s < n; ++s)
    {
        VoronoiCell<T> cell{alloc};
        cell.site_index = s;

        const crd::u32 start_tri = site_to_tri[s];
        if (start_tri == kNullNbr)
        {
            // Site is not a vertex of any Delaunay triangle — should not
            // happen for valid 2D Delaunay output unless site is collinear
            // with others and got "swallowed" by lex-tiebreak. Defensive:
            // emit empty cell.
            cell.is_bounded = false;
            result.cells.push_back(std::move(cell));
            continue;
        }
        const crd::u32 start_si = local_index_of(del.triangle_indices, start_tri, s);
        if (start_si >= 3U)
        {
            result.status = VoronoiStatus2::InternalInvariant;
            return result;
        }

        // CCW walk around site s: from tri t with site at local idx csi,
        // cross the INCOMING edge to s (edge (csi+2)%3 = (v[(csi+2)%3], v[csi]))
        // = nbr[(csi+2)%3]. This rotates CCW around s in a CCW-oriented
        // triangulation. (Crossing the OUTGOING edge nbr[csi] rotates CW.)
        crd::containers::Array<crd::u32> ccw_tris(alloc);
        ccw_tris.push_back(start_tri);
        bool ccw_hit_hull = false;
        crd::u32 cur = start_tri;
        crd::u32 csi = start_si;
        for (crd::u32 step = 0; step < tri_count; ++step)
        {
            const crd::u32 nxt = tri_nbrs[3U * cur + (csi + 2U) % 3U];
            if (nxt == kNullNbr) { ccw_hit_hull = true; break; }
            if (nxt == start_tri)  { break; }
            const crd::u32 nsi = local_index_of(del.triangle_indices, nxt, s);
            if (nsi >= 3U)
            {
                result.status = VoronoiStatus2::InternalInvariant;
                return result;
            }
            ccw_tris.push_back(nxt);
            cur = nxt;
            csi = nsi;
        }

        if (!ccw_hit_hull)
        {
            // Closed loop -> bounded cell.
            cell.is_bounded = true;
            cell.vertex_indices = std::move(ccw_tris);
        }
        else
        {
            // Unbounded cell: walk CW from start_tri to capture the other half.
            // CW step: nbr[csi] = nbr across outgoing edge from s.
            crd::containers::Array<crd::u32> cw_tris(alloc);
            cur = start_tri;
            csi = start_si;
            for (crd::u32 step = 0; step < tri_count; ++step)
            {
                const crd::u32 nxt = tri_nbrs[3U * cur + csi];
                if (nxt == kNullNbr) { break; }
                const crd::u32 nsi = local_index_of(del.triangle_indices, nxt, s);
                if (nsi >= 3U)
                {
                    result.status = VoronoiStatus2::InternalInvariant;
                    return result;
                }
                cw_tris.push_back(nxt);
                cur = nxt;
                csi = nsi;
            }

            // Final CCW order = reverse(cw_tris) + ccw_tris.
            cell.is_bounded = false;
            cell.vertex_indices.reserve(cw_tris.size() + ccw_tris.size());
            for (crd::u32 i = static_cast<crd::u32>(cw_tris.size()); i > 0; --i)
            {
                cell.vertex_indices.push_back(cw_tris[i - 1U]);
            }
            for (crd::u32 i = 0; i < ccw_tris.size(); ++i)
            {
                cell.vertex_indices.push_back(ccw_tris[i]);
            }

            // Compute ray directions (D96).
            // first_ray_dir: incoming ray to vertex_indices.front() (which is
            // either cw_tris.back() reversed = first walked tri in CW, or
            // start_tri if cw_tris is empty). The bounding hull edge of that
            // tri is the CW edge (v[(si+2)%3], s) — the one that terminated
            // the CW walk.
            //
            // last_ray_dir: outgoing ray from vertex_indices.back() = last
            // walked tri in CCW. Bounding hull edge = (s, v[(si+1)%3]) — the
            // one that terminated the CCW walk.
            //
            // Ray direction = perpendicular to hull edge, sign-checked to
            // point AWAY from the third (opposite) tri vertex.

            auto compute_ray_dir = [&](crd::u32 tri_id, crd::u32 hull_edge_k)
                                    -> crd::math::Vec2<T> {
                const crd::u32 ea = del.triangle_indices[3U * tri_id + hull_edge_k];
                const crd::u32 eb = del.triangle_indices[3U * tri_id + (hull_edge_k + 1U) % 3U];
                const crd::u32 oc = del.triangle_indices[3U * tri_id + (hull_edge_k + 2U) % 3U];
                const auto& pa = sites[ea];
                const auto& pb = sites[eb];
                const auto& pc = sites[oc];
                const T ex = pb.x - pa.x;
                const T ey = pb.y - pa.y;
                // Two candidate perpendiculars.
                crd::math::Vec2<T> perp{-ey, ex};
                // Midpoint of edge.
                const T mx = (pa.x + pb.x) * static_cast<T>(0.5);
                const T my = (pa.y + pb.y) * static_cast<T>(0.5);
                // Direction from edge midpoint to opposite vertex pc.
                const T to_opp_x = pc.x - mx;
                const T to_opp_y = pc.y - my;
                // perp points AWAY from opp iff dot(perp, to_opp) < 0.
                // If dot is positive, flip perp.
                const T d = perp.x * to_opp_x + perp.y * to_opp_y;
                if (d > static_cast<T>(0))
                {
                    perp.x = -perp.x;
                    perp.y = -perp.y;
                }
                return perp;
            };

            // first_ray_dir corresponds to vertex_indices.front() = first
            // entry after splice = (cw_tris empty ? start_tri : cw_tris.back()).
            // CW walk terminates when nbr[csi] = null → hull edge in that
            // tri is csi (outgoing edge from s).
            crd::u32 cw_term_tri;
            crd::u32 cw_term_si;
            if (cw_tris.empty())
            {
                cw_term_tri = start_tri;
                cw_term_si  = start_si;
            } else {
                cw_term_tri = cw_tris.back();
                cw_term_si  = local_index_of(del.triangle_indices, cw_term_tri, s);
            }
            const crd::u32 cw_hull_edge = cw_term_si; // outgoing edge from s
            cell.first_ray_dir = compute_ray_dir(cw_term_tri, cw_hull_edge);

            // last_ray_dir corresponds to vertex_indices.back() = ccw_tris.back().
            // CCW walk terminates when nbr[(csi+2)%3] = null → hull edge in
            // that tri is (csi+2)%3 (incoming edge to s).
            const crd::u32 ccw_term_tri = ccw_tris.back();
            const crd::u32 ccw_term_si  = local_index_of(del.triangle_indices, ccw_term_tri, s);
            const crd::u32 ccw_hull_edge = (ccw_term_si + 2U) % 3U;
            cell.last_ray_dir = compute_ray_dir(ccw_term_tri, ccw_hull_edge);
        }

        result.cells.push_back(std::move(cell));
    }

    result.status = VoronoiStatus2::Ok;
    return result;
}

// Explicit instantiations.
template VoronoiResult2<crd::f32>
voronoi_2d<crd::f32>(crd::containers::ConstSpan<crd::math::Vec2<crd::f32>>,
                      crd::memory::IAllocator*);
template VoronoiResult2<crd::f64>
voronoi_2d<crd::f64>(crd::containers::ConstSpan<crd::math::Vec2<crd::f64>>,
                      crd::memory::IAllocator*);

} // namespace crd::geometry::delaunay
