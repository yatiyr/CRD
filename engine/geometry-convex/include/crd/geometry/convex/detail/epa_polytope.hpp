#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-convex — EPA polytope substrate (Phase 3.1.7 v2c).
//
// The data structure EPA expands iteratively to find the Minkowski-difference
// face nearest to origin (= penetration vector). Lives in `detail::` because
// the public surface is `EpaResult<T>` + `epa_penetration<T,A,B>(...)` in
// `epa.hpp` — this header is the storage / face / silhouette engine.
//
// **Layout (all fixed-size stack storage, header-only template):**
//   - `EpaVertex<T>` carries the Minkowski-diff point `w` in A-local plus
//     the parallel A-support / B-support (in their own local frames) plus
//     the vertex_idx pair `(vidx_a, vidx_b)`. Same shape as a `GjkSimplex`
//     slot — EPA seeds itself from the GJK terminating simplex by copying
//     these slots.
//   - `EpaFace<T>` carries 3 vertex indices (CCW from outside) plus the
//     unit outward normal and the signed distance from origin to the face
//     plane (`distance = dot(normal, vertex)` for any vertex on the face;
//     `>= 0` when origin is on the interior side, which it always is once
//     the polytope encloses origin).
//
// **Capacity (PINNED for v2c):** 64 vertices, 128 faces. Well-formed inputs
// converge in 10-30 EPA iterations (Box2D's reference: median 12, P99 22),
// each adding 1 vertex and ~3-5 net faces. 64/128 is comfortably 2× the
// P99 envelope. On overflow, the polytope flags `failed_overflow == true`
// and the public driver returns `converged = false` with the sentinel
// output (`depth=0, normal=zero`). Callers MUST check `converged` before
// reading any field.
//
// **Silhouette algorithm (Catto 2010 GDC):**
// When adding a new support vertex `v*`:
//   1. Mark every face F whose plane has `v*` on the OUTSIDE side
//      (`dot(F.normal, v*.w - vertex_on_F) > 0`) as obsolete.
//   2. For each obsolete face F, walk its 3 directed edges (a,b), (b,c),
//      (c,a). For each edge:
//        - Find the adjacent face F' (the unique face containing the
//          REVERSED edge (b,a) — adjacency is implicit, not stored).
//        - If F' is ALSO obsolete: the edge is interior to the visible
//          region → discard.
//        - Else: the edge is on the silhouette → record (a,b) with v* as
//          the third vertex.
//   3. Replace the obsolete face set with new faces (a, b, v*) — CCW from
//      outside (correct by construction: (a,b) was CCW from F's outside,
//      and v* is on F's outside side, so (a,b,v*) is CCW from outside the
//      new face).
//
// Adjacency is recovered by scanning all faces (O(F) per edge → O(F·V) per
// expansion). At F ≤ 128 this is ~16k integer ops per expansion, ~1µs —
// acceptable. The "half-edge" alternative saves ~2× at the cost of more
// state and a much harder testing story; not justified at this slice's
// scale. v2c-perf-followup can revisit if EPA shows up in profiles.
//
// **Determinism pins:**
//   - `closest_face_idx()` returns the LOWEST face index among faces
//     within `k_distance_epsilon` of the minimum distance. Cross-platform
//     bit-exact tiebreak on symmetric configurations (cube-on-cube face-
//     to-face, etc.).
//   - Vertex de-duplication uses `(vidx_a, vidx_b)` integer compare — no
//     fuzzy `Vec3` comparison. New support whose `(vidx_a, vidx_b)` pair
//     is already in the polytope flags "stuck" and the driver terminates.
//   - Silhouette walk processes faces in index order; new faces are
//     appended in silhouette-edge order. Replay-equal across runs.
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/geometry/convex/support.hpp>
#include <crd/geometry/primitives/constants.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

#include <cmath>
#include <limits>

namespace crd::geometry::convex::detail
{
using crd::math::MathScalar;
using crd::math::Vec3;

// One vertex of the EPA polytope. Same payload shape as a `GjkSimplex` slot
// (the seed for EPA), plus `w` (Minkowski-difference point in A-local) is
// stored explicitly since EPA needs it on every face-plane test.
template <MathScalar T> struct EpaVertex
{
    Vec3<T> w{};         // Minkowski-difference point in A-local
    Vec3<T> w_a_local{}; // A-side support in A-local
    Vec3<T> w_b_local{}; // B-side support in B-local
    crd::u32 vidx_a{k_invalid_vertex};
    crd::u32 vidx_b{k_invalid_vertex};
};

// One face of the polytope. `v[0..2]` are indices into the vertex array,
// arranged CCW when viewed from OUTSIDE the polytope. `normal` is the unit
// outward normal; `distance` is the signed perpendicular distance from
// origin to the face plane (positive when origin is on the INSIDE side —
// which it always is once the polytope encloses origin).
template <MathScalar T> struct EpaFace
{
    crd::u8 v[3]{0, 0, 0};
    Vec3<T> normal{};
    T distance{0};
};

inline constexpr crd::usize k_epa_max_vertices = 64;
inline constexpr crd::usize k_epa_max_faces = 128;

template <MathScalar T> struct EpaPolytope
{
    EpaVertex<T> verts[k_epa_max_vertices]{};
    crd::usize vert_count = 0;
    EpaFace<T> faces[k_epa_max_faces]{};
    crd::usize face_count = 0;
    bool failed_overflow = false;

    // Add a vertex. Returns its index, or `k_epa_max_vertices` (sentinel) on
    // overflow (and sets `failed_overflow = true`).
    [[nodiscard]] crd::usize add_vertex(const EpaVertex<T>& v) noexcept
    {
        if (vert_count >= k_epa_max_vertices)
        {
            failed_overflow = true;
            return k_epa_max_vertices;
        }
        verts[vert_count] = v;
        return vert_count++;
    }

    // Add a face from 3 vertex indices, computing its outward normal + origin
    // distance. The caller is responsible for CCW-from-outside ordering;
    // this method does not verify orientation. Returns `false` on overflow
    // OR on a degenerate (zero-area) face.
    [[nodiscard]] bool add_face(crd::u8 i0, crd::u8 i1, crd::u8 i2) noexcept
    {
        if (face_count >= k_epa_max_faces)
        {
            failed_overflow = true;
            return false;
        }
        const Vec3<T>& a = verts[i0].w;
        const Vec3<T>& b = verts[i1].w;
        const Vec3<T>& c = verts[i2].w;
        const Vec3<T> ab = b - a;
        const Vec3<T> ac = c - a;
        const Vec3<T> n_raw = crd::math::cross(ab, ac);
        const T len_sq = crd::math::dot(n_raw, n_raw);
        if (!(len_sq > std::numeric_limits<T>::min()))
        {
            // Degenerate (collinear) face — silently skip. The polytope is
            // valid without it; the silhouette logic ignores skipped faces
            // because they were never added.
            return false;
        }
        const T inv_len = static_cast<T>(1) / static_cast<T>(std::sqrt(len_sq));
        const Vec3<T> n(n_raw.x * inv_len, n_raw.y * inv_len, n_raw.z * inv_len);
        EpaFace<T>& f = faces[face_count];
        f.v[0] = i0;
        f.v[1] = i1;
        f.v[2] = i2;
        f.normal = n;
        f.distance = crd::math::dot(n, a); // dot(n, any vertex of the face)
        ++face_count;
        return true;
    }

    // Initialize from a tetrahedron of 4 vertices. The 4 faces are emitted
    // in canonical order: opposite v[0], v[1], v[2], v[3]. Each face's CCW
    // ordering is ensured by flipping if the 4th vertex (the one opposite)
    // ends up on the OUTSIDE side of the candidate plane (i.e., normal points
    // toward 4th vertex — would mean it's actually the inward normal).
    //
    // Returns `false` if the tet is degenerate (zero volume) — caller must
    // handle (this happens only with bad starting input; well-formed GJK
    // terminating 4-simplices have non-zero volume by construction).
    [[nodiscard]] bool init_from_tetra(const EpaVertex<T>& v0, const EpaVertex<T>& v1, const EpaVertex<T>& v2,
                                       const EpaVertex<T>& v3) noexcept
    {
        vert_count = 0;
        face_count = 0;
        failed_overflow = false;
        (void)add_vertex(v0);
        (void)add_vertex(v1);
        (void)add_vertex(v2);
        (void)add_vertex(v3);

        // Check tet volume (must be non-zero). Signed volume is
        // dot(v1-v0, cross(v2-v0, v3-v0)) / 6.
        const Vec3<T> e01 = v1.w - v0.w;
        const Vec3<T> e02 = v2.w - v0.w;
        const Vec3<T> e03 = v3.w - v0.w;
        const T signed_vol6 = crd::math::dot(e01, crd::math::cross(e02, e03));
        if (!(signed_vol6 * signed_vol6 > std::numeric_limits<T>::min()))
        {
            return false; // degenerate tet
        }
        // Build 4 candidate faces; flip orientation per-face to ensure CCW
        // from outside (i.e., 4th vertex on the negative side of the face
        // normal). Each face is opposite one vertex.
        struct FaceSpec
        {
            crd::u8 a, b, c, opp;
        };
        const FaceSpec specs[4] = {
            {1, 2, 3, 0}, // opposite v0
            {0, 3, 2, 1}, // opposite v1
            {0, 1, 3, 2}, // opposite v2
            {0, 2, 1, 3}  // opposite v3
        };
        for (const FaceSpec& spec : specs)
        {
            // Try the given order first.
            const Vec3<T>& va = verts[spec.a].w;
            const Vec3<T>& vb = verts[spec.b].w;
            const Vec3<T>& vc = verts[spec.c].w;
            const Vec3<T>& vopp = verts[spec.opp].w;
            const Vec3<T> ab = vb - va;
            const Vec3<T> ac = vc - va;
            const Vec3<T> n_raw = crd::math::cross(ab, ac);
            const T side = crd::math::dot(n_raw, vopp - va);
            // Outward normal means `vopp` is on the negative side: side < 0.
            // If side > 0, flip vertex order (swap b and c) to invert normal.
            bool ok;
            if (side < static_cast<T>(0))
            {
                ok = add_face(spec.a, spec.b, spec.c);
            }
            else
            {
                ok = add_face(spec.a, spec.c, spec.b);
            }
            // `add_face` may fail on degeneracy or overflow. For init_from
            // _tetra, fail-out propagates to the caller.
            if (!ok && failed_overflow)
            {
                return false;
            }
            // (Non-overflow degeneracy on init means the tet is degenerate;
            // caught by the signed_vol6 check above. We don't expect this
            // path.)
        }
        return face_count == 4;
    }

    // Find the index of the face with the smallest distance from origin.
    // Lowest face-index wins on tie (within `k_distance_epsilon`).
    // Returns `face_count` (sentinel = "no valid face") only if the polytope
    // has no faces — never the case after a successful `init_from_tetra`.
    [[nodiscard]] crd::usize closest_face_idx() const noexcept
    {
        if (face_count == 0)
        {
            return face_count;
        }
        crd::usize best_idx = 0;
        T best_dist = faces[0].distance;
        const T eps = crd::geometry::primitives::k_distance_epsilon<T>();
        for (crd::usize i = 1; i < face_count; ++i)
        {
            const T d = faces[i].distance;
            // Strictly less → new best. Within eps and lower index → keep
            // the existing (lower-index) best. This is automatic because
            // we iterate in increasing-index order and update only on
            // `d < best_dist - eps` (strict improvement past the tie band).
            if (d < best_dist - eps)
            {
                best_idx = i;
                best_dist = d;
            }
        }
        return best_idx;
    }

    // Check whether a candidate vertex `(vidx_a, vidx_b)` pair is already
    // present in the polytope. Used to detect "no further progress" in the
    // EPA driver — O(N) integer compare, no fuzzy floats.
    [[nodiscard]] bool vertex_already_present(crd::u32 vidx_a_q, crd::u32 vidx_b_q) const noexcept
    {
        // Analytic shapes report `k_invalid_vertex`; we cannot use index-
        // match termination on those.
        if (vidx_a_q == k_invalid_vertex || vidx_b_q == k_invalid_vertex)
        {
            return false;
        }
        for (crd::usize i = 0; i < vert_count; ++i)
        {
            if (verts[i].vidx_a == vidx_a_q && verts[i].vidx_b == vidx_b_q)
            {
                return true;
            }
        }
        return false;
    }

    // Expand the polytope by adding `new_v` (which must be OUTSIDE the
    // current polytope). Walks visible faces, finds silhouette edges,
    // deletes visible faces, adds new faces connecting silhouette to
    // `new_v`. Returns `false` on overflow.
    //
    // Caller must have already called `add_vertex(new_v)` and pass the
    // resulting index as `new_v_idx`.
    [[nodiscard]] bool expand_silhouette(crd::u8 new_v_idx) noexcept
    {
        const Vec3<T>& new_w = verts[new_v_idx].w;

        // Mark faces visible from new_w. A face is visible iff new_w is on
        // the outside-side of its plane: `dot(normal, new_w) > distance`.
        bool visible[k_epa_max_faces] = {};
        for (crd::usize i = 0; i < face_count; ++i)
        {
            visible[i] = (crd::math::dot(faces[i].normal, new_w) > faces[i].distance);
        }

        // Collect silhouette edges: directed edges of visible faces whose
        // adjacent (reversed-edge) face is NOT visible.
        //
        // Edge storage: pairs of vertex indices. With faces ≤ 128, silhouette
        // edges ≤ ~256 in worst case (each visible face contributes up to 3,
        // each interior edge eats 2 from different faces → ~half remain).
        // Fixed-size buffer with overflow check.
        constexpr crd::usize k_max_silhouette = 3 * k_epa_max_faces; // upper bound
        crd::u8 silhouette_a[k_max_silhouette];
        crd::u8 silhouette_b[k_max_silhouette];
        crd::usize sil_count = 0;

        for (crd::usize i = 0; i < face_count; ++i)
        {
            if (!visible[i])
            {
                continue;
            }
            const EpaFace<T>& f = faces[i];
            for (int e = 0; e < 3; ++e)
            {
                const crd::u8 a = f.v[e];
                const crd::u8 b = f.v[(e + 1) % 3];
                // Find adjacent face (the one containing the reversed edge (b, a)).
                bool adjacent_visible = false;
                bool adjacent_found = false;
                for (crd::usize j = 0; j < face_count; ++j)
                {
                    if (j == i)
                    {
                        continue; // don't compare a face against itself
                    }
                    const EpaFace<T>& fj = faces[j];
                    for (int ej = 0; ej < 3; ++ej)
                    {
                        if (fj.v[ej] == b && fj.v[(ej + 1) % 3] == a)
                        {
                            adjacent_found = true;
                            adjacent_visible = visible[j];
                            break;
                        }
                    }
                    if (adjacent_found)
                    {
                        break;
                    }
                }
                (void)adjacent_found; // silhouette-or-boundary logic below uses adjacent_visible only
                // (a, b) is on silhouette iff its adjacent face is NOT
                // visible. (If no adjacent found, the polytope is degenerate
                // — boundary edge; treat as silhouette so we still connect.)
                if (!adjacent_visible)
                {
                    if (sil_count >= k_max_silhouette)
                    {
                        failed_overflow = true;
                        return false;
                    }
                    silhouette_a[sil_count] = a;
                    silhouette_b[sil_count] = b;
                    ++sil_count;
                }
            }
        }

        // Compact: remove visible faces, keep non-visible in place.
        crd::usize write = 0;
        for (crd::usize i = 0; i < face_count; ++i)
        {
            if (!visible[i])
            {
                if (write != i)
                {
                    faces[write] = faces[i];
                }
                ++write;
            }
        }
        face_count = write;

        // Append new faces, one per silhouette edge: (a, b, new_v_idx) is
        // CCW from outside because (a, b) was CCW from the deleted-face's
        // outside, and new_v_idx is on that same outside side.
        for (crd::usize i = 0; i < sil_count; ++i)
        {
            if (!add_face(silhouette_a[i], silhouette_b[i], new_v_idx))
            {
                if (failed_overflow)
                {
                    return false;
                }
                // Non-overflow degeneracy: skip the face (polytope remains
                // valid, just slightly smaller surface). Common when the
                // silhouette edge is collinear with new_v_idx.
            }
        }
        return true;
    }
};

} // namespace crd::geometry::convex::detail
