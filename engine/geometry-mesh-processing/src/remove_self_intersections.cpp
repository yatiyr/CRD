// ---------------------------------------------------------------------------
// crd-geometry-mesh-processing — v7g self-intersection removal.
//
// See header for the algorithm contract. This TU contains:
//   - Möller 1997 robust triangle-triangle intersection (orient3d gated).
//   - Brute-force broadphase pair enumeration (BVH-accelerated form is a
//     v7g-followon optimization for large meshes; see D60 pin below).
//   - Per-triangle segment accumulation with global vertex dedup across
//     triangles via shared per-pair endpoint indices.
//   - Per-triangle CDT retriangulation: project to 2D via drop-largest-
//     normal-axis, epsilon-dedup, run constrained_delaunay, lift back to 3D.
//   - Falls back to keeping the original triangle when its per-face CDT
//     fails (ConstraintsCrossing or similar) — graceful degradation.
//
// **Pinned design decisions** (carried for ADR-0076 §22 amendment at
// v7-close):
//
//   D60. **Brute-force O(n²) broadphase for v7g.** The phase doc spec
//        mentions BVH broadphase via `TriangleMeshBvh`, but the static
//        BvhTree doesn't expose `find_overlapping_pairs` (only the
//        `DynamicBvh` form does). Building a DynamicBvh per call adds
//        complexity that doesn't pay off on the test corpus (small
//        meshes). The brute-force form is O(triangle_count²) test
//        invocations of `aabb_overlap` (cheap) plus Möller on the
//        overlapping pairs. For meshes < ~1k triangles this is
//        comfortable; for larger meshes the v7g-followon optimization
//        will substitute the DynamicBvh form. Pinned explicitly.
//
//   D61. **Möller 1997 with orient3d gate.** Each candidate pair's
//        early-exit "all on one side of the other's plane" test uses
//        `crd::geometry::primitives::orient3d` (Shewchuk adaptive
//        predicate — EXACT sign, FP-roundoff-immune) on both triangles.
//        Once both straddle, the segment endpoint computation uses
//        plain FP (`dot(N, point) + plane_d` for signed distances,
//        linear interpolation for crossing-edge parameters). The exact
//        gate ensures we never enter the FP path for triangles that
//        don't actually intersect.
//
//   D62. **Cross-triangle vertex stitching via per-pair shared indices.**
//        When the Möller test emits a segment for pair (T_i, T_j), the
//        two endpoint positions are appended to the OUTPUT vertex pool
//        ONCE and the resulting global indices are added to BOTH T_i's
//        and T_j's segment list. The two triangles' independent CDT
//        calls then refer to the same global indices for the shared
//        endpoints → the output remains 2-manifold along the cut
//        (modulo per-triangle CDT epsilon-dedup, which can merge
//        nearby endpoints from different pairs into a single vertex).
//
//   D63. **Per-triangle 2D projection via drop-largest-normal-axis.**
//        T's plane normal N has 3 components; we drop the axis with
//        the largest |N| component (so the projection is most orthogonal,
//        minimising 2D coordinate distortion). For example, if T's
//        normal is mostly +Z, drop Z and project to (x, y). If N.z < 0,
//        swap the projected x and y so the 2D winding stays CCW
//        (matching CDT's input expectation).
//
//   D64. **Epsilon-dedup of per-triangle CDT inputs.** Within a single
//        triangle's CDT call, two segment endpoints from different
//        pairs may land on (nearly) coincident 2D points — e.g., when
//        three triangles meet at a common edge. `opts.dedup_epsilon`
//        merges these to a single CDT input vertex; the local→global
//        index remap then aliases them to the same global vertex.
//
//   D65. **Graceful degradation on CDT failure.** If the per-triangle
//        constrained_delaunay returns `ConstraintsCrossing` (segments
//        cross inside the triangle — the case where Bentley-Ottmann
//        pre-insertion would have helped), keep T's original tessellation
//        in the output (= emit the original 3 indices as a single
//        triangle) and increment `triangles_skipped_cdt_failure`.
//        Better to keep the un-cut triangle than emit garbage.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/mesh_processing/half_edge_mesh.hpp>
#include <crd/geometry/mesh_processing/remove_self_intersections.hpp>
#include <crd/geometry/polygon/cdt.hpp>
#include <crd/geometry/primitives/predicates.hpp>
#include <crd/math/scalar.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <optional>

namespace crd::geometry::mesh_processing
{
namespace
{

template <crd::math::MathScalar T>
inline T scalar_abs(T x) noexcept { return x < T{0} ? -x : x; }

template <crd::math::MathScalar T>
inline crd::math::Vec3<crd::f64> to_f64(const crd::math::Vec3<T>& v) noexcept
{
    return crd::math::Vec3<crd::f64>{static_cast<crd::f64>(v.x),
                                       static_cast<crd::f64>(v.y),
                                       static_cast<crd::f64>(v.z)};
}

struct TriTriSegment3D
{
    crd::math::Vec3<crd::f64> p_a;
    crd::math::Vec3<crd::f64> p_b;
};

// Möller 1997 with orient3d-gated early exits. Returns the intersection
// segment (in f64) when the triangles intersect transversally.
template <crd::math::MathScalar T>
std::optional<TriTriSegment3D> moller_tri_tri_intersect(
    const crd::math::Vec3<T>& v0, const crd::math::Vec3<T>& v1, const crd::math::Vec3<T>& v2,
    const crd::math::Vec3<T>& u0, const crd::math::Vec3<T>& u1, const crd::math::Vec3<T>& u2)
{
    using V3D = crd::math::Vec3<crd::f64>;
    const V3D V0 = to_f64(v0);
    const V3D V1 = to_f64(v1);
    const V3D V2 = to_f64(v2);
    const V3D U0 = to_f64(u0);
    const V3D U1 = to_f64(u1);
    const V3D U2 = to_f64(u2);

    // Phase 1: U on T1's plane (orient3d gate).
    const crd::f64 sU0 = crd::geometry::primitives::orient3d(V0, V1, V2, U0);
    const crd::f64 sU1 = crd::geometry::primitives::orient3d(V0, V1, V2, U1);
    const crd::f64 sU2 = crd::geometry::primitives::orient3d(V0, V1, V2, U2);
    if ((sU0 > 0 && sU1 > 0 && sU2 > 0) || (sU0 < 0 && sU1 < 0 && sU2 < 0))
    {
        return std::nullopt; // T2 entirely on one side of T1's plane
    }
    if (sU0 == 0 && sU1 == 0 && sU2 == 0)
    {
        return std::nullopt; // coplanar — deferred to v7f
    }
    // Touch-only cases (≥ 1 sign zero AND remaining signs same): a vertex
    // of T2 lies on T1's plane; the other two are on the same side. The
    // intersection is at most a single point (vertex or edge touching) —
    // not a transversal cut. Defer to v7f manifoldness repair if needed.
    if ((sU0 == 0 && sU1 * sU2 > 0) ||
        (sU1 == 0 && sU0 * sU2 > 0) ||
        (sU2 == 0 && sU0 * sU1 > 0))
    {
        return std::nullopt;
    }
    // Two zeros + one non-zero: edge of T2 lies in T1's plane. Also a
    // touch-only / coplanar-edge case — defer.
    if ((sU0 == 0 && sU1 == 0) || (sU0 == 0 && sU2 == 0) || (sU1 == 0 && sU2 == 0))
    {
        return std::nullopt;
    }

    // Phase 2: V on T2's plane (orient3d gate).
    const crd::f64 sV0 = crd::geometry::primitives::orient3d(U0, U1, U2, V0);
    const crd::f64 sV1 = crd::geometry::primitives::orient3d(U0, U1, U2, V1);
    const crd::f64 sV2 = crd::geometry::primitives::orient3d(U0, U1, U2, V2);
    if ((sV0 > 0 && sV1 > 0 && sV2 > 0) || (sV0 < 0 && sV1 < 0 && sV2 < 0))
    {
        return std::nullopt;
    }
    if ((sV0 == 0 && sV1 * sV2 > 0) ||
        (sV1 == 0 && sV0 * sV2 > 0) ||
        (sV2 == 0 && sV0 * sV1 > 0))
    {
        return std::nullopt;
    }
    if ((sV0 == 0 && sV1 == 0) || (sV0 == 0 && sV2 == 0) || (sV1 == 0 && sV2 == 0))
    {
        return std::nullopt;
    }

    // Compute signed distances and plane normals (FP).
    const V3D E1 = V3D{V1.x - V0.x, V1.y - V0.y, V1.z - V0.z};
    const V3D E2 = V3D{V2.x - V0.x, V2.y - V0.y, V2.z - V0.z};
    const V3D N1 = crd::math::cross(E1, E2);
    const crd::f64 d1 = -crd::math::dot(N1, V0);

    const V3D F1 = V3D{U1.x - U0.x, U1.y - U0.y, U1.z - U0.z};
    const V3D F2 = V3D{U2.x - U0.x, U2.y - U0.y, U2.z - U0.z};
    const V3D N2 = crd::math::cross(F1, F2);
    const crd::f64 d2 = -crd::math::dot(N2, U0);

    const crd::f64 du0 = crd::math::dot(N1, U0) + d1;
    const crd::f64 du1 = crd::math::dot(N1, U1) + d1;
    const crd::f64 du2 = crd::math::dot(N1, U2) + d1;
    const crd::f64 dv0 = crd::math::dot(N2, V0) + d2;
    const crd::f64 dv1 = crd::math::dot(N2, V1) + d2;
    const crd::f64 dv2 = crd::math::dot(N2, V2) + d2;

    // Compute interval endpoints in 3D for each triangle.
    auto compute_interval = [](V3D A, V3D B, V3D C, crd::f64 dA, crd::f64 dB, crd::f64 dC)
        -> std::pair<V3D, V3D> {
        // Permute so A is the "alone" vertex.
        const bool bc_same = (dB > 0) == (dC > 0);
        const bool ac_same = (dA > 0) == (dC > 0);
        if (!bc_same)
        {
            if (ac_same)
            {
                std::swap(A, B);
                std::swap(dA, dB);
            }
            else
            {
                std::swap(A, C);
                std::swap(dA, dC);
            }
        }
        // A is alone. Crossings on edges AB and AC.
        // Avoid division by zero: if dA == dB or dA == dC, the edge is in
        // the plane (degenerate; should have been caught by all-zero check).
        const crd::f64 denomAB = dA - dB;
        const crd::f64 denomAC = dA - dC;
        const crd::f64 tAB = denomAB == 0.0 ? 0.0 : dA / denomAB;
        const crd::f64 tAC = denomAC == 0.0 ? 0.0 : dA / denomAC;
        const V3D P_AB{A.x + (B.x - A.x) * tAB,
                        A.y + (B.y - A.y) * tAB,
                        A.z + (B.z - A.z) * tAB};
        const V3D P_AC{A.x + (C.x - A.x) * tAC,
                        A.y + (C.y - A.y) * tAC,
                        A.z + (C.z - A.z) * tAC};
        return {P_AB, P_AC};
    };

    auto [P1a, P1b] = compute_interval(V0, V1, V2, dv0, dv1, dv2);
    auto [P2a, P2b] = compute_interval(U0, U1, U2, du0, du1, du2);

    // Project onto the longest axis of the intersection line N1×N2.
    const V3D L = crd::math::cross(N1, N2);
    int        axis = 0;
    crd::f64   max_abs = scalar_abs(L.x);
    if (scalar_abs(L.y) > max_abs) { axis = 1; max_abs = scalar_abs(L.y); }
    if (scalar_abs(L.z) > max_abs) { axis = 2; max_abs = scalar_abs(L.z); }
    auto       get_axis = [axis](const V3D& v) {
        return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
    };
    const crd::f64 q1a = get_axis(P1a);
    const crd::f64 q1b = get_axis(P1b);
    const crd::f64 q2a = get_axis(P2a);
    const crd::f64 q2b = get_axis(P2b);
    const crd::f64 t1_min = q1a < q1b ? q1a : q1b;
    const crd::f64 t1_max = q1a < q1b ? q1b : q1a;
    const crd::f64 t2_min = q2a < q2b ? q2a : q2b;
    const crd::f64 t2_max = q2a < q2b ? q2b : q2a;
    const crd::f64 overlap_min = t1_min > t2_min ? t1_min : t2_min;
    const crd::f64 overlap_max = t1_max < t2_max ? t1_max : t2_max;
    if (overlap_max <= overlap_min) { return std::nullopt; }

    // Look up 3D points whose axis values match overlap_min / overlap_max.
    auto closest = [&](crd::f64 target) -> V3D {
        crd::f64 best_diff = scalar_abs(q1a - target);
        V3D      best      = P1a;
        crd::f64 diff;
        diff = scalar_abs(q1b - target);
        if (diff < best_diff) { best_diff = diff; best = P1b; }
        diff = scalar_abs(q2a - target);
        if (diff < best_diff) { best_diff = diff; best = P2a; }
        diff = scalar_abs(q2b - target);
        if (diff < best_diff) { best = P2b; }
        return best;
    };

    return TriTriSegment3D{closest(overlap_min), closest(overlap_max)};
}

// Per-triangle AABB.
template <crd::math::MathScalar T>
struct TriAABB
{
    crd::math::Vec3<T> lo;
    crd::math::Vec3<T> hi;
};

template <crd::math::MathScalar T>
TriAABB<T> compute_tri_aabb(const crd::math::Vec3<T>& a,
                              const crd::math::Vec3<T>& b,
                              const crd::math::Vec3<T>& c) noexcept
{
    TriAABB<T> r;
    r.lo.x = a.x < b.x ? (a.x < c.x ? a.x : c.x) : (b.x < c.x ? b.x : c.x);
    r.lo.y = a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y);
    r.lo.z = a.z < b.z ? (a.z < c.z ? a.z : c.z) : (b.z < c.z ? b.z : c.z);
    r.hi.x = a.x > b.x ? (a.x > c.x ? a.x : c.x) : (b.x > c.x ? b.x : c.x);
    r.hi.y = a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y);
    r.hi.z = a.z > b.z ? (a.z > c.z ? a.z : c.z) : (b.z > c.z ? b.z : c.z);
    return r;
}

template <crd::math::MathScalar T>
inline bool aabb_overlap(const TriAABB<T>& a, const TriAABB<T>& b) noexcept
{
    if (a.hi.x < b.lo.x || b.hi.x < a.lo.x) { return false; }
    if (a.hi.y < b.lo.y || b.hi.y < a.lo.y) { return false; }
    if (a.hi.z < b.lo.z || b.hi.z < a.lo.z) { return false; }
    return true;
}

struct SegmentInTri
{
    crd::u32 endpoint_a; // global vertex index (into output_positions)
    crd::u32 endpoint_b;
};

// Project 3D point to 2D by dropping the largest-magnitude axis of the
// triangle's plane normal. Returns (x', y') in 2D.
template <crd::math::MathScalar T>
crd::math::Vec2<T> project_to_2d(const crd::math::Vec3<T>& p, int drop_axis,
                                   bool flip_winding) noexcept
{
    T a, b;
    switch (drop_axis)
    {
        case 0: a = p.y; b = p.z; break;
        case 1: a = p.z; b = p.x; break;
        default: a = p.x; b = p.y; break;
    }
    if (flip_winding) { return crd::math::Vec2<T>{b, a}; }
    return crd::math::Vec2<T>{a, b};
}

// Compute drop axis + winding-flip flag for a triangle's plane normal.
template <crd::math::MathScalar T>
void compute_projection_axis(const crd::math::Vec3<T>& normal, int& out_drop_axis,
                              bool& out_flip_winding) noexcept
{
    const T ax = scalar_abs(normal.x);
    const T ay = scalar_abs(normal.y);
    const T az = scalar_abs(normal.z);
    if (ax >= ay && ax >= az)
    {
        out_drop_axis    = 0;
        out_flip_winding = normal.x < T{0};
    }
    else if (ay >= ax && ay >= az)
    {
        out_drop_axis    = 1;
        out_flip_winding = normal.y < T{0};
    }
    else
    {
        out_drop_axis    = 2;
        out_flip_winding = normal.z < T{0};
    }
}

// Retriangulate one triangle T (vertices a, b, c, global indices) using a
// per-triangle CDT with constraint edges = T's boundary + the intersection
// segments. Appends sub-triangle indices to `out_indices`. Returns true on
// success; false if CDT failed (caller falls back to keeping T unchanged).
template <crd::math::MathScalar T>
bool retriangulate_with_segments(const crd::containers::Array<crd::math::Vec3<T>>& positions,
                                   crd::u32 va, crd::u32 vb, crd::u32 vc,
                                   const crd::containers::Array<SegmentInTri>&     segments,
                                   T                                                dedup_epsilon,
                                   crd::containers::Array<crd::u32>&                out_indices,
                                   crd::memory::IAllocator*                         alloc)
{
    if (segments.empty()) { return false; } // caller emits T as-is

    const auto& pa = positions[va];
    const auto& pb = positions[vb];
    const auto& pc = positions[vc];

    // Plane normal for the projection.
    const auto e1 = pb - pa;
    const auto e2 = pc - pa;
    const auto normal = crd::math::cross(e1, e2);
    int  drop_axis    = 0;
    bool flip_winding = false;
    compute_projection_axis(normal, drop_axis, flip_winding);

    // Collect unique global vertices: T's 3 corners + all segment endpoints.
    crd::containers::Array<crd::u32> globals(alloc);
    globals.push_back(va);
    globals.push_back(vb);
    globals.push_back(vc);
    for (crd::u32 i = 0; i < segments.size(); ++i)
    {
        globals.push_back(segments[i].endpoint_a);
        globals.push_back(segments[i].endpoint_b);
    }

    // Project to 2D. Epsilon-dedup by linear scan.
    crd::containers::Array<crd::math::Vec2<T>> points_2d(alloc);
    crd::containers::Array<crd::u32>           dedup_to_unique(alloc); // index into globals → unique index
    dedup_to_unique.resize(globals.size(), crd::u32{0});
    crd::containers::Array<crd::u32>           unique_to_global(alloc);
    for (crd::u32 i = 0; i < globals.size(); ++i)
    {
        const auto p2 = project_to_2d(positions[globals[i]], drop_axis, flip_winding);
        bool       found = false;
        for (crd::u32 u = 0; u < points_2d.size(); ++u)
        {
            const T dx = p2.x - points_2d[u].x;
            const T dy = p2.y - points_2d[u].y;
            if (scalar_abs(dx) <= dedup_epsilon && scalar_abs(dy) <= dedup_epsilon)
            {
                dedup_to_unique[i] = u;
                found = true;
                break;
            }
        }
        if (!found)
        {
            dedup_to_unique[i] = static_cast<crd::u32>(points_2d.size());
            points_2d.push_back(p2);
            unique_to_global.push_back(globals[i]);
        }
    }

    if (points_2d.size() < 3U) { return false; } // degenerate

    // Build constraints. Triangle boundary edges + segment edges. Use
    // canonical-pair form to avoid duplicates.
    crd::containers::Array<crd::geometry::polygon::CdtEdge> constraints(alloc);
    auto push_edge = [&](crd::u32 a, crd::u32 b) {
        if (a == b) { return; }
        for (crd::u32 i = 0; i < constraints.size(); ++i)
        {
            const auto& e = constraints[i];
            if ((e.a == a && e.b == b) || (e.a == b && e.b == a)) { return; }
        }
        constraints.push_back(crd::geometry::polygon::CdtEdge{a, b});
    };
    // T's 3 boundary edges (deduped against the 0,1,2 indices into globals).
    const crd::u32 u_va = dedup_to_unique[0];
    const crd::u32 u_vb = dedup_to_unique[1];
    const crd::u32 u_vc = dedup_to_unique[2];
    push_edge(u_va, u_vb);
    push_edge(u_vb, u_vc);
    push_edge(u_vc, u_va);
    // Segment constraints.
    for (crd::u32 i = 0; i < segments.size(); ++i)
    {
        const crd::u32 ga = dedup_to_unique[3U + 2U * i + 0U];
        const crd::u32 gb = dedup_to_unique[3U + 2U * i + 1U];
        push_edge(ga, gb);
    }

    // Run CDT.
    crd::geometry::polygon::CdtOptions cdt_opts{};
    cdt_opts.keep_only_inside_polygon = false; // we're triangulating a known polygon (T's interior)
    auto cdt_result = crd::geometry::polygon::constrained_delaunay<T>(
        crd::containers::ConstSpan<crd::math::Vec2<T>>{points_2d.data(), points_2d.size()},
        crd::containers::ConstSpan<crd::geometry::polygon::CdtEdge>{constraints.data(), constraints.size()},
        alloc,
        cdt_opts);
    if (cdt_result.status != crd::geometry::polygon::CdtStatus::Ok)
    {
        return false; // graceful degradation
    }

    // Emit sub-triangles, mapping unique-index → global vertex index.
    // For each CDT triangle, if its 3 vertices in CDT space lie INSIDE
    // T (= the area enclosed by the original 3 corners), emit it.
    // Otherwise (= outside the triangle, an artifact of the super-triangle
    // strip and segments that extend beyond T), skip it.
    //
    // Since CDT operates on the convex hull of input points, and our
    // constraint set INCLUDES T's boundary edges, the CDT may produce
    // triangles outside T if our 2D input has points lying outside the
    // original T's 2D footprint. For v7g, all segment endpoints lie
    // strictly INSIDE T (Möller emits endpoints clipped to T's interior),
    // so all CDT output triangles should be inside T. We do a quick
    // sanity check via barycentric centroid test against T's 2D form.

    const auto t_p0 = points_2d[u_va];
    const auto t_p1 = points_2d[u_vb];
    const auto t_p2 = points_2d[u_vc];
    // Signed area of T (2D).
    const T t_area2 = (t_p1.x - t_p0.x) * (t_p2.y - t_p0.y)
                      - (t_p2.x - t_p0.x) * (t_p1.y - t_p0.y);
    const T t_sign = t_area2 < T{0} ? T{-1} : T{1};
    // For each CDT triangle, accept if its centroid is inside T (barycentric).
    for (crd::u32 ti = 0; ti + 2U < cdt_result.triangle_indices.size(); ti += 3U)
    {
        const crd::u32 a = cdt_result.triangle_indices[ti + 0];
        const crd::u32 b = cdt_result.triangle_indices[ti + 1];
        const crd::u32 c = cdt_result.triangle_indices[ti + 2];
        const auto& pa2 = points_2d[a];
        const auto& pb2 = points_2d[b];
        const auto& pc2 = points_2d[c];
        const T centroid_x = (pa2.x + pb2.x + pc2.x) / T{3};
        const T centroid_y = (pa2.y + pb2.y + pc2.y) / T{3};
        // Barycentric in/out test (centroid vs T):
        // Centroid is INSIDE T iff all three barycentric coordinates
        // (computed by signed-area sub-triangles) have the same sign as
        // t_sign.
        auto signed_sub_area = [&](crd::math::Vec2<T> A, crd::math::Vec2<T> B, T cx, T cy) {
            return (B.x - A.x) * (cy - A.y) - (cx - A.x) * (B.y - A.y);
        };
        const T s0 = signed_sub_area(t_p0, t_p1, centroid_x, centroid_y);
        const T s1 = signed_sub_area(t_p1, t_p2, centroid_x, centroid_y);
        const T s2 = signed_sub_area(t_p2, t_p0, centroid_x, centroid_y);
        const bool inside_t = (s0 * t_sign >= T{0}) && (s1 * t_sign >= T{0})
                              && (s2 * t_sign >= T{0});
        if (!inside_t) { continue; }
        // Map and emit in original (3D-CCW) orientation. If we flipped
        // winding for the projection, un-flip on output.
        const crd::u32 ga = unique_to_global[a];
        const crd::u32 gb = unique_to_global[b];
        const crd::u32 gc = unique_to_global[c];
        if (flip_winding)
        {
            out_indices.push_back(ga);
            out_indices.push_back(gc);
            out_indices.push_back(gb);
        }
        else
        {
            out_indices.push_back(ga);
            out_indices.push_back(gb);
            out_indices.push_back(gc);
        }
    }
    return true;
}

} // anonymous namespace

template <crd::math::MathScalar T>
HalfEdgeMesh<T> remove_self_intersections(const HalfEdgeMesh<T>&                       input,
                                            const RemoveSelfIntersectionsOptions<T>&   opts,
                                            RemoveSelfIntersectionsReport*             out_report)
{
    RemoveSelfIntersectionsReport report{};
    auto                          report_out = [&] {
        if (out_report != nullptr) { *out_report = report; }
    };

    crd::memory::IAllocator* alloc = opts.output_allocator != nullptr
                                          ? opts.output_allocator
                                          : input.allocator();
    CRD_ASSERT(alloc != nullptr);

    if (input.face_count() == 0U)
    {
        report.status = RemoveSelfIntersectionsStatus::EmptyMesh;
        report_out();
        return HalfEdgeMesh<T>{alloc};
    }

    // Extract input to indexed form.
    crd::containers::Array<crd::math::Vec3<T>> positions(alloc);
    crd::containers::Array<crd::u32>           indices(alloc);
    input.to_indexed(positions, indices);

    const crd::u32 tri_count = static_cast<crd::u32>(indices.size() / 3U);
    if (tri_count == 0U)
    {
        report.status = RemoveSelfIntersectionsStatus::EmptyMesh;
        report_out();
        return HalfEdgeMesh<T>{alloc};
    }

    // Precompute per-triangle AABBs.
    crd::containers::Array<TriAABB<T>> aabbs(alloc);
    aabbs.resize(tri_count, TriAABB<T>{});
    for (crd::u32 t = 0; t < tri_count; ++t)
    {
        aabbs[t] = compute_tri_aabb(positions[indices[3U * t + 0]],
                                       positions[indices[3U * t + 1]],
                                       positions[indices[3U * t + 2]]);
    }

    // Per-triangle segment list (built up by pair enumeration).
    crd::containers::Array<crd::containers::Array<SegmentInTri>> segments_per_tri(alloc);
    for (crd::u32 t = 0; t < tri_count; ++t)
    {
        segments_per_tri.push_back(crd::containers::Array<SegmentInTri>{alloc});
    }

    // O(n²) broadphase (D60). Test each pair (i, j) with i < j; skip
    // adjacent triangles (sharing 1+ vertices — these "intersect" at
    // their shared edge/vertex but it's not a self-intersection).
    auto share_vertex = [&](crd::u32 ti, crd::u32 tj) {
        const crd::u32 a0 = indices[3U * ti + 0];
        const crd::u32 a1 = indices[3U * ti + 1];
        const crd::u32 a2 = indices[3U * ti + 2];
        const crd::u32 b0 = indices[3U * tj + 0];
        const crd::u32 b1 = indices[3U * tj + 1];
        const crd::u32 b2 = indices[3U * tj + 2];
        return a0 == b0 || a0 == b1 || a0 == b2 || a1 == b0 || a1 == b1 || a1 == b2
               || a2 == b0 || a2 == b1 || a2 == b2;
    };

    for (crd::u32 i = 0; i < tri_count; ++i)
    {
        for (crd::u32 j = i + 1; j < tri_count; ++j)
        {
            if (!aabb_overlap(aabbs[i], aabbs[j])) { continue; }
            if (share_vertex(i, j)) { continue; }
            ++report.candidate_pairs_tested;
            const auto& v0 = positions[indices[3U * i + 0]];
            const auto& v1 = positions[indices[3U * i + 1]];
            const auto& v2 = positions[indices[3U * i + 2]];
            const auto& u0 = positions[indices[3U * j + 0]];
            const auto& u1 = positions[indices[3U * j + 1]];
            const auto& u2 = positions[indices[3U * j + 2]];
            auto seg = moller_tri_tri_intersect(v0, v1, v2, u0, u1, u2);
            if (!seg) { continue; }
            ++report.intersection_pairs_detected;
            // Append the segment's endpoints to the positions array, share
            // global indices between T_i and T_j.
            const crd::u32 idx_a = static_cast<crd::u32>(positions.size());
            positions.push_back(crd::math::Vec3<T>{static_cast<T>(seg->p_a.x),
                                                     static_cast<T>(seg->p_a.y),
                                                     static_cast<T>(seg->p_a.z)});
            const crd::u32 idx_b = static_cast<crd::u32>(positions.size());
            positions.push_back(crd::math::Vec3<T>{static_cast<T>(seg->p_b.x),
                                                     static_cast<T>(seg->p_b.y),
                                                     static_cast<T>(seg->p_b.z)});
            report.intersection_vertices_added += 2U;
            segments_per_tri[i].push_back(SegmentInTri{idx_a, idx_b});
            segments_per_tri[j].push_back(SegmentInTri{idx_a, idx_b});
        }
    }

    // Build output index buffer: emit triangles unchanged unless they have
    // segments; for those, run per-triangle CDT retriangulation.
    crd::containers::Array<crd::u32> out_indices(alloc);
    out_indices.reserve(indices.size());
    for (crd::u32 t = 0; t < tri_count; ++t)
    {
        const crd::u32 va = indices[3U * t + 0];
        const crd::u32 vb = indices[3U * t + 1];
        const crd::u32 vc = indices[3U * t + 2];
        if (segments_per_tri[t].empty())
        {
            out_indices.push_back(va);
            out_indices.push_back(vb);
            out_indices.push_back(vc);
            continue;
        }
        const crd::u32 before = static_cast<crd::u32>(out_indices.size());
        const bool ok = retriangulate_with_segments(positions, va, vb, vc,
                                                       segments_per_tri[t],
                                                       opts.dedup_epsilon,
                                                       out_indices,
                                                       alloc);
        if (!ok)
        {
            // CDT failed; fall back to keeping the original.
            out_indices.resize(before);
            out_indices.push_back(va);
            out_indices.push_back(vb);
            out_indices.push_back(vc);
            ++report.triangles_skipped_cdt_failure;
        }
        else
        {
            ++report.triangles_retriangulated;
        }
    }

    // Build output mesh.
    HalfEdgeMesh<T> output{alloc};
    const auto bs = output.build_from(
        crd::containers::ConstSpan<crd::math::Vec3<T>>{positions.data(), positions.size()},
        crd::containers::ConstSpan<crd::u32>{out_indices.data(), out_indices.size()});
    (void)bs;

    if (report.intersection_pairs_detected == 0U)
    {
        report.status = RemoveSelfIntersectionsStatus::NoSelfIntersections;
    }
    report.output_vertices = output.vertex_count();
    report.output_faces    = output.face_count();
    report_out();
    return output;
}

template HalfEdgeMesh<crd::f32> remove_self_intersections<crd::f32>(
    const HalfEdgeMesh<crd::f32>&,
    const RemoveSelfIntersectionsOptions<crd::f32>&,
    RemoveSelfIntersectionsReport*);
template HalfEdgeMesh<crd::f64> remove_self_intersections<crd::f64>(
    const HalfEdgeMesh<crd::f64>&,
    const RemoveSelfIntersectionsOptions<crd::f64>&,
    RemoveSelfIntersectionsReport*);

} // namespace crd::geometry::mesh_processing
