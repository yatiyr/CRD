#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-convex — Ericson §9.5 sub-distance algorithm (Voronoi-region
// simplex reduction). Phase 3.1.7 v2a; ADR-0076 §4 pin #1: Ericson order,
// NOT van den Bergen.
//
// Given a simplex S of 1..4 points in R³, find the closest point P on conv(S)
// to the origin, AND identify which sub-simplex (vertex / edge / face) of S
// contains P. The new simplex is conv(those surviving vertices); the
// barycentric weights of P on the surviving set are reported so callers can
// reconstruct corresponding witnesses on other tracked point clouds (the GJK
// driver tracks parallel A-supports and B-supports in shape A's local frame
// and combines them with the same weights to get the closest-pair witnesses).
//
// Why "sub-distance" and not "Johnson's distance algorithm" / "van den
// Bergen": Johnson's lookup-table form has well-known numerical pathologies
// (the comparisons across the table cells are not consistent under finite
// precision — the same input can land in multiple regions, or none). Ericson
// §9.5's nested-dot-product cascade is the form Box2D and Bullet ship; it
// matches the closest-point-on-triangle / closest-point-on-tetrahedron forms
// in `closest_point.hpp` (which solve the *same* problem with a *fixed*
// query point) but specialised for the origin query — saves the `p - a`
// subtractions and lets the Voronoi tests reduce to dot products on the
// simplex edges directly. Cross-platform bit-exact under IEEE FMA
// contraction (`-ffp-contract=off` everywhere per ADR-0063).
//
// Return shape: `(SubDistanceResult{ closest, mask, weights[4] })`. `mask`
// is a 4-bit packed mask over the original 1/2/3/4-simplex slots saying
// which vertices survived; `weights[i]` is the barycentric weight on the
// i'th original slot (zero for non-surviving slots). The caller never
// re-permutes the simplex array — it walks the mask and reads the surviving
// slots in original order. (The GJK driver immediately re-packs the
// surviving slots into the first `popcount(mask)` array positions for the
// next iteration — that's the only place the permutation lives.)
//
// Degenerate inputs:
//   - 1-simplex with the point at the origin → closest = origin, weight = 1,
//     mask = 0001. (Origin already in simplex; GJK driver flags overlap.)
//   - 2-simplex with coincident points → reduce to the lower-index vertex.
//   - Collinear 3-simplex → reduce to the edge containing the projection.
//   - Coplanar 4-simplex → reduce to the face containing the projection.
//   - Origin in interior (3-simplex coplanar with origin, or 4-simplex
//     containing origin) → mask retains all vertices, weights from
//     barycentrics, closest = origin (the GJK driver detects this and
//     flags overlap).
//
// All routines are constexpr-eligible for templated `T` — they only use
// dot / +/- / *T. No `sqrt` (sub-distance works in squared space).
// ---------------------------------------------------------------------------

#include <crd/core/types.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>

namespace crd::geometry::convex::detail
{
using crd::math::MathScalar;
using crd::math::Vec3;

// Result of a sub-distance reduction. `mask` bit `i` set ⇒ original slot
// `i` is in the surviving simplex with weight `weights[i]`; weights sum to 1
// across the surviving set (zero for non-surviving slots).
template <MathScalar T> struct SubDistanceResult
{
    Vec3<T> closest{};            // closest point on conv(S) to origin
    T weights[4]{};               // barycentric weights on the original slots (sum to 1)
    crd::u8 mask = 0;             // bit i set ⇒ slot i survives
};

// ===========================================================================
// 1-simplex: { a }. closest = a, weight = 1.
// ===========================================================================
template <MathScalar T> [[nodiscard]] constexpr SubDistanceResult<T> sub_distance_1(const Vec3<T>& a) noexcept
{
    SubDistanceResult<T> r;
    r.closest = a;
    r.weights[0] = static_cast<T>(1);
    r.mask = 0b0001;
    return r;
}

// ===========================================================================
// 2-simplex: edge { a, b }. Three Voronoi regions: vertex a, edge ab, vertex b.
//
// Project origin onto line ab. Let t = -dot(a, b-a) / dot(b-a, b-a) — this
// is the parameter where the closest point on the *infinite line* lies. If
// t ≤ 0, vertex a wins; if t ≥ 1, vertex b wins; else the edge interior.
// ===========================================================================
template <MathScalar T>
[[nodiscard]] constexpr SubDistanceResult<T> sub_distance_2(const Vec3<T>& a, const Vec3<T>& b) noexcept
{
    const Vec3<T> ab = b - a;
    const T ab_dot_ab = crd::math::dot(ab, ab);
    // Degenerate edge — both endpoints coincide. Reduce to the (lower-index)
    // vertex a.
    if (!(ab_dot_ab > std::numeric_limits<T>::min()))
    {
        return sub_distance_1(a);
    }
    // Parameter of origin's projection on line through a,b. Origin is at 0,
    // so dot(origin - a, b - a) = -dot(a, b-a).
    const T t = -crd::math::dot(a, ab) / ab_dot_ab;
    if (t <= static_cast<T>(0))
    {
        return sub_distance_1(a); // Voronoi region of vertex a
    }
    if (t >= static_cast<T>(1))
    {
        SubDistanceResult<T> r;
        r.closest = b;
        r.weights[1] = static_cast<T>(1);
        r.mask = 0b0010;
        return r;
    }
    SubDistanceResult<T> r;
    r.closest = Vec3<T>(a.x + ab.x * t, a.y + ab.y * t, a.z + ab.z * t);
    r.weights[0] = static_cast<T>(1) - t;
    r.weights[1] = t;
    r.mask = 0b0011;
    return r;
}

namespace sd_internal
{
// Reduce a SubDistanceResult coming from one of the lower-arity routines
// back into the original 4-slot frame, by remapping its `mask` and
// `weights` indices from `{a_index, b_index, c_index, d_index}` to
// `{0,1,2,3}`.
template <MathScalar T>
[[nodiscard]] constexpr SubDistanceResult<T> remap(const SubDistanceResult<T>& src,
                                                   crd::u8 i0, crd::u8 i1, crd::u8 i2, crd::u8 i3) noexcept
{
    SubDistanceResult<T> r;
    r.closest = src.closest;
    const crd::u8 idx[4] = {i0, i1, i2, i3};
    crd::u8 m = 0;
    for (crd::u8 s = 0; s < 4; ++s)
    {
        if (src.mask & (1U << s))
        {
            m |= static_cast<crd::u8>(1U << idx[s]);
            r.weights[idx[s]] = src.weights[s];
        }
    }
    r.mask = m;
    return r;
}
} // namespace sd_internal

// ===========================================================================
// 3-simplex: triangle { a, b, c }. Seven Voronoi regions: 3 vertices, 3 edges,
// 1 interior face. Ericson §5.1.5 cascade, specialised for the origin query.
//
// Same algorithmic structure as `cp_triangle` in
// `crd-geometry-primitives::closest_point.hpp`, but operates on the *origin*
// (so `ap = -a`, `bp = -b`, `cp = -c`) and reports the barycentric weights +
// the surviving slot mask (the `closest_point` form returns only the point).
// ===========================================================================
template <MathScalar T>
[[nodiscard]] constexpr SubDistanceResult<T> sub_distance_3(const Vec3<T>& a, const Vec3<T>& b,
                                                            const Vec3<T>& c) noexcept
{
    const Vec3<T> ab = b - a;
    const Vec3<T> ac = c - a;
    // Vertex regions
    // dot(ab, -a) ≤ 0 && dot(ac, -a) ≤ 0  ⇒  vertex a
    const T d1 = -crd::math::dot(ab, a);
    const T d2 = -crd::math::dot(ac, a);
    if (d1 <= static_cast<T>(0) && d2 <= static_cast<T>(0))
    {
        return sub_distance_1(a);
    }
    const T d3 = -crd::math::dot(ab, b);
    const T d4 = -crd::math::dot(ac, b);
    if (d3 >= static_cast<T>(0) && d4 <= d3)
    {
        SubDistanceResult<T> r;
        r.closest = b;
        r.weights[1] = static_cast<T>(1);
        r.mask = 0b0010;
        return r;
    }
    // Edge AB region: vc * sign tests
    const T vc = d1 * d4 - d3 * d2;
    if (vc <= static_cast<T>(0) && d1 >= static_cast<T>(0) && d3 <= static_cast<T>(0))
    {
        const T t = d1 / (d1 - d3);
        SubDistanceResult<T> r;
        r.closest = Vec3<T>(a.x + ab.x * t, a.y + ab.y * t, a.z + ab.z * t);
        r.weights[0] = static_cast<T>(1) - t;
        r.weights[1] = t;
        r.mask = 0b0011;
        return r;
    }
    const T d5 = -crd::math::dot(ab, c);
    const T d6 = -crd::math::dot(ac, c);
    if (d6 >= static_cast<T>(0) && d5 <= d6)
    {
        SubDistanceResult<T> r;
        r.closest = c;
        r.weights[2] = static_cast<T>(1);
        r.mask = 0b0100;
        return r;
    }
    const T vb = d5 * d2 - d1 * d6;
    if (vb <= static_cast<T>(0) && d2 >= static_cast<T>(0) && d6 <= static_cast<T>(0))
    {
        const T t = d2 / (d2 - d6);
        SubDistanceResult<T> r;
        r.closest = Vec3<T>(a.x + ac.x * t, a.y + ac.y * t, a.z + ac.z * t);
        r.weights[0] = static_cast<T>(1) - t;
        r.weights[2] = t;
        r.mask = 0b0101;
        return r;
    }
    const T va = d3 * d6 - d5 * d4;
    if (va <= static_cast<T>(0) && (d4 - d3) >= static_cast<T>(0) && (d5 - d6) >= static_cast<T>(0))
    {
        const T t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        SubDistanceResult<T> r;
        r.closest = Vec3<T>(b.x + (c.x - b.x) * t, b.y + (c.y - b.y) * t, b.z + (c.z - b.z) * t);
        r.weights[1] = static_cast<T>(1) - t;
        r.weights[2] = t;
        r.mask = 0b0110;
        return r;
    }
    // Interior face region — origin projects inside the triangle.
    // Degenerate (collinear) triangle: fall back to the nearest edge.
    const T sum = va + vb + vc;
    if (!(sum > std::numeric_limits<T>::min()))
    {
        // Collinear: try each edge sub-distance, keep the closest.
        const SubDistanceResult<T> e_ab = sub_distance_2(a, b);
        const SubDistanceResult<T> e_ac = sub_distance_2(a, c);
        const SubDistanceResult<T> e_bc = sub_distance_2(b, c);
        const T q_ab = crd::math::dot(e_ab.closest, e_ab.closest);
        const T q_ac = crd::math::dot(e_ac.closest, e_ac.closest);
        const T q_bc = crd::math::dot(e_bc.closest, e_bc.closest);
        // Slot remap: edge ab uses slots (0,1); ac (0,2); bc (1,2).
        if (q_ab <= q_ac && q_ab <= q_bc)
        {
            return sd_internal::remap<T>(e_ab, 0, 1, 0, 0);
        }
        if (q_ac <= q_bc)
        {
            return sd_internal::remap<T>(e_ac, 0, 2, 0, 0);
        }
        return sd_internal::remap<T>(e_bc, 1, 2, 0, 0);
    }
    const T denom = static_cast<T>(1) / sum;
    const T v = vb * denom;
    const T w = vc * denom;
    SubDistanceResult<T> r;
    r.closest = Vec3<T>(a.x + ab.x * v + ac.x * w, a.y + ab.y * v + ac.y * w, a.z + ab.z * v + ac.z * w);
    r.weights[0] = static_cast<T>(1) - v - w;
    r.weights[1] = v;
    r.weights[2] = w;
    r.mask = 0b0111;
    return r;
}

// ===========================================================================
// 4-simplex: tetrahedron { a, b, c, d }. Most-recent vertex `d` (the new
// support) drove us here; the simplex is built such that `d` is the most
// promising vertex (its hyperplane separates the origin from the previous
// simplex). Test each of the 4 outward-facing planes; if origin is on the
// outside of plane (b,c,d), reduce to (b,c,d) and recurse; etc. Standard
// Ericson §5.1.6 ClosestPointOnTetrahedronToPoint cascade.
//
// The tetra's vertex order convention here matches `closest_point.hpp`'s
// `closest_point(Tetrahedron, p)`: faces are (b,c,d), (a,c,d), (a,b,d),
// (a,b,c) — each opposite the vertex it omits. We test "is origin outside
// face F (i.e. on the side away from the omitted vertex)?", and if so
// recurse into the face's sub_distance_3.
//
// If origin is inside all four halfspaces ⇒ origin is in the tetra ⇒
// closest = origin, all four weights from barycentric coords, mask = 1111.
// GJK driver detects this case and flags overlap.
// ===========================================================================
template <MathScalar T>
[[nodiscard]] constexpr SubDistanceResult<T> sub_distance_4(const Vec3<T>& a, const Vec3<T>& b, const Vec3<T>& c,
                                                            const Vec3<T>& d) noexcept
{
    // Signed-volume orientation test: is origin on the same side of plane
    // (p, q, r) as `s`? Returns positive when yes.
    auto signed_side = [](const Vec3<T>& p, const Vec3<T>& q, const Vec3<T>& r, const Vec3<T>& s) noexcept {
        const Vec3<T> n = crd::math::cross(q - p, r - p);
        const T sd_s = crd::math::dot(n, s - p);
        const T sd_o = -crd::math::dot(n, p); // signed distance of origin
        return sd_s * sd_o;
    };
    SubDistanceResult<T> best;
    T best_d2 = std::numeric_limits<T>::infinity();
    bool any_outside = false;

    // Face (b, c, d), opposite a. Outside ⇒ origin on the opposite side of
    // the face from `a` ⇒ recurse into the face.
    if (signed_side(b, c, d, a) < static_cast<T>(0))
    {
        any_outside = true;
        const SubDistanceResult<T> face = sub_distance_3(b, c, d);
        const T q = crd::math::dot(face.closest, face.closest);
        if (q < best_d2)
        {
            best_d2 = q;
            best = sd_internal::remap<T>(face, 1, 2, 3, 0);
        }
    }
    // Face (a, c, d), opposite b.
    if (signed_side(a, c, d, b) < static_cast<T>(0))
    {
        any_outside = true;
        const SubDistanceResult<T> face = sub_distance_3(a, c, d);
        const T q = crd::math::dot(face.closest, face.closest);
        if (q < best_d2)
        {
            best_d2 = q;
            best = sd_internal::remap<T>(face, 0, 2, 3, 0);
        }
    }
    // Face (a, b, d), opposite c.
    if (signed_side(a, b, d, c) < static_cast<T>(0))
    {
        any_outside = true;
        const SubDistanceResult<T> face = sub_distance_3(a, b, d);
        const T q = crd::math::dot(face.closest, face.closest);
        if (q < best_d2)
        {
            best_d2 = q;
            best = sd_internal::remap<T>(face, 0, 1, 3, 0);
        }
    }
    // Face (a, b, c), opposite d.
    if (signed_side(a, b, c, d) < static_cast<T>(0))
    {
        any_outside = true;
        const SubDistanceResult<T> face = sub_distance_3(a, b, c);
        const T q = crd::math::dot(face.closest, face.closest);
        if (q < best_d2)
        {
            best_d2 = q;
            best = sd_internal::remap<T>(face, 0, 1, 2, 0);
        }
    }
    if (any_outside)
    {
        return best;
    }
    // No face's signed_side fired. Two cases:
    //   (a) `vol_tot != 0` — origin is in the tetra's interior (overlap).
    //   (b) `vol_tot ≈ 0` — degenerate (coplanar) 4-simplex. Origin is
    //       generally NOT in the plane; the closest point is on the face
    //       (= the coplanar triangle). The signed_side products all came
    //       out exactly zero because the face normals project to zero on
    //       the (origin-vertex) direction along the missing axis.
    //       Recover by running sub_distance_3 on every face and picking the
    //       closest — this is what the `any_outside` cascade would do
    //       in the non-degenerate version.
    const Vec3<T> ab = b - a;
    const Vec3<T> ac = c - a;
    const Vec3<T> ad = d - a;
    const T vol_tot = crd::math::dot(ab, crd::math::cross(ac, ad));
    if (!(vol_tot * vol_tot > std::numeric_limits<T>::min()))
    {
        // Coplanar 4-simplex — enumerate all 4 faces and pick the closest.
        // (Same shape as the `any_outside` cascade above, but unconditional
        // because the sign tests are inconclusive here.)
        SubDistanceResult<T> best_face;
        T best_face_d2 = std::numeric_limits<T>::infinity();
        const SubDistanceResult<T> f0 = sub_distance_3(b, c, d);
        const T q0 = crd::math::dot(f0.closest, f0.closest);
        if (q0 < best_face_d2)
        {
            best_face_d2 = q0;
            best_face = sd_internal::remap<T>(f0, 1, 2, 3, 0);
        }
        const SubDistanceResult<T> f1 = sub_distance_3(a, c, d);
        const T q1 = crd::math::dot(f1.closest, f1.closest);
        if (q1 < best_face_d2)
        {
            best_face_d2 = q1;
            best_face = sd_internal::remap<T>(f1, 0, 2, 3, 0);
        }
        const SubDistanceResult<T> f2 = sub_distance_3(a, b, d);
        const T q2 = crd::math::dot(f2.closest, f2.closest);
        if (q2 < best_face_d2)
        {
            best_face_d2 = q2;
            best_face = sd_internal::remap<T>(f2, 0, 1, 3, 0);
        }
        const SubDistanceResult<T> f3 = sub_distance_3(a, b, c);
        const T q3 = crd::math::dot(f3.closest, f3.closest);
        if (q3 < best_face_d2)
        {
            best_face_d2 = q3;
            best_face = sd_internal::remap<T>(f3, 0, 1, 2, 0);
        }
        return best_face;
    }
    // Origin is inside the tetra (interior to all 4 halfspaces). Closest =
    // origin; full barycentric weights from the tet of vertices.
    SubDistanceResult<T> r;
    r.closest = Vec3<T>(static_cast<T>(0), static_cast<T>(0), static_cast<T>(0));
    // Barycentric of origin in tet (a,b,c,d) via signed volumes.
    // λ_a ∝ vol(o, b, c, d), etc.; sum = vol(a,b,c,d) (signed). Stable
    // form: 4 cross-product triples.
    const T inv_vol = static_cast<T>(1) / vol_tot;
    // vol(o,b,c,d) = dot(b - o, cross(c - o, d - o)) = dot(b, cross(c, d))
    // Standard cofactor form; signs match the (a,b,c,d) orientation.
    const T lam_a = crd::math::dot(b, crd::math::cross(c, d)) * inv_vol;
    const T lam_b = -crd::math::dot(a, crd::math::cross(c, d)) * inv_vol;
    const T lam_c = crd::math::dot(a, crd::math::cross(b, d)) * inv_vol;
    const T lam_d = -crd::math::dot(a, crd::math::cross(b, c)) * inv_vol;
    r.weights[0] = lam_a;
    r.weights[1] = lam_b;
    r.weights[2] = lam_c;
    r.weights[3] = lam_d;
    r.mask = 0b1111;
    return r;
}

} // namespace crd::geometry::convex::detail
