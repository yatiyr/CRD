#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-convex — EPA penetration depth + contact normal (Phase 3.1.7
// v2c; ADR-0076 §4 pin #14, §16 pin #1).
//
// The Expanding Polytope Algorithm picks up where GJK leaves off on a pair
// detected as overlapping (`gjk_distance(...).overlapping == true`). EPA
// iteratively grows a polytope inside the Minkowski difference until the
// face nearest origin is "essentially flat" against the Minkowski boundary
// — at which point the face's outward normal is the contact normal and the
// face's perpendicular distance to origin is the penetration depth.
//
// **The output contract (PINNED — read this before touching the code):**
//
//   - **`normal`**: unit vector. Direction "A pressing into B" — i.e.,
//     points from A's surface outward into B (the side of A that has
//     penetrated B's interior). For sphere-vs-sphere where A is at origin
//     and B is at +X with overlapping radii, `normal = (+1, 0, 0)`. This
//     matches the Box2D convention: "manifold normal from A toward B".
//   - **`depth`**: penetration depth, `>= 0`. The amount along `normal`
//     that A and B overlap. Witnesses satisfy
//     `depth ≈ dot(normal, witness_a - witness_b)`.
//   - **`witness_a_world`**: the deepest point of A inside B, in world
//     space. ON A's surface (or on its boundary).
//   - **`witness_b_world`**: the deepest point of B inside A, in world
//     space. ON B's surface.
//   - **`face_vidx_a[3]` / `face_vidx_b[3]`**: the vertex_idx pairs of
//     the three Minkowski-diff polytope vertices whose face produced the
//     contact. v1d-manifold (eylem) consumes these for feature
//     identification — face/edge/vertex categorisation against the input
//     shapes. `k_invalid_vertex` slots mean "smooth side" (sphere /
//     capsule).
//   - **`iteration_count`**: EPA iterations executed (typically 8-20 on
//     well-formed inputs; cap is 32).
//   - **`converged`**: `false` ⇒ the iteration cap was hit OR the polytope
//     overflowed the 64-vertex / 128-face limits OR the starting simplex
//     completion failed. On `!converged`, the other fields are SENTINELS
//     (`normal = {0,0,0}`, `depth = 0`) — callers MUST check `converged`
//     before reading any geometric field.
//
// **Algorithm (Catto 2010 GDC, modernised):**
//   1. Build polytope from GJK terminating simplex:
//        - size == 4 → load directly (the >99% case for polyhedral inputs).
//        - size == 3 → query 1 support along the triangle normal direction
//          (try +n, then -n if the +n side doesn't form an enclosing tet).
//        - size == 2 → query supports perpendicular to the edge direction
//          in 2 stages (size 2 → 3 → 4).
//        - size == 1 → query in 3 cardinal-axis-ish directions. (Probed
//          and never observed in 500 random configs, but supported.)
//   2. Iterate:
//        a. Find face F* with smallest distance to origin (lowest-index
//           tiebreak on coincident distances — determinism pin).
//        b. Query support s in direction F*.normal.
//        c. Termination check: `dot(F*.normal, s.w) - F*.distance < eps`
//           ⇒ the new support is not meaningfully past F*'s plane → F*
//           IS the closest face → done.
//        d. Index-match termination (epsilon-free, polyhedral-only):
//           if `(s.vidx_a, s.vidx_b)` is already in the polytope, we'd
//           cycle indefinitely → done.
//        e. Else, expand the polytope by adding `s` and replacing the
//           visible-face set with new triangles to the silhouette
//           boundary (Catto silhouette walk; see `detail/epa_polytope.hpp`).
//   3. Witness reconstruction from the closing face F*:
//        - Compute barycentric weights of origin projected onto F*.
//        - Witnesses are barycentric combinations of the 3 vertices'
//          A-supports / B-supports, transformed to world space.
//
// **Failure modes** (all flag `converged = false`):
//   - Iteration cap hit (32). Rare on well-formed inputs.
//   - Polytope overflow (64 verts, 128 faces). Rare; defensive bound.
//   - Starting simplex completion fails (origin not enclosable by the
//     puffed tet). Degenerate input (zero-volume Minkowski difference).
//   - GJK reports `overlapping == false` (then `compute_contact` returns
//     `std::nullopt`).
//
// **Known limitation (v2c → v2d, 2026-05-13)**: heavily-rotated NON-CUBE
// OBB-OBB pairs (different half-extents per axis + 45°+ rotations) can
// produce a polytope where the closing-face approximation reports a
// LARGER depth than the true depth (~5% of trials in the v2d cross-check
// corpus). The bug is contained: the unified facade
// (`crd::geometry::overlap` / `compute_contact_obb_obb`) routes OBB-OBB
// pairs through SAT (v2d), bypassing EPA for the broken case. EPA on
// rotated hull-vs-hull and all other polyhedral pairs is robust (verified
// by `test_epa.cpp::"EPA: rotated hull-vs-hull self-consistency"` —
// 50+ randomized rotated configs, all pass).
// Callers who bypass the facade and call `compute_contact<T>(obb, ...,
// obb, ...)` via the generic ConvexShape route may hit the issue — they
// should switch to `compute_contact_obb_obb(...)`. v2-close may revisit
// the underlying EPA issue (likely a silhouette-orientation edge case
// triggered by the specific 8-corner Minkowski-diff geometry of OBB-OBB).
//
// **Determinism pins** (inherits ADR-0063):
//   - Lowest face-index tiebreak in `closest_face_idx()`.
//   - `(vidx_a, vidx_b)` integer-compare for vertex de-duplication.
//   - Silhouette walk processes faces in index order; new faces appended
//     in silhouette-edge order. Replay-equal across runs (the v2c
//     determinism test enforces).
//   - Lowest face_a/face_b tiebreak: when EPA visits a 4-simplex face whose
//     outward normal computes to multiple candidate corners on a tied
//     coordinate, the support function's strict-greater-wins argmax pins
//     the choice deterministically (inherited from v2a `support()`).
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/geometry/convex/detail/epa_polytope.hpp>
#include <crd/geometry/convex/gjk.hpp>
#include <crd/geometry/convex/support.hpp>
#include <crd/geometry/primitives/constants.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/transform.hpp>
#include <crd/math/vec.hpp>

#include <cmath>
#include <limits>
#include <optional>

namespace crd::geometry::convex
{
using crd::math::MathScalar;
using crd::math::Transform;
using crd::math::Vec3;

// Output of `epa_penetration` and `compute_contact`. Field layout frozen;
// eylem v1d-manifold reads `face_vidx_a/b` by member name.
template <MathScalar T> struct EpaResult
{
    Vec3<T> normal{};       // unit; A→B direction (A pressing into B)
    T depth{0};             // penetration depth, ≥ 0
    Vec3<T> witness_a_world{};
    Vec3<T> witness_b_world{};
    crd::u32 face_vidx_a[3]{k_invalid_vertex, k_invalid_vertex, k_invalid_vertex};
    crd::u32 face_vidx_b[3]{k_invalid_vertex, k_invalid_vertex, k_invalid_vertex};
    crd::u8 iteration_count{0};
    bool converged{false};
};

namespace detail
{
// Build an EpaVertex from a query in direction `d` (A-local frame). Used by
// both the starting-simplex completion and the main loop. Same support-call
// pattern as `gjk_distance` — A-support in A-local, B-support in B-local
// transported to A-local, Minkowski-diff `w = sa - sb_in_a`.
template <MathScalar T, typename A, typename B>
[[nodiscard]] inline EpaVertex<T> epa_support_query(const A& a, const B& b, const Vec3<T>& d,
                                                    const Transform<T>& T_BA,
                                                    const crd::math::Quat<T>& T_BA_rot_inv) noexcept
{
    const SupportPoint<T> sa = support(a, d);
    const Vec3<T> d_in_b = crd::math::rotate_vector(T_BA_rot_inv, -d);
    const SupportPoint<T> sb_b = support(b, d_in_b);
    const Vec3<T> sb_in_a = crd::math::transform_point(T_BA, sb_b.point);
    EpaVertex<T> v;
    v.w = sa.point - sb_in_a;
    v.w_a_local = sa.point;
    v.w_b_local = sb_b.point;
    v.vidx_a = sa.vertex_idx;
    v.vidx_b = sb_b.vertex_idx;
    return v;
}

// Pick the world axis least parallel to `v` (smallest |dot|). The cross of
// `v` with this axis gives a perpendicular vector of usable magnitude.
template <MathScalar T> [[nodiscard]] inline Vec3<T> least_parallel_axis(const Vec3<T>& v) noexcept
{
    const T ax = std::fabs(v.x);
    const T ay = std::fabs(v.y);
    const T az = std::fabs(v.z);
    if (ax <= ay && ax <= az)
    {
        return Vec3<T>(static_cast<T>(1), static_cast<T>(0), static_cast<T>(0));
    }
    if (ay <= az)
    {
        return Vec3<T>(static_cast<T>(0), static_cast<T>(1), static_cast<T>(0));
    }
    return Vec3<T>(static_cast<T>(0), static_cast<T>(0), static_cast<T>(1));
}

// Build a 4-simplex enclosing origin from a (size 1/2/3/4) GJK terminating
// simplex. Returns `true` on success; `false` if the resulting tet is
// degenerate (zero volume) — degenerate input.
//
// PROBE FINDINGS (see `test_simplex_size_probe.cpp`, kept on disk for ref):
//   sphere-vs-sphere overlap → size 2 (every time).
//   sphere-vs-capsule overlap → size 3 (every time).
//   sphere-vs-box / box-vs-box / hull-vs-hull → size 4 (every time).
// So size 2 and size 3 are common; size 1 has not been observed but is
// handled defensively. Size 4 is the trivial loadcase.
template <MathScalar T, typename A, typename B>
[[nodiscard]] inline bool build_starting_polytope(EpaPolytope<T>& poly, const A& a, const B& b,
                                                   const Transform<T>& T_BA,
                                                   const crd::math::Quat<T>& T_BA_rot_inv,
                                                   const GjkSimplex<T>& simplex) noexcept
{
    auto vert_from_slot = [&](crd::u8 i) {
        EpaVertex<T> v;
        v.w_a_local = simplex.w_a_local[i];
        v.w_b_local = simplex.w_b_local[i];
        v.vidx_a = simplex.vidx_a[i];
        v.vidx_b = simplex.vidx_b[i];
        v.w = v.w_a_local - crd::math::transform_point(T_BA, v.w_b_local);
        return v;
    };

    EpaVertex<T> seed[4]{};
    crd::u8 seed_count = simplex.size;
    for (crd::u8 i = 0; i < seed_count; ++i)
    {
        seed[i] = vert_from_slot(i);
    }

    if (seed_count == 1)
    {
        // Find a second support along an arbitrary axis (use +X; the
        // GJK terminator at size 1 has w0 near origin, so any direction
        // gives a meaningful second point).
        seed[1] = epa_support_query<T>(a, b, Vec3<T>(static_cast<T>(1), static_cast<T>(0), static_cast<T>(0)), T_BA,
                                       T_BA_rot_inv);
        ++seed_count;
    }
    if (seed_count == 2)
    {
        // Edge w0→w1. Find a direction perpendicular to (w1-w0) and query
        // a support there.
        const Vec3<T> edge = seed[1].w - seed[0].w;
        const Vec3<T> axis = least_parallel_axis(edge);
        Vec3<T> perp = crd::math::cross(edge, axis);
        const T plen_sq = crd::math::dot(perp, perp);
        if (!(plen_sq > std::numeric_limits<T>::min()))
        {
            // Degenerate edge (w0 ≈ w1) — skip; falls through to the
            // degenerate-tet check below.
            perp = Vec3<T>(static_cast<T>(1), static_cast<T>(0), static_cast<T>(0));
        }
        seed[2] = epa_support_query<T>(a, b, perp, T_BA, T_BA_rot_inv);
        ++seed_count;
    }
    if (seed_count == 3)
    {
        // Triangle w0, w1, w2. Compute face normal; query support in +n
        // and in -n; pick whichever forms an enclosing tet (origin on the
        // INSIDE side of every face of the resulting tet).
        const Vec3<T> e01 = seed[1].w - seed[0].w;
        const Vec3<T> e02 = seed[2].w - seed[0].w;
        Vec3<T> n = crd::math::cross(e01, e02);
        const T nlen_sq = crd::math::dot(n, n);
        if (!(nlen_sq > std::numeric_limits<T>::min()))
        {
            // Degenerate (collinear) triangle.
            return false;
        }
        const T inv_nlen = static_cast<T>(1) / static_cast<T>(std::sqrt(nlen_sq));
        n.x *= inv_nlen;
        n.y *= inv_nlen;
        n.z *= inv_nlen;
        const EpaVertex<T> pos = epa_support_query<T>(a, b, n, T_BA, T_BA_rot_inv);
        const EpaVertex<T> neg = epa_support_query<T>(a, b, Vec3<T>(-n.x, -n.y, -n.z), T_BA, T_BA_rot_inv);
        // Pick the side that places origin INSIDE the resulting tet. A
        // robust check: signed volume of (origin, w0, w1, w2) has a sign;
        // signed volume of (w3, w0, w1, w2) must have the OPPOSITE sign
        // for origin to be on the same side as w3 from the (w0,w1,w2)
        // plane — wait, that's the opposite.
        //
        // Simpler: signed distance of origin from the (w0,w1,w2) plane is
        // `-dot(n, w0)` (n is unit). Signed distance of `pos.w` from the
        // same plane is `dot(n, pos.w - w0)`. For origin to be on the
        // SAME side as `pos.w` we need same-sign products. If origin and
        // pos are on the same side of the triangle, taking `pos` as the
        // 4th vertex gives a tet with the triangle on a face NOT enclosing
        // origin. So we want the OPPOSITE side: pick the candidate that's
        // on the OPPOSITE side of the triangle from origin.
        //
        // Origin's signed distance: `-dot(n, w0)`. We want a candidate whose
        // signed distance has the OPPOSITE sign. The signed distance of
        // `pos.w` from the plane is `dot(n, pos.w) - dot(n, w0)` and pos
        // was queried in +n so `dot(n, pos.w)` is the maximum projection
        // along +n, definitely ≥ dot(n, w0) → its signed distance is ≥ 0.
        // Similarly `neg.w`'s signed distance is ≤ 0.
        // Origin's signed distance = -dot(n, w0). If > 0, pick neg (so
        // candidate is on the other side, ≤ 0). If < 0, pick pos. If == 0
        // (origin on the triangle plane), the contact has depth 0 — pick
        // either; we use pos.
        const T origin_signed = -crd::math::dot(n, seed[0].w);
        if (origin_signed > static_cast<T>(0))
        {
            seed[3] = neg;
        }
        else
        {
            seed[3] = pos;
        }
        ++seed_count;
    }
    if (seed_count != 4)
    {
        return false;
    }
    return poly.init_from_tetra(seed[0], seed[1], seed[2], seed[3]);
}
} // namespace detail

// EPA driver. Caller must have verified `gjk_distance` returned
// `overlapping == true` and pass the terminating simplex from that call.
// Returns an `EpaResult` with `converged == false` if the polytope fails to
// converge within 32 iterations OR overflows the fixed-size storage OR the
// starting simplex can't be completed to an enclosing tet.
template <MathScalar T, typename A, typename B>
    requires ConvexShape<A, T> && ConvexShape<B, T>
[[nodiscard]] inline EpaResult<T> epa_penetration(const A& a, const Transform<T>& xform_a, const B& b,
                                                  const Transform<T>& xform_b,
                                                  const GjkSimplex<T>& starting_simplex) noexcept
{
    EpaResult<T> result;
    result.converged = false;

    const Transform<T> T_BA = crd::math::inversed(xform_a) * xform_b;
    const crd::math::Quat<T> T_BA_rot_inv = crd::math::inversed(T_BA.rotation);

    detail::EpaPolytope<T> poly;
    if (!detail::build_starting_polytope<T>(poly, a, b, T_BA, T_BA_rot_inv, starting_simplex))
    {
        return result; // sentinel: !converged, normal=0, depth=0
    }
    if (poly.failed_overflow)
    {
        return result;
    }

    // Iteration cap: 48 (matches Bullet's `btGjkEpaSolver2`; Box2D uses 32,
    // which is enough for purely polyhedral shapes but tight for smooth-vs-
    // smooth where the polytope-facet error converges sublinearly).
    constexpr crd::u8 k_max_iter = 48;

    // Termination is a **two-component epsilon**: absolute (the distance-eps
    // floor, catches near-zero depths) + relative (a fraction of the current
    // closing distance, suited to the polytope's facet-error scaling).
    //
    // Why not just the absolute distance-eps from `crd-geometry-primitives`?
    // For a smooth Min-diff (sphere-vs-sphere), the polytope's closest-face
    // distance approaches the true depth from below, with the *gap* between
    // polytope and Min-diff scaling as O(1/N²) for N vertices. At
    // `eps_abs = 1e-6` the algorithm would need ~50+ vertices for sphere-
    // sphere overlap to converge — far exceeding the 64-vertex budget and
    // returning a sentinel-failure on routine inputs. The relative epsilon
    // (1e-3) is the physics-grade tolerance (Box2D / Bullet contact slop
    // is in this band) and lets EPA terminate at ~15-20 iters with a
    // ~0.1%-accurate depth.
    //
    // The reported `depth` is the polytope-facet distance — a LOWER BOUND
    // on the true penetration depth (the polytope is strictly inside the
    // Min-diff). At `eps_rel = 1e-3` the reported depth is ≥ 99.9% of true.
    const T eps_abs = crd::geometry::primitives::k_distance_epsilon<T>();
    const T eps_rel = static_cast<T>(1e-3);

    crd::usize best_face_idx = 0;

    for (crd::u8 iter = 0; iter < k_max_iter; ++iter)
    {
        result.iteration_count = static_cast<crd::u8>(iter + 1);

        const crd::usize cf = poly.closest_face_idx();
        if (cf >= poly.face_count)
        {
            return result; // no faces (degenerate)
        }
        best_face_idx = cf;
        const detail::EpaFace<T>& F = poly.faces[cf];

        // Query support in the direction of F's outward normal.
        const detail::EpaVertex<T> sv = detail::epa_support_query<T>(a, b, F.normal, T_BA, T_BA_rot_inv);

        // Termination: new support is not meaningfully past F's plane.
        const T support_dist = crd::math::dot(F.normal, sv.w);
        const T tol = eps_abs + eps_rel * std::fabs(F.distance);
        if ((support_dist - F.distance) <= tol)
        {
            // F is the closing face; build the result below the loop.
            result.converged = true;
            break;
        }
        // Index-match termination (polyhedral-only).
        if (poly.vertex_already_present(sv.vidx_a, sv.vidx_b))
        {
            result.converged = true;
            break;
        }

        const crd::usize new_idx_us = poly.add_vertex(sv);
        if (new_idx_us >= detail::k_epa_max_vertices)
        {
            return result; // overflow
        }
        const crd::u8 new_idx = static_cast<crd::u8>(new_idx_us);
        if (!poly.expand_silhouette(new_idx))
        {
            return result; // overflow during expansion
        }
    }

    if (!result.converged)
    {
        return result; // iter cap hit
    }

    // Reconstruct witnesses from the closing face. F.normal points outward;
    // F.distance is the distance from origin to F's plane. The closing point
    // (projection of origin onto F) is `F.normal * F.distance`. Express this
    // in barycentric coordinates of F's three vertices in A-local; the same
    // weights yield the A-side and B-side witnesses.
    const detail::EpaFace<T>& F = poly.faces[best_face_idx];
    const detail::EpaVertex<T>& A0 = poly.verts[F.v[0]];
    const detail::EpaVertex<T>& A1 = poly.verts[F.v[1]];
    const detail::EpaVertex<T>& A2 = poly.verts[F.v[2]];

    // Closing point on F nearest origin (signed distance along outward
    // normal × normal = displacement from origin to plane).
    const Vec3<T> p(F.normal.x * F.distance, F.normal.y * F.distance, F.normal.z * F.distance);

    // Project p onto the triangle (A0.w, A1.w, A2.w) to get barycentric
    // weights. Use the standard 2D-in-3D barycentric formula.
    const Vec3<T> v0 = A1.w - A0.w;
    const Vec3<T> v1 = A2.w - A0.w;
    const Vec3<T> v2 = p - A0.w;
    const T d00 = crd::math::dot(v0, v0);
    const T d01 = crd::math::dot(v0, v1);
    const T d11 = crd::math::dot(v1, v1);
    const T d20 = crd::math::dot(v2, v0);
    const T d21 = crd::math::dot(v2, v1);
    const T denom = d00 * d11 - d01 * d01;
    T u = static_cast<T>(1) / static_cast<T>(3);
    T v = u;
    T w_bary = u;
    if (denom * denom > std::numeric_limits<T>::min())
    {
        const T inv_denom = static_cast<T>(1) / denom;
        v = (d11 * d20 - d01 * d21) * inv_denom;
        w_bary = (d00 * d21 - d01 * d20) * inv_denom;
        u = static_cast<T>(1) - v - w_bary;
    }
    // Clamp to [0, 1] in case the projection is slightly outside the
    // triangle (numerical drift on near-edge configurations).
    if (u < static_cast<T>(0))
    {
        u = static_cast<T>(0);
    }
    if (v < static_cast<T>(0))
    {
        v = static_cast<T>(0);
    }
    if (w_bary < static_cast<T>(0))
    {
        w_bary = static_cast<T>(0);
    }
    const T sum = u + v + w_bary;
    if (sum > std::numeric_limits<T>::min())
    {
        const T inv_sum = static_cast<T>(1) / sum;
        u *= inv_sum;
        v *= inv_sum;
        w_bary *= inv_sum;
    }

    // Combine A-supports / B-supports with these weights to get the
    // witnesses in each shape's local frame.
    const Vec3<T> wa_local(A0.w_a_local.x * u + A1.w_a_local.x * v + A2.w_a_local.x * w_bary,
                           A0.w_a_local.y * u + A1.w_a_local.y * v + A2.w_a_local.y * w_bary,
                           A0.w_a_local.z * u + A1.w_a_local.z * v + A2.w_a_local.z * w_bary);
    const Vec3<T> wb_local(A0.w_b_local.x * u + A1.w_b_local.x * v + A2.w_b_local.x * w_bary,
                           A0.w_b_local.y * u + A1.w_b_local.y * v + A2.w_b_local.y * w_bary,
                           A0.w_b_local.z * u + A1.w_b_local.z * v + A2.w_b_local.z * w_bary);
    result.witness_a_world = crd::math::transform_point(xform_a, wa_local);
    result.witness_b_world = crd::math::transform_point(xform_b, wb_local);

    // Normal direction convention pin: A→B. The Minkowski difference
    // here is `A - B`; the closing-face outward normal points away from
    // origin in A-B space, which is the +(A - B) direction at contact —
    // i.e., the A→B world direction.
    //
    // **Witness-consistent depth + normal.** Derive both from the SAME
    // barycentric-weighted Minkowski-diff point the witnesses use:
    //   q_A-local = u·A0.w + v·A1.w + w_bary·A2.w
    // Why not `F.distance` / `F.normal` directly? `F.distance` is the
    // perpendicular distance from origin to the face *plane*; when
    // origin's projection onto the plane is OUTSIDE the triangle, the
    // clamp+renormalize above moves the barycentric to a triangle-
    // boundary point, and q ≠ F.normal·F.distance. Using F.* would then
    // break the contact invariant `witness_a - witness_b = normal · depth`
    // for those configurations (sphere-vs-box trial 7 was the canary).
    // q gives the contact-consistent depth + normal: the witnesses derive
    // from q's barycentric, and `witness_a - witness_b = R_a · q` (R_a
    // because differences-of-positions transform by rotation only — the
    // translation cancels). So `normal_world = R_a · q̂`, `depth = |q|`.
    //
    // When origin's projection IS inside the triangle (the common case),
    // q == F.normal·F.distance and this reduces to the simpler form.
    const Vec3<T> q(A0.w.x * u + A1.w.x * v + A2.w.x * w_bary, A0.w.y * u + A1.w.y * v + A2.w.y * w_bary,
                    A0.w.z * u + A1.w.z * v + A2.w.z * w_bary);
    const T q_len_sq = crd::math::dot(q, q);
    if (q_len_sq > std::numeric_limits<T>::min())
    {
        const T q_len = static_cast<T>(std::sqrt(q_len_sq));
        result.depth = q_len;
        const Vec3<T> normal_local(q.x / q_len, q.y / q_len, q.z / q_len);
        result.normal = crd::math::rotate_vector(xform_a.rotation, normal_local);
    }
    else
    {
        // Pathological: q is at origin (depth ≈ 0 boundary contact).
        result.depth = static_cast<T>(0);
        result.normal = crd::math::rotate_vector(xform_a.rotation, F.normal);
    }

    // Carry the three contributing vertex_idx pairs for v1d-manifold's
    // feature identification. Slots with `k_invalid_vertex` mean "smooth
    // side" (sphere / capsule); v1d-manifold's enumerator handles those.
    result.face_vidx_a[0] = A0.vidx_a;
    result.face_vidx_a[1] = A1.vidx_a;
    result.face_vidx_a[2] = A2.vidx_a;
    result.face_vidx_b[0] = A0.vidx_b;
    result.face_vidx_b[1] = A1.vidx_b;
    result.face_vidx_b[2] = A2.vidx_b;
    return result;
}

// One-stop entry point: run GJK first; if overlapping, run EPA; return
// `nullopt` on no contact. The natural call shape for physics narrowphase.
template <MathScalar T, typename A, typename B>
    requires ConvexShape<A, T> && ConvexShape<B, T>
[[nodiscard]] inline std::optional<EpaResult<T>> compute_contact(const A& a, const Transform<T>& xform_a, const B& b,
                                                                 const Transform<T>& xform_b) noexcept
{
    const GjkResult<T> gjk = gjk_distance<T>(a, xform_a, b, xform_b);
    if (!gjk.overlapping)
    {
        return std::nullopt;
    }
    return epa_penetration<T>(a, xform_a, b, xform_b, gjk.simplex);
}

} // namespace crd::geometry::convex
