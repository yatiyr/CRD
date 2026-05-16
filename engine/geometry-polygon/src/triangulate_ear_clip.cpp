// ---------------------------------------------------------------------------
// crd-geometry-polygon — v6b ear-clipping triangulation w/ hole support.
//
// Classical ear-clipping (Meisters 1975) + Eberly 1999 cut-and-join hole
// bridging + Shewchuk adaptive predicates throughout. See header banner of
// `triangulate_ear_clip.hpp` for the contract.
//
// Implementation notes:
//
//   * Doubly-linked vertex list backed by twin `Array<u32>` `next` / `prev`
//     index buffers (no `std::list` — no STL containers in hot paths per
//     PRINCIPLES.md). One sentinel `kNullIdx = UINT32_MAX` marks "removed".
//
//   * Reflex / convex classification stored in a parallel `Array<u8>`. A
//     vertex is *convex* iff `orient2d(prev, v, next) > 0` for a CCW outer
//     ring. The Shewchuk-exact sign means collinear vertices (orient2d == 0)
//     are treated as REFLEX — they're never ears and never participate in
//     the "any reflex inside this triangle" test as interior points.
//
//   * Ear pick: scan candidate-ear set (the convex vertex indices), pick the
//     smallest current-index. Lex-tuple tiebreak by vertex index gives
//     byte-identical output across compilers / SIMD widths / OSes.
//
//   * Hole bridging (Eberly): for each hole, find its RIGHTMOST vertex `M`
//     (max x; tie max y; tie min index). From `M`, cast a +X ray; find the
//     outer edge with the smallest `t > 0` intersection. The bridge endpoint
//     `V` on the outer is either (a) the visible endpoint of that edge, or
//     (b) a reflex vertex of the outer ring that's inside the triangle
//     `(M, intersection_point, edge_endpoint)`. We insert the bridge as
//     `... V_outer ... M_hole, hole_verts..., M_hole, V_outer ...` —
//     the bridge edge `(M, V)` appears twice in opposite directions, so
//     ear-clipping triangulates the combined polygon while keeping it
//     topologically simple.
//
//   * Holes are bridged one at a time, in order of DESCENDING max-x — this
//     guarantees that each hole's bridge target is the outer (or a previously-
//     bridged hole) and not a hole-to-be-bridged-later. Standard Eberly
//     ordering pin.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/polygon/polygon_predicates.hpp>
#include <crd/geometry/polygon/polygon_types.hpp>
#include <crd/geometry/polygon/triangulate_ear_clip.hpp>
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

// ---- Internal: ear-clip a flat vertex sequence into triangles ---------
//
// Operates on a sequence of `Vec2<T>` positions + a parallel sequence of
// "original indices" (so the output triangle_indices refer back to the
// caller's vertex numbering, not the bridged sequence's local indices).
// For the no-holes path, `original_index[i] = i`. For the bridged path,
// duplicated-bridge vertices appear with the same `original_index` twice.

template <crd::math::MathScalar T>
inline T orient2d_signed(const crd::math::Vec2<T>& a, const crd::math::Vec2<T>& b,
                         const crd::math::Vec2<T>& c) noexcept
{
    return crd::geometry::primitives::orient2d(a, b, c);
}

template <crd::math::MathScalar T>
inline bool point_in_triangle_2d(const crd::math::Vec2<T>& a, const crd::math::Vec2<T>& b,
                                  const crd::math::Vec2<T>& c, const crd::math::Vec2<T>& p) noexcept
{
    // Triangle (a, b, c) is CCW (signed_area > 0). p is INSIDE (strict) iff
    // all three orient2d signs are positive. Boundary (orient2d == 0) is
    // treated as outside — ear-validity must permit a reflex vertex sitting
    // exactly on the candidate triangle's edge (the ear is still legal).
    const T o1 = orient2d_signed(a, b, p);
    const T o2 = orient2d_signed(b, c, p);
    const T o3 = orient2d_signed(c, a, p);
    return o1 > T{0} && o2 > T{0} && o3 > T{0};
}

// Is vertex `i` a convex corner in the current linked-list?
template <crd::math::MathScalar T>
inline bool is_convex(crd::containers::ConstSpan<crd::math::Vec2<T>> verts,
                      const crd::containers::Array<crd::u32>& nxt,
                      const crd::containers::Array<crd::u32>& prv, crd::u32 i) noexcept
{
    const auto& a = verts[prv[i]];
    const auto& b = verts[i];
    const auto& c = verts[nxt[i]];
    return orient2d_signed(a, b, c) > T{0};
}

// Is `(prev[i], i, next[i])` an ear — i.e. CCW + no reflex vertex inside?
template <crd::math::MathScalar T>
inline bool is_ear(crd::containers::ConstSpan<crd::math::Vec2<T>> verts,
                   const crd::containers::Array<crd::u32>&         nxt,
                   const crd::containers::Array<crd::u32>&         prv,
                   const crd::containers::Array<crd::u8>&          is_reflex,
                   crd::u32                                         head,
                   crd::u32                                         i) noexcept
{
    if (!is_convex(verts, nxt, prv, i)) { return false; }
    const auto& a = verts[prv[i]];
    const auto& b = verts[i];
    const auto& c = verts[nxt[i]];
    // Walk the live linked-list, test every reflex vertex other than the
    // ear's own corners against triangle (a, b, c). The classical O(n²)
    // bound — FIST O(n log n) is a future optimization.
    crd::u32 j = head;
    do
    {
        if (j != i && j != prv[i] && j != nxt[i] && is_reflex[j])
        {
            if (point_in_triangle_2d(a, b, c, verts[j])) { return false; }
        }
        j = nxt[j];
    } while (j != head);
    return true;
}

// Classical ear-clip kernel. Returns true on success; fills `out`.
template <crd::math::MathScalar T>
bool ear_clip_kernel(crd::containers::ConstSpan<crd::math::Vec2<T>> verts,
                     crd::containers::ConstSpan<crd::u32>            orig_idx,
                     crd::containers::Array<crd::u32>&               out_triangles,
                     crd::memory::IAllocator*                        alloc)
{
    const crd::u32 n = static_cast<crd::u32>(verts.size());
    if (n < 3U) { return false; }

    crd::containers::Array<crd::u32> nxt(alloc);
    crd::containers::Array<crd::u32> prv(alloc);
    crd::containers::Array<crd::u8>  reflex(alloc);
    nxt.resize(n);
    prv.resize(n);
    reflex.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        nxt[i]    = (i + 1U) % n;
        prv[i]    = (i + n - 1U) % n;
        reflex[i] = 0U;
    }
    // Initial reflex classification.
    for (crd::u32 i = 0; i < n; ++i)
    {
        reflex[i] = is_convex(verts, nxt, prv, i) ? 0U : 1U;
    }

    crd::u32 head  = 0U;
    crd::u32 alive = n;
    // Bounded outer loop — n iterations max guarantees termination even on
    // pathological inputs (we exit with `false` if no ear found in a pass).
    const crd::u32 max_iters = n; // safety bound — degenerate input ⇒ early exit
    for (crd::u32 iter = 0; iter < max_iters && alive > 3U; ++iter)
    {
        // Find the smallest live vertex that's an ear (lex-tuple tiebreak by
        // vertex index — engine-wide determinism pin).
        crd::u32 ear_i = k_null_idx;
        crd::u32 j     = head;
        do
        {
            if (!reflex[j] && is_ear(verts, nxt, prv, reflex, head, j))
            {
                if (ear_i == k_null_idx || j < ear_i) { ear_i = j; }
            }
            j = nxt[j];
        } while (j != head);

        if (ear_i == k_null_idx)
        {
            // No ear found this pass — input is non-simple or numerical
            // degeneracy. Surface as failure (caller wraps in status).
            return false;
        }

        // Emit triangle (prev_ear, ear, next_ear) → indices into ORIGINAL
        // polygon vertex numbering.
        out_triangles.push_back(orig_idx[prv[ear_i]]);
        out_triangles.push_back(orig_idx[ear_i]);
        out_triangles.push_back(orig_idx[nxt[ear_i]]);

        // Splice the ear out of the linked list.
        const crd::u32 p = prv[ear_i];
        const crd::u32 q = nxt[ear_i];
        nxt[p]           = q;
        prv[q]           = p;
        --alive;
        if (head == ear_i) { head = q; }

        // Reclassify the two neighbours — their convex/reflex status may flip.
        if (alive >= 3U)
        {
            reflex[p] = is_convex(verts, nxt, prv, p) ? 0U : 1U;
            reflex[q] = is_convex(verts, nxt, prv, q) ? 0U : 1U;
        }
    }

    if (alive != 3U) { return false; }

    // Final triangle from the three remaining vertices.
    const crd::u32 a = head;
    const crd::u32 b = nxt[a];
    const crd::u32 c = nxt[b];
    out_triangles.push_back(orig_idx[a]);
    out_triangles.push_back(orig_idx[b]);
    out_triangles.push_back(orig_idx[c]);
    return true;
}

// ---- Eberly cut-and-join hole bridging --------------------------------

// Finds the rightmost vertex of a ring; ties broken by max-y, then min-idx.
template <crd::math::MathScalar T>
crd::u32 rightmost_vertex_index(Ring2<T> r) noexcept
{
    crd::u32 best = 0U;
    for (crd::u32 i = 1; i < static_cast<crd::u32>(r.size()); ++i)
    {
        const auto& bv = r[best];
        const auto& cv = r[i];
        if (cv.x > bv.x) { best = i; continue; }
        if (cv.x == bv.x)
        {
            if (cv.y > bv.y) { best = i; continue; }
            if (cv.y == bv.y && i < best) { best = i; }
        }
    }
    return best;
}

template <crd::math::MathScalar T>
struct BridgeFinding
{
    crd::u32 target_local_idx = k_null_idx; // index INTO the bridged vertex sequence so far
    bool     ok               = false;
};

// Find the outer-ring vertex visible from `M` (the hole's rightmost vertex).
// Operates on the partially-bridged sequence (`bridged_verts`) where the
// outer + any-already-bridged-holes have been concatenated.
template <crd::math::MathScalar T>
BridgeFinding<T>
find_bridge_target(crd::containers::ConstSpan<crd::math::Vec2<T>> bridged,
                   const crd::math::Vec2<T>&                       hole_m,
                   crd::memory::IAllocator* /*alloc*/)
{
    // Step 1: cast +X ray from hole_m. For each edge (bridged[i], bridged[next_i]),
    // compute the x-intersect at y = hole_m.y. We need the SMALLEST positive
    // t-intersect (strictly to the right of hole_m).
    const crd::u32 n = static_cast<crd::u32>(bridged.size());
    if (n < 3U) { return {}; }

    T        best_dx          = std::numeric_limits<T>::infinity();
    crd::u32 hit_edge_lo      = k_null_idx; // edge endpoint with smaller y on hit edge
    crd::u32 hit_edge_hi      = k_null_idx; // edge endpoint with larger y
    crd::math::Vec2<T> hit_pt = {T{0}, T{0}};

    for (crd::u32 i = 0; i < n; ++i)
    {
        const crd::u32 j  = (i + 1U) % n;
        const auto&    a  = bridged[i];
        const auto&    b  = bridged[j];
        // Edge straddles or touches y = hole_m.y?
        const bool a_above = a.y >= hole_m.y;
        const bool b_above = b.y >= hole_m.y;
        if (a_above == b_above && !(a.y == hole_m.y || b.y == hole_m.y)) { continue; }
        // Compute x at y = hole_m.y. Skip horizontal edges (same-y endpoints)
        // — they don't yield a unique intersection.
        if (a.y == b.y) { continue; }
        const T t = (hole_m.y - a.y) / (b.y - a.y);
        if (t < T{0} || t > T{1}) { continue; }
        const T x = a.x + t * (b.x - a.x);
        const T dx = x - hole_m.x;
        if (dx <= T{0}) { continue; } // intersection is at or left of M
        if (dx < best_dx)
        {
            best_dx     = dx;
            hit_edge_lo = (a.y < b.y) ? i : j;
            hit_edge_hi = (a.y < b.y) ? j : i;
            hit_pt      = crd::math::Vec2<T>{x, hole_m.y};
        }
    }

    if (hit_edge_lo == k_null_idx) { return {}; } // no visible outer edge

    // Step 2: Eberly's candidate is the edge endpoint with the LARGER x.
    crd::u32 candidate
        = (bridged[hit_edge_lo].x > bridged[hit_edge_hi].x) ? hit_edge_lo : hit_edge_hi;

    // Step 3: walk the bridged ring; any REFLEX vertex inside the triangle
    // (M, hit_pt, bridged[candidate]) becomes a stronger candidate. Pick the
    // one whose angle to (hit_pt - M) is smallest (closest to the +X ray
    // direction). Lex tiebreak by smallest index for determinism.
    const auto& cand_v = bridged[candidate];
    // Build the search triangle in CCW order: M -> hit_pt -> cand_v.
    // (M and hit_pt are both at the same y = hole_m.y; cand_v has y != hole_m.y
    // since horizontal edges were skipped.)
    crd::math::Vec2<T> tA = hole_m;
    crd::math::Vec2<T> tB = hit_pt;
    crd::math::Vec2<T> tC = cand_v;
    // Force CCW orientation — if (tA, tB, tC) is CW, swap to (tA, tC, tB).
    if (orient2d_signed(tA, tB, tC) < T{0})
    {
        const auto tmp = tB;
        tB             = tC;
        tC             = tmp;
    }
    crd::u32 best_idx = candidate;
    // To pick "closest angle to +X ray", use `|(v - M).y| / (v - M).x` — the
    // smaller, the closer to horizontal. Equivalent to: minimize tan(theta).
    T best_ratio = std::numeric_limits<T>::infinity();
    {
        const T dx0 = cand_v.x - hole_m.x;
        if (dx0 > T{0})
        {
            const T dy0 = cand_v.y - hole_m.y;
            best_ratio  = (dy0 < T{0} ? -dy0 : dy0) / dx0;
        }
    }
    for (crd::u32 k = 0; k < n; ++k)
    {
        if (k == candidate) { continue; }
        // Re-classify reflex on the fly: a vertex k is REFLEX iff
        // orient2d(prev_k, k, next_k) <= 0 for a CCW ring (assumed for the
        // bridged sequence's outer-first build order).
        const crd::u32 kp     = (k + n - 1U) % n;
        const crd::u32 kn     = (k + 1U) % n;
        const T        sign_k = orient2d_signed(bridged[kp], bridged[k], bridged[kn]);
        if (sign_k > T{0}) { continue; } // convex — skip
        // Reflex — is it inside the search triangle?
        if (!point_in_triangle_2d(tA, tB, tC, bridged[k])) { continue; }
        const T dx = bridged[k].x - hole_m.x;
        if (dx <= T{0}) { continue; }
        const T dy    = bridged[k].y - hole_m.y;
        const T ratio = (dy < T{0} ? -dy : dy) / dx;
        if (ratio < best_ratio || (ratio == best_ratio && k < best_idx))
        {
            best_ratio = ratio;
            best_idx   = k;
        }
    }

    BridgeFinding<T> out{};
    out.target_local_idx = best_idx;
    out.ok               = true;
    return out;
}

} // namespace

// ---- Public entry: simple ring (no holes) --------------------------------

template <crd::math::MathScalar T>
TriangulationResult<T> triangulate_ear_clip(Ring2<T> ring, crd::memory::IAllocator* alloc,
                                            TriangulateOptions /*opts*/)
{
    TriangulationResult<T> result(alloc);
    if (ring.size() < 3U)
    {
        result.status = TriangulateStatus::EmptyPolygon;
        return result;
    }
    // Builder-side finite-input contract (debug-asserted in Polygon2::add_ring)
    // is re-checked at the boundary for raw-Ring2 callers that skip Polygon2.
    for (crd::usize i = 0; i < ring.size(); ++i)
    {
        if (!crd::geometry::primitives::is_finite(ring[i]))
        {
            result.status = TriangulateStatus::NonFiniteInput;
            return result;
        }
    }
    if (!is_simple(ring))
    {
        result.status = TriangulateStatus::NonSimpleOuter;
        return result;
    }

    // For a simple ring, the original-index map is the identity.
    crd::containers::Array<crd::u32> orig(alloc);
    orig.resize(ring.size());
    for (crd::u32 i = 0; i < static_cast<crd::u32>(ring.size()); ++i) { orig[i] = i; }

    if (!ear_clip_kernel<T>(ring.vertices, crd::containers::ConstSpan<crd::u32>{orig.data(), orig.size()},
                            result.triangle_indices, alloc))
    {
        result.status = TriangulateStatus::NonSimpleOuter;
        result.triangle_indices.clear();
        return result;
    }

    result.triangle_count = static_cast<crd::u32>(result.triangle_indices.size() / 3U);
    result.status         = TriangulateStatus::Ok;
    return result;
}

// ---- Public entry: polygon-with-holes ------------------------------------

template <crd::math::MathScalar T>
TriangulationResult<T> triangulate_ear_clip(PolygonView2<T> poly, crd::memory::IAllocator* alloc,
                                            TriangulateOptions opts)
{
    TriangulationResult<T> result(alloc);
    if (poly.ring_count() == 0U || poly.outer().size() < 3U)
    {
        result.status = TriangulateStatus::EmptyPolygon;
        return result;
    }

    // Validate every ring before bridging — adverse input gets a clean
    // diagnostic, not a corrupt output.
    if (!is_simple(poly.outer()))
    {
        result.status = TriangulateStatus::NonSimpleOuter;
        return result;
    }
    for (crd::u32 h = 1; h < poly.ring_count(); ++h)
    {
        if (!is_simple(poly.ring(h)))
        {
            result.status = TriangulateStatus::NonSimpleHole;
            return result;
        }
    }
    for (crd::u32 r = 0; r < poly.ring_count(); ++r)
    {
        const auto ring = poly.ring(r);
        for (crd::usize i = 0; i < ring.size(); ++i)
        {
            if (!crd::geometry::primitives::is_finite(ring[i]))
            {
                result.status = TriangulateStatus::NonFiniteInput;
                return result;
            }
        }
    }

    // Fast path: no holes — delegate to the simple-ring entry.
    if (poly.hole_count() == 0U)
    {
        return triangulate_ear_clip(poly.outer(), alloc, opts);
    }

    // Build the bridged vertex sequence + an origin-map. Holes are bridged
    // in DESCENDING max-x order (Eberly ordering pin: each bridge target
    // lies on the outer + already-bridged holes, never on a not-yet-bridged
    // hole).
    const crd::u32 hole_count = poly.hole_count();

    // Compute each hole's rightmost vertex + that vertex's x — for ordering
    // + the bridge target finder.
    crd::containers::Array<crd::u32> hole_order(alloc);
    hole_order.resize(hole_count);
    for (crd::u32 h = 0; h < hole_count; ++h) { hole_order[h] = h + 1U; } // skip outer (ring 0)
    // Selection-sort descending by rightmost-x. O(H²); H is small (typically
    // ≤ 8 for fonts, ≤ tens for navmeshes), so this is fine + deterministic.
    for (crd::u32 i = 0; i < hole_count; ++i)
    {
        crd::u32 best   = i;
        const auto ring_i = poly.ring(hole_order[i]);
        T          best_x = ring_i[rightmost_vertex_index(ring_i)].x;
        for (crd::u32 j = i + 1U; j < hole_count; ++j)
        {
            const auto ring_j = poly.ring(hole_order[j]);
            const T    x_j    = ring_j[rightmost_vertex_index(ring_j)].x;
            if (x_j > best_x || (x_j == best_x && hole_order[j] < hole_order[best]))
            {
                best   = j;
                best_x = x_j;
            }
        }
        if (best != i)
        {
            const crd::u32 tmp = hole_order[i];
            hole_order[i]      = hole_order[best];
            hole_order[best]   = tmp;
        }
    }

    // Bridged vertex sequence — starts as a copy of the outer, then each
    // hole's vertices are spliced in via a doubled-bridge insertion.
    crd::containers::Array<crd::math::Vec2<T>> bridged(alloc);
    crd::containers::Array<crd::u32>            orig(alloc);
    const auto outer = poly.outer();
    bridged.reserve(poly.vertices.size() + 2U * hole_count);
    orig.reserve(poly.vertices.size() + 2U * hole_count);
    for (crd::u32 i = 0; i < static_cast<crd::u32>(outer.size()); ++i)
    {
        bridged.push_back(outer[i]);
        orig.push_back(i); // outer maps to indices [0 .. outer.size())
    }

    for (crd::u32 idx = 0; idx < hole_count; ++idx)
    {
        const crd::u32 ring_idx = hole_order[idx];
        const auto     hring    = poly.ring(ring_idx);
        const crd::u32 m_local  = rightmost_vertex_index(hring);
        const auto     M        = hring[m_local];

        // Map from poly-vertex space to the bridged sequence index for
        // origin tracking. poly's flat vertex offset for `ring_idx`.
        const crd::u32 hole_global_base = poly.ring_offsets[ring_idx];

        const auto bf = find_bridge_target<T>(
            crd::containers::ConstSpan<crd::math::Vec2<T>>{bridged.data(), bridged.size()}, M,
            alloc);
        if (!bf.ok)
        {
            result.status = TriangulateStatus::HoleBridgingFailed;
            return result;
        }
        const crd::u32 v_idx = bf.target_local_idx;

        // Splice: insert AFTER position `v_idx` in `bridged`:
        //   [..., V, M, hole_verts (M, M+1, ..., M-1), M, V, V+1, ...]
        // The doubled (V, M) edges are coincident-opposite — ear-clipping
        // treats them as zero-width slivers that get eaten naturally.
        //
        // Build the inserted block:
        //   index 0: M       (origin = hole_global_base + m_local)
        //   index 1..h:      hole verts starting after M, wrapping
        //   index h+1: M     (origin = hole_global_base + m_local)
        //   index h+2: V     (origin = orig[v_idx], i.e. the original poly idx of V)
        const crd::u32 h_size = static_cast<crd::u32>(hring.size());
        const crd::u32 insert_count = h_size + 2U; // M, hole-without-M (rotated), M, V_dup

        // We need to shift bridged + orig contents from position (v_idx + 1) by
        // `insert_count` to make room. Direct in-place would be O(n) per
        // insert which is fine for typical glyph sizes.
        const crd::usize old_size = bridged.size();
        bridged.resize(old_size + insert_count);
        orig.resize(old_size + insert_count);
        for (crd::usize i = old_size; i > static_cast<crd::usize>(v_idx + 1U); --i)
        {
            const crd::usize src = i - 1U;
            const crd::usize dst = src + insert_count;
            bridged[dst]         = bridged[src];
            orig[dst]            = orig[src];
        }
        // Write the inserted block.
        const crd::usize ins_base = static_cast<crd::usize>(v_idx + 1U);
        bridged[ins_base + 0U]    = M;
        orig[ins_base + 0U]       = hole_global_base + m_local;
        // Hole vertices rotated so M is the first; walk forward from M
        // around the hole (m_local, m_local+1, ..., m_local-1) — skip M
        // since it's already placed.
        for (crd::u32 k = 1; k < h_size; ++k)
        {
            const crd::u32 src_idx = (m_local + k) % h_size;
            bridged[ins_base + k]  = hring[src_idx];
            orig[ins_base + k]     = hole_global_base + src_idx;
        }
        bridged[ins_base + h_size]      = M;
        orig[ins_base + h_size]         = hole_global_base + m_local;
        bridged[ins_base + h_size + 1U] = bridged[v_idx];
        orig[ins_base + h_size + 1U]    = orig[v_idx];
    }

    if (!ear_clip_kernel<T>(
            crd::containers::ConstSpan<crd::math::Vec2<T>>{bridged.data(), bridged.size()},
            crd::containers::ConstSpan<crd::u32>{orig.data(), orig.size()}, result.triangle_indices,
            alloc))
    {
        result.status = TriangulateStatus::BridgedSelfIntersect;
        result.triangle_indices.clear();
        return result;
    }

    result.triangle_count = static_cast<crd::u32>(result.triangle_indices.size() / 3U);
    result.status         = TriangulateStatus::Ok;
    return result;
}

// ---- Explicit instantiations ---------------------------------------------

template TriangulationResult<crd::f32> triangulate_ear_clip<crd::f32>(Ring2<crd::f32>,
                                                                       crd::memory::IAllocator*,
                                                                       TriangulateOptions);
template TriangulationResult<crd::f64> triangulate_ear_clip<crd::f64>(Ring2<crd::f64>,
                                                                       crd::memory::IAllocator*,
                                                                       TriangulateOptions);
template TriangulationResult<crd::f32> triangulate_ear_clip<crd::f32>(PolygonView2<crd::f32>,
                                                                       crd::memory::IAllocator*,
                                                                       TriangulateOptions);
template TriangulationResult<crd::f64> triangulate_ear_clip<crd::f64>(PolygonView2<crd::f64>,
                                                                       crd::memory::IAllocator*,
                                                                       TriangulateOptions);

} // namespace crd::geometry::polygon
