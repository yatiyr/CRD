#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-delaunay — internal Bowyer-Watson Delaunay core (v8a + v8b).
//
// Shared between:
//   - `delaunay_2d.cpp`         (v8a lex-sort incremental — exact-order BW)
//   - `delaunay_2d_hilbert.cpp` (v8b Hilbert-sort spatial-locality BW)
//
// Header-only (inline + templates) so each TU emits its own instantiations.
// Pattern matches `engine/geometry-bvh/src/bvh_build_internal.hpp` (the v1f
// parallel-build slice's shared serial kernels). Two distinct public entries
// share the same internal Bowyer-Watson body; the only difference is the
// insertion-order strategy.
//
// **Why share**: keeps the empty-circumcircle invariant, cavity-BFS,
// neighbour-rewiring, and super-triangle handling in ONE place. A fix to
// any of them lands in both algorithms simultaneously. Pinned by ADR-0076
// §23 D~84 at v8-close.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/primitives/predicates.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include <limits>

namespace crd::geometry::delaunay::detail
{

inline constexpr crd::u32 k_null_tri = std::numeric_limits<crd::u32>::max();

struct Tri
{
    crd::u32 v[3]   = {0, 0, 0};
    crd::u32 nbr[3] = {k_null_tri, k_null_tri, k_null_tri};
    crd::u8  alive  = 0U;
};

class TriPool
{
public:
    explicit TriPool(crd::memory::IAllocator* alloc) noexcept
      : m_pool(alloc), m_free(alloc)
    {
    }

    crd::u32 alloc_tri() noexcept
    {
        if (!m_free.empty())
        {
            const crd::u32 idx = m_free.back();
            m_free.pop_back();
            m_pool[idx]       = Tri{};
            m_pool[idx].alive = 1U;
            return idx;
        }
        const crd::u32 idx = static_cast<crd::u32>(m_pool.size());
        Tri            t{};
        t.alive = 1U;
        m_pool.push_back(t);
        return idx;
    }

    void free_tri(crd::u32 idx) noexcept
    {
        m_pool[idx]       = Tri{};
        m_pool[idx].alive = 0U;
        m_free.push_back(idx);
    }

    [[nodiscard]] crd::u32   pool_size() const noexcept { return static_cast<crd::u32>(m_pool.size()); }
    [[nodiscard]] Tri&       operator[](crd::u32 i) noexcept       { return m_pool[i]; }
    [[nodiscard]] const Tri& operator[](crd::u32 i) const noexcept { return m_pool[i]; }
    [[nodiscard]] bool       alive(crd::u32 i) const noexcept      { return m_pool[i].alive != 0U; }

private:
    crd::containers::Array<Tri>      m_pool;
    crd::containers::Array<crd::u32> m_free;
};

inline crd::u32 edge_index(const Tri& t, crd::u32 a, crd::u32 b) noexcept
{
    for (crd::u32 k = 0; k < 3U; ++k)
    {
        if (t.v[k] == a && t.v[(k + 1U) % 3U] == b) { return k; }
    }
    return 3U;
}

template <crd::math::MathScalar T>
inline bool is_finite_vec(const crd::math::Vec2<T>& p) noexcept
{
    return p.x == p.x && p.y == p.y // NaN check
           && p.x != std::numeric_limits<T>::infinity()
           && p.x != -std::numeric_limits<T>::infinity()
           && p.y != std::numeric_limits<T>::infinity()
           && p.y != -std::numeric_limits<T>::infinity();
}

// Build 3 super-triangle vertex positions at 1000× bbox scale, centred on
// the bbox centre.
template <crd::math::MathScalar T>
void build_super_triangle(crd::containers::ConstSpan<crd::math::Vec2<T>> pts,
                           crd::math::Vec2<T>&                            out_s0,
                           crd::math::Vec2<T>&                            out_s1,
                           crd::math::Vec2<T>&                            out_s2)
{
    T xmin = pts[0].x, xmax = pts[0].x, ymin = pts[0].y, ymax = pts[0].y;
    for (crd::usize i = 1; i < pts.size(); ++i)
    {
        if (pts[i].x < xmin) { xmin = pts[i].x; }
        if (pts[i].x > xmax) { xmax = pts[i].x; }
        if (pts[i].y < ymin) { ymin = pts[i].y; }
        if (pts[i].y > ymax) { ymax = pts[i].y; }
    }
    const T cx = (xmin + xmax) * static_cast<T>(0.5);
    const T cy = (ymin + ymax) * static_cast<T>(0.5);
    const T dx = xmax - xmin;
    const T dy = ymax - ymin;
    T       maxd = dx > dy ? dx : dy;
    if (maxd <= static_cast<T>(0)) { maxd = static_cast<T>(1); }
    const T scale = maxd * static_cast<T>(1000);
    out_s0 = crd::math::Vec2<T>{cx - static_cast<T>(3) * scale, cy - scale};
    out_s1 = crd::math::Vec2<T>{cx + static_cast<T>(3) * scale, cy - scale};
    out_s2 = crd::math::Vec2<T>{cx,                              cy + static_cast<T>(3) * scale};
}

// Locate triangle containing query point via jump-walk from `hint`.
// Returns the containing triangle id, or k_null_tri on degenerate failure.
template <crd::math::MathScalar T>
crd::u32 locate_triangle(const TriPool&                                    pool,
                          const crd::containers::Array<crd::math::Vec2<T>>& aug_pts,
                          crd::u32                                          hint,
                          const crd::math::Vec2<T>&                         q,
                          crd::u32                                          max_steps)
{
    crd::u32 cur = hint;
    for (crd::u32 step = 0; step < max_steps; ++step)
    {
        if (cur == k_null_tri) { return k_null_tri; }
        if (!pool.alive(cur)) { return k_null_tri; }
        const Tri&  t = pool[cur];
        const auto& a = aug_pts[t.v[0]];
        const auto& b = aug_pts[t.v[1]];
        const auto& c = aug_pts[t.v[2]];
        const T     s0 = crd::geometry::primitives::orient2d(a, b, q);
        const T     s1 = crd::geometry::primitives::orient2d(b, c, q);
        const T     s2 = crd::geometry::primitives::orient2d(c, a, q);
        if (s0 >= static_cast<T>(0) && s1 >= static_cast<T>(0) && s2 >= static_cast<T>(0))
        {
            return cur;
        }
        crd::u32 cross_edge = 3U;
        if (s0 < static_cast<T>(0)) { cross_edge = 0U; }
        else if (s1 < static_cast<T>(0)) { cross_edge = 1U; }
        else if (s2 < static_cast<T>(0)) { cross_edge = 2U; }
        if (cross_edge >= 3U) { return cur; }
        cur = t.nbr[cross_edge];
    }
    return k_null_tri;
}

// Cavity-build + re-triangulate. Inserts vertex `q_idx` (with position
// `q_pos`) into the triangulation containing it (starting from
// `containing_tri`). Returns one of the new triangle ids (to seed the next
// jump-walk hint), or k_null_tri on failure.
template <crd::math::MathScalar T>
crd::u32 insert_point(TriPool&                                          pool,
                       const crd::containers::Array<crd::math::Vec2<T>>& aug_pts,
                       crd::u32                                          q_idx,
                       const crd::math::Vec2<T>&                         q_pos,
                       crd::u32                                          containing_tri,
                       crd::memory::IAllocator*                          alloc)
{
    // Phase 1: cavity expansion via BFS (Shewchuk `incircle`).
    crd::containers::Array<crd::u8>  is_bad(alloc);
    is_bad.resize(pool.pool_size(), crd::u8{0});
    crd::containers::Array<crd::u32> queue(alloc);
    queue.push_back(containing_tri);
    is_bad[containing_tri] = 1U;
    crd::containers::Array<crd::u32> bad_tris(alloc);
    bad_tris.push_back(containing_tri);
    crd::u32 qi = 0;
    while (qi < queue.size())
    {
        const crd::u32 cur = queue[qi++];
        const Tri&     t   = pool[cur];
        for (crd::u32 k = 0; k < 3U; ++k)
        {
            const crd::u32 nbr = t.nbr[k];
            if (nbr == k_null_tri) { continue; }
            if (nbr >= is_bad.size()) { is_bad.resize(nbr + 1U, crd::u8{0}); }
            if (is_bad[nbr] != 0U) { continue; }
            if (!pool.alive(nbr)) { continue; }
            const Tri& tn = pool[nbr];
            const T s = crd::geometry::primitives::incircle(
                aug_pts[tn.v[0]], aug_pts[tn.v[1]], aug_pts[tn.v[2]], q_pos);
            if (s > static_cast<T>(0))
            {
                is_bad[nbr] = 1U;
                queue.push_back(nbr);
                bad_tris.push_back(nbr);
            }
        }
    }

    // Phase 2: collect cavity boundary edges.
    struct CavityEdge
    {
        crd::u32 a;
        crd::u32 b;
        crd::u32 outer_nbr;
    };
    crd::containers::Array<CavityEdge> cavity(alloc);
    for (crd::u32 bi = 0; bi < bad_tris.size(); ++bi)
    {
        const crd::u32 ti = bad_tris[bi];
        const Tri&     t  = pool[ti];
        for (crd::u32 k = 0; k < 3U; ++k)
        {
            const crd::u32 nbr     = t.nbr[k];
            const bool     nbr_bad = (nbr != k_null_tri) && (nbr < is_bad.size()) && (is_bad[nbr] != 0U);
            if (nbr_bad) { continue; }
            CavityEdge e{};
            e.a         = t.v[k];
            e.b         = t.v[(k + 1U) % 3U];
            e.outer_nbr = nbr;
            cavity.push_back(e);
        }
    }

    // Phase 3: free bad triangles.
    for (crd::u32 bi = 0; bi < bad_tris.size(); ++bi)
    {
        pool.free_tri(bad_tris[bi]);
    }

    // Phase 4: re-triangulate cavity by fanning new triangles from q.
    crd::containers::Array<crd::u32> new_tris(alloc);
    new_tris.reserve(cavity.size());
    for (crd::u32 ci = 0; ci < cavity.size(); ++ci)
    {
        const auto&    e   = cavity[ci];
        const crd::u32 nti = pool.alloc_tri();
        Tri&           nt  = pool[nti];
        nt.v[0]   = e.a;
        nt.v[1]   = e.b;
        nt.v[2]   = q_idx;
        nt.nbr[0] = e.outer_nbr;
        if (e.outer_nbr != k_null_tri)
        {
            Tri&           outer = pool[e.outer_nbr];
            const crd::u32 ek    = edge_index(outer, e.b, e.a);
            if (ek < 3U) { outer.nbr[ek] = nti; }
        }
        new_tris.push_back(nti);
    }

    // Phase 5: wire nbr[1] and nbr[2] between new triangles (O(K²) edge match).
    for (crd::u32 i = 0; i < new_tris.size(); ++i)
    {
        Tri&           ti   = pool[new_tris[i]];
        const crd::u32 ti_a = ti.v[0];
        const crd::u32 ti_b = ti.v[1];
        if (ti.nbr[2] == k_null_tri)
        {
            for (crd::u32 j = 0; j < new_tris.size(); ++j)
            {
                if (i == j) { continue; }
                const Tri& tj = pool[new_tris[j]];
                if (tj.v[1] == ti_a && tj.v[2] == q_idx)
                {
                    ti.nbr[2] = new_tris[j];
                    break;
                }
            }
        }
        if (ti.nbr[1] == k_null_tri)
        {
            for (crd::u32 j = 0; j < new_tris.size(); ++j)
            {
                if (i == j) { continue; }
                const Tri& tj = pool[new_tris[j]];
                if (tj.v[0] == ti_b && tj.v[2] == q_idx)
                {
                    ti.nbr[1] = new_tris[j];
                    break;
                }
            }
        }
    }

    return new_tris.empty() ? k_null_tri : new_tris[0];
}

} // namespace crd::geometry::delaunay::detail
