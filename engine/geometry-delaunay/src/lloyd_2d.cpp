// ---------------------------------------------------------------------------
// crd-geometry-delaunay — v8e 2D Lloyd's CVT relaxation implementation.
//
// See lloyd_2d.hpp for the algorithm contract. This TU owns:
//   - Validation pass (TooFewPoints / NonFiniteInput / DuplicatePoint /
//     BboxInvalid).
//   - Auto-bbox derivation when bbox_set = false (input bbox + 10% pad).
//   - Lloyd iteration loop: voronoi -> per-cell centroid -> displacement
//     check.
//   - Polygon centroid via the standard signed-area formula.
//   - Sutherland-Hodgman polygon-vs-bbox clipping for unbounded cells
//     under HullPolicy::ClipToBbox (also clips bounded cells defensively
//     to handle cells whose vertices fall just outside the bbox).
//   - Closure of unbounded cell boundaries: extend rays to bbox boundary,
//     walk bbox corners CCW between exit and entry, then clip.
//
// Pinned design decisions (carryover for ADR-0076 §23 at v8-close):
//
//   D102. **HullPolicy default = Fix**. Simple, robust, always works.
//         ClipToBbox is the "true Lloyd in a closed domain" option.
//
//   D103. **Convergence = max per-iteration displacement < tolerance**
//         (absolute, in input coord units; NOT relative). Caller scales
//         tolerance to expected coord magnitude.
//
//   D104. **Polygon centroid via signed-area formula**. Reuses
//         `crd::geometry::polygon::centroid(PolygonView2<T>)`.
//
//   D105. **Sutherland-Hodgman against axis-aligned bbox** for ClipToBbox.
//         Walks each of 4 bbox edges in CCW order, clipping the polygon
//         in turn. Result is a closed convex polygon inside the bbox.
//
//   D106. **Unbounded cell closure**: extend `first_ray_dir` and
//         `last_ray_dir` from their respective vertex positions to
//         intersection points with the bbox boundary; insert any bbox
//         corners between the two exits via CCW corner walk; pass the
//         resulting closed polygon through Sutherland-Hodgman.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/delaunay/lloyd_2d.hpp>
#include <crd/geometry/delaunay/voronoi_2d.hpp>
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

// Polygon centroid via signed-area formula.
template <crd::math::MathScalar T>
crd::math::Vec2<T> polygon_centroid_2d(const crd::math::Vec2<T>* verts, crd::u32 n) noexcept
{
    if (n < 3U)
    {
        // Degenerate — fall back to arithmetic mean.
        T cx = static_cast<T>(0);
        T cy = static_cast<T>(0);
        for (crd::u32 i = 0; i < n; ++i)
        {
            cx += verts[i].x;
            cy += verts[i].y;
        }
        if (n == 0U) { return crd::math::Vec2<T>{}; }
        const T inv_n = static_cast<T>(1) / static_cast<T>(n);
        return crd::math::Vec2<T>{cx * inv_n, cy * inv_n};
    }
    T cx = static_cast<T>(0);
    T cy = static_cast<T>(0);
    T a  = static_cast<T>(0);
    for (crd::u32 i = 0; i < n; ++i)
    {
        const auto& p = verts[i];
        const auto& q = verts[(i + 1U) % n];
        const T cross = p.x * q.y - q.x * p.y;
        a  += cross;
        cx += (p.x + q.x) * cross;
        cy += (p.y + q.y) * cross;
    }
    a *= static_cast<T>(0.5);
    if (a == static_cast<T>(0))
    {
        // Degenerate (collinear) — fallback to arithmetic mean.
        T mx = static_cast<T>(0);
        T my = static_cast<T>(0);
        for (crd::u32 i = 0; i < n; ++i)
        {
            mx += verts[i].x;
            my += verts[i].y;
        }
        const T inv_n = static_cast<T>(1) / static_cast<T>(n);
        return crd::math::Vec2<T>{mx * inv_n, my * inv_n};
    }
    const T inv_6a = static_cast<T>(1) / (static_cast<T>(6) * a);
    return crd::math::Vec2<T>{cx * inv_6a, cy * inv_6a};
}

// Bbox-edge enumeration for Sutherland-Hodgman. Edges in CCW order around
// the bbox interior:
//   edge 0: y = ymin (bottom), keep points with y >= ymin
//   edge 1: x = xmax (right),  keep points with x <= xmax
//   edge 2: y = ymax (top),    keep points with y <= ymax
//   edge 3: x = xmin (left),   keep points with x >= xmin
template <crd::math::MathScalar T>
inline bool inside_bbox_edge(const crd::math::Vec2<T>& p, crd::u32 edge,
                              T xmin, T ymin, T xmax, T ymax) noexcept
{
    switch (edge)
    {
        case 0U: return p.y >= ymin;
        case 1U: return p.x <= xmax;
        case 2U: return p.y <= ymax;
        case 3U: return p.x >= xmin;
        default: return true;
    }
}

template <crd::math::MathScalar T>
inline crd::math::Vec2<T> intersect_bbox_edge(const crd::math::Vec2<T>& a,
                                                const crd::math::Vec2<T>& b,
                                                crd::u32 edge,
                                                T xmin, T ymin, T xmax, T ymax) noexcept
{
    // Solve for the segment a + t*(b - a) intersecting the bbox edge.
    T t = static_cast<T>(0);
    switch (edge)
    {
        case 0U: // y = ymin
            t = (ymin - a.y) / (b.y - a.y);
            return crd::math::Vec2<T>{a.x + t * (b.x - a.x), ymin};
        case 1U: // x = xmax
            t = (xmax - a.x) / (b.x - a.x);
            return crd::math::Vec2<T>{xmax, a.y + t * (b.y - a.y)};
        case 2U: // y = ymax
            t = (ymax - a.y) / (b.y - a.y);
            return crd::math::Vec2<T>{a.x + t * (b.x - a.x), ymax};
        case 3U: // x = xmin
            t = (xmin - a.x) / (b.x - a.x);
            return crd::math::Vec2<T>{xmin, a.y + t * (b.y - a.y)};
        default:
            return a;
    }
}

// Sutherland-Hodgman polygon clip against axis-aligned bbox.
template <crd::math::MathScalar T>
void clip_polygon_to_bbox(crd::containers::Array<crd::math::Vec2<T>>& poly,
                           T xmin, T ymin, T xmax, T ymax,
                           crd::memory::IAllocator* alloc)
{
    crd::containers::Array<crd::math::Vec2<T>> next_poly(alloc);
    for (crd::u32 edge = 0; edge < 4U; ++edge)
    {
        next_poly.clear();
        const crd::u32 n = static_cast<crd::u32>(poly.size());
        if (n == 0U) { return; }
        for (crd::u32 i = 0; i < n; ++i)
        {
            const auto& cur  = poly[i];
            const auto& prev = poly[(i + n - 1U) % n];
            const bool cur_in  = inside_bbox_edge(cur,  edge, xmin, ymin, xmax, ymax);
            const bool prev_in = inside_bbox_edge(prev, edge, xmin, ymin, xmax, ymax);
            if (cur_in)
            {
                if (!prev_in)
                {
                    next_poly.push_back(intersect_bbox_edge(prev, cur, edge, xmin, ymin, xmax, ymax));
                }
                next_poly.push_back(cur);
            }
            else if (prev_in)
            {
                next_poly.push_back(intersect_bbox_edge(prev, cur, edge, xmin, ymin, xmax, ymax));
            }
        }
        poly.clear();
        for (crd::u32 i = 0; i < next_poly.size(); ++i)
        {
            poly.push_back(next_poly[i]);
        }
    }
}

// Identify which CCW-ordered bbox edge a boundary point sits on (or nearest).
// Returns 0 (bottom), 1 (right), 2 (top), 3 (left). Used during unbounded-
// cell closure to walk bbox corners between ray exits.
template <crd::math::MathScalar T>
crd::u32 bbox_side_of(const crd::math::Vec2<T>& p,
                       T xmin, T ymin, T xmax, T ymax) noexcept
{
    const T eps = std::max(std::max(xmax - xmin, ymax - ymin) * static_cast<T>(1e-6),
                           static_cast<T>(1e-6));
    if (std::abs(p.y - ymin) <= eps) { return 0U; }
    if (std::abs(p.x - xmax) <= eps) { return 1U; }
    if (std::abs(p.y - ymax) <= eps) { return 2U; }
    if (std::abs(p.x - xmin) <= eps) { return 3U; }
    // Fallback: nearest side.
    const T db = std::abs(p.y - ymin);
    const T dr = std::abs(p.x - xmax);
    const T dt = std::abs(p.y - ymax);
    const T dl = std::abs(p.x - xmin);
    if (db <= dr && db <= dt && db <= dl) { return 0U; }
    if (dr <= dt && dr <= dl)             { return 1U; }
    if (dt <= dl)                          { return 2U; }
    return 3U;
}

// Find the t > 0 along ray (origin + t*dir) that intersects the bbox
// boundary. Returns the smallest positive t (axis-aligned slab test).
template <crd::math::MathScalar T>
T ray_bbox_exit_t(const crd::math::Vec2<T>& origin,
                   const crd::math::Vec2<T>& dir,
                   T xmin, T ymin, T xmax, T ymax) noexcept
{
    const T inf = std::numeric_limits<T>::infinity();
    T t_best = inf;
    if (dir.x > static_cast<T>(0))
    {
        const T t = (xmax - origin.x) / dir.x;
        if (t > static_cast<T>(0) && t < t_best) { t_best = t; }
    }
    else if (dir.x < static_cast<T>(0))
    {
        const T t = (xmin - origin.x) / dir.x;
        if (t > static_cast<T>(0) && t < t_best) { t_best = t; }
    }
    if (dir.y > static_cast<T>(0))
    {
        const T t = (ymax - origin.y) / dir.y;
        if (t > static_cast<T>(0) && t < t_best) { t_best = t; }
    }
    else if (dir.y < static_cast<T>(0))
    {
        const T t = (ymin - origin.y) / dir.y;
        if (t > static_cast<T>(0) && t < t_best) { t_best = t; }
    }
    return t_best;
}

// Build a closed polygon for an unbounded Voronoi cell against the bbox.
// CCW order: bounded vertex_indices traversed, plus rays extended to bbox
// + CCW corner walk between the two exit points.
template <crd::math::MathScalar T>
void build_unbounded_cell_polygon(const VoronoiResult2<T>&                     vor,
                                    const VoronoiCell<T>&                       cell,
                                    T xmin, T ymin, T xmax, T ymax,
                                    crd::containers::Array<crd::math::Vec2<T>>& out_poly)
{
    out_poly.clear();
    const crd::u32 n = static_cast<crd::u32>(cell.vertex_indices.size());

    // Compute ray exit points.
    const auto& v_first = vor.voronoi_vertices[cell.vertex_indices[0]];
    const auto& v_last  = vor.voronoi_vertices[cell.vertex_indices[n - 1U]];
    const T t_first = ray_bbox_exit_t(v_first,
                                       crd::math::Vec2<T>{-cell.first_ray_dir.x, -cell.first_ray_dir.y},
                                       xmin, ymin, xmax, ymax);
    const T t_last  = ray_bbox_exit_t(v_last, cell.last_ray_dir, xmin, ymin, xmax, ymax);
    if (!(t_first < std::numeric_limits<T>::infinity()) ||
        !(t_last  < std::numeric_limits<T>::infinity()))
    {
        // Rays don't intersect bbox — defensive; emit bounded vertices only.
        for (crd::u32 i = 0; i < n; ++i)
        {
            out_poly.push_back(vor.voronoi_vertices[cell.vertex_indices[i]]);
        }
        return;
    }
    const crd::math::Vec2<T> p_in{v_first.x - cell.first_ray_dir.x * t_first,
                                   v_first.y - cell.first_ray_dir.y * t_first};
    const crd::math::Vec2<T> p_out{v_last.x + cell.last_ray_dir.x * t_last,
                                    v_last.y + cell.last_ray_dir.y * t_last};

    // CCW polygon: p_in -> v_first -> ... -> v_last -> p_out -> bbox corners
    // (CCW walk from p_out's side to p_in's side) -> back to p_in.
    out_poly.push_back(p_in);
    for (crd::u32 i = 0; i < n; ++i)
    {
        out_poly.push_back(vor.voronoi_vertices[cell.vertex_indices[i]]);
    }
    out_poly.push_back(p_out);

    // CCW corner walk. bbox sides in CCW order: 0=bottom, 1=right, 2=top, 3=left.
    // Corners in CCW order starting at bottom-right:
    //   corner after side 0 (bottom -> right): (xmax, ymin)
    //   corner after side 1 (right  -> top):   (xmax, ymax)
    //   corner after side 2 (top    -> left):  (xmin, ymax)
    //   corner after side 3 (left   -> bottom):(xmin, ymin)
    const crd::math::Vec2<T> corners[4] = {
        {xmax, ymin}, {xmax, ymax}, {xmin, ymax}, {xmin, ymin},
    };
    const crd::u32 side_out = bbox_side_of(p_out, xmin, ymin, xmax, ymax);
    const crd::u32 side_in  = bbox_side_of(p_in,  xmin, ymin, xmax, ymax);
    crd::u32 cur_side = side_out;
    while (cur_side != side_in)
    {
        out_poly.push_back(corners[cur_side]);
        cur_side = (cur_side + 1U) % 4U;
    }
}

template <crd::math::MathScalar T>
inline bool is_finite_vec2(const crd::math::Vec2<T>& p) noexcept
{
    return std::isfinite(static_cast<double>(p.x)) && std::isfinite(static_cast<double>(p.y));
}

// Translate VoronoiStatus2 -> LloydStatus2 for diagnostic propagation.
inline LloydStatus2 propagate_voronoi_status(VoronoiStatus2 s) noexcept
{
    switch (s)
    {
        case VoronoiStatus2::Ok:                return LloydStatus2::Ok;
        case VoronoiStatus2::TooFewPoints:      return LloydStatus2::TooFewPoints;
        case VoronoiStatus2::NonFiniteInput:    return LloydStatus2::NonFiniteInput;
        case VoronoiStatus2::DuplicatePoint:    return LloydStatus2::DuplicatePoint;
        case VoronoiStatus2::InternalInvariant: return LloydStatus2::InternalInvariant;
    }
    return LloydStatus2::InternalInvariant;
}

} // anonymous namespace

template <crd::math::MathScalar T>
LloydResult2<T>
lloyd_relax_2d(crd::containers::ConstSpan<crd::math::Vec2<T>> sites,
                const LloydOptions2<T>&                         opts,
                crd::memory::IAllocator*                        alloc)
{
    LloydResult2<T> result{alloc};
    const crd::u32 n = static_cast<crd::u32>(sites.size());

    // Validate.
    if (n < 3U)
    {
        result.status = LloydStatus2::TooFewPoints;
        return result;
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        if (!is_finite_vec2(sites[i]))
        {
            result.status = LloydStatus2::NonFiniteInput;
            return result;
        }
    }
    // Duplicate-point detection (lex scan).
    // Cheaper: rely on voronoi_2d's propagation. But pre-validate so we
    // don't run a wasted Voronoi pass.
    {
        crd::containers::Array<crd::u32> order(alloc);
        order.resize(n, crd::u32{0});
        for (crd::u32 i = 0; i < n; ++i) { order[i] = i; }
        crd::containers::sort(order.data(), order.data() + order.size(),
                               [&](crd::u32 a, crd::u32 b) noexcept {
                                   const auto& pa = sites[a];
                                   const auto& pb = sites[b];
                                   if (pa.x != pb.x) { return pa.x < pb.x; }
                                   if (pa.y != pb.y) { return pa.y < pb.y; }
                                   return a < b;
                               });
        for (crd::u32 i = 1; i < n; ++i)
        {
            const auto& pa = sites[order[i - 1U]];
            const auto& pb = sites[order[i]];
            if (pa.x == pb.x && pa.y == pb.y)
            {
                result.status = LloydStatus2::DuplicatePoint;
                return result;
            }
        }
    }

    // Determine bbox if ClipToBbox.
    T xmin = static_cast<T>(0);
    T ymin = static_cast<T>(0);
    T xmax = static_cast<T>(0);
    T ymax = static_cast<T>(0);
    if (opts.hull_policy == HullPolicy2::ClipToBbox)
    {
        if (opts.bbox_set)
        {
            xmin = opts.bbox_min.x;
            ymin = opts.bbox_min.y;
            xmax = opts.bbox_max.x;
            ymax = opts.bbox_max.y;
        }
        else
        {
            T x0 = sites[0].x;
            T x1 = sites[0].x;
            T y0 = sites[0].y;
            T y1 = sites[0].y;
            for (crd::u32 i = 1; i < n; ++i)
            {
                if (sites[i].x < x0) { x0 = sites[i].x; }
                if (sites[i].x > x1) { x1 = sites[i].x; }
                if (sites[i].y < y0) { y0 = sites[i].y; }
                if (sites[i].y > y1) { y1 = sites[i].y; }
            }
            const T dx = x1 - x0;
            const T dy = y1 - y0;
            const T pad_x = (dx > static_cast<T>(0)) ? dx * static_cast<T>(0.1) : static_cast<T>(0.1);
            const T pad_y = (dy > static_cast<T>(0)) ? dy * static_cast<T>(0.1) : static_cast<T>(0.1);
            xmin = x0 - pad_x;
            ymin = y0 - pad_y;
            xmax = x1 + pad_x;
            ymax = y1 + pad_y;
        }
        if (!(xmin < xmax) || !(ymin < ymax))
        {
            result.status = LloydStatus2::BboxInvalid;
            return result;
        }
    }

    // Initialise relaxed_sites from input.
    result.relaxed_sites.reserve(n);
    for (crd::u32 i = 0; i < n; ++i) { result.relaxed_sites.push_back(sites[i]); }

    // Iteration loop.
    crd::containers::Array<crd::math::Vec2<T>> new_sites(alloc);
    crd::containers::Array<crd::math::Vec2<T>> cell_poly(alloc);
    for (crd::u32 iter = 0; iter < opts.max_iterations; ++iter)
    {
        auto vor = voronoi_2d<T>(
            crd::containers::ConstSpan<crd::math::Vec2<T>>{
                result.relaxed_sites.data(), result.relaxed_sites.size()},
            alloc);
        if (!vor.ok())
        {
            result.status = propagate_voronoi_status(vor.status);
            return result;
        }

        new_sites.clear();
        new_sites.reserve(n);
        T max_disp2 = static_cast<T>(0);
        for (crd::u32 s = 0; s < n; ++s)
        {
            const auto& cell = vor.cells[s];
            crd::math::Vec2<T> new_pos = result.relaxed_sites[s];

            if (cell.is_bounded)
            {
                cell_poly.clear();
                for (crd::u32 vi : cell.vertex_indices)
                {
                    cell_poly.push_back(vor.voronoi_vertices[vi]);
                }
                if (opts.hull_policy == HullPolicy2::ClipToBbox)
                {
                    clip_polygon_to_bbox(cell_poly, xmin, ymin, xmax, ymax, alloc);
                }
                if (cell_poly.size() >= 3U)
                {
                    new_pos = polygon_centroid_2d(cell_poly.data(),
                                                    static_cast<crd::u32>(cell_poly.size()));
                }
            }
            else
            {
                // Unbounded cell.
                if (opts.hull_policy == HullPolicy2::Fix)
                {
                    // Keep site put.
                }
                else // ClipToBbox
                {
                    build_unbounded_cell_polygon(vor, cell, xmin, ymin, xmax, ymax, cell_poly);
                    clip_polygon_to_bbox(cell_poly, xmin, ymin, xmax, ymax, alloc);
                    if (cell_poly.size() >= 3U)
                    {
                        new_pos = polygon_centroid_2d(cell_poly.data(),
                                                        static_cast<crd::u32>(cell_poly.size()));
                    }
                }
            }

            new_sites.push_back(new_pos);
            const T dx = new_pos.x - result.relaxed_sites[s].x;
            const T dy = new_pos.y - result.relaxed_sites[s].y;
            const T d2 = dx * dx + dy * dy;
            if (d2 > max_disp2) { max_disp2 = d2; }
        }

        // Atomic site swap (Jacobi-style).
        for (crd::u32 i = 0; i < n; ++i)
        {
            result.relaxed_sites[i] = new_sites[i];
        }
        result.iterations_run = iter + 1U;
        result.final_max_displacement = crd::math::sqrt(max_disp2);
        if (result.final_max_displacement < opts.tolerance)
        {
            result.converged = true;
            result.status = LloydStatus2::Ok;
            return result;
        }
    }

    result.status = LloydStatus2::NotConverged;
    return result;
}

// Explicit instantiations.
template LloydResult2<crd::f32>
lloyd_relax_2d<crd::f32>(crd::containers::ConstSpan<crd::math::Vec2<crd::f32>>,
                          const LloydOptions2<crd::f32>&, crd::memory::IAllocator*);
template LloydResult2<crd::f64>
lloyd_relax_2d<crd::f64>(crd::containers::ConstSpan<crd::math::Vec2<crd::f64>>,
                          const LloydOptions2<crd::f64>&, crd::memory::IAllocator*);

} // namespace crd::geometry::delaunay
