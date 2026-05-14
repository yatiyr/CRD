#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-convex — SAT box-pair fast path (Phase 3.1.7 v2d).
//
// Specialised OBB-vs-OBB and AABB-vs-OBB contact via the Separating Axis
// Theorem (Gottschalk 1996, "Collision queries using oriented bounding
// boxes"). Faster than the generic GJK+EPA path for box pairs by ~2-3×
// because:
//   - No iteration (15 fixed axis tests, no convergence loop).
//   - No polytope construction or silhouette walks.
//   - Vectorisable inner loops (dot products + abs + min).
//
// **The 15 candidate separating axes** (Ericson §4.4.1):
//   - 3 face normals of A (A's 3 local axes).
//   - 3 face normals of B (B's 3 local axes, expressed in A-local).
//   - 9 edge-cross axes (cross of each A-axis with each B-axis).
//
// On each axis L, project the center-to-center vector D and the two OBBs'
// "radii" (Σ |dot(axis, L) * half_extent|). Separation iff `|D·L| > r_a +
// r_b`; otherwise depth-along-L = `(r_a + r_b - |D·L|) / |L|`. Min-depth
// axis (across all 15) is the contact axis; sign(D·L) orients the contact
// normal toward B.
//
// **Determinism** (inherits ADR-0063, applies per ADR-0076 §4 pin #14):
//   - Axis traversal: A-face (0..2) → B-face (3..5) → edge-cross (6..14)
//     in lexicographic (i, j) order. Lowest-`axis_kind` wins on ties
//     within `k_distance_epsilon`.
//   - Witness corner picking: `(dot(axis, normal) < 0 ? -h : +h)` — zero-
//     dot ties go to `+h`. Same rule as `support()` for OBB3; ensures
//     `witness_a - witness_b == normal · depth` exactly on face-vertex
//     contact across platforms.
//   - Edge-edge contact uses `closest_points(Segment3, Segment3)`
//     (Ericson §5.1.9 from `closest_point.hpp`) for the witness pair,
//     so the invariant holds for edge-edge as well.
//
// **Robustness pin (advisor 2026-05-13)**: edge-cross axes with squared
// length below `k_parallel_epsilon²` are SKIPPED (parallel-axis case —
// the cross product is near-zero and cannot separate). The face-normal
// axes (1-6) still cover these configurations (one of A's face normals
// also serves as one of B's), so no information is lost.
//
// **Perf followup deferred to v2-close** (advisor): the cross-axis depth
// comparison currently calls `sqrt` per axis (up to 9 sqrts/pair). Squared-
// metric form (`raw² / |L|²` compared across all 15 axes, single final
// sqrt) is ~2× faster on the hot path; lock in here, follow up later.
//
// **API shape**:
//   - `SatResult<T>` — analogous to `EpaResult<T>` (and v2-close will
//     unify both into `ContactResult<T>`; eylem v1d-manifold reads the
//     final shape and v2c/v2d converge on it).
//   - `sat_obb_obb(a, xa, b, xb) → SatResult<T>` — primary entry.
//   - `sat_aabb_obb(a, b, xb) → SatResult<T>` — 6-line wrapper that
//     promotes the AABB to an axis-aligned OBB at world origin.
//   - `crd::geometry::overlap(OBB, Transform, OBB, Transform)` overload
//     dispatches OBB-OBB through SAT instead of the generic ConvexShape
//     GJK path (added in `gjk.hpp`'s facade alongside the convex one).
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/geometry/primitives/closest_point.hpp> // closest_points(Segment3, Segment3) for edge-edge witnesses
#include <crd/geometry/primitives/constants.hpp>     // k_distance_epsilon, k_parallel_epsilon
#include <crd/geometry/primitives/primitives.hpp>    // OBB3, AABB3, Segment3
#include <crd/math/mat.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/transform.hpp>
#include <crd/math/vec.hpp>

#include <cmath>
#include <limits>

namespace crd::geometry::convex
{
using crd::math::MathScalar;
using crd::math::Transform;
using crd::math::Vec3;

// Output of `sat_obb_obb` / `sat_aabb_obb`. Field layout frozen for v2d;
// v2-close will introduce a unified `ContactResult<T>` that both this and
// `EpaResult<T>` get refactored into (eylem v1d-manifold reads the final
// shape).
template <MathScalar T> struct SatResult
{
    Vec3<T> normal{};        // unit vector, A→B direction (A pressing into B)
    T depth{0};              // penetration depth, ≥ 0 (only meaningful when overlapping)
    Vec3<T> witness_a_world{}; // deepest point of A inside B, world space
    Vec3<T> witness_b_world{}; // deepest point of B inside A, world space

    // Which separating axis was the minimum-depth (and thus the contact axis):
    //   0..2  ⇒ face normal of A (A's local axis 0/1/2)
    //   3..5  ⇒ face normal of B (B's local axis 0/1/2)
    //   6..14 ⇒ edge-cross axis (i, j) where i = (kind-6)/3, j = (kind-6)%3
    crd::u8 axis_kind{0};

    bool overlapping{false};
    bool converged{false}; // true on every well-formed input; false reserved for degenerate edge cases
};

namespace sat_detail
{
// Componentwise multiplication of 3x3 matrices' columns. NOT a matrix product;
// just helper to express B's axes in A-local frame via `R_BA * B.orientation`.
//
// We have:
//   - `R_BA` is the rotation that maps B-local vectors into A-local: each
//     column is one of B's local axes expressed in A-local.
//   - But `R_BA` itself is built from the quaternion in T_BA; what we actually
//     compute is `rotate_vector(T_BA.rotation, col)` for each column of
//     B's orientation matrix.
template <MathScalar T>
[[nodiscard]] inline crd::math::Mat3<T> b_axes_in_a_local(const crd::math::Quat<T>& q_BA,
                                                          const crd::math::Mat3<T>& b_orient) noexcept
{
    return crd::math::Mat3<T>(crd::math::rotate_vector(q_BA, b_orient.c0),
                              crd::math::rotate_vector(q_BA, b_orient.c1),
                              crd::math::rotate_vector(q_BA, b_orient.c2));
}

// Build A's deepest corner in direction `+normal` (A-local frame): the
// corner-picking rule consistent with `support(OBB3, +normal)`.
template <MathScalar T>
[[nodiscard]] inline Vec3<T> deepest_corner_along(const Vec3<T>& center, const crd::math::Mat3<T>& axes,
                                                  const Vec3<T>& half, const Vec3<T>& normal) noexcept
{
    const T dx = crd::math::dot(axes.c0, normal);
    const T dy = crd::math::dot(axes.c1, normal);
    const T dz = crd::math::dot(axes.c2, normal);
    const T sx = dx < static_cast<T>(0) ? -half.x : half.x;
    const T sy = dy < static_cast<T>(0) ? -half.y : half.y;
    const T sz = dz < static_cast<T>(0) ? -half.z : half.z;
    return Vec3<T>(center.x + axes.c0.x * sx + axes.c1.x * sy + axes.c2.x * sz,
                   center.y + axes.c0.y * sx + axes.c1.y * sy + axes.c2.y * sz,
                   center.z + axes.c0.z * sx + axes.c1.z * sy + axes.c2.z * sz);
}

// Build the contact edge of an OBB given the cross-axis index `i` (0..2),
// the OBB's axes and half-extents, and the direction `+normal`. The contact
// edge is parallel to `axes.c[i]`; the perpendicular offsets are picked to
// place the edge midpoint as far in `+normal` direction as possible (the
// edge that's "poking into the other OBB").
//
// Returns the edge as a Segment3 (a, b) where a = midpoint - h_i * axes_i
// and b = midpoint + h_i * axes_i.
template <MathScalar T>
[[nodiscard]] inline primitives::Segment3<T> contact_edge_along(const Vec3<T>& center, const crd::math::Mat3<T>& axes,
                                                                 const Vec3<T>& half, int i,
                                                                 const Vec3<T>& normal) noexcept
{
    // `i` is the axis the edge is parallel to. The two perpendicular axis
    // indices have their signs picked from `normal`.
    const int k = (i + 1) % 3;
    const int l = (i + 2) % 3;
    const Vec3<T> ax_i = (i == 0) ? axes.c0 : (i == 1) ? axes.c1 : axes.c2;
    const Vec3<T> ax_k = (k == 0) ? axes.c0 : (k == 1) ? axes.c1 : axes.c2;
    const Vec3<T> ax_l = (l == 0) ? axes.c0 : (l == 1) ? axes.c1 : axes.c2;
    const T h_i = (i == 0) ? half.x : (i == 1) ? half.y : half.z;
    const T h_k = (k == 0) ? half.x : (k == 1) ? half.y : half.z;
    const T h_l = (l == 0) ? half.x : (l == 1) ? half.y : half.z;
    const T dk = crd::math::dot(ax_k, normal);
    const T dl = crd::math::dot(ax_l, normal);
    const T sk = dk < static_cast<T>(0) ? -h_k : h_k;
    const T sl = dl < static_cast<T>(0) ? -h_l : h_l;
    const Vec3<T> midpoint(center.x + ax_k.x * sk + ax_l.x * sl, center.y + ax_k.y * sk + ax_l.y * sl,
                           center.z + ax_k.z * sk + ax_l.z * sl);
    const Vec3<T> a(midpoint.x - ax_i.x * h_i, midpoint.y - ax_i.y * h_i, midpoint.z - ax_i.z * h_i);
    const Vec3<T> b(midpoint.x + ax_i.x * h_i, midpoint.y + ax_i.y * h_i, midpoint.z + ax_i.z * h_i);
    return primitives::Segment3<T>(a, b);
}

// One axis test. Returns true if separated (caller exits early); else updates
// the running min-depth if this axis improves it. The axis L is passed
// UNNORMALIZED for the cross-axis case (with `inv_len = 1/|L|` for depth
// rescaling). For face axes |L|=1 ⇒ inv_len = 1.
template <MathScalar T>
[[nodiscard]] inline bool sat_axis_test(const Vec3<T>& L, T inv_len, const Vec3<T>& D, const Vec3<T>& a_h_dot_L,
                                        const Vec3<T>& b_h_dot_L, crd::u8 axis_kind, T& best_depth, crd::u8& best_kind,
                                        Vec3<T>& best_axis_local, T& best_inv_len) noexcept
{
    // `a_h_dot_L.x = |dot(a_ax[0], L) * h_a[0]|`, etc. Sum gives r_a (the
    // half-projected radius of A along L). Same shape for r_b.
    const T r_a = a_h_dot_L.x + a_h_dot_L.y + a_h_dot_L.z;
    const T r_b = b_h_dot_L.x + b_h_dot_L.y + b_h_dot_L.z;
    const T d_proj = crd::math::dot(D, L);
    const T d_abs = d_proj < static_cast<T>(0) ? -d_proj : d_proj;
    const T raw_overlap = (r_a + r_b) - d_abs;
    if (raw_overlap < static_cast<T>(0))
    {
        return true; // separated
    }
    // Normalized depth = raw_overlap / |L|.
    const T depth_here = raw_overlap * inv_len;
    const T eps = crd::geometry::primitives::k_distance_epsilon<T>();
    if (depth_here < best_depth - eps)
    {
        best_depth = depth_here;
        best_kind = axis_kind;
        // Orient L toward B (sign(d_proj) > 0 means D points toward +L
        // already; if < 0, flip L so the contact normal points A→B).
        if (d_proj < static_cast<T>(0))
        {
            best_axis_local = Vec3<T>(-L.x, -L.y, -L.z);
        }
        else
        {
            best_axis_local = L;
        }
        best_inv_len = inv_len;
    }
    return false;
}
} // namespace sat_detail

// ---- sat_obb_obb -----------------------------------------------------------
//
// Primary SAT entry. Handles the full OBB-vs-OBB case including non-identity
// `xform_a.rotation` (the OBBs' modeling-frame orientations carried in
// `OBB3::orientation` plus the transform's rigid rotation both compose).
template <MathScalar T>
[[nodiscard]] inline SatResult<T> sat_obb_obb(const primitives::OBB3<T>& a, const Transform<T>& xform_a,
                                              const primitives::OBB3<T>& b, const Transform<T>& xform_b) noexcept
{
    SatResult<T> result;
    result.converged = true;

    // Work entirely in A's local frame. A's modeling-frame axes are the
    // columns of `a.orientation`; A's center is `a.center` directly. B
    // gets transported across.
    const Transform<T> T_BA = crd::math::inversed(xform_a) * xform_b;
    const Vec3<T> b_center_in_a(T_BA.translation.x + crd::math::rotate_vector(T_BA.rotation, b.center).x,
                                T_BA.translation.y + crd::math::rotate_vector(T_BA.rotation, b.center).y,
                                T_BA.translation.z + crd::math::rotate_vector(T_BA.rotation, b.center).z);
    const crd::math::Mat3<T> b_axes_in_a = sat_detail::b_axes_in_a_local(T_BA.rotation, b.orientation);

    const Vec3<T> D(b_center_in_a.x - a.center.x, b_center_in_a.y - a.center.y, b_center_in_a.z - a.center.z);

    // A's axes / half: direct access from a.orientation and a.half_extents.
    const Vec3<T> a_ax[3] = {a.orientation.c0, a.orientation.c1, a.orientation.c2};
    const Vec3<T> b_ax[3] = {b_axes_in_a.c0, b_axes_in_a.c1, b_axes_in_a.c2};
    const T a_h[3] = {a.half_extents.x, a.half_extents.y, a.half_extents.z};
    const T b_h[3] = {b.half_extents.x, b.half_extents.y, b.half_extents.z};

    // Helper to compute `|dot(axis[i], L) * half[i]|` vectors.
    auto h_dot = [](const Vec3<T> (&axes)[3], const T (&half)[3], const Vec3<T>& L) {
        const T d0 = crd::math::dot(axes[0], L);
        const T d1 = crd::math::dot(axes[1], L);
        const T d2 = crd::math::dot(axes[2], L);
        return Vec3<T>(d0 < 0 ? -d0 * half[0] : d0 * half[0], d1 < 0 ? -d1 * half[1] : d1 * half[1],
                       d2 < 0 ? -d2 * half[2] : d2 * half[2]);
    };

    T best_depth = std::numeric_limits<T>::infinity();
    crd::u8 best_kind = 0;
    Vec3<T> best_axis_local{};
    T best_inv_len = static_cast<T>(1);

    // --- Axes 0-2: A's face normals (|L|=1 since orientation is orthonormal).
    for (int i = 0; i < 3; ++i)
    {
        const Vec3<T>& L = a_ax[i];
        const Vec3<T> a_proj = h_dot(a_ax, a_h, L);
        const Vec3<T> b_proj = h_dot(b_ax, b_h, L);
        if (sat_detail::sat_axis_test<T>(L, static_cast<T>(1), D, a_proj, b_proj, static_cast<crd::u8>(i), best_depth,
                                          best_kind, best_axis_local, best_inv_len))
        {
            return result; // overlapping=false, converged=true — separated
        }
    }
    // --- Axes 3-5: B's face normals.
    for (int j = 0; j < 3; ++j)
    {
        const Vec3<T>& L = b_ax[j];
        const Vec3<T> a_proj = h_dot(a_ax, a_h, L);
        const Vec3<T> b_proj = h_dot(b_ax, b_h, L);
        if (sat_detail::sat_axis_test<T>(L, static_cast<T>(1), D, a_proj, b_proj, static_cast<crd::u8>(3 + j),
                                          best_depth, best_kind, best_axis_local, best_inv_len))
        {
            return result;
        }
    }
    // --- Axes 6-14: 9 edge-cross axes. Skip if |L|² < parallel² (parallel
    // edges — that cross axis can't separate; face axes still cover the case).
    const T parallel_eps_sq = crd::geometry::primitives::k_parallel_epsilon<T>() *
                              crd::geometry::primitives::k_parallel_epsilon<T>();
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
        {
            const Vec3<T> L = crd::math::cross(a_ax[i], b_ax[j]);
            const T len_sq = crd::math::dot(L, L);
            if (len_sq < parallel_eps_sq)
            {
                continue;
            }
            const T inv_len = static_cast<T>(1) / static_cast<T>(std::sqrt(len_sq));
            const Vec3<T> a_proj = h_dot(a_ax, a_h, L);
            const Vec3<T> b_proj = h_dot(b_ax, b_h, L);
            if (sat_detail::sat_axis_test<T>(L, inv_len, D, a_proj, b_proj,
                                              static_cast<crd::u8>(6 + i * 3 + j), best_depth, best_kind,
                                              best_axis_local, best_inv_len))
            {
                return result;
            }
        }
    }

    // ---- All 15 axes passed. Build the contact data. -----------------------
    // `best_axis_local` is currently UNNORMALIZED for cross-axes; scale by
    // `best_inv_len` to get the unit contact normal in A-local.
    const Vec3<T> normal_local(best_axis_local.x * best_inv_len, best_axis_local.y * best_inv_len,
                                best_axis_local.z * best_inv_len);

    // World normal: rotate A-local normal by A's transform rotation. (Depth
    // is rotation-invariant; carries to world unchanged.)
    result.normal = crd::math::rotate_vector(xform_a.rotation, normal_local);
    result.depth = best_depth;
    result.overlapping = true;
    result.axis_kind = best_kind;

    // ---- Witness reconstruction --------------------------------------------
    if (best_kind <= 2)
    {
        // Contact axis is one of A's face normals. The contact is FACE-vs-
        // VERTEX (or face-edge / face-face) where A provides the face and
        // B provides the deepest-poking vertex. Witness on B = B's deepest
        // corner in -normal direction (B's vertex poking into A). Witness
        // on A = that B-corner projected onto A's contact face plane,
        // i.e., witness_b + normal · depth — so `witness_a - witness_b
        // == normal · depth` exactly (the contact-invariant pin).
        //
        // We CANNOT just pick "A's deepest corner in +normal" independently:
        // for non-cube OBBs the perpendicular components of A's and B's
        // corners don't align, so `wa - wb` would drift perpendicular to
        // the contact normal — breaking the invariant. The face-side
        // witness must come from projecting the vertex-side witness.
        const Vec3<T> minus_normal(-normal_local.x, -normal_local.y, -normal_local.z);
        const Vec3<T> witness_b_in_a =
            sat_detail::deepest_corner_along<T>(b_center_in_a, b_axes_in_a, b.half_extents, minus_normal);
        const Vec3<T> witness_a_in_a(witness_b_in_a.x + normal_local.x * best_depth,
                                     witness_b_in_a.y + normal_local.y * best_depth,
                                     witness_b_in_a.z + normal_local.z * best_depth);
        result.witness_a_world = crd::math::transform_point(xform_a, witness_a_in_a);
        result.witness_b_world = crd::math::transform_point(xform_a, witness_b_in_a);
    }
    else if (best_kind <= 5)
    {
        // Contact axis is one of B's face normals. Symmetric: B provides
        // the face, A provides the vertex. Witness on A = A's deepest
        // corner in +normal; witness on B = witness_a - normal · depth.
        const crd::math::Mat3<T> a_axes_m(a_ax[0], a_ax[1], a_ax[2]);
        const Vec3<T> witness_a_in_a =
            sat_detail::deepest_corner_along<T>(a.center, a_axes_m, a.half_extents, normal_local);
        const Vec3<T> witness_b_in_a(witness_a_in_a.x - normal_local.x * best_depth,
                                     witness_a_in_a.y - normal_local.y * best_depth,
                                     witness_a_in_a.z - normal_local.z * best_depth);
        result.witness_a_world = crd::math::transform_point(xform_a, witness_a_in_a);
        result.witness_b_world = crd::math::transform_point(xform_a, witness_b_in_a);
    }
    else
    {
        // Edge-edge contact. Identify the two contributing edges:
        //   - A's edge is parallel to A's axis `i = (best_kind - 6) / 3`.
        //   - B's edge is parallel to B's axis `j = (best_kind - 6) % 3`.
        // Position each edge as far in ±normal as possible (the side that's
        // poking into the other OBB).
        const int i = static_cast<int>((best_kind - 6) / 3);
        const int j = static_cast<int>((best_kind - 6) % 3);
        const crd::math::Mat3<T> a_axes_m(a_ax[0], a_ax[1], a_ax[2]);
        const primitives::Segment3<T> edge_a =
            sat_detail::contact_edge_along<T>(a.center, a_axes_m, a.half_extents, i, normal_local);
        const Vec3<T> minus_normal(-normal_local.x, -normal_local.y, -normal_local.z);
        const primitives::Segment3<T> edge_b =
            sat_detail::contact_edge_along<T>(b_center_in_a, b_axes_in_a, b.half_extents, j, minus_normal);
        // Ericson §5.1.9 closest-points-on-two-segments. Robust on parallel +
        // degenerate inputs (see `primitives/closest_point.hpp`).
        Vec3<T> wa_in_a;
        Vec3<T> wb_in_a;
        primitives::closest_points(edge_a, edge_b, wa_in_a, wb_in_a);
        result.witness_a_world = crd::math::transform_point(xform_a, wa_in_a);
        result.witness_b_world = crd::math::transform_point(xform_a, wb_in_a);
    }
    return result;
}

// ---- sat_aabb_obb (thin wrapper) ------------------------------------------
//
// AABB-vs-OBB. The AABB is treated as an axis-aligned OBB at world identity
// (its center = (min+max)/2, half-extents = (max-min)/2). 6-line wrapper.
template <MathScalar T>
[[nodiscard]] inline SatResult<T> sat_aabb_obb(const primitives::AABB3<T>& a, const primitives::OBB3<T>& b,
                                                const Transform<T>& xform_b) noexcept
{
    const Vec3<T> a_center((a.min.x + a.max.x) * static_cast<T>(0.5), (a.min.y + a.max.y) * static_cast<T>(0.5),
                           (a.min.z + a.max.z) * static_cast<T>(0.5));
    const Vec3<T> a_half((a.max.x - a.min.x) * static_cast<T>(0.5), (a.max.y - a.min.y) * static_cast<T>(0.5),
                         (a.max.z - a.min.z) * static_cast<T>(0.5));
    const primitives::OBB3<T> aobb(a_center, a_half, crd::math::Mat3<T>::identity());
    return sat_obb_obb<T>(aobb, Transform<T>(Vec3<T>(static_cast<T>(0)), crd::math::Quat<T>::identity()), b, xform_b);
}

} // namespace crd::geometry::convex

// ---- Unified facade overloads (ADR-0076 §16 pin #1) ------------------------
//
// `crd::geometry::overlap(OBB3, Transform, OBB3, Transform)` overload —
// strictly more specific than the generic `ConvexShape` form (which would
// dispatch to GJK), so it preempts cleanly. Same `bool` return shape, but
// 2-3× the throughput on box pairs.
//
// `crd::geometry::compute_contact_obb_obb(OBB3, Transform, OBB3, Transform)`
// — explicit contact entry for OBB pairs; returns `optional<SatResult>` so
// the caller can distinguish "no contact" (`nullopt`) from "contact with
// these fields". Will be the natural entry for eylem v1d narrowphase when
// it ships.

namespace crd::geometry
{
template <crd::math::MathScalar T>
[[nodiscard]] inline bool overlap(const primitives::OBB3<T>& a, const crd::math::Transform<T>& xform_a,
                                  const primitives::OBB3<T>& b, const crd::math::Transform<T>& xform_b) noexcept
{
    return convex::sat_obb_obb<T>(a, xform_a, b, xform_b).overlapping;
}

template <crd::math::MathScalar T>
[[nodiscard]] inline std::optional<convex::SatResult<T>>
compute_contact_obb_obb(const primitives::OBB3<T>& a, const crd::math::Transform<T>& xform_a,
                        const primitives::OBB3<T>& b, const crd::math::Transform<T>& xform_b) noexcept
{
    const convex::SatResult<T> r = convex::sat_obb_obb<T>(a, xform_a, b, xform_b);
    if (!r.overlapping)
    {
        return std::nullopt;
    }
    return r;
}
} // namespace crd::geometry
