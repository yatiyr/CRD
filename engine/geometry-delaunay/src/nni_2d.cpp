// ---------------------------------------------------------------------------
// crd-geometry-delaunay — v8f Sibson NNI 2D implementation.
//
// See nni_2d.hpp for algorithm contract. Bowyer-Watson cavity-based form
// (Belikov-Semenov 1997). Build is precomputed once; query is O(cavity_size)
// per call (typically a handful of triangles).
//
// Pinned design decisions D109-D113 (carryover for ADR-0076 §23 at v8-close):
//
//   D109. **Triangle adjacency rebuild via sort-and-scan** over 3T
//         half-edges, matching v8d-2d's pattern (no HashMap, deterministic).
//
//   D110. **Cavity BFS termination**: tri T joins cavity iff
//         `incircle(T_verts, q) > 0` (Stage D adaptive, v8a paydown). Lex-
//         tiebreak deterministic on cocircular boundary.
//
//   D111. **Cavity boundary CCW walk** via `next_v[u] = v` map (each
//         boundary edge u -> v with cavity on the LEFT). Boundary forms a
//         single cycle for q strictly inside the convex hull.
//
//   D112. **Stolen polygon vertex order**: for natural neighbour n_i with
//         CCW-prev n_{i-1} and CCW-next n_{i+1}, polygon = [
//         circumcenter(q, n_{i-1}, n_i),
//         circumcenters of cavity tris incident to n_i (walked from
//         (n_{i-1}, n_i) side to (n_i, n_{i+1}) side),
//         circumcenter(q, n_i, n_{i+1}) ]. Area via standard signed-area
//         formula (absolute value — orientation handled by walk direction).
//
//   D113. **OnSite detection**: if jump-walk locates a triangle T and q's
//         coords match one of T's vertices exactly, return that vertex's
//         value with status OnSite. Distinguishes "interpolating AT a site"
//         from "interpolating near a site" — exact answer, no Sibson math.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/delaunay/delaunay_2d.hpp>
#include <crd/geometry/delaunay/nni_2d.hpp>
#include <crd/geometry/primitives/circumcenter.hpp>
#include <crd/geometry/primitives/predicates.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>

namespace crd::geometry::delaunay
{

namespace
{

constexpr crd::u32 kNullNbr = std::numeric_limits<crd::u32>::max();

inline NniStatus propagate_delaunay_status(DelaunayStatus s) noexcept
{
    switch (s)
    {
        case DelaunayStatus::Ok:                return NniStatus::Ok;
        case DelaunayStatus::TooFewPoints:      return NniStatus::TooFewPoints;
        case DelaunayStatus::NonFiniteInput:    return NniStatus::NonFiniteInput;
        case DelaunayStatus::DuplicatePoint:    return NniStatus::DuplicatePoint;
        case DelaunayStatus::InternalInvariant: return NniStatus::InternalInvariant;
    }
    return NniStatus::InternalInvariant;
}

// Rebuild Delaunay triangle adjacency via sort-and-scan on 3T half-edges.
// Mirror of v8d-2d's `build_tri_adjacency`. Output `tri_nbrs[3*t + k]` =
// adjacent tri across edge k of tri t, or kNullNbr if hull edge.
void build_tri_adjacency_for_nni(const crd::containers::Array<crd::u32>& tri_indices,
                                   crd::u32                                  tri_count,
                                   crd::memory::IAllocator*                  alloc,
                                   crd::containers::Array<crd::u32>&         out_nbrs)
{
    out_nbrs.resize(static_cast<crd::usize>(tri_count) * 3U, kNullNbr);

    struct HalfEdge
    {
        crd::u32 a;
        crd::u32 b;
        crd::u32 tri;
        crd::u32 k;
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
            out_nbrs[3U * h0.tri + h0.k] = h1.tri;
            out_nbrs[3U * h1.tri + h1.k] = h0.tri;
            ++i;
        }
    }
}

template <crd::math::MathScalar T>
inline bool is_finite_vec2(const crd::math::Vec2<T>& p) noexcept
{
    return std::isfinite(static_cast<double>(p.x)) && std::isfinite(static_cast<double>(p.y));
}

// Jump-walk locate: find a triangle containing q. Returns kNullNbr if q is
// outside the convex hull (jump-walk hits a null neighbour without finding
// containment). Per v8d-2d locate pattern.
template <crd::math::MathScalar T>
crd::u32 locate_tri(const crd::containers::Array<crd::u32>& tri_indices,
                     const crd::containers::Array<crd::u32>& tri_nbrs,
                     const crd::math::Vec2<T>*               sites,
                     crd::u32                                  start_tri,
                     const crd::math::Vec2<T>&                 q,
                     crd::u32                                  max_steps) noexcept
{
    crd::u32 cur = start_tri;
    for (crd::u32 step = 0; step < max_steps; ++step)
    {
        if (cur == kNullNbr) { return kNullNbr; }
        const crd::u32 ia = tri_indices[3U * cur + 0U];
        const crd::u32 ib = tri_indices[3U * cur + 1U];
        const crd::u32 ic = tri_indices[3U * cur + 2U];
        const T s0 = crd::geometry::primitives::orient2d(sites[ia], sites[ib], q);
        const T s1 = crd::geometry::primitives::orient2d(sites[ib], sites[ic], q);
        const T s2 = crd::geometry::primitives::orient2d(sites[ic], sites[ia], q);
        if (s0 >= static_cast<T>(0) && s1 >= static_cast<T>(0) && s2 >= static_cast<T>(0))
        {
            return cur;
        }
        crd::u32 cross_edge = 3U;
        if (s0 < static_cast<T>(0))      { cross_edge = 0U; }
        else if (s1 < static_cast<T>(0)) { cross_edge = 1U; }
        else if (s2 < static_cast<T>(0)) { cross_edge = 2U; }
        if (cross_edge >= 3U) { return cur; }
        cur = tri_nbrs[3U * cur + cross_edge];
    }
    return kNullNbr;
}

// Signed area of a polygon (positive for CCW).
template <crd::math::MathScalar T>
T polygon_signed_area(const crd::math::Vec2<T>* verts, crd::u32 n) noexcept
{
    if (n < 3U) { return static_cast<T>(0); }
    T sum = static_cast<T>(0);
    for (crd::u32 i = 0; i < n; ++i)
    {
        const auto& p = verts[i];
        const auto& q = verts[(i + 1U) % n];
        sum += p.x * q.y - q.x * p.y;
    }
    return sum * static_cast<T>(0.5);
}

} // anonymous namespace

template <crd::math::MathScalar T>
NniInterpolator2<T>::NniInterpolator2(crd::containers::ConstSpan<crd::math::Vec2<T>> sites,
                                        crd::containers::ConstSpan<T>                  values,
                                        crd::memory::IAllocator*                        alloc)
  : m_alloc(alloc), m_sites(alloc), m_values(alloc),
    m_tri_indices(alloc), m_tri_neighbours(alloc), m_circumcentres(alloc)
{
    if (sites.size() != values.size())
    {
        m_build_status = NniStatus::InternalInvariant;
        return;
    }
    m_sites.reserve(sites.size());
    m_values.reserve(values.size());
    for (crd::usize i = 0; i < sites.size(); ++i) { m_sites.push_back(sites[i]); }
    for (crd::usize i = 0; i < values.size(); ++i) { m_values.push_back(values[i]); }

    auto del = delaunay_2d<T>(sites, alloc);
    if (!del.ok())
    {
        m_build_status = propagate_delaunay_status(del.status);
        return;
    }
    m_tri_count   = del.triangle_count;
    m_tri_indices = std::move(del.triangle_indices);
    build_tri_adjacency_for_nni(m_tri_indices, m_tri_count, alloc, m_tri_neighbours);

    m_circumcentres.resize(m_tri_count);
    for (crd::u32 t = 0; t < m_tri_count; ++t)
    {
        const crd::u32 ia = m_tri_indices[3U * t + 0U];
        const crd::u32 ib = m_tri_indices[3U * t + 1U];
        const crd::u32 ic = m_tri_indices[3U * t + 2U];
        m_circumcentres[t] = crd::geometry::primitives::circumcenter_2d(
            m_sites[ia], m_sites[ib], m_sites[ic]);
    }
    m_build_status = NniStatus::Ok;
}

template <crd::math::MathScalar T>
NniResult<T> NniInterpolator2<T>::interpolate(const crd::math::Vec2<T>& query) const
{
    NniResult<T> result{};
    if (m_build_status != NniStatus::Ok)
    {
        result.status = m_build_status;
        return result;
    }
    if (!is_finite_vec2(query))
    {
        result.status = NniStatus::QueryNonFinite;
        return result;
    }

    // Phase 1: locate containing triangle.
    const crd::u32 max_walk = m_tri_count * 4U + 16U;
    const crd::u32 containing = locate_tri<T>(m_tri_indices, m_tri_neighbours,
                                                m_sites.data(), 0U, query, max_walk);
    if (containing == kNullNbr)
    {
        result.status = NniStatus::OutsideHull;
        return result;
    }

    // OnSite check: if query coordinates match one of the containing tri's
    // vertices exactly, return that vertex's value.
    for (crd::u32 k = 0; k < 3U; ++k)
    {
        const crd::u32 vi = m_tri_indices[3U * containing + k];
        const auto&    s  = m_sites[vi];
        if (s.x == query.x && s.y == query.y)
        {
            result.value  = m_values[vi];
            result.status = NniStatus::OnSite;
            return result;
        }
    }

    // Phase 2: BFS cavity via Stage D incircle.
    crd::containers::Array<crd::u8>  in_cavity(m_alloc);
    in_cavity.resize(m_tri_count, crd::u8{0});
    crd::containers::Array<crd::u32> cavity_tris(m_alloc);
    cavity_tris.push_back(containing);
    in_cavity[containing] = 1U;
    crd::u32 qi = 0;
    while (qi < cavity_tris.size())
    {
        const crd::u32 cur = cavity_tris[qi++];
        for (crd::u32 k = 0; k < 3U; ++k)
        {
            const crd::u32 nbr = m_tri_neighbours[3U * cur + k];
            if (nbr == kNullNbr) { continue; }
            if (in_cavity[nbr] != 0U) { continue; }
            const crd::u32 a = m_tri_indices[3U * nbr + 0U];
            const crd::u32 b = m_tri_indices[3U * nbr + 1U];
            const crd::u32 c = m_tri_indices[3U * nbr + 2U];
            const T s = crd::geometry::primitives::incircle(
                m_sites[a], m_sites[b], m_sites[c], query);
            if (s > static_cast<T>(0))
            {
                in_cavity[nbr] = 1U;
                cavity_tris.push_back(nbr);
            }
        }
    }

    // Phase 3: collect cavity boundary edges (u -> v with cavity on LEFT).
    // For each cavity tri's edge whose opposite neighbour is NOT cavity
    // (including hull = null), emit (v[k], v[(k+1)%3]).
    struct BoundaryEdge { crd::u32 u; crd::u32 v; };
    crd::containers::Array<BoundaryEdge> boundary(m_alloc);
    for (crd::u32 ci = 0; ci < cavity_tris.size(); ++ci)
    {
        const crd::u32 ti = cavity_tris[ci];
        for (crd::u32 k = 0; k < 3U; ++k)
        {
            const crd::u32 nbr = m_tri_neighbours[3U * ti + k];
            const bool nbr_cavity = (nbr != kNullNbr) && (in_cavity[nbr] != 0U);
            if (nbr_cavity) { continue; }
            // Cavity boundary edge (whether the outer side is another tri OR
            // a hull edge with null neighbour). For q strictly inside the
            // convex hull this still forms a closed boundary cycle since
            // hull edges within the cavity contribute the convex-hull
            // segments of the cavity boundary.
            BoundaryEdge e{};
            e.u = m_tri_indices[3U * ti + k];
            e.v = m_tri_indices[3U * ti + (k + 1U) % 3U];
            boundary.push_back(e);
        }
    }
    if (boundary.size() < 3U)
    {
        result.status = NniStatus::InternalInvariant;
        return result;
    }

    // Build next_v map and walk the cycle.
    // Use a flat array indexed by site id since vertex ids are bounded by
    // input site count.
    crd::containers::Array<crd::u32> next_v(m_alloc);
    next_v.resize(m_sites.size(), kNullNbr);
    for (const auto& e : boundary) { next_v[e.u] = e.v; }
    crd::containers::Array<crd::u32> cycle(m_alloc);
    const crd::u32 start_u = boundary[0].u;
    cycle.push_back(start_u);
    crd::u32 cur_v = next_v[start_u];
    while (cur_v != start_u && cycle.size() < boundary.size())
    {
        cycle.push_back(cur_v);
        cur_v = next_v[cur_v];
    }
    if (cycle.size() != boundary.size())
    {
        result.status = NniStatus::InternalInvariant;
        return result;
    }
    const crd::u32 num_neighbours = static_cast<crd::u32>(cycle.size());

    // Phase 4: for each natural neighbour n_i, compute stolen area.
    //
    // Walk cavity tris incident to n_i in order from cavity edge
    // (n_{i-1}, n_i) to cavity edge (n_i, n_{i+1}). To do this, find the
    // starting cavity tri (the one containing edge (n_{i-1}, n_i)) and
    // walk via neighbour links through internal edges until reaching the
    // tri containing edge (n_i, n_{i+1}).
    //
    // Helper: find cavity tri containing directed boundary edge (u, v).
    auto find_cavity_tri_with_edge = [&](crd::u32 u, crd::u32 v) -> crd::u32 {
        for (crd::u32 ci = 0; ci < cavity_tris.size(); ++ci)
        {
            const crd::u32 ti = cavity_tris[ci];
            for (crd::u32 k = 0; k < 3U; ++k)
            {
                if (m_tri_indices[3U * ti + k] == u &&
                    m_tri_indices[3U * ti + (k + 1U) % 3U] == v)
                {
                    return ti;
                }
            }
        }
        return kNullNbr;
    };

    // Helper: in tri t, find local index of vertex n.
    auto local_of = [&](crd::u32 t, crd::u32 vi) -> crd::u32 {
        for (crd::u32 k = 0; k < 3U; ++k)
        {
            if (m_tri_indices[3U * t + k] == vi) { return k; }
        }
        return 3U;
    };

    crd::containers::Array<crd::math::Vec2<T>> stolen_poly(m_alloc);
    T total_stolen = static_cast<T>(0);
    crd::containers::Array<T> stolen_areas(m_alloc);
    stolen_areas.resize(num_neighbours, static_cast<T>(0));

    for (crd::u32 i = 0; i < num_neighbours; ++i)
    {
        const crd::u32 n_prev = cycle[(i + num_neighbours - 1U) % num_neighbours];
        const crd::u32 n_i    = cycle[i];
        const crd::u32 n_next = cycle[(i + 1U) % num_neighbours];

        // New Voronoi vertices: circumcentres of (q, n_prev, n_i) and
        // (q, n_i, n_next).
        const crd::math::Vec2<T> new_left = crd::geometry::primitives::circumcenter_2d(
            query, m_sites[n_prev], m_sites[n_i]);
        const crd::math::Vec2<T> new_right = crd::geometry::primitives::circumcenter_2d(
            query, m_sites[n_i], m_sites[n_next]);

        // Walk cavity tris incident to n_i from start tri (containing edge
        // (n_prev, n_i)) to end tri (containing edge (n_i, n_next)).
        const crd::u32 start_t = find_cavity_tri_with_edge(n_prev, n_i);
        const crd::u32 end_t   = find_cavity_tri_with_edge(n_i, n_next);
        if (start_t == kNullNbr || end_t == kNullNbr)
        {
            result.status = NniStatus::InternalInvariant;
            return result;
        }

        stolen_poly.clear();
        stolen_poly.push_back(new_left);

        crd::u32 cur_t = start_t;
        crd::u32 prev_t = kNullNbr;
        const crd::u32 cavity_safety = static_cast<crd::u32>(cavity_tris.size()) + 2U;
        for (crd::u32 step = 0; step < cavity_safety; ++step)
        {
            stolen_poly.push_back(m_circumcentres[cur_t]);
            if (cur_t == end_t) { break; }
            // Find the next cavity tri across the edge of cur_t that
            // contains n_i but is NOT the edge we came in on (i.e., not the
            // boundary edge (n_prev, n_i)).
            // Edges of cur_t incident to n_i:
            //   edge k where v[k] = n_i (outgoing) and edge (k+2)%3 where
            //   v[(k+2)%3+1] = n_i (incoming).
            const crd::u32 ni_local = local_of(cur_t, n_i);
            if (ni_local >= 3U) { result.status = NniStatus::InternalInvariant; return result; }
            const crd::u32 edge_out = ni_local;                 // (v[ni_local], v[ni_local+1])
            const crd::u32 edge_in  = (ni_local + 2U) % 3U;     // (v[ni_local-1], v[ni_local])
            // The "internal" edges at n_i are the two; one shares with prev_t
            // (we came in on), one shares with the next cavity tri. Skip
            // boundary edges (cavity-side null neighbours).
            crd::u32 next_t = kNullNbr;
            for (crd::u32 ke : {edge_out, edge_in})
            {
                const crd::u32 nbr = m_tri_neighbours[3U * cur_t + ke];
                if (nbr == kNullNbr) { continue; }
                if (in_cavity[nbr] == 0U) { continue; }
                if (nbr == prev_t) { continue; }
                next_t = nbr;
                break;
            }
            if (next_t == kNullNbr)
            {
                // Should have found end_t before exhausting.
                result.status = NniStatus::InternalInvariant;
                return result;
            }
            prev_t = cur_t;
            cur_t  = next_t;
        }
        if (cur_t != end_t)
        {
            result.status = NniStatus::InternalInvariant;
            return result;
        }

        stolen_poly.push_back(new_right);

        const T area = std::fabs(polygon_signed_area<T>(stolen_poly.data(),
                                                          static_cast<crd::u32>(stolen_poly.size())));
        stolen_areas[i] = area;
        total_stolen += area;
    }

    if (total_stolen <= static_cast<T>(0))
    {
        result.status = NniStatus::InternalInvariant;
        return result;
    }

    // Phase 5: weighted sum.
    const T inv_total = static_cast<T>(1) / total_stolen;
    T value = static_cast<T>(0);
    for (crd::u32 i = 0; i < num_neighbours; ++i)
    {
        const T w = stolen_areas[i] * inv_total;
        value += w * m_values[cycle[i]];
    }
    result.value  = value;
    result.status = NniStatus::Ok;
    return result;
}

template <crd::math::MathScalar T>
NniResult<T>
sibson_interpolate_2d(crd::containers::ConstSpan<crd::math::Vec2<T>> sites,
                       crd::containers::ConstSpan<T>                  values,
                       const crd::math::Vec2<T>&                      query,
                       crd::memory::IAllocator*                        alloc)
{
    NniInterpolator2<T> interp{sites, values, alloc};
    if (interp.build_status() != NniStatus::Ok)
    {
        NniResult<T> r{};
        r.status = interp.build_status();
        return r;
    }
    return interp.interpolate(query);
}

// Explicit instantiations.
template class NniInterpolator2<crd::f32>;
template class NniInterpolator2<crd::f64>;

template NniResult<crd::f32>
sibson_interpolate_2d<crd::f32>(crd::containers::ConstSpan<crd::math::Vec2<crd::f32>>,
                                  crd::containers::ConstSpan<crd::f32>,
                                  const crd::math::Vec2<crd::f32>&, crd::memory::IAllocator*);
template NniResult<crd::f64>
sibson_interpolate_2d<crd::f64>(crd::containers::ConstSpan<crd::math::Vec2<crd::f64>>,
                                  crd::containers::ConstSpan<crd::f64>,
                                  const crd::math::Vec2<crd::f64>&, crd::memory::IAllocator*);

} // namespace crd::geometry::delaunay
