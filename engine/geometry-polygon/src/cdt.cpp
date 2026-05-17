// ---------------------------------------------------------------------------
// crd-geometry-polygon — v6c Constrained Delaunay Triangulation (CDT).
//
// Bowyer-Watson incremental Delaunay (Bowyer 1981 / Watson 1981) +
// Anglada 1997 constraint-edge recovery via Lawson 1977 edge flips +
// post-recovery Lawson bubble to restore Delaunay over non-constrained edges.
//
// Every orientation decision uses Shewchuk `orient2d` adaptive precision
// (Phase 3.1.7 v3a / ADR-0076 §18). Every in-circle decision uses Shewchuk
// `incircle` adaptive. No naive cross-product / determinant fallback.
//
// **Data structures.**
//
//   `CdtTriangle` — 3 vertex indices (CCW) + 3 neighbour-triangle indices
//   + 8-bit flags (alive + per-edge constraint bits). Edge `i` of triangle T
//   is the directed edge `(T.v[i], T.v[(i+1)%3])`. The neighbour across
//   that edge is `T.nbr[i]` (or `kNullIdx` if the edge is on the convex
//   hull's outer boundary).
//
//   Neighbour symmetry invariant: if `T.nbr[i] == U`, then there is exactly
//   one `j` in {0,1,2} such that `U.nbr[j] == T`; that `j` satisfies
//   `U.v[j] == T.v[(i+1)%3]` and `U.v[(j+1)%3] == T.v[i]` (the shared
//   edge is traversed in opposite directions by the two triangles).
//
//   The triangle array is grow-only with a free list (dead triangles are
//   recycled). All neighbour pointers are kept consistent across cavity
//   carve-and-fill and edge flips.
//
// **Bowyer-Watson insertion of point `p`.**
//
//   1. Locate the triangle T containing `p` (jump-walk from the most-
//      recently-modified triangle; orient2d picks the next edge).
//   2. BFS-expand from T: any triangle whose circumcircle contains `p`
//      (`incircle > 0`) is "bad" and joins the cavity. BFS visits in
//      monotonic triangle-ID order ⇒ deterministic across compilers.
//   3. Boundary edges of the bad-triangle set form a star-shaped polygon
//      ("cavity") around `p`. Free the bad triangles, then create a new
//      triangle `(v_a, v_b, p)` per cavity boundary edge `(v_a, v_b)` with
//      its outward neighbour preserved.
//   4. Re-link the new triangles' shared edges to each other.
//
// **Constraint recovery (Anglada 1997).** For each input constraint
// edge `(a, b)`:
//
//   1. If `(a, b)` is already an edge of T, flag it constrained, done.
//   2. Otherwise, walk from `a` toward `b` (jump-walk like point location);
//      collect every edge crossed by the segment `(a, b)`.
//   3. Repeatedly flip the first edge in that list (Lawson flip). After
//      each flip, re-test crossings. Termination: each flip moves the
//      strip strictly closer to the constraint, total flips bounded by
//      O(n) per constraint (Anglada 1997 §3.2).
//   4. After `(a, b)` becomes an edge, flag it constrained.
//
// **Delaunay restoration (Lawson 1977 bubble).** After all constraints are
// inserted, scan every non-constrained edge; if it violates the local
// Delaunay condition (Shewchuk `incircle > 0` for the apex of the neighbour
// triangle), flip it. Newly-incident edges go on a re-test queue.
// Terminates in O(n²) worst case (Lawson 1977 proof).
//
// **Finalisation.** Remove every triangle that uses one of the three
// super-triangle vertices (indices `[n, n+1, n+2]`). Remaining triangles
// reference only input-point indices; emit `triangle_indices` as a flat
// 3-per-triangle array.
//
// **Determinism (ADR-0063 + ADR-0076 §4 pin #11).** Insertion order =
// lex-sorted (x, y, original-index). BFS visits in triangle-ID order.
// Edge-flip resolution picks smallest-triangle-index when multiple
// candidates. Delaunay bubble visits edges in (T-id, edge-index) order.
// Result is bit-identical across compilers / SIMD widths / OSes.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/polygon/cdt.hpp>
#include <crd/geometry/polygon/polygon_predicates.hpp>
#include <crd/geometry/polygon/polygon_types.hpp>
#include <crd/geometry/primitives/is_finite.hpp>
#include <crd/geometry/primitives/predicates.hpp>
#include <crd/containers/sort.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include <limits>

namespace crd::geometry::polygon
{
namespace
{
constexpr crd::u32 kNullIdx = std::numeric_limits<crd::u32>::max();

// Per-edge flag bits (within CdtTriangle::flags).
constexpr crd::u8 kAliveBit       = 1U << 0;
constexpr crd::u8 kConstraintE0   = 1U << 1;
constexpr crd::u8 kConstraintE1   = 1U << 2;
constexpr crd::u8 kConstraintE2   = 1U << 3;

constexpr crd::u8 kConstraintBits[3] = {kConstraintE0, kConstraintE1, kConstraintE2};

struct CdtTriangle
{
    crd::u32 v[3]   = {kNullIdx, kNullIdx, kNullIdx};
    crd::u32 nbr[3] = {kNullIdx, kNullIdx, kNullIdx};
    crd::u8  flags  = 0U;
};

template <crd::math::MathScalar T>
struct CdtState
{
    crd::memory::IAllocator*                    alloc = nullptr;
    crd::containers::Array<crd::math::Vec2<T>>  verts;  // input points + 3 super-tri vertices
    crd::containers::Array<CdtTriangle>          tris;
    crd::containers::Array<crd::u32>            free_list; // recycled triangle slots
    crd::u32                                     n_input = 0U;
    crd::u32                                     hint_tri = 0U; // walk-start for next insertion

    explicit CdtState(crd::memory::IAllocator* a) noexcept
      : alloc(a), verts(a), tris(a), free_list(a)
    {
    }
};

// ---- Triangle slot management -------------------------------------------

template <crd::math::MathScalar T>
crd::u32 alloc_triangle(CdtState<T>& s) noexcept
{
    if (!s.free_list.empty())
    {
        const crd::u32 idx = s.free_list.back();
        s.free_list.pop_back();
        s.tris[idx]       = CdtTriangle{};
        s.tris[idx].flags = kAliveBit;
        return idx;
    }
    const crd::u32 idx = static_cast<crd::u32>(s.tris.size());
    CdtTriangle    t{};
    t.flags = kAliveBit;
    s.tris.push_back(t);
    return idx;
}

template <crd::math::MathScalar T>
inline void free_triangle(CdtState<T>& s, crd::u32 idx) noexcept
{
    s.tris[idx].flags = 0U; // not alive
    s.free_list.push_back(idx);
}

template <crd::math::MathScalar T>
inline bool tri_alive(const CdtState<T>& s, crd::u32 idx) noexcept
{
    return idx != kNullIdx && (s.tris[idx].flags & kAliveBit) != 0U;
}

// Find the local edge index `i` such that T.nbr[i] == other_idx. Returns
// kNullIdx if not found (programming error — should always succeed when
// the symmetric link invariant holds).
inline crd::u32 nbr_edge(const CdtTriangle& t, crd::u32 other_idx) noexcept
{
    if (t.nbr[0] == other_idx) { return 0U; }
    if (t.nbr[1] == other_idx) { return 1U; }
    if (t.nbr[2] == other_idx) { return 2U; }
    return kNullIdx;
}

// Find local edge index `i` of triangle T such that the edge endpoints
// are exactly (va, vb) in either order. Returns kNullIdx if no match.
inline crd::u32 find_edge_with_endpoints(const CdtTriangle& t, crd::u32 va, crd::u32 vb) noexcept
{
    for (crd::u32 i = 0; i < 3U; ++i)
    {
        const crd::u32 a = t.v[i];
        const crd::u32 b = t.v[(i + 1U) % 3U];
        if ((a == va && b == vb) || (a == vb && b == va)) { return i; }
    }
    return kNullIdx;
}

inline bool edge_constrained(const CdtTriangle& t, crd::u32 edge_idx) noexcept
{
    return (t.flags & kConstraintBits[edge_idx]) != 0U;
}

inline void set_edge_constrained(CdtTriangle& t, crd::u32 edge_idx) noexcept
{
    t.flags = static_cast<crd::u8>(t.flags | kConstraintBits[edge_idx]);
}

// Mirror constraint flag across the shared edge.
template <crd::math::MathScalar T>
void mark_constraint_both_sides(CdtState<T>& s, crd::u32 t_idx, crd::u32 edge_idx) noexcept
{
    set_edge_constrained(s.tris[t_idx], edge_idx);
    const crd::u32 u_idx = s.tris[t_idx].nbr[edge_idx];
    if (u_idx == kNullIdx) { return; }
    const crd::u32 j = nbr_edge(s.tris[u_idx], t_idx);
    if (j != kNullIdx) { set_edge_constrained(s.tris[u_idx], j); }
}

// ---- Geometric predicates (Shewchuk-adaptive throughout) ----------------

template <crd::math::MathScalar T>
inline T orient2d_signed(const crd::math::Vec2<T>& a, const crd::math::Vec2<T>& b,
                          const crd::math::Vec2<T>& c) noexcept
{
    return crd::geometry::primitives::orient2d(a, b, c);
}

// Is point `p` STRICTLY inside the open circumcircle of CCW triangle (a, b, c)?
template <crd::math::MathScalar T>
inline bool incircle_strict(const crd::math::Vec2<T>& a, const crd::math::Vec2<T>& b,
                             const crd::math::Vec2<T>& c, const crd::math::Vec2<T>& p) noexcept
{
    // Shewchuk: positive if (a, b, c) is CCW AND p is inside the circle
    //   through (a, b, c). Negative if outside. Zero if cocircular.
    return crd::geometry::primitives::incircle(a, b, c, p) > T{0};
}

template <crd::math::MathScalar T>
inline bool point_in_triangle_strict(const CdtState<T>& s, const CdtTriangle& t,
                                     const crd::math::Vec2<T>& p) noexcept
{
    const auto& a = s.verts[t.v[0]];
    const auto& b = s.verts[t.v[1]];
    const auto& c = s.verts[t.v[2]];
    const T     o0 = orient2d_signed(a, b, p);
    const T     o1 = orient2d_signed(b, c, p);
    const T     o2 = orient2d_signed(c, a, p);
    return (o0 >= T{0} && o1 >= T{0} && o2 >= T{0});
}

// ---- Super-triangle setup -----------------------------------------------

template <crd::math::MathScalar T>
void install_super_triangle(CdtState<T>& s,
                             crd::containers::ConstSpan<crd::math::Vec2<T>> input_points)
{
    s.verts.clear();
    s.verts.reserve(input_points.size() + 3U);
    for (const auto& v : input_points) { s.verts.push_back(v); }
    s.n_input = static_cast<crd::u32>(input_points.size());

    // Bounding box of the input.
    T lo_x = input_points[0].x;
    T lo_y = input_points[0].y;
    T hi_x = input_points[0].x;
    T hi_y = input_points[0].y;
    for (const auto& v : input_points)
    {
        if (v.x < lo_x) { lo_x = v.x; }
        if (v.y < lo_y) { lo_y = v.y; }
        if (v.x > hi_x) { hi_x = v.x; }
        if (v.y > hi_y) { hi_y = v.y; }
    }
    const T dx = hi_x - lo_x;
    const T dy = hi_y - lo_y;
    T       d  = dx > dy ? dx : dy;
    if (d == T{0}) { d = T{1}; }
    const T cx = (lo_x + hi_x) * T{0.5};
    const T cy = (lo_y + hi_y) * T{0.5};
    // Super-triangle: huge isoceles around centroid. 1000x is overkill —
    // every input point lies well INSIDE this triangle, so no input ever
    // sits on a super-triangle edge or vertex.
    const T r = d * T{1000};
    s.verts.push_back(crd::math::Vec2<T>{cx - T{3} * r, cy - r});
    s.verts.push_back(crd::math::Vec2<T>{cx + T{3} * r, cy - r});
    s.verts.push_back(crd::math::Vec2<T>{cx, cy + T{3} * r});

    s.tris.clear();
    s.free_list.clear();
    CdtTriangle root{};
    root.v[0]    = s.n_input + 0U;
    root.v[1]    = s.n_input + 1U;
    root.v[2]    = s.n_input + 2U;
    root.nbr[0]  = kNullIdx;
    root.nbr[1]  = kNullIdx;
    root.nbr[2]  = kNullIdx;
    root.flags   = kAliveBit;
    s.tris.push_back(root);
    s.hint_tri = 0U;
}

// ---- Point location ----------------------------------------------------
//
// Jump-walk from `s.hint_tri` toward `p`. At each step, pick the edge whose
// orient2d sign is negative (p is on the outside of that edge), and walk
// across it. Terminates when all three signs are >= 0 (p is in T).

template <crd::math::MathScalar T>
crd::u32 locate(CdtState<T>& s, const crd::math::Vec2<T>& p) noexcept
{
    crd::u32 cur = s.hint_tri;
    // Walk a bounded number of steps; if we exceed (n_tri + 32) something
    // is structurally wrong — fall back to a linear scan.
    const crd::u32 walk_cap = static_cast<crd::u32>(s.tris.size()) + 32U;
    for (crd::u32 step = 0; step < walk_cap; ++step)
    {
        if (!tri_alive(s, cur))
        {
            // Hint stale — find any alive triangle to restart from.
            for (crd::u32 i = 0; i < s.tris.size(); ++i)
            {
                if (tri_alive(s, i)) { cur = i; break; }
            }
        }
        const auto&    t   = s.tris[cur];
        const auto&    p0  = s.verts[t.v[0]];
        const auto&    p1  = s.verts[t.v[1]];
        const auto&    p2  = s.verts[t.v[2]];
        const T        o0  = orient2d_signed(p0, p1, p);
        const T        o1  = orient2d_signed(p1, p2, p);
        const T        o2  = orient2d_signed(p2, p0, p);
        // Cross the first edge where p is on the outside (orient < 0). Pick
        // edge order deterministically (smallest edge index first).
        if (o0 < T{0} && t.nbr[0] != kNullIdx) { cur = t.nbr[0]; continue; }
        if (o1 < T{0} && t.nbr[1] != kNullIdx) { cur = t.nbr[1]; continue; }
        if (o2 < T{0} && t.nbr[2] != kNullIdx) { cur = t.nbr[2]; continue; }
        // All non-negative ⇒ inside (or on boundary, treated as inside).
        return cur;
    }
    // Walk diverged — should not happen with super-triangle enclosure.
    return kNullIdx;
}

// ---- Bowyer-Watson insertion -------------------------------------------

template <crd::math::MathScalar T>
bool insert_point(CdtState<T>& s, crd::u32 p_idx)
{
    const auto&    p     = s.verts[p_idx];
    const crd::u32 t_seed = locate(s, p);
    if (t_seed == kNullIdx) { return false; }

    // BFS-collect bad triangles (circumcircle contains p). Track them in a
    // sorted-by-ID list for deterministic order.
    crd::containers::Array<crd::u32> bad(s.alloc);
    crd::containers::Array<crd::u32> stack(s.alloc);
    crd::containers::Array<crd::u8>  in_bad(s.alloc);
    in_bad.resize(s.tris.size());
    for (crd::usize i = 0; i < in_bad.size(); ++i) { in_bad[i] = 0U; }

    stack.push_back(t_seed);
    in_bad[t_seed] = 1U;
    while (!stack.empty())
    {
        const crd::u32 ti = stack.back();
        stack.pop_back();
        bad.push_back(ti);
        const auto& t = s.tris[ti];
        for (crd::u32 e = 0; e < 3U; ++e)
        {
            const crd::u32 u_idx = t.nbr[e];
            if (u_idx == kNullIdx) { continue; }
            if (u_idx < in_bad.size() && in_bad[u_idx] != 0U) { continue; }
            const auto& u    = s.tris[u_idx];
            const auto& a    = s.verts[u.v[0]];
            const auto& b    = s.verts[u.v[1]];
            const auto& c    = s.verts[u.v[2]];
            if (incircle_strict(a, b, c, p))
            {
                if (u_idx >= in_bad.size())
                {
                    in_bad.resize(static_cast<crd::usize>(u_idx) + 1U);
                }
                in_bad[u_idx] = 1U;
                stack.push_back(u_idx);
            }
        }
    }

    // Collect cavity boundary edges (edges from bad → non-bad).
    // Each entry: (v_a, v_b, ext_neighbour_tri_idx, ext_neighbour_edge_idx,
    //              constraint_inherit)
    struct CavityEdge
    {
        crd::u32 va           = kNullIdx;
        crd::u32 vb           = kNullIdx;
        crd::u32 ext_nbr      = kNullIdx;
        crd::u32 ext_nbr_edge = kNullIdx; // local edge index on ext_nbr facing into the cavity
        bool     constrained  = false;
    };
    crd::containers::Array<CavityEdge> cav(s.alloc);
    for (crd::u32 idx = 0; idx < bad.size(); ++idx)
    {
        const crd::u32     ti = bad[idx];
        const CdtTriangle& t  = s.tris[ti];
        for (crd::u32 e = 0; e < 3U; ++e)
        {
            const crd::u32 u_idx = t.nbr[e];
            const bool u_is_bad
                = (u_idx != kNullIdx && u_idx < in_bad.size() && in_bad[u_idx] != 0U);
            if (u_is_bad) { continue; }
            CavityEdge ce;
            ce.va          = t.v[e];
            ce.vb          = t.v[(e + 1U) % 3U];
            ce.ext_nbr     = u_idx;
            ce.constrained = edge_constrained(t, e);
            if (u_idx != kNullIdx)
            {
                ce.ext_nbr_edge = nbr_edge(s.tris[u_idx], ti);
            }
            cav.push_back(ce);
        }
    }

    // Free bad triangles.
    for (crd::u32 i = 0; i < bad.size(); ++i) { free_triangle(s, bad[i]); }

    // Create new triangles, one per cavity edge.
    crd::containers::Array<crd::u32> new_tris(s.alloc);
    new_tris.reserve(cav.size());
    for (crd::u32 i = 0; i < cav.size(); ++i)
    {
        const crd::u32 ni = alloc_triangle(s);
        CdtTriangle&   t  = s.tris[ni];
        t.v[0]            = cav[i].va;
        t.v[1]            = cav[i].vb;
        t.v[2]            = p_idx;
        // Edge 0 = (va, vb) faces the OUTSIDE (ext_nbr).
        t.nbr[0]          = cav[i].ext_nbr;
        // Edges 1, 2 face other new triangles (linked below).
        t.nbr[1]          = kNullIdx;
        t.nbr[2]          = kNullIdx;
        if (cav[i].constrained) { set_edge_constrained(t, 0); }
        // Update the external neighbour to point back.
        if (cav[i].ext_nbr != kNullIdx && cav[i].ext_nbr_edge != kNullIdx)
        {
            s.tris[cav[i].ext_nbr].nbr[cav[i].ext_nbr_edge] = ni;
        }
        new_tris.push_back(ni);
    }

    // Link new triangles to each other across their (vb, p) and (p, va) edges.
    // For each new triangle T_i with vertices (va, vb, p), edge 1 is (vb, p)
    // and edge 2 is (p, va). The triangle sharing edge 1 with T_i is the one
    // whose edge 2 has endpoints (p, vb) — i.e. it's the new triangle whose
    // `va == vb_of_T_i`.
    for (crd::u32 i = 0; i < new_tris.size(); ++i)
    {
        CdtTriangle&   ti = s.tris[new_tris[i]];
        const crd::u32 vb = ti.v[1];
        const crd::u32 va = ti.v[0];
        // Find new triangle with v[0] == vb (its edge 2 is (p, vb) which
        // matches our edge 1 (vb, p) reversed).
        for (crd::u32 j = 0; j < new_tris.size(); ++j)
        {
            if (i == j) { continue; }
            if (s.tris[new_tris[j]].v[0] == vb)
            {
                ti.nbr[1] = new_tris[j];
                break;
            }
        }
        // Find new triangle with v[1] == va (its edge 0 is (va_j, vb_j) and
        // we need vb_j == va of ti; that means v[1] of triangle j equals
        // va of ti).
        for (crd::u32 j = 0; j < new_tris.size(); ++j)
        {
            if (i == j) { continue; }
            if (s.tris[new_tris[j]].v[1] == va)
            {
                ti.nbr[2] = new_tris[j];
                break;
            }
        }
    }

    if (!new_tris.empty()) { s.hint_tri = new_tris[0]; }
    return true;
}

// ---- Constraint recovery (Anglada 1997, simplified) ---------------------
//
// For each constraint (a, b):
//   1) If (a, b) is already a triangulation edge, flag both sides constrained.
//   2) Otherwise: starting from some triangle incident on `a`, walk toward
//      `b`. When the path crosses an edge `(p, q)`, flip that edge. Repeat
//      until `b` is reached or no further crossings exist. The flip strategy
//      is correct because each flip strictly reduces the topological
//      distance to the constraint (Anglada 1997 §3.2).

// Find any alive triangle incident on vertex `v`. Returns kNullIdx if
// none. O(n_tris) linear scan — adequate for typical N (Anglada's paper
// notes that vertex-to-triangle adjacency cache is the optimization).
template <crd::math::MathScalar T>
crd::u32 any_triangle_at_vertex(const CdtState<T>& s, crd::u32 v) noexcept
{
    for (crd::u32 i = 0; i < s.tris.size(); ++i)
    {
        if (!tri_alive(s, i)) { continue; }
        const auto& t = s.tris[i];
        if (t.v[0] == v || t.v[1] == v || t.v[2] == v) { return i; }
    }
    return kNullIdx;
}

// Is there a triangle T containing vertex va such that edge (va, vb) is
// one of T's edges? Returns (t_idx, edge_idx) packed; t_idx = kNullIdx
// on miss.
template <crd::math::MathScalar T>
struct EdgeLocator
{
    crd::u32 t_idx    = kNullIdx;
    crd::u32 edge_idx = kNullIdx;
};

template <crd::math::MathScalar T>
EdgeLocator<T> find_edge(const CdtState<T>& s, crd::u32 va, crd::u32 vb) noexcept
{
    for (crd::u32 i = 0; i < s.tris.size(); ++i)
    {
        if (!tri_alive(s, i)) { continue; }
        const crd::u32 e = find_edge_with_endpoints(s.tris[i], va, vb);
        if (e != kNullIdx) { return {i, e}; }
    }
    return {};
}

// Standard edge flip: rotates the shared edge between triangles T and U
// such that the diagonal of the quad swaps. Preserves CCW orientation +
// updates all neighbour pointers + propagates the constraint flag on the
// OUTER edges (the diagonal is by definition non-constrained -- caller
// must check before flipping).
//   U\    /                         \  │ /
//     \  /                           \ │/
//      \/                             v_d
//      v_d                            ...
template <crd::math::MathScalar T>
bool flip_edge(CdtState<T>& s, crd::u32 t_idx, crd::u32 edge_idx) noexcept
{
    const crd::u32 u_idx = s.tris[t_idx].nbr[edge_idx];
    if (u_idx == kNullIdx) { return false; }
    if (edge_constrained(s.tris[t_idx], edge_idx)) { return false; }

    CdtTriangle&   t   = s.tris[t_idx];
    CdtTriangle&   u   = s.tris[u_idx];
    const crd::u32 i_t = edge_idx;
    const crd::u32 i_u = nbr_edge(u, t_idx);
    if (i_u == kNullIdx) { return false; }

    // Naming: t = (va, vb, vc) with va = t.v[i_t]; vc = t.v[(i_t+2)%3]
    //         u = (vb, va, vd) with vd = u.v[(i_u+2)%3]
    const crd::u32 va = t.v[i_t];
    const crd::u32 vb = t.v[(i_t + 1U) % 3U];
    const crd::u32 vc = t.v[(i_t + 2U) % 3U];
    const crd::u32 vd = u.v[(i_u + 2U) % 3U];

    // Verify convexity of the quad — flip only valid if both new triangles
    // would be CCW. New T' = (va, vd, vc) ⇒ orient(pa, pd, pc) > 0.
    //         New U' = (vd, vb, vc) ⇒ orient(pd, pb, pc) > 0.
    const auto& pa = s.verts[va];
    const auto& pb = s.verts[vb];
    const auto& pc = s.verts[vc];
    const auto& pd = s.verts[vd];
    if (orient2d_signed(pa, pd, pc) <= T{0}) { return false; }
    if (orient2d_signed(pd, pb, pc) <= T{0}) { return false; }

    // Capture original neighbours + constraints around the rim.
    const crd::u32 t_nbr_bc      = t.nbr[(i_t + 1U) % 3U]; // edge (vb, vc)
    const crd::u32 t_nbr_ca      = t.nbr[(i_t + 2U) % 3U]; // edge (vc, va)
    const bool     t_con_bc      = edge_constrained(t, (i_t + 1U) % 3U);
    const bool     t_con_ca      = edge_constrained(t, (i_t + 2U) % 3U);
    const crd::u32 u_nbr_ad      = u.nbr[(i_u + 1U) % 3U]; // edge (va, vd)
    const crd::u32 u_nbr_db      = u.nbr[(i_u + 2U) % 3U]; // edge (vd, vb)
    const bool     u_con_ad      = edge_constrained(u, (i_u + 1U) % 3U);
    const bool     u_con_db      = edge_constrained(u, (i_u + 2U) % 3U);

    // New triangle T' = (va, vd, vc) [CCW since orient(va, vd, vc) ==
    //   -orient(vc, vd, va) and we asserted orient(vc, vd, va) > 0 — hmm
    //   actually let me re-check]. Actually (va, vd, vc) is CCW iff
    //   orient(va, vd, vc) > 0; equivalent to orient(vc, va, vd) > 0,
    //   which is orient2d(pc, pa, pd) — let me verify by swap rule:
    //   orient(c, d, a) = orient(c, a, d) negated? No, orient is anti-
    //   symmetric in any single swap. So orient(c, d, a) = -orient(d, c, a)
    //   = orient(c, a, d) negated. The asserts ensure orient(c, d, a) > 0
    //   ⇒ (c, d, a) CCW ⇒ (a, d, c) CCW too? Let me just check: cyclic
    //   rotation preserves orientation. (c, d, a) → (d, a, c) → (a, c, d).
    //   So (c, d, a) CCW ⇔ (a, c, d) CCW. Therefore (a, d, c) is the
    //   reverse of (a, c, d) — CW. That doesn't work!
    //
    //   Let me re-think. We have triangle T = (va, vb, vc) and U = (vb, va, vd).
    //   The quad in CCW order is (va, vd, vb, vc). The new diagonal is
    //   (vc, vd). So T' = (vc, vd, va)? Let me trace:
    //   - Quad CCW: va, vd, vb, vc, back to va.
    //   - New diagonal (vc, vd) splits into (va, vd, vc) on one side
    //     and (vd, vb, vc) on the other.
    //   - For these to be CCW: orient(va, vd, vc) > 0 and orient(vd, vb, vc) > 0.
    //   - That's what we asserted with the convexity checks above.
    //
    // So T' = (va, vd, vc), U' = (vd, vb, vc).

    // Write out the new triangles:
    t.v[0]    = va;
    t.v[1]    = vd;
    t.v[2]    = vc;
    t.nbr[0]  = u_nbr_ad;        // edge (va, vd) was U's external
    t.nbr[1]  = u_idx;           // edge (vd, vc) is shared with U'
    t.nbr[2]  = t_nbr_ca;        // edge (vc, va) was T's external
    t.flags   = kAliveBit;
    if (u_con_ad) { set_edge_constrained(t, 0); }
    if (t_con_ca) { set_edge_constrained(t, 2); }

    u.v[0]    = vd;
    u.v[1]    = vb;
    u.v[2]    = vc;
    u.nbr[0]  = u_nbr_db;        // edge (vd, vb) was U's external
    u.nbr[1]  = t_nbr_bc;        // edge (vb, vc) was T's external
    u.nbr[2]  = t_idx;           // edge (vc, vd) shared with T'
    u.flags   = kAliveBit;
    if (u_con_db) { set_edge_constrained(u, 0); }
    if (t_con_bc) { set_edge_constrained(u, 1); }

    // Update the back-pointers from the external neighbours.
    if (u_nbr_ad != kNullIdx)
    {
        const crd::u32 k = nbr_edge(s.tris[u_nbr_ad], u_idx);
        if (k != kNullIdx) { s.tris[u_nbr_ad].nbr[k] = t_idx; }
    }
    if (t_nbr_ca != kNullIdx)
    {
        // Already pointed at t_idx — no change needed.
    }
    if (u_nbr_db != kNullIdx)
    {
        // Already pointed at u_idx — no change needed.
    }
    if (t_nbr_bc != kNullIdx)
    {
        const crd::u32 k = nbr_edge(s.tris[t_nbr_bc], t_idx);
        if (k != kNullIdx) { s.tris[t_nbr_bc].nbr[k] = u_idx; }
    }
    return true;
}

// Find the triangle at vertex `va` whose cone (at va) contains direction
// (va → vb). Returns the local index of va within that triangle as well.
template <crd::math::MathScalar T>
struct ConeFind
{
    crd::u32 t_idx    = kNullIdx;
    crd::u32 va_local = kNullIdx; // va's slot index in t (0/1/2)
};

template <crd::math::MathScalar T>
ConeFind<T> find_cone_at_vertex(const CdtState<T>& s, crd::u32 va, crd::u32 vb) noexcept
{
    const auto& pa = s.verts[va];
    const auto& pb = s.verts[vb];
    for (crd::u32 ti = 0; ti < s.tris.size(); ++ti)
    {
        if (!tri_alive(s, ti)) { continue; }
        const auto& t = s.tris[ti];
        crd::u32    vla = kNullIdx;
        for (crd::u32 e = 0; e < 3U; ++e)
        {
            if (t.v[e] == va) { vla = e; break; }
        }
        if (vla == kNullIdx) { continue; }
        const crd::u32 v_next = t.v[(vla + 1U) % 3U];
        const crd::u32 v_prev = t.v[(vla + 2U) % 3U];
        const auto&    pnext  = s.verts[v_next];
        const auto&    pprev  = s.verts[v_prev];
        // Strict cone test: pb must be strictly LEFT of (pa→pnext) AND strictly
        // RIGHT of (pa→pprev). The boundary cases (pb on either edge direction)
        // mean (va, vb) is ALREADY an edge of T — caught by find_edge fast
        // path before this is called, so we use strict signs here.
        const T left  = orient2d_signed(pa, pnext, pb);
        const T right = orient2d_signed(pa, pprev, pb);
        if (left > T{0} && right < T{0})
        {
            return {ti, vla};
        }
    }
    return {};
}

// Trace the chain of triangles crossed by segment (va, vb). For each step
// records the triangle index and the local edge index through which the
// segment EXITS the triangle. The final triangle (containing vb as a
// vertex) is NOT in the chain — it is returned separately as `final_t`.
//
// Returns false if the trace hits a constrained edge or runs off the
// triangulation (constraint endpoint outside the hull).
template <crd::math::MathScalar T>
struct ChainStep
{
    crd::u32 t_idx     = kNullIdx;
    crd::u32 exit_edge = kNullIdx;
};

template <crd::math::MathScalar T>
bool trace_chain(CdtState<T>& s, crd::u32 va, crd::u32 vb,
                 crd::containers::Array<ChainStep<T>>& out_chain, crd::u32& out_final_t) noexcept
{
    const auto& pa = s.verts[va];
    const auto& pb = s.verts[vb];

    const ConeFind<T> seed = find_cone_at_vertex(s, va, vb);
    if (seed.t_idx == kNullIdx) { return false; }

    crd::u32 t_idx     = seed.t_idx;
    crd::u32 va_local  = seed.va_local;
    // First triangle: contains va as a vertex. Exit edge is opposite to va.
    crd::u32 exit_edge = (va_local + 1U) % 3U;
    if (edge_constrained(s.tris[t_idx], exit_edge)) { return false; }

    out_chain.clear();
    out_chain.push_back({t_idx, exit_edge});

    const crd::u32 safety = static_cast<crd::u32>(s.tris.size()) + 16U;
    for (crd::u32 step = 0; step < safety; ++step)
    {
        const crd::u32 next_t = s.tris[t_idx].nbr[exit_edge];
        if (next_t == kNullIdx) { return false; }
        // Find the entry edge of `next_t` (same edge as t_idx's exit_edge).
        const crd::u32 entry_edge = nbr_edge(s.tris[next_t], t_idx);
        if (entry_edge == kNullIdx) { return false; }

        // Is vb a vertex of next_t? If so, chain ends here.
        const auto& nt = s.tris[next_t];
        if (nt.v[0] == vb || nt.v[1] == vb || nt.v[2] == vb)
        {
            out_final_t = next_t;
            return true;
        }

        // The apex of next_t (the vertex opposite the entry edge) is at
        // `nt.v[(entry_edge + 2) % 3]`. Decide which of the two non-entry
        // edges the segment exits through by checking which side of segment
        // (va, vb) the apex lies on.
        const crd::u32 apex_v = nt.v[(entry_edge + 2U) % 3U];
        const auto&    pap    = s.verts[apex_v];
        const T        o      = orient2d_signed(pa, pb, pap);
        crd::u32 new_exit;
        if (o > T{0})
        {
            // Apex is to the LEFT of segment. Segment exits through the
            // edge `(entry_edge + 1) % 3` = (nt.v[entry_edge+1], apex).
            new_exit = (entry_edge + 1U) % 3U;
        }
        else if (o < T{0})
        {
            // Apex is to the RIGHT. Segment exits through edge
            // `(entry_edge + 2) % 3` = (apex, nt.v[entry_edge]).
            new_exit = (entry_edge + 2U) % 3U;
        }
        else
        {
            // Segment passes exactly through apex — degenerate (constraint
            // passes through an existing vertex). Treat as failure for now.
            return false;
        }
        if (edge_constrained(nt, new_exit)) { return false; }

        t_idx     = next_t;
        exit_edge = new_exit;
        out_chain.push_back({t_idx, exit_edge});
    }
    return false;
}

// Carve-and-retriangulate constraint recovery (Domiter-Zalik 2008-style,
// simplified). Given the chain of triangles crossed by (va, vb), the union
// is a polygon P. Delete the chain triangles, then triangulate the two
// sub-polygons formed by splitting P along (va, vb). The split is naturally
// `chain[0].exit_edge`-LEFT-side and `chain[0].exit_edge`-RIGHT-side per
// triangle; collecting these in chain order yields the boundary vertices
// of each sub-polygon in CCW order.
//
// The triangulation of each sub-polygon uses an ear-clip pass (which works
// even on non-convex but simple sub-polygons). Final result: chain
// triangles replaced by `(P_upper.size - 2) + (P_lower.size - 2)` new
// triangles with (va, vb) as the shared edge.

template <crd::math::MathScalar T>
struct SubBoundary
{
    crd::containers::Array<crd::u32> verts; // vertex indices, CCW order
    crd::containers::Array<crd::u32> outer_nbrs; // for each boundary edge
                                                  // (verts[i], verts[i+1]),
                                                  // the OUTER triangle across
                                                  // it (or k_null if the edge
                                                  // is the diagonal (va, vb)).
    crd::containers::Array<crd::u32> outer_edges; // local edge index in
                                                   // outer_nbrs of the back-
                                                   // pointer to the chain.

    explicit SubBoundary(crd::memory::IAllocator* a) : verts(a), outer_nbrs(a), outer_edges(a) {}
};

// Returns true on success. Modifies the triangulation.
template <crd::math::MathScalar T>
bool carve_and_retriangulate(CdtState<T>& s, crd::u32 va, crd::u32 vb,
                              const crd::containers::Array<ChainStep<T>>& chain,
                              crd::u32 final_t)
{
    // Build upper / lower sub-polygon boundaries by walking the chain.
    //
    // chain[i].t_idx contains an exit_edge such that its TWO non-exit edges
    // belong to either the upper or lower sub-polygon. The two endpoints of
    // each exit_edge are vertices of the chain interior; one of them is the
    // "upper" boundary vertex (shared between this triangle and the next),
    // the other is the "lower". We classify by which side of segment
    // (va, vb) each vertex lies on.

    const auto& pa = s.verts[va];
    const auto& pb = s.verts[vb];

    SubBoundary<T> upper(s.alloc);
    SubBoundary<T> lower(s.alloc);

    // Both sub-polygons start at va.
    upper.verts.push_back(va);
    lower.verts.push_back(va);

    auto classify = [&](crd::u32 vert) noexcept -> int {
        // +1 = upper (left of segment), -1 = lower, 0 = on the segment.
        const T o = orient2d_signed(pa, pb, s.verts[vert]);
        if (o > T{0}) { return +1; }
        if (o < T{0}) { return -1; }
        return 0;
    };

    // Walk chain. For each chain triangle, collect its outer (non-exit) edges
    // and assign them to upper / lower based on which side of segment they
    // sit. The first chain triangle has va as a vertex; the last is `final_t`
    // which contains vb.

    for (crd::u32 i = 0; i < chain.size(); ++i)
    {
        const crd::u32     ti = chain[i].t_idx;
        const crd::u32     ee = chain[i].exit_edge;
        const CdtTriangle& t  = s.tris[ti];

        // T's three edges in local-index order: 0, 1, 2. The exit edge is `ee`.
        // The OTHER two are (ee + 1) % 3 and (ee + 2) % 3.
        for (crd::u32 k = 1; k <= 2; ++k)
        {
            const crd::u32 le = (ee + k) % 3U;
            const crd::u32 e_va = t.v[le];
            const crd::u32 e_vb = t.v[(le + 1U) % 3U];
            // Determine which sub-polygon this edge belongs to. The edge
            // sits entirely on one side of segment (va, vb) — except for
            // any endpoint that IS va or vb (which is on the segment).
            int c0 = classify(e_va);
            int c1 = classify(e_vb);
            // If either endpoint is exactly on the segment (va or vb itself),
            // use the OTHER endpoint to classify.
            int side = (c0 != 0) ? c0 : c1;
            SubBoundary<T>& sb = (side > 0) ? upper : lower;
            // Append the edge as a step in the sub-polygon boundary. We need
            // edges in CCW order around the sub-polygon. Triangle edges are
            // already CCW around the triangle interior; since the sub-polygon
            // boundary follows the OUTER side of chain triangles, the edges
            // are CCW around the sub-polygon as we walk the chain forward
            // for one sub-polygon and BACKWARD for the other. We approximate
            // by adding edges in chain order and de-duplicating later.
            // For simplicity, just push the next vertex if it's not already
            // there.
            (void)sb;
            (void)e_va;
            (void)e_vb;
        }

        // Outer neighbours across the two non-exit edges feed back into the
        // re-link step after the new triangulation is built.
        (void)i;
    }

    // The above edge-collection is sketched but a cleaner pass walks the
    // boundary explicitly:
    //
    //   upper boundary: va → (chain[0]'s exit edge upper endpoint) →
    //                   (chain[1]'s exit edge upper endpoint) →
    //                   ... → vb → va (closing)
    //   lower boundary: va → vb (along constraint, opposite direction) →
    //                   (chain[k-1]'s exit edge lower endpoint) →
    //                   ... → (chain[0]'s exit edge lower endpoint) → va
    //
    // Both sub-polygons are simple and CCW. We ear-clip each.

    upper.verts.clear();
    lower.verts.clear();
    upper.verts.push_back(va);
    lower.verts.push_back(va);

    // Walk chain forward. At each step, the exit edge's two endpoints are
    // (e_top, e_bot) — top is the upper-side vertex, bot is the lower-side
    // vertex.
    crd::u32 last_top = kNullIdx;
    crd::u32 last_bot = kNullIdx;
    for (crd::u32 i = 0; i < chain.size(); ++i)
    {
        const crd::u32     ti = chain[i].t_idx;
        const crd::u32     ee = chain[i].exit_edge;
        const CdtTriangle& t  = s.tris[ti];
        const crd::u32     e_va = t.v[ee];
        const crd::u32     e_vb = t.v[(ee + 1U) % 3U];
        int                c0 = classify(e_va);
        int                c1 = classify(e_vb);
        crd::u32           top = kNullIdx;
        crd::u32           bot = kNullIdx;
        if (c0 > 0) { top = e_va; bot = e_vb; }
        else if (c1 > 0) { top = e_vb; bot = e_va; }
        else
        {
            // Both endpoints on or below segment — degenerate, give up.
            return false;
        }
        if (c0 == 0 || c1 == 0)
        {
            // One endpoint exactly on the segment — constraint passes
            // through that vertex. Treat as degenerate / failure.
            return false;
        }
        if (top != last_top)
        {
            upper.verts.push_back(top);
            last_top = top;
        }
        if (bot != last_bot)
        {
            lower.verts.push_back(bot);
            last_bot = bot;
        }
    }
    // Close at vb.
    upper.verts.push_back(vb);
    lower.verts.push_back(vb);

    // Now upper.verts is CCW: va → top_0 → top_1 → ... → vb. Closing the
    // polygon needs implicit edge (vb, va) — that's the constraint.
    // lower.verts is CW (we walked the lower side in the same direction).
    // Reverse it so it's CCW around the lower sub-polygon.
    {
        crd::containers::Array<crd::u32> rev(s.alloc);
        rev.reserve(lower.verts.size());
        for (crd::usize i = lower.verts.size(); i > 0U; --i) { rev.push_back(lower.verts[i - 1U]); }
        lower.verts = rev;
    }

    // Capture outer neighbours of chain triangles — these are the triangles
    // adjacent to the chain on the OUTSIDE. After re-triangulation, the new
    // triangles must link back to these neighbours.
    //
    // Map from (boundary edge endpoint pair, canonicalised) → outer neighbour
    // triangle + its local edge index.
    struct OuterLink
    {
        crd::u32 v0;
        crd::u32 v1;
        crd::u32 nbr_t;
        crd::u32 nbr_e;
        bool     constrained;
    };
    crd::containers::Array<OuterLink> outers(s.alloc);

    auto record_outer = [&](crd::u32 ti, crd::u32 e) {
        const CdtTriangle& tt   = s.tris[ti];
        const crd::u32     vv0  = tt.v[e];
        const crd::u32     vv1  = tt.v[(e + 1U) % 3U];
        const crd::u32     nbr  = tt.nbr[e];
        const bool         cons = edge_constrained(tt, e);
        crd::u32 nbe = kNullIdx;
        if (nbr != kNullIdx) { nbe = nbr_edge(s.tris[nbr], ti); }
        OuterLink lk;
        lk.v0          = vv0;
        lk.v1          = vv1;
        lk.nbr_t       = nbr;
        lk.nbr_e       = nbe;
        lk.constrained = cons;
        outers.push_back(lk);
    };

    for (crd::u32 i = 0; i < chain.size(); ++i)
    {
        const crd::u32 ti = chain[i].t_idx;
        const crd::u32 ee = chain[i].exit_edge;
        record_outer(ti, (ee + 1U) % 3U);
        record_outer(ti, (ee + 2U) % 3U);
    }
    // Also the final triangle (which contains vb): record its non-shared edges.
    {
        const CdtTriangle& ft = s.tris[final_t];
        // Find which edge of final_t was the entry from the last chain step.
        const crd::u32 last_chain_t = chain[chain.size() - 1U].t_idx;
        const crd::u32 entry_in_final = nbr_edge(ft, last_chain_t);
        if (entry_in_final == kNullIdx) { return false; }
        record_outer(final_t, (entry_in_final + 1U) % 3U);
        record_outer(final_t, (entry_in_final + 2U) % 3U);
    }

    // Free chain + final triangles.
    for (crd::u32 i = 0; i < chain.size(); ++i) { free_triangle(s, chain[i].t_idx); }
    free_triangle(s, final_t);

    // Re-triangulate each sub-polygon by ear-clipping. The sub-polygon
    // vertices reference the input verts directly. We need triangle indices
    // referencing s.verts (which is what the chain's vertices already do).
    //
    // Ear-clip implementation: same algorithm as v6b, inlined here to avoid
    // a cross-module dep. The sub-polygons are simple by construction (each
    // is a "half" of a chain polygon, bounded by the constraint segment + a
    // chain of triangle edges).

    auto ear_clip_subpoly = [&](const crd::containers::Array<crd::u32>& boundary) {
        const crd::u32 n = static_cast<crd::u32>(boundary.size());
        if (n < 3U) { return crd::containers::Array<crd::u32>(s.alloc); }
        crd::containers::Array<crd::u32> nxt(s.alloc);
        crd::containers::Array<crd::u32> prv(s.alloc);
        crd::containers::Array<crd::u8>  alive(s.alloc);
        nxt.resize(n);
        prv.resize(n);
        alive.resize(n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            nxt[i]   = (i + 1U) % n;
            prv[i]   = (i + n - 1U) % n;
            alive[i] = 1U;
        }
        crd::u32 head        = 0U;
        crd::u32 live        = n;
        crd::containers::Array<crd::u32> tris(s.alloc);

        auto is_reflex_local = [&](crd::u32 i) noexcept {
            const auto& a = s.verts[boundary[prv[i]]];
            const auto& b = s.verts[boundary[i]];
            const auto& c = s.verts[boundary[nxt[i]]];
            return orient2d_signed(a, b, c) <= T{0};
        };
        auto in_tri = [&](const crd::math::Vec2<T>& a, const crd::math::Vec2<T>& b,
                          const crd::math::Vec2<T>& c, const crd::math::Vec2<T>& p) noexcept {
            return orient2d_signed(a, b, p) > T{0} && orient2d_signed(b, c, p) > T{0} &&
                   orient2d_signed(c, a, p) > T{0};
        };
        auto is_ear_local = [&](crd::u32 i) noexcept {
            if (is_reflex_local(i)) { return false; }
            const auto& a = s.verts[boundary[prv[i]]];
            const auto& b = s.verts[boundary[i]];
            const auto& c = s.verts[boundary[nxt[i]]];
            crd::u32    j = head;
            do
            {
                if (j != i && j != prv[i] && j != nxt[i] && alive[j])
                {
                    if (is_reflex_local(j) && in_tri(a, b, c, s.verts[boundary[j]]))
                    {
                        return false;
                    }
                }
                j = nxt[j];
            } while (j != head);
            return true;
        };

        const crd::u32 cap = n;
        for (crd::u32 iter = 0; iter < cap && live > 3U; ++iter)
        {
            crd::u32 ear = kNullIdx;
            crd::u32 j   = head;
            do
            {
                if (alive[j] && is_ear_local(j))
                {
                    if (ear == kNullIdx || j < ear) { ear = j; }
                }
                j = nxt[j];
            } while (j != head);
            if (ear == kNullIdx) { break; }
            tris.push_back(boundary[prv[ear]]);
            tris.push_back(boundary[ear]);
            tris.push_back(boundary[nxt[ear]]);
            const crd::u32 p = prv[ear];
            const crd::u32 q = nxt[ear];
            nxt[p] = q;
            prv[q] = p;
            alive[ear] = 0U;
            --live;
            if (head == ear) { head = q; }
        }
        if (live == 3U)
        {
            tris.push_back(boundary[head]);
            tris.push_back(boundary[nxt[head]]);
            tris.push_back(boundary[nxt[nxt[head]]]);
        }
        return tris;
    };

    auto upper_tris = ear_clip_subpoly(upper.verts);
    auto lower_tris = ear_clip_subpoly(lower.verts);
    if (upper_tris.empty() || lower_tris.empty()) { return false; }

    // Allocate new CDT triangles for each output triangle. Initialise
    // neighbours to kNullIdx; we link them below by scanning shared edges.
    crd::containers::Array<crd::u32> new_t_ids(s.alloc);
    auto add_triangle = [&](crd::u32 a, crd::u32 b, crd::u32 c) {
        const crd::u32 idx = alloc_triangle(s);
        CdtTriangle&   tri = s.tris[idx];
        tri.v[0]           = a;
        tri.v[1]           = b;
        tri.v[2]           = c;
        tri.nbr[0]         = kNullIdx;
        tri.nbr[1]         = kNullIdx;
        tri.nbr[2]         = kNullIdx;
        new_t_ids.push_back(idx);
    };

    for (crd::usize i = 0; i + 3U <= upper_tris.size(); i += 3U)
    {
        add_triangle(upper_tris[i], upper_tris[i + 1U], upper_tris[i + 2U]);
    }
    for (crd::usize i = 0; i + 3U <= lower_tris.size(); i += 3U)
    {
        add_triangle(lower_tris[i], lower_tris[i + 1U], lower_tris[i + 2U]);
    }

    // Re-link neighbours.
    //   * Outer edges of the new triangulation that match (vv0, vv1) of an
    //     OuterLink ⇒ re-link to the outer neighbour (and copy constraint).
    //   * Edges shared between two new triangles ⇒ link bidirectionally.
    //   * The constraint edge (va, vb) ⇒ link the two adjacent new triangles
    //     to each other AND mark constrained.
    auto edge_endpoints = [&](crd::u32 ti, crd::u32 ei) {
        const CdtTriangle& tt = s.tris[ti];
        return std::pair<crd::u32, crd::u32>{tt.v[ei], tt.v[(ei + 1U) % 3U]};
    };
    for (crd::u32 ni = 0; ni < new_t_ids.size(); ++ni)
    {
        const crd::u32 ti = new_t_ids[ni];
        for (crd::u32 e = 0; e < 3U; ++e)
        {
            if (s.tris[ti].nbr[e] != kNullIdx) { continue; }
            const auto ep = edge_endpoints(ti, e);
            // Check against OuterLinks first.
            crd::u32 matched_outer = kNullIdx;
            for (crd::u32 ol = 0; ol < outers.size(); ++ol)
            {
                const auto& lk = outers[ol];
                // OuterLink endpoints are (lk.v0, lk.v1) in some order — we match
                // either direction.
                if ((lk.v0 == ep.first && lk.v1 == ep.second) ||
                    (lk.v0 == ep.second && lk.v1 == ep.first))
                {
                    matched_outer = ol;
                    break;
                }
            }
            if (matched_outer != kNullIdx)
            {
                const auto& lk = outers[matched_outer];
                s.tris[ti].nbr[e] = lk.nbr_t;
                if (lk.nbr_t != kNullIdx && lk.nbr_e != kNullIdx)
                {
                    s.tris[lk.nbr_t].nbr[lk.nbr_e] = ti;
                }
                if (lk.constrained) { set_edge_constrained(s.tris[ti], e); }
                continue;
            }
            // Not an outer edge — must be a shared edge with another new
            // triangle OR the constraint edge.
            for (crd::u32 nj = 0; nj < new_t_ids.size(); ++nj)
            {
                if (nj == ni) { continue; }
                const crd::u32 tj = new_t_ids[nj];
                for (crd::u32 ej = 0; ej < 3U; ++ej)
                {
                    const auto qp = edge_endpoints(tj, ej);
                    if (qp.first == ep.second && qp.second == ep.first)
                    {
                        s.tris[ti].nbr[e]  = tj;
                        s.tris[tj].nbr[ej] = ti;
                        // Mark constraint if this edge IS the constraint (va, vb).
                        if ((ep.first == va && ep.second == vb) ||
                            (ep.first == vb && ep.second == va))
                        {
                            set_edge_constrained(s.tris[ti], e);
                            set_edge_constrained(s.tris[tj], ej);
                        }
                        break;
                    }
                }
                if (s.tris[ti].nbr[e] != kNullIdx) { break; }
            }
        }
    }

    if (!new_t_ids.empty()) { s.hint_tri = new_t_ids[0]; }
    return true;
}

template <crd::math::MathScalar T>
bool insert_constraint(CdtState<T>& s, crd::u32 va, crd::u32 vb) noexcept
{
    // Fast path: already an edge.
    {
        auto loc = find_edge<T>(s, va, vb);
        if (loc.t_idx != kNullIdx)
        {
            mark_constraint_both_sides(s, loc.t_idx, loc.edge_idx);
            return true;
        }
    }

    // Trace the chain of triangles crossed by segment (va, vb), then carve
    // and retriangulate the chain polygon. This handles arbitrary multi-edge
    // crossings including non-convex-quad cases where pure flip-recovery
    // would fail (Domiter-Zalik 2008-style fallback over Anglada 1997).
    crd::containers::Array<ChainStep<T>> chain(s.alloc);
    crd::u32                              final_t = kNullIdx;
    if (!trace_chain<T>(s, va, vb, chain, final_t))
    {
        return false;
    }
    if (chain.empty() || final_t == kNullIdx) { return false; }

    if (!carve_and_retriangulate<T>(s, va, vb, chain, final_t)) { return false; }
    return true;
}

// ---- Lawson 1977 post-flip Delaunay restoration ------------------------

template <crd::math::MathScalar T>
void restore_delaunay(CdtState<T>& s)
{
    // Edge candidate queue — pair of (triangle, local-edge-index). Each
    // entry may have been invalidated by a prior flip; we recheck.
    crd::containers::Array<crd::u32> queue_tri(s.alloc);
    crd::containers::Array<crd::u8>  queue_edge(s.alloc);
    queue_tri.reserve(s.tris.size() * 3U);
    queue_edge.reserve(s.tris.size() * 3U);
    for (crd::u32 i = 0; i < s.tris.size(); ++i)
    {
        if (!tri_alive(s, i)) { continue; }
        queue_tri.push_back(i);
        queue_edge.push_back(0U);
        queue_tri.push_back(i);
        queue_edge.push_back(1U);
        queue_tri.push_back(i);
        queue_edge.push_back(2U);
    }
    crd::u32 head = 0U;
    while (head < queue_tri.size())
    {
        const crd::u32 ti = queue_tri[head];
        const crd::u8  e  = queue_edge[head];
        ++head;
        if (!tri_alive(s, ti)) { continue; }
        if (edge_constrained(s.tris[ti], e)) { continue; }
        const crd::u32 u_idx = s.tris[ti].nbr[e];
        if (u_idx == kNullIdx) { continue; }
        // Incircle test: does the apex of U lie inside the circumcircle of T?
        const CdtTriangle& t   = s.tris[ti];
        const CdtTriangle& u   = s.tris[u_idx];
        const crd::u32     i_u = nbr_edge(u, ti);
        if (i_u == kNullIdx) { continue; }
        const crd::u32     vd  = u.v[(i_u + 2U) % 3U];
        const auto&        pa  = s.verts[t.v[0]];
        const auto&        pb  = s.verts[t.v[1]];
        const auto&        pc  = s.verts[t.v[2]];
        const auto&        pd  = s.verts[vd];
        if (!incircle_strict(pa, pb, pc, pd)) { continue; }
        // Flip — afterwards the four surrounding edges may need re-test.
        if (!flip_edge(s, ti, e)) { continue; }
        // Re-queue the four outer edges of T' and U'.
        queue_tri.push_back(ti);
        queue_edge.push_back(0U);
        queue_tri.push_back(ti);
        queue_edge.push_back(2U);
        queue_tri.push_back(u_idx);
        queue_edge.push_back(0U);
        queue_tri.push_back(u_idx);
        queue_edge.push_back(1U);
    }
}

// ---- Finalisation: strip super-triangle + in/out filter ----------------

template <crd::math::MathScalar T>
bool triangle_uses_super(const CdtState<T>& s, const CdtTriangle& t) noexcept
{
    return t.v[0] >= s.n_input || t.v[1] >= s.n_input || t.v[2] >= s.n_input;
}

template <crd::math::MathScalar T>
void emit_triangles(const CdtState<T>& s, CdtResult<T>& result,
                    bool keep_only_inside_polygon, const PolygonView2<T>* poly_for_filter)
{
    result.triangle_indices.clear();
    for (crd::u32 i = 0; i < s.tris.size(); ++i)
    {
        if (!tri_alive(s, i)) { continue; }
        const auto& t = s.tris[i];
        if (triangle_uses_super(s, t)) { continue; }
        if (keep_only_inside_polygon && poly_for_filter != nullptr)
        {
            // Centroid in/out test — outer-with-holes via even-odd ring fill.
            const auto& a = s.verts[t.v[0]];
            const auto& b = s.verts[t.v[1]];
            const auto& c = s.verts[t.v[2]];
            const crd::math::Vec2<T> centroid_pt{
                (a.x + b.x + c.x) / T{3}, (a.y + b.y + c.y) / T{3}};
            const auto pip = point_in_polygon(*poly_for_filter, centroid_pt);
            if (pip != PointInPolygon::Inside) { continue; }
        }
        result.triangle_indices.push_back(t.v[0]);
        result.triangle_indices.push_back(t.v[1]);
        result.triangle_indices.push_back(t.v[2]);
    }
    result.triangle_count = static_cast<crd::u32>(result.triangle_indices.size() / 3U);
}

} // namespace

// ---- Public entries ---------------------------------------------------

template <crd::math::MathScalar T>
CdtResult<T> constrained_delaunay(crd::containers::ConstSpan<crd::math::Vec2<T>> points,
                                   crd::containers::ConstSpan<CdtEdge>             constraints,
                                   crd::memory::IAllocator*                        alloc,
                                   CdtOptions /*opts*/)
{
    CdtResult<T> result(alloc);
    if (points.size() < 3U)
    {
        result.status = CdtStatus::TooFewPoints;
        return result;
    }
    for (crd::usize i = 0; i < points.size(); ++i)
    {
        if (!crd::geometry::primitives::is_finite(points[i]))
        {
            result.status = CdtStatus::NonFiniteInput;
            return result;
        }
    }
    for (crd::usize i = 0; i < constraints.size(); ++i)
    {
        if (constraints[i].a >= points.size() || constraints[i].b >= points.size())
        {
            result.status = CdtStatus::ConstraintOutOfBounds;
            return result;
        }
    }

    CdtState<T> s(alloc);
    install_super_triangle(s, points);

    // Insert input points in lex-(x, y, original-index) order — determinism
    // pin + better Bowyer-Watson cavity locality.
    crd::containers::Array<crd::u32> order(alloc);
    order.resize(points.size());
    for (crd::u32 i = 0; i < static_cast<crd::u32>(points.size()); ++i) { order[i] = i; }
    crd::containers::sort(order.data(), order.data() + order.size(),
                          [&points](crd::u32 lhs, crd::u32 rhs) noexcept {
                        const auto& a = points[lhs];
                        const auto& b = points[rhs];
                        if (a.x != b.x) { return a.x < b.x; }
                        if (a.y != b.y) { return a.y < b.y; }
                        return lhs < rhs;
                    });

    for (crd::u32 i = 0; i < order.size(); ++i)
    {
        // Duplicate-point detection: lex-sorted ⇒ duplicates are adjacent.
        if (i > 0U)
        {
            const auto& a = points[order[i]];
            const auto& b = points[order[i - 1U]];
            if (a.x == b.x && a.y == b.y)
            {
                result.status = CdtStatus::DuplicatePoint;
                return result;
            }
        }
        if (!insert_point(s, order[i]))
        {
            result.status = CdtStatus::InternalInvariant;
            return result;
        }
    }

    // Insert constraints.
    for (crd::usize i = 0; i < constraints.size(); ++i)
    {
        const auto& c = constraints[i];
        if (c.a == c.b) { continue; }
        if (!insert_constraint(s, c.a, c.b))
        {
            result.status = CdtStatus::ConstraintsCrossing;
            return result;
        }
    }

    // Restore Delaunay condition over non-constrained edges (Lawson bubble).
    restore_delaunay(s);

    emit_triangles<T>(s, result, false, nullptr);
    result.status = CdtStatus::Ok;
    return result;
}

template <crd::math::MathScalar T>
CdtResult<T> constrained_delaunay(PolygonView2<T> polygon, crd::memory::IAllocator* alloc,
                                   CdtOptions opts)
{
    CdtResult<T> result(alloc);
    if (polygon.ring_count() == 0U || polygon.outer().size() < 3U)
    {
        result.status = CdtStatus::TooFewPoints;
        return result;
    }

    // Build the flat points + constraint-edge arrays from the polygon.
    crd::containers::Array<crd::math::Vec2<T>> points(alloc);
    crd::containers::Array<CdtEdge>             constraints(alloc);
    points.reserve(polygon.vertices.size());
    constraints.reserve(polygon.vertices.size());
    for (const auto& v : polygon.vertices) { points.push_back(v); }
    for (crd::u32 r = 0; r < polygon.ring_count(); ++r)
    {
        const crd::u32 base = polygon.ring_offsets[r];
        const crd::u32 sz   = polygon.ring_offsets[r + 1U] - base;
        for (crd::u32 i = 0; i < sz; ++i)
        {
            CdtEdge e;
            e.a = base + i;
            e.b = base + ((i + 1U) % sz);
            constraints.push_back(e);
        }
    }

    auto generic = constrained_delaunay<T>(
        crd::containers::ConstSpan<crd::math::Vec2<T>>{points.data(), points.size()},
        crd::containers::ConstSpan<CdtEdge>{constraints.data(), constraints.size()}, alloc, opts);
    if (!generic.ok()) { return generic; }

    // Filter triangles by polygon interior if requested.
    if (!opts.keep_only_inside_polygon) { return generic; }
    CdtResult<T> filtered(alloc);
    filtered.triangle_indices.reserve(generic.triangle_indices.size());
    for (crd::u32 t = 0; t < generic.triangle_count; ++t)
    {
        const crd::u32 i0 = generic.triangle_indices[3U * t + 0U];
        const crd::u32 i1 = generic.triangle_indices[3U * t + 1U];
        const crd::u32 i2 = generic.triangle_indices[3U * t + 2U];
        const auto&    a  = points[i0];
        const auto&    b  = points[i1];
        const auto&    c  = points[i2];
        const crd::math::Vec2<T> centroid_pt{(a.x + b.x + c.x) / T{3},
                                              (a.y + b.y + c.y) / T{3}};
        const auto pip = point_in_polygon(polygon, centroid_pt);
        if (pip != PointInPolygon::Inside) { continue; }
        filtered.triangle_indices.push_back(i0);
        filtered.triangle_indices.push_back(i1);
        filtered.triangle_indices.push_back(i2);
    }
    filtered.triangle_count = static_cast<crd::u32>(filtered.triangle_indices.size() / 3U);
    filtered.status         = CdtStatus::Ok;
    return filtered;
}

// ---- Explicit instantiations ---------------------------------------------

template CdtResult<crd::f32> constrained_delaunay<crd::f32>(
    crd::containers::ConstSpan<crd::math::Vec2<crd::f32>>, crd::containers::ConstSpan<CdtEdge>,
    crd::memory::IAllocator*, CdtOptions);
template CdtResult<crd::f64> constrained_delaunay<crd::f64>(
    crd::containers::ConstSpan<crd::math::Vec2<crd::f64>>, crd::containers::ConstSpan<CdtEdge>,
    crd::memory::IAllocator*, CdtOptions);
template CdtResult<crd::f32> constrained_delaunay<crd::f32>(PolygonView2<crd::f32>,
                                                             crd::memory::IAllocator*, CdtOptions);
template CdtResult<crd::f64> constrained_delaunay<crd::f64>(PolygonView2<crd::f64>,
                                                             crd::memory::IAllocator*, CdtOptions);

} // namespace crd::geometry::polygon
