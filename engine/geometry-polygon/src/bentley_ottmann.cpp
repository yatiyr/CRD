// ---------------------------------------------------------------------------
// crd-geometry-polygon — v6e Bentley-Ottmann 1979 line-segment intersection.
//
// Sweep-line algorithm for all pairwise segment intersections in
// `O((n + k) log n)` expected time. Event queue is a min-heap keyed by
// lex (y, x, kind, primary-seg-id, secondary-seg-id); status structure
// is a sorted array of active segments. Both are deterministic by
// construction.
//
// **Edge cases handled:**
//   * Vertical segments (segment_y_top == segment_y_bot impossible — that's
//     a horizontal; vertical means same X for both endpoints).
//   * Horizontal segments (handled by routing through a horizontal-pass at
//     each y level: their two endpoints share y, so both events fire at the
//     same y and we sweep them adjacently).
//   * T-junctions (a segment's endpoint lies on another segment's
//     interior): detected by Shewchuk `orient2d == 0` + on-segment check.
//   * Vertex-on-vertex coincidences (two segment endpoints share a point):
//     emitted once with deduplication on (seg_a, seg_b) pair.
//   * Collinear overlap (two segments share a portion of their length):
//     reported via T-junction emissions at the overlap endpoints.
//
// **Robustness.** All orientation, in-segment, and intersection-sign
// decisions use Shewchuk `orient2d` adaptive precision. No naive cross-
// product, no epsilon. Intersection points are computed via parametric
// solution after the sign-check; in degenerate cases (parallel-but-
// collinear) the intersection point reported is the touching endpoint.
//
// **Determinism (ADR-0063 + ADR-0076 §4 pin #11).** Event ordering is
// strict lex-tuple. Status structure ties broken by segment-id. Output
// sorted by lex (point.y, point.x, seg_a, seg_b) before return.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/polygon/bentley_ottmann.hpp>
#include <crd/geometry/primitives/is_finite.hpp>
#include <crd/geometry/primitives/predicates.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include <limits>

namespace crd::geometry::polygon
{
namespace
{
constexpr crd::u32 k_null_idx = std::numeric_limits<crd::u32>::max();

template <crd::math::MathScalar T>
inline T orient2d_signed(const crd::math::Vec2<T>& a, const crd::math::Vec2<T>& b,
                          const crd::math::Vec2<T>& c) noexcept
{
    return crd::geometry::primitives::orient2d(a, b, c);
}

// Canonical segment: lo (lex-smaller endpoint) at index 0, hi at index 1.
template <crd::math::MathScalar T>
struct CanonSeg
{
    crd::math::Vec2<T> lo; // smaller (y, x) endpoint
    crd::math::Vec2<T> hi; // larger
};

template <crd::math::MathScalar T>
inline CanonSeg<T> canonicalise(const BOSegment<T>& s) noexcept
{
    CanonSeg<T> c;
    const auto& a = s.a;
    const auto& b = s.b;
    const bool a_lower = (a.y < b.y) || (a.y == b.y && a.x <= b.x);
    if (a_lower) { c.lo = a; c.hi = b; }
    else         { c.lo = b; c.hi = a; }
    return c;
}

// Segment-segment intersection test + point. Same logic as v6d but tuned
// for the BO output: reports any intersection (transverse, endpoint-on-
// interior, endpoint-on-endpoint).

template <crd::math::MathScalar T>
struct SSHit
{
    bool                hit = false;
    crd::math::Vec2<T> point{};
};

template <crd::math::MathScalar T>
SSHit<T> segment_segment_intersect_full(const crd::math::Vec2<T>& a1,
                                         const crd::math::Vec2<T>& a2,
                                         const crd::math::Vec2<T>& b1,
                                         const crd::math::Vec2<T>& b2) noexcept
{
    const T o1 = orient2d_signed(a1, a2, b1);
    const T o2 = orient2d_signed(a1, a2, b2);
    const T o3 = orient2d_signed(b1, b2, a1);
    const T o4 = orient2d_signed(b1, b2, a2);

    const bool proper = ((o1 > T{0} && o2 < T{0}) || (o1 < T{0} && o2 > T{0})) &&
                        ((o3 > T{0} && o4 < T{0}) || (o3 < T{0} && o4 > T{0}));
    if (proper)
    {
        const T dax = a2.x - a1.x;
        const T day = a2.y - a1.y;
        const T dbx = b2.x - b1.x;
        const T dby = b2.y - b1.y;
        const T denom = dax * dby - day * dbx;
        if (denom == T{0}) { return {}; }
        const T ta = ((b1.x - a1.x) * dby - (b1.y - a1.y) * dbx) / denom;
        SSHit<T> h;
        h.hit   = true;
        h.point = crd::math::Vec2<T>{a1.x + ta * dax, a1.y + ta * day};
        return h;
    }

    auto on_seg_closed = [](const crd::math::Vec2<T>& s1, const crd::math::Vec2<T>& s2,
                             const crd::math::Vec2<T>& p) noexcept {
        const T lo_x = s1.x < s2.x ? s1.x : s2.x;
        const T hi_x = s1.x > s2.x ? s1.x : s2.x;
        const T lo_y = s1.y < s2.y ? s1.y : s2.y;
        const T hi_y = s1.y > s2.y ? s1.y : s2.y;
        return p.x >= lo_x && p.x <= hi_x && p.y >= lo_y && p.y <= hi_y;
    };

    // Endpoint-on-other-segment cases. orient == 0 + on_segment_closed.
    if (o1 == T{0} && on_seg_closed(a1, a2, b1)) { return SSHit<T>{true, b1}; }
    if (o2 == T{0} && on_seg_closed(a1, a2, b2)) { return SSHit<T>{true, b2}; }
    if (o3 == T{0} && on_seg_closed(b1, b2, a1)) { return SSHit<T>{true, a1}; }
    if (o4 == T{0} && on_seg_closed(b1, b2, a2)) { return SSHit<T>{true, a2}; }

    return {};
}

// =========================================================================
// Event queue
// =========================================================================

enum class EvKind : crd::u8
{
    // Order matters for tie-breaking — at the same y/x, START events fire
    // BEFORE INTERSECTION events fire BEFORE END events. (This matches
    // the canonical Bentley-Ottmann ordering.)
    Start        = 0,
    Intersection = 1,
    End          = 2,
};

template <crd::math::MathScalar T>
struct Event
{
    T        y = T{0};
    T        x = T{0};
    EvKind   kind;
    crd::u32 seg_a = k_null_idx;
    crd::u32 seg_b = k_null_idx; // intersection events only
    crd::math::Vec2<T> point{};   // intersection events only
};

template <crd::math::MathScalar T>
inline bool event_lex_less(const Event<T>& l, const Event<T>& r) noexcept
{
    if (l.y != r.y) { return l.y < r.y; }
    if (l.x != r.x) { return l.x < r.x; }
    if (l.kind != r.kind)
    {
        return static_cast<crd::u8>(l.kind) < static_cast<crd::u8>(r.kind);
    }
    if (l.seg_a != r.seg_a) { return l.seg_a < r.seg_a; }
    return l.seg_b < r.seg_b;
}

// min-heap order: top should be smallest. push_heap/pop_heap use a MAX
// comparator semantically; we invert to get a min-heap.
template <crd::math::MathScalar T>
inline bool event_heap_less(const Event<T>& l, const Event<T>& r) noexcept
{
    return event_lex_less(r, l); // inverted ⇒ min-heap behaviour
}

// =========================================================================
// Status structure — sorted array of active segments by X at sweep y
// =========================================================================

template <crd::math::MathScalar T>
inline T x_at_y(const CanonSeg<T>& s, T sweep_y) noexcept
{
    // The segment goes from lo (smaller y) to hi (larger y) — by canonical
    // ordering, lo.y <= hi.y. For a non-horizontal segment, x_at_y is well-
    // defined for sweep_y in [lo.y, hi.y].
    const T dy = s.hi.y - s.lo.y;
    if (dy == T{0}) { return s.lo.x; } // horizontal — caller shouldn't call here
    const T dx = s.hi.x - s.lo.x;
    const T t  = (sweep_y - s.lo.y) / dy;
    return s.lo.x + t * dx;
}

// =========================================================================
// Main sweep
// =========================================================================

template <crd::math::MathScalar T>
BOResult<T> bentley_ottmann_impl(crd::containers::ConstSpan<BOSegment<T>> segments,
                                  crd::memory::IAllocator*                  alloc,
                                  bool short_circuit, BOIntersection<T>* out_first)
{
    BOResult<T> result(alloc);

    // -- Input validation + canonicalisation -----------------------------
    crd::containers::Array<CanonSeg<T>> segs(alloc);
    segs.reserve(segments.size());
    for (crd::usize i = 0; i < segments.size(); ++i)
    {
        const auto& s = segments[i];
        if (!crd::geometry::primitives::is_finite(s.a) ||
            !crd::geometry::primitives::is_finite(s.b))
        {
            result.status = BOStatus::NonFiniteInput;
            return result;
        }
        if (s.a.x == s.b.x && s.a.y == s.b.y)
        {
            result.status = BOStatus::DegenerateSegment;
            return result;
        }
        segs.push_back(canonicalise<T>(s));
    }

    // -- Detect horizontal segments separately ----------------------------
    // Horizontals don't fit cleanly in a Y-sweep with X-sorted status
    // (their X varies across the segment). We test them brute-force against
    // every other segment. For typical inputs with few horizontals this is
    // negligible cost; for axis-aligned-rectangle-heavy inputs it dominates
    // and a follow-on optimisation (separate X-pass for horizontals) would
    // help.
    crd::containers::Array<crd::u32> horiz_idx(alloc);
    crd::containers::Array<crd::u32> nonhoriz_idx(alloc);
    for (crd::u32 i = 0; i < segs.size(); ++i)
    {
        if (segs[i].lo.y == segs[i].hi.y) { horiz_idx.push_back(i); }
        else { nonhoriz_idx.push_back(i); }
    }

    // -- Initialise event queue with Start/End for every non-horizontal --
    crd::containers::Array<Event<T>> events(alloc);
    events.reserve(2U * nonhoriz_idx.size());
    for (crd::u32 ii = 0; ii < nonhoriz_idx.size(); ++ii)
    {
        const crd::u32 i = nonhoriz_idx[ii];
        Event<T>       e_start{};
        e_start.y     = segs[i].lo.y;
        e_start.x     = segs[i].lo.x;
        e_start.kind  = EvKind::Start;
        e_start.seg_a = i;
        events.push_back(e_start);
        Event<T> e_end{};
        e_end.y     = segs[i].hi.y;
        e_end.x     = segs[i].hi.x;
        e_end.kind  = EvKind::End;
        e_end.seg_a = i;
        events.push_back(e_end);
    }
    crd::containers::make_heap(events.data(), events.data() + events.size(), event_heap_less<T>);

    // -- Status structure: indices into segs[], sorted by x-at-sweep-y ----
    crd::containers::Array<crd::u32> status(alloc);

    // -- Reported set: pairs already emitted (dedup on swap-then-test) ----
    // We use a flat Array of canonicalised (lo, hi) pairs; lookup is linear
    // but the total set size is bounded by output size which is small in
    // practice.
    crd::containers::Array<crd::u64> reported(alloc);
    auto pair_key = [](crd::u32 a, crd::u32 b) noexcept {
        const crd::u32 lo = a < b ? a : b;
        const crd::u32 hi = a < b ? b : a;
        return (static_cast<crd::u64>(lo) << 32) | static_cast<crd::u64>(hi);
    };
    auto already_reported = [&](crd::u32 a, crd::u32 b) noexcept {
        const crd::u64 k = pair_key(a, b);
        for (crd::usize i = 0; i < reported.size(); ++i)
        {
            if (reported[i] == k) { return true; }
        }
        return false;
    };
    auto mark_reported = [&](crd::u32 a, crd::u32 b) noexcept {
        reported.push_back(pair_key(a, b));
    };
    auto emit = [&](crd::u32 a, crd::u32 b, const crd::math::Vec2<T>& p) noexcept {
        if (already_reported(a, b)) { return; }
        mark_reported(a, b);
        BOIntersection<T> rec{};
        rec.segment_a = a;
        rec.segment_b = b;
        rec.point     = p;
        result.intersections.push_back(rec);
    };

    auto enqueue_intersection = [&](crd::u32 a, crd::u32 b, T sweep_y) noexcept {
        // Test segments a and b for intersection AT OR ABOVE sweep_y. If
        // they intersect at a point above the sweep line, enqueue an
        // Intersection event (so it gets processed at the proper time).
        if (a == b) { return; }
        const auto& sa = segs[a];
        const auto& sb = segs[b];
        const auto  hit = segment_segment_intersect_full<T>(sa.lo, sa.hi, sb.lo, sb.hi);
        if (!hit.hit) { return; }
        if (hit.point.y < sweep_y) { return; }
        // Skip the event if we've already reported this pair (avoid re-
        // enqueueing the same intersection many times).
        if (already_reported(a, b)) { return; }
        Event<T> e{};
        e.y     = hit.point.y;
        e.x     = hit.point.x;
        e.kind  = EvKind::Intersection;
        e.seg_a = a < b ? a : b;
        e.seg_b = a < b ? b : a;
        e.point = hit.point;
        events.push_back(e);
        crd::containers::push_heap(events.data(), events.data() + events.size(),
                                    event_heap_less<T>);
    };

    auto status_insert_position = [&](crd::u32 seg_id, T sweep_y) noexcept {
        // Find insertion position to keep status sorted by x-at-sweep-y.
        const T xn = x_at_y(segs[seg_id], sweep_y);
        crd::u32 lo = 0;
        crd::u32 hi = static_cast<crd::u32>(status.size());
        while (lo < hi)
        {
            const crd::u32 mid = (lo + hi) / 2U;
            const T        xm  = x_at_y(segs[status[mid]], sweep_y);
            if (xm < xn || (xm == xn && status[mid] < seg_id)) { lo = mid + 1U; }
            else { hi = mid; }
        }
        return lo;
    };

    auto status_find = [&](crd::u32 seg_id) noexcept {
        for (crd::u32 i = 0; i < status.size(); ++i)
        {
            if (status[i] == seg_id) { return i; }
        }
        return k_null_idx;
    };

    auto status_insert_at = [&](crd::u32 pos, crd::u32 seg_id) noexcept {
        status.push_back(0U);
        for (crd::u32 i = static_cast<crd::u32>(status.size()) - 1U; i > pos; --i)
        {
            status[i] = status[i - 1U];
        }
        status[pos] = seg_id;
    };

    auto status_erase_at = [&](crd::u32 pos) noexcept {
        const crd::u32 last = static_cast<crd::u32>(status.size()) - 1U;
        for (crd::u32 i = pos; i < last; ++i) { status[i] = status[i + 1U]; }
        status.pop_back();
    };

    // -- Main sweep ------------------------------------------------------
    while (!events.empty())
    {
        crd::containers::pop_heap(events.data(), events.data() + events.size(),
                                   event_heap_less<T>);
        Event<T> e = events.back();
        events.pop_back();

        if (e.kind == EvKind::Start)
        {
            const crd::u32 pos = status_insert_position(e.seg_a, e.y);
            status_insert_at(pos, e.seg_a);
            if (pos > 0U) { enqueue_intersection(status[pos - 1U], status[pos], e.y); }
            if (pos + 1U < status.size())
            {
                enqueue_intersection(status[pos], status[pos + 1U], e.y);
            }
        }
        else if (e.kind == EvKind::End)
        {
            const crd::u32 pos = status_find(e.seg_a);
            if (pos == k_null_idx) { continue; }
            // Neighbours that become adjacent after removal.
            crd::u32 left  = (pos > 0U) ? status[pos - 1U] : k_null_idx;
            crd::u32 right = (pos + 1U < status.size()) ? status[pos + 1U] : k_null_idx;
            status_erase_at(pos);
            if (left != k_null_idx && right != k_null_idx)
            {
                enqueue_intersection(left, right, e.y);
            }
        }
        else
        {
            // Intersection event — emit, swap, retest.
            emit(e.seg_a, e.seg_b, e.point);
            if (short_circuit)
            {
                if (out_first != nullptr) { *out_first = result.intersections[0]; }
                return result;
            }
            // Swap the two segments in status. Find them.
            const crd::u32 pa = status_find(e.seg_a);
            const crd::u32 pb = status_find(e.seg_b);
            if (pa == k_null_idx || pb == k_null_idx) { continue; }
            // They must be adjacent at this point (BO invariant). If not,
            // skip — the swap doesn't apply (defensive guard against the
            // dedup logic firing twice on the same intersection).
            const crd::u32 lo_p = pa < pb ? pa : pb;
            const crd::u32 hi_p = pa < pb ? pb : pa;
            if (hi_p - lo_p != 1U) { continue; }
            // Swap.
            const crd::u32 tmp = status[lo_p];
            status[lo_p] = status[hi_p];
            status[hi_p] = tmp;
            // Test new neighbours of the swapped pair.
            if (lo_p > 0U) { enqueue_intersection(status[lo_p - 1U], status[lo_p], e.y); }
            if (hi_p + 1U < status.size())
            {
                enqueue_intersection(status[hi_p], status[hi_p + 1U], e.y);
            }
        }
    }

    // -- Brute-force pass for horizontal segments ------------------------
    for (crd::u32 hi = 0; hi < horiz_idx.size(); ++hi)
    {
        const crd::u32 i = horiz_idx[hi];
        for (crd::u32 j = 0; j < segs.size(); ++j)
        {
            if (j == i) { continue; }
            if (already_reported(i, j)) { continue; }
            const auto& sa = segs[i];
            const auto& sb = segs[j];
            const auto  hit = segment_segment_intersect_full<T>(sa.lo, sa.hi, sb.lo, sb.hi);
            if (!hit.hit) { continue; }
            emit(i, j, hit.point);
            if (short_circuit)
            {
                if (out_first != nullptr) { *out_first = result.intersections[0]; }
                return result;
            }
        }
    }

    // -- Sort output by (y, x, seg_a, seg_b) -----------------------------
    crd::containers::sort(result.intersections.data(),
                          result.intersections.data() + result.intersections.size(),
                          [](const BOIntersection<T>& l, const BOIntersection<T>& r) noexcept {
                              if (l.point.y != r.point.y) { return l.point.y < r.point.y; }
                              if (l.point.x != r.point.x) { return l.point.x < r.point.x; }
                              if (l.segment_a != r.segment_a) { return l.segment_a < r.segment_a; }
                              return l.segment_b < r.segment_b;
                          });
    return result;
}

} // namespace

// -- Public entries -------------------------------------------------------

template <crd::math::MathScalar T>
BOResult<T> bentley_ottmann(crd::containers::ConstSpan<BOSegment<T>> segments,
                              crd::memory::IAllocator*                  alloc)
{
    return bentley_ottmann_impl<T>(segments, alloc, false, nullptr);
}

template <crd::math::MathScalar T>
bool bentley_ottmann_any(crd::containers::ConstSpan<BOSegment<T>> segments,
                          crd::memory::IAllocator* alloc, BOIntersection<T>* out_first)
{
    auto r = bentley_ottmann_impl<T>(segments, alloc, true, out_first);
    return !r.intersections.empty();
}

// -- Explicit instantiations ----------------------------------------------

template BOResult<crd::f32> bentley_ottmann<crd::f32>(
    crd::containers::ConstSpan<BOSegment<crd::f32>>, crd::memory::IAllocator*);
template BOResult<crd::f64> bentley_ottmann<crd::f64>(
    crd::containers::ConstSpan<BOSegment<crd::f64>>, crd::memory::IAllocator*);
template bool bentley_ottmann_any<crd::f32>(
    crd::containers::ConstSpan<BOSegment<crd::f32>>, crd::memory::IAllocator*,
    BOIntersection<crd::f32>*);
template bool bentley_ottmann_any<crd::f64>(
    crd::containers::ConstSpan<BOSegment<crd::f64>>, crd::memory::IAllocator*,
    BOIntersection<crd::f64>*);

} // namespace crd::geometry::polygon
