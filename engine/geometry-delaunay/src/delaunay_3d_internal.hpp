#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-delaunay — internal 3D Bowyer-Watson Delaunay core (v8c).
//
// Header-only (inline + templates). Mirror of `delaunay_2d_internal.hpp` but
// for 3D tetrahedralisation. Not shared with the 2D core — slot layout is
// different (4 verts/4 nbrs vs 3/3) and the face-table machinery has no
// 2D analog.
//
// **Internal slot** (D90):
//   `Tet { u32 v[4]; u32 nbr[4]; u8 alive; }` — `v[i]` is vertex id (input
//   index, or N+0/1/2/3 for super-tet); `nbr[i]` is the tet opposite v[i]
//   (sharing the face that omits v[i]), or `k_null_tet` if outer-boundary.
//   `static_assert(sizeof(Tet) <= 40)` pins layout.
//
// **Face table** (D92):
//   For a positively-oriented tet (v0, v1, v2, v3), the face opposite v[i]
//   is `(v[face_vertices[i][0]], v[face_vertices[i][1]], v[face_vertices[i][2]])`
//   ordered such that orient3d(face, v[i]) > 0. This lets us extract cavity
//   boundary faces in the SAME orientation across both bad-and-outer tets,
//   which makes the new tet (face_v0, face_v1, face_v2, q) automatically
//   positively oriented when q is on the cavity-interior side of the face.
//
//   face_vertices[0] = {1, 3, 2}  // opposite v0
//   face_vertices[1] = {0, 2, 3}  // opposite v1
//   face_vertices[2] = {0, 3, 1}  // opposite v2
//   face_vertices[3] = {0, 1, 2}  // opposite v3
//
//   Verified via orient3d sign analysis (transposition parity).
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

namespace crd::geometry::delaunay::detail3d
{

inline constexpr crd::u32 k_null_tet = std::numeric_limits<crd::u32>::max();

// Face table — for positively-oriented tet (v0, v1, v2, v3), the canonical
// outward-oriented face opposite v[i] uses these vertex permutations.
inline constexpr crd::u32 face_vertices[4][3] = {
    {1U, 3U, 2U}, // opposite v0
    {0U, 2U, 3U}, // opposite v1
    {0U, 3U, 1U}, // opposite v2
    {0U, 1U, 2U}, // opposite v3
};

struct Tet
{
    crd::u32 v[4]   = {0, 0, 0, 0};
    crd::u32 nbr[4] = {k_null_tet, k_null_tet, k_null_tet, k_null_tet};
    crd::u8  alive  = 0U;
};

static_assert(sizeof(Tet) <= 40U, "Tet slot exceeds 40 bytes — layout regression");

class TetPool
{
public:
    explicit TetPool(crd::memory::IAllocator* alloc) noexcept
      : m_pool(alloc), m_free(alloc)
    {
    }

    crd::u32 alloc_tet() noexcept
    {
        if (!m_free.empty())
        {
            const crd::u32 idx = m_free.back();
            m_free.pop_back();
            m_pool[idx]       = Tet{};
            m_pool[idx].alive = 1U;
            return idx;
        }
        const crd::u32 idx = static_cast<crd::u32>(m_pool.size());
        Tet            t{};
        t.alive = 1U;
        m_pool.push_back(t);
        return idx;
    }

    void free_tet(crd::u32 idx) noexcept
    {
        m_pool[idx]       = Tet{};
        m_pool[idx].alive = 0U;
        m_free.push_back(idx);
    }

    [[nodiscard]] crd::u32   pool_size() const noexcept { return static_cast<crd::u32>(m_pool.size()); }
    [[nodiscard]] Tet&       operator[](crd::u32 i) noexcept       { return m_pool[i]; }
    [[nodiscard]] const Tet& operator[](crd::u32 i) const noexcept { return m_pool[i]; }
    [[nodiscard]] bool       alive(crd::u32 i) const noexcept      { return m_pool[i].alive != 0U; }

private:
    crd::containers::Array<Tet>      m_pool;
    crd::containers::Array<crd::u32> m_free;
};

// Find which face index `i` of `t` corresponds to the canonical-ordered face
// (a, b, c). Returns 4 if no such face. Used during cavity neighbour
// rewiring — given a known outward face (a, b, c) emitted from one tet, find
// its slot in the adjacent outer tet so we can update outer.nbr[k] = new_tet.
inline crd::u32 find_face_with_vertices(const Tet& t, crd::u32 a, crd::u32 b, crd::u32 c) noexcept
{
    for (crd::u32 i = 0; i < 4U; ++i)
    {
        const crd::u32 fa = t.v[face_vertices[i][0]];
        const crd::u32 fb = t.v[face_vertices[i][1]];
        const crd::u32 fc = t.v[face_vertices[i][2]];
        // Compare as unordered (multiset of 3 vertices); the face is the
        // same triangle regardless of which CCW rotation each tet sees.
        // For matching, we sort the triple by index.
        crd::u32 s0 = fa, s1 = fb, s2 = fc;
        if (s0 > s1) { const crd::u32 tmp = s0; s0 = s1; s1 = tmp; }
        if (s1 > s2) { const crd::u32 tmp = s1; s1 = s2; s2 = tmp; }
        if (s0 > s1) { const crd::u32 tmp = s0; s0 = s1; s1 = tmp; }
        crd::u32 q0 = a, q1 = b, q2 = c;
        if (q0 > q1) { const crd::u32 tmp = q0; q0 = q1; q1 = tmp; }
        if (q1 > q2) { const crd::u32 tmp = q1; q1 = q2; q2 = tmp; }
        if (q0 > q1) { const crd::u32 tmp = q0; q0 = q1; q1 = tmp; }
        if (s0 == q0 && s1 == q1 && s2 == q2) { return i; }
    }
    return 4U;
}

template <crd::math::MathScalar T>
inline bool is_finite_vec(const crd::math::Vec3<T>& p) noexcept
{
    return p.x == p.x && p.y == p.y && p.z == p.z
           && p.x != std::numeric_limits<T>::infinity()
           && p.x != -std::numeric_limits<T>::infinity()
           && p.y != std::numeric_limits<T>::infinity()
           && p.y != -std::numeric_limits<T>::infinity()
           && p.z != std::numeric_limits<T>::infinity()
           && p.z != -std::numeric_limits<T>::infinity();
}

// Build 4 super-tetrahedron vertex positions at 1000× bbox scale, centred
// on the bbox centre. The ordering is chosen such that
// `orient3d(s0, s1, s2, s3) > 0` (positively oriented).
template <crd::math::MathScalar T>
void build_super_tet(crd::containers::ConstSpan<crd::math::Vec3<T>> pts,
                      crd::math::Vec3<T>&                            out_s0,
                      crd::math::Vec3<T>&                            out_s1,
                      crd::math::Vec3<T>&                            out_s2,
                      crd::math::Vec3<T>&                            out_s3)
{
    T xmin = pts[0].x, xmax = pts[0].x;
    T ymin = pts[0].y, ymax = pts[0].y;
    T zmin = pts[0].z, zmax = pts[0].z;
    for (crd::usize i = 1; i < pts.size(); ++i)
    {
        if (pts[i].x < xmin) { xmin = pts[i].x; }
        if (pts[i].x > xmax) { xmax = pts[i].x; }
        if (pts[i].y < ymin) { ymin = pts[i].y; }
        if (pts[i].y > ymax) { ymax = pts[i].y; }
        if (pts[i].z < zmin) { zmin = pts[i].z; }
        if (pts[i].z > zmax) { zmax = pts[i].z; }
    }
    const T cx = (xmin + xmax) * static_cast<T>(0.5);
    const T cy = (ymin + ymax) * static_cast<T>(0.5);
    const T cz = (zmin + zmax) * static_cast<T>(0.5);
    const T dx = xmax - xmin;
    const T dy = ymax - ymin;
    const T dz = zmax - zmin;
    T       maxd = dx > dy ? dx : dy;
    if (dz > maxd) { maxd = dz; }
    if (maxd <= static_cast<T>(0)) { maxd = static_cast<T>(1); }
    const T scale = maxd * static_cast<T>(1000);

    // Place 4 super-vertices symmetric about (cx, cy, cz). Ordering chosen
    // so orient3d(s0, s1, s2, s3) > 0 per Shewchuk's convention
    // (`orient3d > 0` iff the 4th point is BELOW the plane abc):
    //   s0 = (cx - 3·scale, cy - scale,    cz - scale)
    //   s1 = (cx,            cy + 3·scale, cz - scale)  // swapped vs s2
    //   s2 = (cx + 3·scale, cy - scale,    cz - scale)  // swapped vs s1
    //   s3 = (cx,            cy,            cz + 3·scale)
    // Base (s0, s1, s2) is CW viewed from +z (= CCW viewed from -z); s3 is
    // at +z. Per Shewchuk: orient3d > 0 iff s3 is below the (s0, s1, s2)
    // plane in CCW-from-the-back sense. The base normal under this ordering
    // points DOWN (-z), so s3 (at +z) is on the BELOW side → orient3d > 0.
    const T big = static_cast<T>(3) * scale;
    out_s0 = crd::math::Vec3<T>{cx - big, cy - scale, cz - scale};
    out_s1 = crd::math::Vec3<T>{cx,        cy + big,   cz - scale};
    out_s2 = crd::math::Vec3<T>{cx + big, cy - scale, cz - scale};
    out_s3 = crd::math::Vec3<T>{cx,        cy,         cz + big};
}

// Locate tet containing query point via jump-walk from `hint`. Returns the
// containing tet id, or k_null_tet on degenerate failure.
//
// For positively-oriented tet (v0, v1, v2, v3) and query q, q is INSIDE iff
// orient3d(face_i_of_tet_oriented_outward, q) <= 0 for all 4 faces. Sign
// convention: outward face has orient3d(face, v[i]) > 0 (the apex IS the
// outward direction). For q inside the tet, orient3d(face, q) ≤ 0
// (q is on the opposite side from the apex).
//
// Wait — let me re-derive. For face opposite v[i] with canonical CCW
// (face[0], face[1], face[2]) such that orient3d(face[0], face[1], face[2], v[i]) > 0:
//   - v[i] is on the POSITIVE side.
//   - q is on the SAME side as v[i] iff orient3d(face[0], face[1], face[2], q) > 0.
//   - q is INSIDE the tet iff q is on the same side as v[i] for ALL 4 faces,
//     i.e., orient3d(face_i, q) >= 0 for all i.
//   - If orient3d(face_i, q) < 0 for some i, q is on the OUTER side of face_i,
//     so we cross face_i (move to nbr[i]).
template <crd::math::MathScalar T>
crd::u32 locate_tet(const TetPool&                                    pool,
                     const crd::containers::Array<crd::math::Vec3<T>>& aug_pts,
                     crd::u32                                          hint,
                     const crd::math::Vec3<T>&                         q,
                     crd::u32                                          max_steps)
{
    crd::u32 cur = hint;
    for (crd::u32 step = 0; step < max_steps; ++step)
    {
        if (cur == k_null_tet) { return k_null_tet; }
        if (!pool.alive(cur)) { return k_null_tet; }
        const Tet& t = pool[cur];
        crd::u32 cross_face = 4U;
        for (crd::u32 i = 0; i < 4U; ++i)
        {
            const auto& fa = aug_pts[t.v[face_vertices[i][0]]];
            const auto& fb = aug_pts[t.v[face_vertices[i][1]]];
            const auto& fc = aug_pts[t.v[face_vertices[i][2]]];
            const T s = crd::geometry::primitives::orient3d(fa, fb, fc, q);
            if (s < static_cast<T>(0))
            {
                cross_face = i;
                break; // deterministic: lowest face index wins
            }
        }
        if (cross_face >= 4U) { return cur; } // inside
        cur = t.nbr[cross_face];
    }
    return k_null_tet;
}

// Cavity-build + re-tetrahedralise. Inserts vertex `q_idx` (with position
// `q_pos`) into the triangulation containing it (starting from
// `containing_tet`). Returns one of the new tet ids (to seed the next
// jump-walk hint), or k_null_tet on failure. Updates `out_cavity_max` with
// the cavity size if larger than existing.
template <crd::math::MathScalar T>
crd::u32 insert_point_3d(TetPool&                                          pool,
                          const crd::containers::Array<crd::math::Vec3<T>>& aug_pts,
                          crd::u32                                          q_idx,
                          const crd::math::Vec3<T>&                         q_pos,
                          crd::u32                                          containing_tet,
                          crd::memory::IAllocator*                          alloc,
                          crd::u32&                                          out_cavity_max,
                          bool&                                              out_invariant_failure)
{
    out_invariant_failure = false;

    // Phase 1: cavity expansion via BFS using Shewchuk Stage D `insphere`.
    crd::containers::Array<crd::u8> is_bad(alloc);
    is_bad.resize(pool.pool_size(), crd::u8{0});
    crd::containers::Array<crd::u32> queue(alloc);
    queue.push_back(containing_tet);
    is_bad[containing_tet] = 1U;
    crd::containers::Array<crd::u32> bad_tets(alloc);
    bad_tets.push_back(containing_tet);
    crd::u32 qi = 0;
    while (qi < queue.size())
    {
        const crd::u32 cur = queue[qi++];
        const Tet&     t   = pool[cur];
        for (crd::u32 k = 0; k < 4U; ++k)
        {
            const crd::u32 nbr = t.nbr[k];
            if (nbr == k_null_tet) { continue; }
            if (nbr >= is_bad.size()) { is_bad.resize(nbr + 1U, crd::u8{0}); }
            if (is_bad[nbr] != 0U) { continue; }
            if (!pool.alive(nbr)) { continue; }
            const Tet& tn = pool[nbr];
            const T s = crd::geometry::primitives::insphere(
                aug_pts[tn.v[0]], aug_pts[tn.v[1]], aug_pts[tn.v[2]], aug_pts[tn.v[3]], q_pos);
            if (s > static_cast<T>(0))
            {
                is_bad[nbr] = 1U;
                queue.push_back(nbr);
                bad_tets.push_back(nbr);
            }
        }
    }
    if (bad_tets.size() > out_cavity_max) { out_cavity_max = static_cast<crd::u32>(bad_tets.size()); }

    // Phase 2: collect cavity boundary faces (with their outer-neighbour
    // ids). A face of a bad tet is a cavity boundary if its neighbour-tet
    // is NOT bad.
    struct CavityFace
    {
        crd::u32 v0;
        crd::u32 v1;
        crd::u32 v2;
        crd::u32 outer_nbr; // tet on the OUTSIDE of the cavity (alive, not bad), or k_null_tet
    };
    crd::containers::Array<CavityFace> cavity(alloc);
    for (crd::u32 bi = 0; bi < bad_tets.size(); ++bi)
    {
        const crd::u32 ti = bad_tets[bi];
        const Tet&     t  = pool[ti];
        for (crd::u32 i = 0; i < 4U; ++i)
        {
            const crd::u32 nbr = t.nbr[i];
            const bool nbr_bad = (nbr != k_null_tet) && (nbr < is_bad.size()) && (is_bad[nbr] != 0U);
            if (nbr_bad) { continue; }
            CavityFace f{};
            f.v0        = t.v[face_vertices[i][0]];
            f.v1        = t.v[face_vertices[i][1]];
            f.v2        = t.v[face_vertices[i][2]];
            f.outer_nbr = nbr;
            cavity.push_back(f);
        }
    }

    // Defensive star-shape check (D91): for every cavity boundary face,
    // verify orient3d(face_v0, face_v1, face_v2, q) > 0. With Stage D
    // insphere this MUST hold for valid input; failure means input is
    // degenerate beyond what the predicates can resolve (return invariant
    // error rather than build a corrupt mesh).
    for (crd::u32 ci = 0; ci < cavity.size(); ++ci)
    {
        const auto& f = cavity[ci];
        const T s = crd::geometry::primitives::orient3d(
            aug_pts[f.v0], aug_pts[f.v1], aug_pts[f.v2], q_pos);
        if (s <= static_cast<T>(0))
        {
            out_invariant_failure = true;
            return k_null_tet;
        }
    }

    // Phase 3: free bad tets.
    for (crd::u32 bi = 0; bi < bad_tets.size(); ++bi)
    {
        pool.free_tet(bad_tets[bi]);
    }

    // Phase 4: re-tetrahedralise cavity by fanning new tets from q.
    // For each cavity face (v0, v1, v2, outer_nbr), allocate new tet
    // (v0, v1, v2, q) — automatically positively oriented (D91 verified
    // above). `nbr[3]` (face opposite q = the cavity boundary face) =
    // outer_nbr; update outer's neighbour pointer back to new tet.
    // nbr[0], nbr[1], nbr[2] (faces containing q) wired in Phase 5.
    crd::containers::Array<crd::u32> new_tets(alloc);
    new_tets.reserve(cavity.size());
    for (crd::u32 ci = 0; ci < cavity.size(); ++ci)
    {
        const auto&    f   = cavity[ci];
        const crd::u32 nti = pool.alloc_tet();
        Tet&           nt  = pool[nti];
        nt.v[0] = f.v0;
        nt.v[1] = f.v1;
        nt.v[2] = f.v2;
        nt.v[3] = q_idx;
        nt.nbr[3] = f.outer_nbr;
        if (f.outer_nbr != k_null_tet)
        {
            Tet&           outer = pool[f.outer_nbr];
            const crd::u32 fk    = find_face_with_vertices(outer, f.v0, f.v1, f.v2);
            if (fk < 4U) { outer.nbr[fk] = nti; }
        }
        new_tets.push_back(nti);
    }

    // Phase 5: wire nbr[0..2] between new tets. Each new tet has 3 faces
    // that contain q; each such face is shared with exactly one other new
    // tet (the one sharing the cavity-boundary edge). O(K²) edge match —
    // K typically 20-50 for 3D, so 400-2500 ops total per insertion.
    //
    // For new tet T = (v0, v1, v2, q):
    //   Face 0 (opposite v0) = (v1, q, v2) per face_vertices[0] = {1, 3, 2}.
    //       The two cavity-boundary vertices in this face are v1 and v2.
    //       Look for another new tet T' = (v0', v1', v2', q) where {v1', v2'}
    //       (as cavity vertices, ignoring q) = {v1, v2} as an unordered pair.
    //   Face 1 (opposite v1) = (v0, v2, q) per face_vertices[1] = {0, 2, 3}.
    //       Cavity vertices: v0, v2. Match T' with cavity edge {v0, v2}.
    //   Face 2 (opposite v2) = (v0, q, v1) per face_vertices[2] = {0, 3, 1}.
    //       Cavity vertices: v0, v1. Match T' with cavity edge {v0, v1}.
    for (crd::u32 i = 0; i < new_tets.size(); ++i)
    {
        Tet& ti = pool[new_tets[i]];
        // The 3 cavity-boundary edges of this new tet:
        //   face 0 edge = {v[1], v[2]}
        //   face 1 edge = {v[0], v[2]}
        //   face 2 edge = {v[0], v[1]}
        const crd::u32 edges[3][2] = {
            {ti.v[1], ti.v[2]},
            {ti.v[0], ti.v[2]},
            {ti.v[0], ti.v[1]},
        };
        for (crd::u32 fi = 0; fi < 3U; ++fi)
        {
            if (ti.nbr[fi] != k_null_tet) { continue; }
            crd::u32 e0 = edges[fi][0];
            crd::u32 e1 = edges[fi][1];
            if (e0 > e1) { const crd::u32 tmp = e0; e0 = e1; e1 = tmp; }
            for (crd::u32 j = 0; j < new_tets.size(); ++j)
            {
                if (i == j) { continue; }
                const Tet& tj = pool[new_tets[j]];
                // tj also has q as v[3]; its cavity-boundary edges are
                // pairs of {tj.v[0], tj.v[1], tj.v[2]}.
                const crd::u32 tj_edges[3][2] = {
                    {tj.v[1], tj.v[2]},
                    {tj.v[0], tj.v[2]},
                    {tj.v[0], tj.v[1]},
                };
                for (crd::u32 fj = 0; fj < 3U; ++fj)
                {
                    crd::u32 te0 = tj_edges[fj][0];
                    crd::u32 te1 = tj_edges[fj][1];
                    if (te0 > te1) { const crd::u32 tmp = te0; te0 = te1; te1 = tmp; }
                    if (te0 == e0 && te1 == e1)
                    {
                        ti.nbr[fi] = new_tets[j];
                        // Don't break outer loop — let tj match this in its turn.
                        break;
                    }
                }
                if (ti.nbr[fi] != k_null_tet) { break; }
            }
        }
    }

    return new_tets.empty() ? k_null_tet : new_tets[0];
}

} // namespace crd::geometry::delaunay::detail3d
