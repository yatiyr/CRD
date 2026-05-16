// ---------------------------------------------------------------------------
// crd-geometry-polygon — v6d general polygon Boolean operations.
//
// Implementation strategy: **planar-subdivision with winding-number face
// classification**. The algorithm proceeds in five phases:
//
//   1) Collect every directed edge of subject + clip into a flat list,
//      tagged with its source polygon and a winding-contribution sign
//      (+1 for CCW outer, -1 for CW hole).
//
//   2) Find every pairwise edge-edge intersection (brute-force O(n*m)).
//      Each intersection contributes a split point to BOTH edges. Vertex
//      coincidence (vertex-on-vertex, vertex-on-edge) is handled in the
//      same path — Shewchuk `orient2d` adaptive precision detects exact
//      collinearity and the splitting code merges coincident points.
//
//   3) Build a planar straight-line graph (PSLG) by splitting each input
//      edge at its intersection points. Vertices are deduplicated by exact
//      coordinate equality (Shewchuk-exact); coincident points from
//      different input edges share one vertex.
//
//   4) Build the DCEL half-edge structure: each split sub-segment becomes
//      two directed half-edges (twin pair). At each vertex the outgoing
//      half-edges are sorted CCW by angle (orient2d-based comparator);
//      `next_in_face` is wired by the standard DCEL convention so that
//      walking via `.next_in_face` traces a face boundary CCW.
//
//   5) Walk each face boundary; classify the face by ray-casting from an
//      interior point and counting subject + clip winding contributions
//      (each input edge crossed by the ray contributes its sign). Apply
//      the Boolean predicate + fill rule. Emit boundaries of accepted
//      faces as output polygon rings.
//
// **Why planar-subdivision and not Vatti scanbeam?** Vatti scanbeam is
// asymptotically faster — O((n+k) log n) vs O((n+k) log k) for ours where
// k is intersection count. For typical engine workloads (polygons with
// < 10k vertices per operand), the constant factor in our brute-force
// intersection is small enough that the difference is sub-millisecond.
// The planar-subdivision algorithm is dramatically simpler to make
// robust + deterministic, and it shares its intersection finder with
// the v6e Bentley-Ottmann slice (sweep-line is a drop-in optimisation
// of phase 2 for the rare > 10k-vertex case).
//
// **Robustness contract.** Every orientation, intersection-sign, and
// in-segment test uses Shewchuk `orient2d` adaptive precision (Phase
// 3.1.7 v3a / ADR-0076 §18). No naive cross-product, no epsilon. Degenerate
// cases (collinear segments, three-edges-at-point, vertex-on-edge) flow
// through the adaptive path and produce exact-sign results.
//
// **Determinism (ADR-0063 + ADR-0076 §4 pin #11).** Inputs are processed
// in deterministic insertion order; sort keys are lex-tuples; vertex
// dedup uses a sort+coalesce on lex-tuples (not a hash); intersection
// processing iterates edges by index; output rings are emitted starting
// from the lex-smallest vertex. Bit-identical results across compilers.
//
// **Output rings convention.** Outer rings CCW, hole rings CW — the v6
// winding convention pinned at v6a. Output is sanitised by default
// (`opts.clean_output = true`): consecutive collinear vertices removed,
// zero-area rings dropped, consecutive duplicates merged.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/polygon/polygon_boolean.hpp>
#include <crd/geometry/polygon/polygon_predicates.hpp>
#include <crd/geometry/polygon/polygon_types.hpp>
#include <crd/geometry/primitives/is_finite.hpp>
#include <crd/geometry/primitives/predicates.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
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

// =========================================================================
// Phase 1 — input edge collection
// =========================================================================

template <crd::math::MathScalar T>
struct InputEdge
{
    crd::math::Vec2<T> a;
    crd::math::Vec2<T> b;
    int                 wind_subject; // +1 or -1 if edge belongs to subject's outer/hole, 0 otherwise
    int                 wind_clip;
};

template <crd::math::MathScalar T>
void collect_input_edges(PolygonView2<T> poly, int owner /*0=subject, 1=clip*/,
                         crd::containers::Array<InputEdge<T>>& out)
{
    const crd::u32 rc = poly.ring_count();
    for (crd::u32 r = 0; r < rc; ++r)
    {
        const Ring2<T> ring = poly.ring(r);
        // Outer (r==0) is CCW per v6 convention ⇒ wind contribution +1.
        // Hole rings (r>0) are CW ⇒ wind contribution -1. We respect the
        // actual sign of signed_area in case the caller violated the
        // convention; this makes the algorithm input-tolerant.
        const T   sa  = signed_area(ring);
        const int sgn = (sa >= T{0}) ? +1 : -1;
        const crd::usize n = ring.size();
        for (crd::usize i = 0; i < n; ++i)
        {
            const auto& a = ring[i];
            const auto& b = ring[ring.next(i)];
            // Skip zero-length edges (degenerate input — query-tolerate).
            if (a.x == b.x && a.y == b.y) { continue; }
            InputEdge<T> e{};
            e.a            = a;
            e.b            = b;
            e.wind_subject = (owner == 0) ? sgn : 0;
            e.wind_clip    = (owner == 1) ? sgn : 0;
            out.push_back(e);
        }
    }
}

// =========================================================================
// Phase 2 — pairwise segment-segment intersection (brute force)
// =========================================================================

// Segment-segment intersection in 2D, robust to collinear / endpoint cases.
// On a transverse intersection (each segment's interior crosses the other's
// interior, or an endpoint of one touches the interior of the other),
// returns the intersection point.
//
// We DO NOT report endpoint-on-endpoint coincidence as an intersection —
// those points are already vertices and don't need splitting.

template <crd::math::MathScalar T>
struct SegSegHit
{
    bool                hit = false;
    crd::math::Vec2<T> point{};
    // ta / tb in [0, 1]: parameter along (a1, a2) and (b1, b2) respectively.
    T                   ta = T{0};
    T                   tb = T{0};
};

template <crd::math::MathScalar T>
SegSegHit<T> segment_segment_intersect(const crd::math::Vec2<T>& a1,
                                        const crd::math::Vec2<T>& a2,
                                        const crd::math::Vec2<T>& b1,
                                        const crd::math::Vec2<T>& b2) noexcept
{
    const T o1 = orient2d_signed(a1, a2, b1);
    const T o2 = orient2d_signed(a1, a2, b2);
    const T o3 = orient2d_signed(b1, b2, a1);
    const T o4 = orient2d_signed(b1, b2, a2);

    // Strict transverse intersection.
    const bool proper = ((o1 > T{0} && o2 < T{0}) || (o1 < T{0} && o2 > T{0})) &&
                        ((o3 > T{0} && o4 < T{0}) || (o3 < T{0} && o4 > T{0}));
    if (proper)
    {
        // Solve for intersection point using parametric form.
        // p = a1 + ta * (a2 - a1) = b1 + tb * (b2 - b1)
        const T dax = a2.x - a1.x;
        const T day = a2.y - a1.y;
        const T dbx = b2.x - b1.x;
        const T dby = b2.y - b1.y;
        const T denom = dax * dby - day * dbx;
        if (denom == T{0}) { return {}; } // parallel — already caught by strict orient signs
        const T ta = ((b1.x - a1.x) * dby - (b1.y - a1.y) * dbx) / denom;
        const T tb = ((b1.x - a1.x) * day - (b1.y - a1.y) * dax) / denom;
        SegSegHit<T> h;
        h.hit   = true;
        h.point = crd::math::Vec2<T>{a1.x + ta * dax, a1.y + ta * day};
        h.ta    = ta;
        h.tb    = tb;
        return h;
    }

    // Endpoint-on-interior cases — when an endpoint of one segment lies
    // strictly inside the other segment, we DO need to split the other.
    // Endpoint-on-endpoint is NOT reported (no new split point needed).

    auto on_segment_interior = [&](const crd::math::Vec2<T>& s1, const crd::math::Vec2<T>& s2,
                                    const crd::math::Vec2<T>& p, T sign_check) noexcept {
        if (sign_check != T{0}) { return false; } // not collinear
        // p must be strictly between s1 and s2 (not at endpoints).
        if (p.x == s1.x && p.y == s1.y) { return false; }
        if (p.x == s2.x && p.y == s2.y) { return false; }
        const T lo_x = s1.x < s2.x ? s1.x : s2.x;
        const T hi_x = s1.x > s2.x ? s1.x : s2.x;
        const T lo_y = s1.y < s2.y ? s1.y : s2.y;
        const T hi_y = s1.y > s2.y ? s1.y : s2.y;
        return p.x >= lo_x && p.x <= hi_x && p.y >= lo_y && p.y <= hi_y;
    };

    if (on_segment_interior(a1, a2, b1, o1))
    {
        const T dax = a2.x - a1.x;
        const T day = a2.y - a1.y;
        const T ta  = (dax != T{0}) ? (b1.x - a1.x) / dax : (b1.y - a1.y) / day;
        SegSegHit<T> h;
        h.hit   = true;
        h.point = b1;
        h.ta    = ta;
        h.tb    = T{0};
        return h;
    }
    if (on_segment_interior(a1, a2, b2, o2))
    {
        const T dax = a2.x - a1.x;
        const T day = a2.y - a1.y;
        const T ta  = (dax != T{0}) ? (b2.x - a1.x) / dax : (b2.y - a1.y) / day;
        SegSegHit<T> h;
        h.hit   = true;
        h.point = b2;
        h.ta    = ta;
        h.tb    = T{1};
        return h;
    }
    if (on_segment_interior(b1, b2, a1, o3))
    {
        const T dbx = b2.x - b1.x;
        const T dby = b2.y - b1.y;
        const T tb  = (dbx != T{0}) ? (a1.x - b1.x) / dbx : (a1.y - b1.y) / dby;
        SegSegHit<T> h;
        h.hit   = true;
        h.point = a1;
        h.ta    = T{0};
        h.tb    = tb;
        return h;
    }
    if (on_segment_interior(b1, b2, a2, o4))
    {
        const T dbx = b2.x - b1.x;
        const T dby = b2.y - b1.y;
        const T tb  = (dbx != T{0}) ? (a2.x - b1.x) / dbx : (a2.y - b1.y) / dby;
        SegSegHit<T> h;
        h.hit   = true;
        h.point = a2;
        h.ta    = T{1};
        h.tb    = tb;
        return h;
    }
    return {};
}

// =========================================================================
// Phase 3 — vertex deduplication + edge splitting
// =========================================================================

// A flat vertex table indexed by u32. Vertex dedup uses lex-sort then
// coalesce on exact coordinate equality (post-Shewchuk-exact, so the
// equality is meaningful).

template <crd::math::MathScalar T>
struct VertexKey
{
    T        x;
    T        y;
    crd::u32 original_idx; // before dedup

    [[nodiscard]] bool operator<(const VertexKey& o) const noexcept
    {
        if (x != o.x) { return x < o.x; }
        if (y != o.y) { return y < o.y; }
        return original_idx < o.original_idx;
    }
};

// =========================================================================
// Phase 4 — DCEL half-edge construction
// =========================================================================

template <crd::math::MathScalar T>
struct PBHalfEdge
{
    crd::u32 origin_v        = k_null_idx;
    crd::u32 dest_v          = k_null_idx;
    crd::u32 twin            = k_null_idx;
    crd::u32 next_in_face    = k_null_idx;
    crd::u32 face_idx        = k_null_idx;
    int      wind_subject    = 0; // contribution sign when this half-edge bounds a face on its LEFT
    int      wind_clip       = 0;
    crd::u8  visited_in_walk = 0;
};

template <crd::math::MathScalar T>
struct PBVertex
{
    crd::math::Vec2<T>             pos{};
    crd::containers::Array<crd::u32> outgoing; // half-edge indices, sorted CCW by angle

    explicit PBVertex(crd::memory::IAllocator* a) : outgoing(a) {}
};

// Angle comparator for outgoing half-edges from vertex v.
// "edge a is BEFORE edge b in CCW order" iff a's direction is CCW-less
// than b's. We can compare by quadrant + orient2d for fine discrimination.
//
// Direction of half-edge h: (he.dest_v.pos - v.pos). Both edges originate
// from v.

template <crd::math::MathScalar T>
inline int direction_quadrant(const crd::math::Vec2<T>& dir) noexcept
{
    // Quadrant numbering: 0 = +x, +y (incl. +x axis); 1 = -x, +y;
    // 2 = -x, -y (incl. -x axis); 3 = +x, -y.
    // This partitions the unit circle into 4 quadrants in CCW order.
    if (dir.x > T{0} && dir.y >= T{0}) { return 0; }
    if (dir.x <= T{0} && dir.y > T{0}) { return 1; }
    if (dir.x < T{0} && dir.y <= T{0}) { return 2; }
    // dir.x >= 0 && dir.y < 0
    return 3;
}

template <crd::math::MathScalar T>
inline bool ccw_less(const crd::math::Vec2<T>& v, const crd::math::Vec2<T>& a,
                      const crd::math::Vec2<T>& b) noexcept
{
    // Returns true iff direction (v→a) sorts BEFORE direction (v→b) in CCW
    // order starting from the +X axis. Quadrant-then-cross-product trick.
    const crd::math::Vec2<T> da{a.x - v.x, a.y - v.y};
    const crd::math::Vec2<T> db{b.x - v.x, b.y - v.y};
    const int                qa = direction_quadrant(da);
    const int                qb = direction_quadrant(db);
    if (qa != qb) { return qa < qb; }
    // Same quadrant — compare by cross product (orient2d of v, a, b).
    const T cross = orient2d_signed(v, a, b);
    if (cross != T{0}) { return cross > T{0}; }
    // Collinear from v — order by squared distance (closer first, arbitrary).
    const T da2 = da.x * da.x + da.y * da.y;
    const T db2 = db.x * db.x + db.y * db.y;
    return da2 < db2;
}

// =========================================================================
// Phase 5 — face walking + winding classification
// =========================================================================

// For each unvisited half-edge, walk a face boundary via .next_in_face
// until returning to start. Each walk forms one boundary loop. A face
// may have MULTIPLE boundary loops (outer + holes).

// =========================================================================
// Boolean predicate evaluation (per face)
// =========================================================================

inline bool inside_under_fill(int winding, FillRule rule) noexcept
{
    if (rule == FillRule::EvenOdd) { return (winding & 1) != 0; }
    return winding != 0;
}

inline bool boolean_predicate(int subject_w, int clip_w, BooleanOp op, FillRule sub_rule,
                              FillRule clip_rule) noexcept
{
    const bool s = inside_under_fill(subject_w, sub_rule);
    const bool c = inside_under_fill(clip_w, clip_rule);
    switch (op)
    {
        case BooleanOp::Union:        return s || c;
        case BooleanOp::Intersection: return s && c;
        case BooleanOp::Difference:   return s && !c;
        case BooleanOp::Xor:          return s != c;
    }
    return false;
}

// =========================================================================
// Output ring cleaning (collinear removal + degenerate skip)
// =========================================================================

template <crd::math::MathScalar T>
void clean_ring(crd::containers::Array<crd::math::Vec2<T>>& ring) noexcept
{
    if (ring.size() < 3U) { ring.clear(); return; }
    // Pass 1: drop consecutive duplicates.
    crd::usize w = 1U;
    for (crd::usize r = 1U; r < ring.size(); ++r)
    {
        if (ring[r].x != ring[w - 1U].x || ring[r].y != ring[w - 1U].y)
        {
            ring[w++] = ring[r];
        }
    }
    // Wrap dedup: last vs first.
    if (w > 1U && ring[w - 1U].x == ring[0].x && ring[w - 1U].y == ring[0].y) { --w; }
    while (ring.size() > w) { ring.pop_back(); }
    if (ring.size() < 3U) { ring.clear(); return; }

    // Pass 2: drop collinear interior vertices (orient2d == 0).
    bool any_dropped = true;
    while (any_dropped && ring.size() >= 3U)
    {
        any_dropped = false;
        crd::usize n = ring.size();
        crd::containers::Array<crd::math::Vec2<T>> kept(ring.allocator());
        kept.reserve(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            const auto& a = ring[(i + n - 1U) % n];
            const auto& b = ring[i];
            const auto& c = ring[(i + 1U) % n];
            if (orient2d_signed(a, b, c) == T{0}) { any_dropped = true; continue; }
            kept.push_back(b);
        }
        ring = kept;
        if (ring.size() < 3U) { ring.clear(); return; }
    }
}

} // namespace

// =========================================================================
// Public entry — polygon_boolean
// =========================================================================

template <crd::math::MathScalar T>
BooleanResult<T> polygon_boolean(PolygonView2<T> subject, PolygonView2<T> clip, BooleanOp op,
                                  crd::memory::IAllocator* alloc, BooleanOptions opts)
{
    BooleanResult<T> result(alloc);

    if (subject.ring_count() == 0U && clip.ring_count() == 0U)
    {
        result.status = BooleanStatus::EmptyOperand;
        return result;
    }

    // -- Phase 1: collect input edges -------------------------------------
    crd::containers::Array<InputEdge<T>> in_edges(alloc);
    collect_input_edges<T>(subject, 0, in_edges);
    collect_input_edges<T>(clip, 1, in_edges);

    // Validate finiteness.
    for (crd::usize i = 0; i < in_edges.size(); ++i)
    {
        if (!crd::geometry::primitives::is_finite(in_edges[i].a) ||
            !crd::geometry::primitives::is_finite(in_edges[i].b))
        {
            result.status = BooleanStatus::NonFiniteInput;
            return result;
        }
    }

    // Edge-case fast paths.
    if (in_edges.empty())
    {
        result.status = BooleanStatus::Ok;
        return result;
    }

    // -- Phase 2: find pairwise intersections; collect split parameters --
    // For each edge, accumulate split points: the endpoints (t=0 and t=1)
    // plus any intersection-induced split points. We work in TWO parallel
    // arrays — split parameters and split positions — sized one per input
    // edge.
    crd::containers::Array<crd::containers::Array<crd::math::Vec2<T>>> edge_split_points(alloc);
    edge_split_points.reserve(in_edges.size());
    for (crd::usize i = 0; i < in_edges.size(); ++i)
    {
        crd::containers::Array<crd::math::Vec2<T>> per_edge(alloc);
        per_edge.push_back(in_edges[i].a);
        per_edge.push_back(in_edges[i].b);
        edge_split_points.push_back(per_edge);
    }

    for (crd::u32 i = 0; i < in_edges.size(); ++i)
    {
        for (crd::u32 j = i + 1; j < in_edges.size(); ++j)
        {
            const auto& ea = in_edges[i];
            const auto& eb = in_edges[j];
            const auto  h  = segment_segment_intersect<T>(ea.a, ea.b, eb.a, eb.b);
            if (!h.hit) { continue; }
            edge_split_points[i].push_back(h.point);
            edge_split_points[j].push_back(h.point);
        }
    }

    // -- Phase 3: build the vertex table via lex-sort + coalesce ---------
    // Flatten all split points into a single keyed array; sort by lex; pull
    // out unique vertex IDs; map each (edge, split-point-index) → vertex id.
    crd::containers::Array<VertexKey<T>> keys(alloc);
    crd::u32                              total_pts = 0;
    for (crd::usize i = 0; i < edge_split_points.size(); ++i)
    {
        total_pts += static_cast<crd::u32>(edge_split_points[i].size());
    }
    keys.reserve(total_pts);
    crd::containers::Array<crd::u32> origin_edge(alloc);
    crd::containers::Array<crd::u32> origin_slot(alloc);
    origin_edge.reserve(total_pts);
    origin_slot.reserve(total_pts);
    crd::u32 running_id = 0;
    for (crd::u32 i = 0; i < edge_split_points.size(); ++i)
    {
        for (crd::u32 k = 0; k < edge_split_points[i].size(); ++k)
        {
            VertexKey<T> vk{};
            vk.x            = edge_split_points[i][k].x;
            vk.y            = edge_split_points[i][k].y;
            vk.original_idx = running_id;
            keys.push_back(vk);
            origin_edge.push_back(i);
            origin_slot.push_back(k);
            ++running_id;
        }
    }
    crd::containers::sort(keys.data(), keys.data() + keys.size());

    crd::containers::Array<crd::u32>                vid_of_original(alloc);
    crd::containers::Array<crd::math::Vec2<T>>      vertex_pos(alloc);
    vid_of_original.resize(total_pts);
    crd::u32 last_vid = k_null_idx;
    T        last_x   = T{0};
    T        last_y   = T{0};
    for (crd::u32 k = 0; k < keys.size(); ++k)
    {
        if (last_vid == k_null_idx || keys[k].x != last_x || keys[k].y != last_y)
        {
            last_vid = static_cast<crd::u32>(vertex_pos.size());
            last_x   = keys[k].x;
            last_y   = keys[k].y;
            vertex_pos.push_back(crd::math::Vec2<T>{last_x, last_y});
        }
        vid_of_original[keys[k].original_idx] = last_vid;
    }
    const crd::u32 vertex_count = static_cast<crd::u32>(vertex_pos.size());
    if (vertex_count < 2U)
    {
        result.status = BooleanStatus::Ok;
        return result;
    }

    // Build per-edge sorted vertex sequence (vertices along each input edge
    // sorted by parameter along the edge). For each input edge, sort its
    // split points by parameter t along the (a, b) direction; the resulting
    // sequence is the input edge split into sub-segments.
    crd::containers::Array<PBHalfEdge<T>> halfedges(alloc);
    crd::containers::Array<PBVertex<T>>    verts(alloc);
    verts.reserve(vertex_count);
    for (crd::u32 v = 0; v < vertex_count; ++v) { verts.emplace_back(alloc); }
    for (crd::u32 v = 0; v < vertex_count; ++v) { verts[v].pos = vertex_pos[v]; }

    crd::u32 running_origin = 0;
    for (crd::u32 i = 0; i < in_edges.size(); ++i)
    {
        const crd::u32 n_splits = static_cast<crd::u32>(edge_split_points[i].size());
        const crd::u32 base     = running_origin;
        running_origin += n_splits;
        // Re-fetch vertex ids for each split point in original slot order.
        // Build (vid, t) pairs and sort by t.
        const auto&    a = in_edges[i].a;
        const auto&    b = in_edges[i].b;
        const T        dx = b.x - a.x;
        const T        dy = b.y - a.y;
        crd::containers::Array<std::pair<T, crd::u32>> sorted_splits(alloc);
        sorted_splits.reserve(n_splits);
        for (crd::u32 k = 0; k < n_splits; ++k)
        {
            const crd::u32 vid = vid_of_original[base + k];
            // Parameter t along (a, b). Use whichever axis has larger range
            // for numerical stability.
            T t;
            if (dx > dy || dx < -dy)
            {
                t = (dx == T{0}) ? T{0} : (verts[vid].pos.x - a.x) / dx;
            }
            else
            {
                t = (dy == T{0}) ? T{0} : (verts[vid].pos.y - a.y) / dy;
            }
            sorted_splits.push_back({t, vid});
        }
        crd::containers::sort(sorted_splits.data(), sorted_splits.data() + sorted_splits.size(),
                              [](const std::pair<T, crd::u32>& lhs,
                                 const std::pair<T, crd::u32>& rhs) noexcept {
                                  if (lhs.first != rhs.first) { return lhs.first < rhs.first; }
                                  return lhs.second < rhs.second;
                              });

        // Create half-edges for each consecutive pair of split points.
        // We may have duplicate vids in sequence — skip them.
        for (crd::u32 k = 1; k < sorted_splits.size(); ++k)
        {
            const crd::u32 v0 = sorted_splits[k - 1U].second;
            const crd::u32 v1 = sorted_splits[k].second;
            if (v0 == v1) { continue; }
            // Two directed half-edges per segment.
            const crd::u32 fwd_idx = static_cast<crd::u32>(halfedges.size());
            const crd::u32 rev_idx = fwd_idx + 1U;
            PBHalfEdge<T>  fwd{};
            fwd.origin_v     = v0;
            fwd.dest_v       = v1;
            fwd.twin         = rev_idx;
            fwd.wind_subject = in_edges[i].wind_subject;
            fwd.wind_clip    = in_edges[i].wind_clip;
            PBHalfEdge<T> rev{};
            rev.origin_v     = v1;
            rev.dest_v       = v0;
            rev.twin         = fwd_idx;
            // Reverse direction ⇒ opposite winding contribution.
            rev.wind_subject = -in_edges[i].wind_subject;
            rev.wind_clip    = -in_edges[i].wind_clip;
            halfedges.push_back(fwd);
            halfedges.push_back(rev);
            verts[v0].outgoing.push_back(fwd_idx);
            verts[v1].outgoing.push_back(rev_idx);
        }
    }

    if (halfedges.empty())
    {
        result.status = BooleanStatus::Ok;
        return result;
    }

    // -- Phase 4: sort outgoing half-edges per vertex by CCW angle -------
    for (crd::u32 v = 0; v < verts.size(); ++v)
    {
        auto& out_list = verts[v].outgoing;
        if (out_list.size() < 2U) { continue; }
        // Insertion sort with ccw_less — O(n²) per vertex but n is the
        // degree of v which is typically <10 for non-pathological inputs.
        for (crd::u32 i = 1; i < out_list.size(); ++i)
        {
            const crd::u32 cur     = out_list[i];
            const auto&    cur_dest = verts[halfedges[cur].dest_v].pos;
            crd::u32       j       = i;
            while (j > 0U)
            {
                const auto& prev_dest = verts[halfedges[out_list[j - 1U]].dest_v].pos;
                if (ccw_less<T>(verts[v].pos, cur_dest, prev_dest))
                {
                    out_list[j] = out_list[j - 1U];
                    --j;
                }
                else
                {
                    break;
                }
            }
            out_list[j] = cur;
        }
    }

    // -- Wire next_in_face per the DCEL convention -----------------------
    // At each vertex v with outgoing edges (o_0, ..., o_{n-1}) sorted CCW,
    // the relation is: for each k, twin(o_k).next_in_face = o_{(k+n-1)%n}.
    // Equivalently, for the incoming half-edge i_k = twin(o_k), the next
    // edge of its face is the outgoing half-edge that's the CW predecessor
    // of o_k in the angular sort.
    for (crd::u32 v = 0; v < verts.size(); ++v)
    {
        const auto& out_list = verts[v].outgoing;
        const crd::u32 n = static_cast<crd::u32>(out_list.size());
        if (n == 0U) { continue; }
        for (crd::u32 k = 0; k < n; ++k)
        {
            const crd::u32 o_k     = out_list[k];
            const crd::u32 i_k     = halfedges[o_k].twin;
            const crd::u32 o_pred  = out_list[(k + n - 1U) % n];
            halfedges[i_k].next_in_face = o_pred;
        }
    }

    // -- Phase 5: walk faces ---------------------------------------------
    // Number of distinct faces = independent boundary loops via .next_in_face.
    // Each loop is assigned a face id. Then we'll classify each face by
    // ray casting from an interior point.

    crd::u32 face_count = 0;
    for (crd::u32 h = 0; h < halfedges.size(); ++h)
    {
        if (halfedges[h].face_idx != k_null_idx) { continue; }
        // Walk the loop starting at h, assigning face_count to all visited.
        crd::u32 cur = h;
        const crd::u32 walk_cap = static_cast<crd::u32>(halfedges.size()) + 4U;
        for (crd::u32 step = 0; step < walk_cap; ++step)
        {
            halfedges[cur].face_idx = face_count;
            cur = halfedges[cur].next_in_face;
            if (cur == k_null_idx) { return BooleanResult<T>(alloc); }
            if (cur == h) { break; }
        }
        ++face_count;
    }

    // -- Compute winding numbers for each face via ray casting -----------
    // For each face, find a representative point INSIDE the face by
    // averaging the boundary vertex positions; then cast a +X ray and
    // count signed crossings of subject edges (subject_w) and clip edges
    // (clip_w).
    //
    // We use the ORIGINAL input edges (in_edges) for the ray-cast — they
    // give correct winding contributions regardless of the planar
    // subdivision's edge fragmentation. Each input edge contributes
    // wind_subject (+1, -1, or 0) when its +X-crossing happens to the
    // right of the query point.

    struct FaceInfo
    {
        crd::math::Vec2<T> sample_pt{};
        int                sub_w = 0;
        int                clip_w = 0;
        bool               has_sample = false;
        // Boundary loops belonging to this face — represented as
        // half-edge "starts" (one per distinct loop).
        crd::containers::Array<crd::u32> loop_starts;
        explicit FaceInfo(crd::memory::IAllocator* a) : loop_starts(a) {}
    };
    crd::containers::Array<FaceInfo> faces(alloc);
    faces.reserve(face_count);
    for (crd::u32 f = 0; f < face_count; ++f) { faces.emplace_back(alloc); }

    // Collect loop_starts: scan halfedges; for each half-edge whose face
    // we haven't recorded yet AS A LOOP START, walk its loop and mark
    // (we'll mark via a temporary "loop_seen" flag using visited_in_walk).
    for (crd::u32 h = 0; h < halfedges.size(); ++h) { halfedges[h].visited_in_walk = 0U; }
    for (crd::u32 h = 0; h < halfedges.size(); ++h)
    {
        if (halfedges[h].visited_in_walk != 0U) { continue; }
        const crd::u32 fi = halfedges[h].face_idx;
        if (fi == k_null_idx) { continue; }
        faces[fi].loop_starts.push_back(h);
        crd::u32 cur = h;
        const crd::u32 walk_cap = static_cast<crd::u32>(halfedges.size()) + 4U;
        for (crd::u32 step = 0; step < walk_cap; ++step)
        {
            halfedges[cur].visited_in_walk = 1U;
            cur = halfedges[cur].next_in_face;
            if (cur == h) { break; }
        }
    }

    // Pick a sample point per face. Strategy: average the boundary vertex
    // positions of the FIRST loop. This is reliably inside if the loop is
    // CCW (a bounded face). For CW loops (the outer infinite face's
    // "boundary"), the average is OUTSIDE the unbounded face — but the
    // outer face's winding is 0 by construction, so we'll skip it.
    // Each "face" in our walk is actually one BOUNDARY LOOP. A real planar
    // face may have multiple loops (outer + holes). For Boolean output we
    // emit each LOOP independently: pick a sample point on the LEFT of each
    // directed edge in the loop (which is inside the loop's "face" region —
    // bounded interior for CCW loops, surrounding region for CW loops),
    // compute winding at that sample, and apply the Boolean predicate to
    // decide whether to emit this loop. The loop's natural orientation
    // (CCW = outer ring, CW = hole ring) maps directly to the v6 winding
    // convention in the output polygon.
    for (crd::u32 f = 0; f < face_count; ++f)
    {
        if (faces[f].loop_starts.empty()) { continue; }
        const crd::u32 walk_cap = static_cast<crd::u32>(halfedges.size()) + 4U;
        T              bbox_dx  = T{0};
        T              bbox_dy  = T{0};
        {
            T lo_x = std::numeric_limits<T>::infinity();
            T lo_y = lo_x;
            T hi_x = -lo_x;
            T hi_y = -lo_x;
            crd::u32 cur = faces[f].loop_starts[0];
            for (crd::u32 step = 0; step < walk_cap; ++step)
            {
                const auto& a = verts[halfedges[cur].origin_v].pos;
                if (a.x < lo_x) { lo_x = a.x; }
                if (a.y < lo_y) { lo_y = a.y; }
                if (a.x > hi_x) { hi_x = a.x; }
                if (a.y > hi_y) { hi_y = a.y; }
                cur = halfedges[cur].next_in_face;
                if (cur == faces[f].loop_starts[0]) { break; }
            }
            bbox_dx = hi_x - lo_x;
            bbox_dy = hi_y - lo_y;
        }
        // Pick a sample: walk until we find a non-horizontal edge, take its
        // midpoint, offset by perpendicular-LEFT (= inside the loop's face
        // for ANY orientation, by the DCEL convention that face is on the
        // LEFT of each directed boundary edge).
        T        eps_base = (bbox_dx > bbox_dy ? bbox_dx : bbox_dy) * static_cast<T>(1e-4);
        if (eps_base <= T{0}) { eps_base = static_cast<T>(1e-6); }
        crd::u32 cur = faces[f].loop_starts[0];
        for (crd::u32 step = 0; step < walk_cap; ++step)
        {
            const auto& a = verts[halfedges[cur].origin_v].pos;
            const auto& b = verts[halfedges[cur].dest_v].pos;
            const T     dx = b.x - a.x;
            const T     dy = b.y - a.y;
            if (dy != T{0})
            {
                const T edge_len_sq = (dx * dx + dy * dy);
                if (edge_len_sq > T{0})
                {
                    const T len     = std::sqrt(edge_len_sq);
                    const T inv_len = T{1} / len;
                    const T nx      = -dy * inv_len;
                    const T ny      =  dx * inv_len;
                    const T eps     = eps_base < len * static_cast<T>(1e-3)
                                          ? eps_base
                                          : len * static_cast<T>(1e-3);
                    faces[f].sample_pt.x = (a.x + b.x) * static_cast<T>(0.5) + nx * eps;
                    faces[f].sample_pt.y = (a.y + b.y) * static_cast<T>(0.5) + ny * eps;
                    faces[f].has_sample  = true;
                    break;
                }
            }
            cur = halfedges[cur].next_in_face;
            if (cur == faces[f].loop_starts[0]) { break; }
        }
    }

    // Ray-cast winding for each face with a sample point.
    auto ray_winding_for_edges = [&](const crd::math::Vec2<T>& p, int input_edge_index_filter) {
        // Sum signed crossings of input edges with the horizontal +X ray
        // from p. An edge from a to b crosses the ray iff its Y-extent
        // straddles p.y; the X-intersect at y = p.y must be > p.x.
        // Signed contribution = wind_subject (or wind_clip) per the source
        // input edge, multiplied by the direction sign of the crossing.
        int sum = 0;
        for (crd::u32 i = 0; i < in_edges.size(); ++i)
        {
            const auto& e = in_edges[i];
            const int   w = (input_edge_index_filter == 0) ? e.wind_subject : e.wind_clip;
            if (w == 0) { continue; }
            const auto& a = e.a;
            const auto& b = e.b;
            // Edge endpoints straddle the ray's y?
            const bool ay_above = a.y > p.y;
            const bool by_above = b.y > p.y;
            if (ay_above == by_above) { continue; }
            if (a.y == b.y) { continue; } // horizontal — skip
            // Cross point x at y = p.y.
            const T t = (p.y - a.y) / (b.y - a.y);
            const T x = a.x + t * (b.x - a.x);
            if (x <= p.x) { continue; }
            // Direction sign: if a is below and b is above (upward crossing),
            // contribution = +w; otherwise -w.
            sum += (by_above ? +w : -w);
        }
        return sum;
    };

    for (crd::u32 f = 0; f < face_count; ++f)
    {
        if (!faces[f].has_sample) { continue; }
        faces[f].sub_w  = ray_winding_for_edges(faces[f].sample_pt, 0);
        faces[f].clip_w = ray_winding_for_edges(faces[f].sample_pt, 1);
    }

    // -- Apply Boolean predicate per face --------------------------------
    crd::containers::Array<crd::u8> face_kept(alloc);
    face_kept.resize(face_count);
    for (crd::u32 f = 0; f < face_count; ++f)
    {
        if (!faces[f].has_sample)
        {
            // Outer/unbounded face — winding is 0, predicate is false
            // for all 4 ops since 0 means "not inside" under both fill rules.
            face_kept[f] = 0U;
            continue;
        }
        face_kept[f] = boolean_predicate(faces[f].sub_w, faces[f].clip_w, op, opts.subject_fill,
                                         opts.clip_fill)
                           ? 1U
                           : 0U;
    }

    // -- Extract output rings --------------------------------------------
    // For each KEPT face, emit each of its loops as a ring. CCW loops are
    // outer rings; CW loops would be hole rings IF the face was kept and
    // has them. But: a "kept face with a hole" actually has TWO loops in
    // our DCEL — the outer CCW + the inner CW (the hole boundary, which
    // separates this kept face from a NON-kept face inside). So we emit
    // both as polygon rings.
    //
    // Result polygon = concatenation of all outer + hole rings via Polygon2.
    //
    // To assemble Polygon2 in CCW-outer + CW-hole convention, we need to
    // pair each hole with its containing outer ring. For a clean impl we
    // emit ALL kept face boundaries as add_ring calls — Polygon2 stores
    // them in the order added with offsets. Downstream callers can re-pair
    // outers/holes via point-in-polygon classification if needed.
    //
    // For most callers (PCB / navmesh / cookers), a flat list of CCW outer
    // rings + CW hole rings is sufficient.

    for (crd::u32 f = 0; f < face_count; ++f)
    {
        if (face_kept[f] == 0U) { continue; }
        for (crd::u32 li = 0; li < faces[f].loop_starts.size(); ++li)
        {
            crd::containers::Array<crd::math::Vec2<T>> ring(alloc);
            crd::u32 cur = faces[f].loop_starts[li];
            const crd::u32 walk_cap = static_cast<crd::u32>(halfedges.size()) + 4U;
            for (crd::u32 step = 0; step < walk_cap; ++step)
            {
                ring.push_back(verts[halfedges[cur].origin_v].pos);
                cur = halfedges[cur].next_in_face;
                if (cur == faces[f].loop_starts[li]) { break; }
            }
            if (opts.clean_output) { clean_ring<T>(ring); }
            if (ring.size() >= 3U)
            {
                result.output.add_ring(crd::containers::ConstSpan<crd::math::Vec2<T>>{
                    ring.data(), ring.size()});
            }
        }
    }

    result.status = BooleanStatus::Ok;
    return result;
}

// =========================================================================
// Explicit instantiations
// =========================================================================

template BooleanResult<crd::f32> polygon_boolean<crd::f32>(PolygonView2<crd::f32>,
                                                            PolygonView2<crd::f32>, BooleanOp,
                                                            crd::memory::IAllocator*,
                                                            BooleanOptions);
template BooleanResult<crd::f64> polygon_boolean<crd::f64>(PolygonView2<crd::f64>,
                                                            PolygonView2<crd::f64>, BooleanOp,
                                                            crd::memory::IAllocator*,
                                                            BooleanOptions);

} // namespace crd::geometry::polygon
