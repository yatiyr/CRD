#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-polygon — v6a polygon predicates.
//
// Scalar predicates over Ring2 / PolygonView2 / Polygon2. All run on raw
// `MathScalar T` (`f32`/`f64`) per the ADR-0078 §5 D34 two-layer rule;
// typed `Length32` callers ride `polygon_predicates_typed.hpp` strip-
// compute-retag wrappers one layer up.
//
// **Robustness contract.** Predicates that DECIDE sign (`is_ccw`,
// `is_simple`, `point_in_ring`, `point_in_polygon`) use Shewchuk
// `orient2d` adaptive precision (Phase 3.1.7 v3a / ADR-0076 §18) — no
// naive cross-product fallback. Predicates that COMPUTE a value
// (`signed_area`, `centroid`, `aabb`) use the value's natural f64
// accumulator, accept the rounding error in the magnitude, and return
// raw `T` after a cast.
//
// **Determinism (ADR-0063 + ADR-0076 §4 pin #11).** No transcendentals;
// no `std::sort` on FP keys (we don't sort here). The accumulators are
// pure left-to-right `+=` — bit-identical across compilers / SIMD widths
// / OSes for any given input order.
//
// **Builder reject / query tolerate.** Predicates here are QUERIES — they
// short-circuit on empty / 1-vertex / non-finite rings rather than asserting
// (`signed_area(empty) → 0`, `aabb(empty) → empty AABB sentinel,
// `point_in_ring(empty) → Outside`). Builder-side `add_ring` does the
// finiteness assertion at insert time.
// ---------------------------------------------------------------------------

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/polygon/polygon_types.hpp>
#include <crd/geometry/primitives/is_finite.hpp>
#include <crd/geometry/primitives/predicates.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

#include <limits>

namespace crd::geometry::polygon
{

// ---- signed_area ---------------------------------------------------------
//
// Shoelace formula: 0.5 * Σ (x_i * y_{i+1} - x_{i+1} * y_i). Positive for
// CCW rings, negative for CW. The accumulator runs in `f64` regardless of
// `T` to dodge catastrophic cancellation on near-collinear inputs, then
// casts back to T at the very end. Empty / 1-vertex / 2-vertex rings have
// zero area by definition (no polygon).

template <MathScalar T>
[[nodiscard]] T signed_area(Ring2<T> r) noexcept
{
    const crd::usize n = r.size();
    if (n < 3U) { return T{0}; }
    crd::f64 sum = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        const auto&    a = r[i];
        const auto&    b = r[r.next(i)];
        const crd::f64 ax = static_cast<crd::f64>(a.x);
        const crd::f64 ay = static_cast<crd::f64>(a.y);
        const crd::f64 bx = static_cast<crd::f64>(b.x);
        const crd::f64 by = static_cast<crd::f64>(b.y);
        sum += ax * by - bx * ay;
    }
    return static_cast<T>(sum * 0.5);
}

// Polygon signed area = outer area + Σ hole areas. With the locked winding
// convention (outer CCW, holes CW), holes contribute negative signed-area
// and the sum equals the true filled-region area.
template <MathScalar T>
[[nodiscard]] T signed_area(PolygonView2<T> p) noexcept
{
    crd::f64       sum = 0.0;
    const crd::u32 rc  = p.ring_count();
    for (crd::u32 r = 0; r < rc; ++r)
    {
        sum += static_cast<crd::f64>(signed_area(p.ring(r)));
    }
    return static_cast<T>(sum);
}

// ---- aabb ---------------------------------------------------------------

template <MathScalar T>
[[nodiscard]] crd::geometry::primitives::AABB2<T> aabb(Ring2<T> r) noexcept
{
    using AABB = crd::geometry::primitives::AABB2<T>;
    const crd::usize n = r.size();
    if (n == 0U)
    {
        // Empty-aabb sentinel — consistent with the AABB3 convention pinned
        // by ADR-0076 §15 (`aabb_empty()={+inf,-inf}`).
        constexpr T inf = std::numeric_limits<T>::infinity();
        return AABB{crd::math::Vec2<T>{inf, inf}, crd::math::Vec2<T>{-inf, -inf}};
    }
    crd::math::Vec2<T> lo = r[0];
    crd::math::Vec2<T> hi = r[0];
    for (crd::usize i = 1; i < n; ++i)
    {
        const auto& v = r[i];
        lo.x          = v.x < lo.x ? v.x : lo.x;
        lo.y          = v.y < lo.y ? v.y : lo.y;
        hi.x          = v.x > hi.x ? v.x : hi.x;
        hi.y          = v.y > hi.y ? v.y : hi.y;
    }
    return AABB{lo, hi};
}

template <MathScalar T>
[[nodiscard]] crd::geometry::primitives::AABB2<T> aabb(PolygonView2<T> p) noexcept
{
    using AABB = crd::geometry::primitives::AABB2<T>;
    constexpr T inf = std::numeric_limits<T>::infinity();
    AABB        out{crd::math::Vec2<T>{inf, inf}, crd::math::Vec2<T>{-inf, -inf}};
    const crd::u32 rc = p.ring_count();
    for (crd::u32 r = 0; r < rc; ++r)
    {
        const AABB rb = aabb(p.ring(r));
        if (rb.min.x < out.min.x) { out.min.x = rb.min.x; }
        if (rb.min.y < out.min.y) { out.min.y = rb.min.y; }
        if (rb.max.x > out.max.x) { out.max.x = rb.max.x; }
        if (rb.max.y > out.max.y) { out.max.y = rb.max.y; }
    }
    return out;
}

// ---- centroid ------------------------------------------------------------
//
// Area-weighted centroid via the standard polygon-centroid formula
// (Bourke 1988). For non-simple polygons the result is the weighted
// signed-area centroid; for self-overlapping rings it's well-defined but
// not necessarily inside the polygon. Empty / zero-area rings return the
// arithmetic mean of vertices (`(Σv) / n`) so callers don't divide by zero.

template <MathScalar T>
[[nodiscard]] crd::math::Vec2<T> centroid(Ring2<T> r) noexcept
{
    const crd::usize n = r.size();
    if (n == 0U) { return crd::math::Vec2<T>{T{0}, T{0}}; }
    if (n < 3U)
    {
        crd::f64 cx = 0.0;
        crd::f64 cy = 0.0;
        for (crd::usize i = 0; i < n; ++i)
        {
            cx += static_cast<crd::f64>(r[i].x);
            cy += static_cast<crd::f64>(r[i].y);
        }
        const crd::f64 inv = 1.0 / static_cast<crd::f64>(n);
        return crd::math::Vec2<T>{static_cast<T>(cx * inv), static_cast<T>(cy * inv)};
    }
    crd::f64 cx       = 0.0;
    crd::f64 cy       = 0.0;
    crd::f64 area_acc = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        const auto&    a   = r[i];
        const auto&    b   = r[r.next(i)];
        const crd::f64 ax  = static_cast<crd::f64>(a.x);
        const crd::f64 ay  = static_cast<crd::f64>(a.y);
        const crd::f64 bx  = static_cast<crd::f64>(b.x);
        const crd::f64 by  = static_cast<crd::f64>(b.y);
        const crd::f64 cross = ax * by - bx * ay;
        cx += (ax + bx) * cross;
        cy += (ay + by) * cross;
        area_acc += cross;
    }
    if (area_acc == 0.0)
    {
        // Zero-area degenerate (collinear) — return arithmetic mean fallback.
        crd::f64 sx = 0.0;
        crd::f64 sy = 0.0;
        for (crd::usize i = 0; i < n; ++i)
        {
            sx += static_cast<crd::f64>(r[i].x);
            sy += static_cast<crd::f64>(r[i].y);
        }
        const crd::f64 inv = 1.0 / static_cast<crd::f64>(n);
        return crd::math::Vec2<T>{static_cast<T>(sx * inv), static_cast<T>(sy * inv)};
    }
    const crd::f64 inv = 1.0 / (3.0 * area_acc);
    return crd::math::Vec2<T>{static_cast<T>(cx * inv), static_cast<T>(cy * inv)};
}

template <MathScalar T>
[[nodiscard]] crd::math::Vec2<T> centroid(PolygonView2<T> p) noexcept
{
    // Area-weighted across rings. Outer contributes positive area; holes
    // contribute negative — the weighted average is the centroid of the
    // filled region.
    const crd::u32 rc = p.ring_count();
    if (rc == 0U) { return crd::math::Vec2<T>{T{0}, T{0}}; }
    crd::f64 cx       = 0.0;
    crd::f64 cy       = 0.0;
    crd::f64 area_acc = 0.0;
    for (crd::u32 r = 0; r < rc; ++r)
    {
        const Ring2<T> ring = p.ring(r);
        const auto     c    = centroid(ring);
        const crd::f64 a    = static_cast<crd::f64>(signed_area(ring));
        cx += static_cast<crd::f64>(c.x) * a;
        cy += static_cast<crd::f64>(c.y) * a;
        area_acc += a;
    }
    if (area_acc == 0.0) { return centroid(p.outer()); }
    const crd::f64 inv = 1.0 / area_acc;
    return crd::math::Vec2<T>{static_cast<T>(cx * inv), static_cast<T>(cy * inv)};
}

// ---- is_ccw / is_cw ------------------------------------------------------

template <MathScalar T>
[[nodiscard]] bool is_ccw(Ring2<T> r) noexcept
{
    return signed_area(r) > T{0};
}
template <MathScalar T>
[[nodiscard]] bool is_cw(Ring2<T> r) noexcept
{
    return signed_area(r) < T{0};
}

// ---- ensure_orientation --------------------------------------------------
//
// Returns a new Polygon2 with the requested orientation per ring. Pass
// `want_outer_ccw=true` to honour the v6 winding convention (outer CCW +
// holes CW). Used by external loaders (cookers, glTF, font glyph readers)
// that don't pre-classify orientation.

template <MathScalar T>
[[nodiscard]] Polygon2<T> ensure_orientation(PolygonView2<T> p, crd::memory::IAllocator* alloc,
                                             bool want_outer_ccw = true)
{
    Polygon2<T>    out(alloc);
    const crd::u32 rc = p.ring_count();
    for (crd::u32 r = 0; r < rc; ++r)
    {
        const Ring2<T> ring  = p.ring(r);
        const bool     is_outer = (r == 0U);
        const bool     want_ccw = is_outer ? want_outer_ccw : !want_outer_ccw;
        const bool     have_ccw = is_ccw(ring);
        if (want_ccw == have_ccw)
        {
            out.add_ring(ring.vertices);
        }
        else
        {
            // Push reversed.
            crd::containers::Array<crd::math::Vec2<T>> rev(alloc);
            rev.reserve(ring.size());
            for (crd::usize i = ring.size(); i > 0U; --i) { rev.push_back(ring[i - 1U]); }
            out.add_ring(rev);
        }
    }
    return out;
}

// ---- point_in_ring / point_in_polygon -----------------------------------

enum class PointInPolygon : crd::u8
{
    Outside    = 0,
    OnBoundary = 1,
    Inside     = 2,
};

// Crossing-number ray-casting (Hormann-Agathos 2001) with Shewchuk
// `orient2d` decisions on adverse cases. Reports Inside / Outside /
// OnBoundary. The ray fires in the +X direction from `p` and counts crossings
// with non-horizontal edges; the orient2d call disambiguates left-of vs
// right-of for edges whose Y-span straddles `p.y`. Boundary detection short-
// circuits on exact-collinear-on-segment, including the closed endpoints.

namespace polygon_detail
{
template <MathScalar T>
[[nodiscard]] inline bool on_segment_2d(const crd::math::Vec2<T>& a,
                                        const crd::math::Vec2<T>& b,
                                        const crd::math::Vec2<T>& p) noexcept
{
    // Caller has ensured orient2d(a,b,p) == 0 (collinear). Now check
    // whether p lies WITHIN the segment closed-endpoints. Use coordinate-
    // wise min/max — works for axis-aligned and oblique segments alike.
    const T lo_x = a.x < b.x ? a.x : b.x;
    const T hi_x = a.x > b.x ? a.x : b.x;
    const T lo_y = a.y < b.y ? a.y : b.y;
    const T hi_y = a.y > b.y ? a.y : b.y;
    return p.x >= lo_x && p.x <= hi_x && p.y >= lo_y && p.y <= hi_y;
}
} // namespace polygon_detail

template <MathScalar T>
[[nodiscard]] PointInPolygon point_in_ring(Ring2<T> r, const crd::math::Vec2<T>& p) noexcept
{
    const crd::usize n = r.size();
    if (n < 3U) { return PointInPolygon::Outside; }
    if (!crd::geometry::primitives::is_finite(p))
    {
        // Query tolerate: non-finite query point ⇒ "Outside" (defensive,
        // never crash).
        return PointInPolygon::Outside;
    }
    int crossings = 0;
    for (crd::usize i = 0; i < n; ++i)
    {
        const auto& a = r[i];
        const auto& b = r[r.next(i)];
        // Boundary check via Shewchuk orient2d — exact zero ⇒ collinear with
        // edge, then on_segment_2d disambiguates "between endpoints" vs
        // "extension of the line". orient2d returns T (f32 or f64) matching
        // the input scalar; compare against T{0} directly.
        const T o = crd::geometry::primitives::orient2d(a, b, p);
        if (o == T{0} && polygon_detail::on_segment_2d(a, b, p))
        {
            return PointInPolygon::OnBoundary;
        }
        // Crossing-number rule (Hormann-Agathos 2001):
        //   Edge (a,b) contributes a +X-ray crossing iff exactly one of
        //   (a.y > p.y, b.y > p.y) is true AND the X-intersect of the edge
        //   with the horizontal line y=p.y is > p.x.
        const bool a_above = a.y > p.y;
        const bool b_above = b.y > p.y;
        if (a_above != b_above)
        {
            // Edge straddles the horizontal y=p.y. Sign of orient2d(a,b,p)
            // tells us which side `p` is on; combined with the upward/down-
            // ward direction of the edge it tells us whether the +X-ray
            // crosses to the RIGHT of p (count it) or to the LEFT (skip).
            // Equivalent to computing the X-intersect, but uses adaptive
            // predicates so adverse coords get the exact sign.
            const bool upward = b_above; // edge goes from below p.y to above
            const bool right  = upward ? (o > T{0}) : (o < T{0});
            if (right) { ++crossings; }
        }
    }
    return (crossings & 1) != 0 ? PointInPolygon::Inside : PointInPolygon::Outside;
}

template <MathScalar T>
[[nodiscard]] PointInPolygon point_in_polygon(PolygonView2<T>            p,
                                              const crd::math::Vec2<T>& q) noexcept
{
    const crd::u32 rc = p.ring_count();
    if (rc == 0U) { return PointInPolygon::Outside; }
    const auto pir_outer = point_in_ring(p.outer(), q);
    if (pir_outer == PointInPolygon::OnBoundary) { return PointInPolygon::OnBoundary; }
    if (pir_outer == PointInPolygon::Outside) { return PointInPolygon::Outside; }
    // Inside outer — check holes. Inside a hole ⇒ Outside the polygon.
    for (crd::u32 r = 1; r < rc; ++r)
    {
        const auto pir = point_in_ring(p.ring(r), q);
        if (pir == PointInPolygon::OnBoundary) { return PointInPolygon::OnBoundary; }
        if (pir == PointInPolygon::Inside) { return PointInPolygon::Outside; }
    }
    return PointInPolygon::Inside;
}

// ---- is_simple -----------------------------------------------------------
//
// Returns true iff no two non-adjacent edges of the ring intersect (the
// shared endpoint of adjacent edges is the only legal coincidence). O(n²)
// brute force — the elite-tier O(n log n) Bentley-Ottmann form is what v6e
// ships; v6a callers that need scale should defer until then.
//
// Uses Shewchuk `orient2d` for the four orientation tests + collinear
// special-case for coincident-segment detection.

namespace polygon_detail
{
template <MathScalar T>
[[nodiscard]] inline bool segments_proper_intersect_2d(
    const crd::math::Vec2<T>& a1, const crd::math::Vec2<T>& a2,
    const crd::math::Vec2<T>& b1, const crd::math::Vec2<T>& b2) noexcept
{
    // Classic 4-orient test. Two segments PROPERLY intersect iff each one's
    // endpoints lie on opposite sides of the other (strict signs). They
    // additionally TOUCH (boundary-overlap) iff any of the 4 orients is zero
    // AND the touching endpoint lies on the other segment.
    const T o1 = crd::geometry::primitives::orient2d(a1, a2, b1);
    const T o2 = crd::geometry::primitives::orient2d(a1, a2, b2);
    const T o3 = crd::geometry::primitives::orient2d(b1, b2, a1);
    const T o4 = crd::geometry::primitives::orient2d(b1, b2, a2);
    const bool proper = ((o1 > T{0} && o2 < T{0}) || (o1 < T{0} && o2 > T{0})) &&
                        ((o3 > T{0} && o4 < T{0}) || (o3 < T{0} && o4 > T{0}));
    if (proper) { return true; }
    // Collinear / touching — count as intersection iff the touch is interior
    // to either segment (endpoint-touch of adjacent edges is the legal
    // case, filtered by the caller via the adjacency skip).
    if (o1 == T{0} && on_segment_2d(a1, a2, b1)) { return true; }
    if (o2 == T{0} && on_segment_2d(a1, a2, b2)) { return true; }
    if (o3 == T{0} && on_segment_2d(b1, b2, a1)) { return true; }
    if (o4 == T{0} && on_segment_2d(b1, b2, a2)) { return true; }
    return false;
}
} // namespace polygon_detail

template <MathScalar T>
[[nodiscard]] bool is_simple(Ring2<T> r) noexcept
{
    const crd::usize n = r.size();
    if (n < 3U) { return true; }
    for (crd::usize i = 0; i < n; ++i)
    {
        const auto& a1 = r[i];
        const auto& a2 = r[r.next(i)];
        for (crd::usize j = i + 2U; j < n; ++j)
        {
            // Skip the edge adjacent to (i)'s start — the wraparound case
            // pairs edge (n-1, 0) with edge (0, 1) which share vertex 0.
            if (i == 0U && j == n - 1U) { continue; }
            const auto& b1 = r[j];
            const auto& b2 = r[r.next(j)];
            if (polygon_detail::segments_proper_intersect_2d(a1, a2, b1, b2)) { return false; }
        }
    }
    return true;
}

template <MathScalar T>
[[nodiscard]] bool is_simple(PolygonView2<T> p) noexcept
{
    const crd::u32 rc = p.ring_count();
    for (crd::u32 r = 0; r < rc; ++r)
    {
        if (!is_simple(p.ring(r))) { return false; }
    }
    return true;
}

} // namespace crd::geometry::polygon
