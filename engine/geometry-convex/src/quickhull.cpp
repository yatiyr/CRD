// ---------------------------------------------------------------------------
// crd-geometry-convex — 3D Quickhull implementation (Phase 3.1.7 v3c;
// ADR-0076 §4 pin #11, §18).
//
// Internal structure:
//
//   1. `QhFace` — internal triangle face with vertex indices into the input
//      array, 3 neighbor face indices, cached outward normal + plane offset,
//      outside-set (input-point indices above the face), tombstone flag.
//
//   2. `QhMesh` — the working data: an Array<QhFace>. Faces are added by
//      append; removed by tombstone (so other faces' neighbor indices stay
//      stable). A final compaction pass rebuilds the result arrays.
//
//   3. `quickhull_impl` — the main driver. Decomposes into:
//        a. Initial tetrahedron construction (with degenerate fallbacks).
//        b. Outside-set initialization.
//        c. Main iteration loop.
//        d. Result extraction.
//
// **Determinism (ADR-0076 §4 pin #11)**: every face/point/neighbor traversal
// proceeds in increasing index order; "furthest" / "highest" ties break by
// lowest input index; sign tests use v3a `orient3d` (full Stage D — bit-
// exact across compilers / SIMD / OS).
//
// **Convention pins**:
//   - Face vertex order is CCW from outside (`orient3d(v0, v1, v2, p) > 0`
//     when p is INSIDE the hull — Shewchuk's "below = positive").
//   - Outward normal direction: a point p is OUTSIDE the face when
//     `orient3d(v0, v1, v2, p) < 0`. This is the Shewchuk convention
//     applied consistently to outward-facing hull faces.
// ---------------------------------------------------------------------------

#include <crd/geometry/convex/quickhull.hpp>

#include <crd/geometry/convex/convex_hull_2d.hpp>
#include <crd/geometry/primitives/hull_adjacency.hpp>
#include <crd/geometry/primitives/is_finite.hpp>
#include <crd/geometry/primitives/predicates.hpp>

#include <cmath>
#include <type_traits>

namespace crd::geometry::convex
{
namespace quickhull_detail
{
template <crd::math::MathScalar T> using Vec3T = crd::math::Vec3<T>;

// Internal face record.
template <crd::math::MathScalar T> struct QhFace
{
    crd::u32 v[3];          // input vertex indices (CCW from outside)
    crd::u32 neighbors[3];  // neighboring face indices; ~0u = uninitialized
    Vec3T<T> normal;        // outward normal (cached; for fast distance tests)
    T plane_d;              // plane offset: normal·v0 + plane_d == 0 (after building)
    crd::containers::Array<crd::u32> outside_set; // input point indices above this face
    bool removed = false;   // tombstone — face replaced by horizon-walk

    explicit QhFace(crd::memory::IAllocator* alloc) noexcept : outside_set(alloc)
    {
        v[0] = v[1] = v[2] = 0;
        neighbors[0] = neighbors[1] = neighbors[2] = ~crd::u32{0};
        normal = Vec3T<T>(0, 0, 0);
        plane_d = static_cast<T>(0);
    }

    QhFace(const QhFace&) = delete;
    QhFace& operator=(const QhFace&) = delete;
    QhFace(QhFace&&) noexcept = default;
    QhFace& operator=(QhFace&&) noexcept = default;
};

// Signed distance from point `p` to face's plane, in the outward-normal
// direction. Positive = above (outside hull); negative = below (inside).
// Uses cached normal + plane_d for the hot path; sign-correctness for the
// EXACT decision uses orient3d separately.
template <crd::math::MathScalar T>
[[nodiscard]] inline T plane_distance(const QhFace<T>& f, const Vec3T<T>& p) noexcept
{
    return crd::math::dot(f.normal, p) + f.plane_d;
}

// Compute and cache the outward face plane for face `f` given the 3 input
// vertices' positions + the 4th input vertex (interior witness) that must lie
// on the inside half-space (orient3d(v0, v1, v2, interior) > 0 in Shewchuk
// convention — interior is "below" = inside).
template <crd::math::MathScalar T>
inline void compute_face_plane(QhFace<T>& f, const Vec3T<T>& v0, const Vec3T<T>& v1,
                                const Vec3T<T>& v2) noexcept
{
    const Vec3T<T> edge1 = v1 - v0;
    const Vec3T<T> edge2 = v2 - v0;
    Vec3T<T> n = crd::math::cross(edge1, edge2);
    const T n_len = std::sqrt(crd::math::dot(n, n));
    if (n_len > static_cast<T>(0))
    {
        n = n * (static_cast<T>(1) / n_len);
    }
    f.normal = n;
    f.plane_d = -crd::math::dot(n, v0);
}

// Squared distance from point `p` to line through (a, b).
template <crd::math::MathScalar T>
[[nodiscard]] inline T distance_squared_to_line(const Vec3T<T>& p, const Vec3T<T>& a,
                                                  const Vec3T<T>& b) noexcept
{
    const Vec3T<T> ab = b - a;
    const Vec3T<T> ap = p - a;
    const T ab_dot_ab = crd::math::dot(ab, ab);
    if (ab_dot_ab <= static_cast<T>(0))
    {
        return crd::math::dot(ap, ap);
    }
    const T t = crd::math::dot(ap, ab) / ab_dot_ab;
    const Vec3T<T> closest = a + ab * t;
    const Vec3T<T> diff = p - closest;
    return crd::math::dot(diff, diff);
}

// Find the initial 4-point tetrahedron from the input points. Returns:
//   - 4 distinct input indices if successful.
//   - sets `is_coincident` / `is_colinear` / `is_coplanar` flag if the input
//     degenerates below 3D.
template <crd::math::MathScalar T>
[[nodiscard]] bool find_initial_tetrahedron(crd::containers::ConstSpan<Vec3T<T>> points,
                                              T degenerate_eps,
                                              crd::u32& out_p0, crd::u32& out_p1,
                                              crd::u32& out_p2, crd::u32& out_p3,
                                              bool& out_coincident, bool& out_colinear,
                                              bool& out_coplanar) noexcept
{
    out_coincident = false;
    out_colinear = false;
    out_coplanar = false;

    const crd::usize n = points.size();
    if (n == 0)
    {
        return false;
    }

    // Step 1: find 6 axis-extremal indices (min/max on x, y, z).
    crd::u32 ext[6] = {0, 0, 0, 0, 0, 0}; // min_x, max_x, min_y, max_y, min_z, max_z
    for (crd::u32 i = 1; i < n; ++i)
    {
        if (points[i].x < points[ext[0]].x)
            ext[0] = i;
        if (points[i].x > points[ext[1]].x)
            ext[1] = i;
        if (points[i].y < points[ext[2]].y)
            ext[2] = i;
        if (points[i].y > points[ext[3]].y)
            ext[3] = i;
        if (points[i].z < points[ext[4]].z)
            ext[4] = i;
        if (points[i].z > points[ext[5]].z)
            ext[5] = i;
    }

    // Step 2: pick the 2 most-spread extremals (largest pairwise distance).
    T best_dist_sq = static_cast<T>(0);
    crd::u32 p0 = 0;
    crd::u32 p1 = 0;
    for (int i = 0; i < 6; ++i)
    {
        for (int j = i + 1; j < 6; ++j)
        {
            const Vec3T<T> diff = points[ext[j]] - points[ext[i]];
            const T d_sq = crd::math::dot(diff, diff);
            if (d_sq > best_dist_sq)
            {
                best_dist_sq = d_sq;
                p0 = ext[i] < ext[j] ? ext[i] : ext[j];
                p1 = ext[i] < ext[j] ? ext[j] : ext[i];
            }
        }
    }

    if (best_dist_sq <= static_cast<T>(0))
    {
        // All points are coincident.
        out_coincident = true;
        out_p0 = 0;
        return false;
    }

    // Step 3: find point furthest from line p0-p1.
    T best_line_dist_sq = static_cast<T>(0);
    crd::u32 p2 = ~crd::u32{0};
    for (crd::u32 i = 0; i < n; ++i)
    {
        if (i == p0 || i == p1)
            continue;
        const T d_sq = distance_squared_to_line(points[i], points[p0], points[p1]);
        if (d_sq > best_line_dist_sq ||
            (d_sq == best_line_dist_sq && p2 != ~crd::u32{0} && i < p2))
        {
            best_line_dist_sq = d_sq;
            p2 = i;
        }
    }

    if (p2 == ~crd::u32{0} || best_line_dist_sq < degenerate_eps * best_dist_sq)
    {
        // All points are collinear.
        out_colinear = true;
        out_p0 = p0;
        out_p1 = p1;
        return false;
    }

    // Step 4: find point furthest from plane p0-p1-p2 using orient3d sign +
    // magnitude. We use the full Stage D for the sign (degeneracy decision)
    // and the cached normal·distance for the magnitude.
    const Vec3T<T> v0 = points[p0];
    const Vec3T<T> v1 = points[p1];
    const Vec3T<T> v2 = points[p2];
    const Vec3T<T> edge1 = v1 - v0;
    const Vec3T<T> edge2 = v2 - v0;
    Vec3T<T> normal_p012 = crd::math::cross(edge1, edge2);
    const T n_len = std::sqrt(crd::math::dot(normal_p012, normal_p012));
    if (n_len <= static_cast<T>(0))
    {
        out_colinear = true;
        out_p0 = p0;
        out_p1 = p1;
        return false;
    }
    normal_p012 = normal_p012 * (static_cast<T>(1) / n_len);
    const T plane_d_012 = -crd::math::dot(normal_p012, v0);

    T best_abs_dist = static_cast<T>(0);
    crd::u32 p3 = ~crd::u32{0};
    for (crd::u32 i = 0; i < n; ++i)
    {
        if (i == p0 || i == p1 || i == p2)
            continue;
        const T d = crd::math::dot(normal_p012, points[i]) + plane_d_012;
        const T abs_d = std::fabs(d);
        if (abs_d > best_abs_dist || (abs_d == best_abs_dist && p3 != ~crd::u32{0} && i < p3))
        {
            best_abs_dist = abs_d;
            p3 = i;
        }
    }

    if (p3 == ~crd::u32{0} || best_abs_dist < degenerate_eps * std::sqrt(best_dist_sq))
    {
        // All points are coplanar.
        out_coplanar = true;
        out_p0 = p0;
        out_p1 = p1;
        out_p2 = p2;
        return false;
    }

    // Use exact orient3d (Stage D) for the final degeneracy decision. If the
    // exact sign is zero despite a non-trivial f64 distance, the points are
    // actually coplanar within f64 representation.
    // For QhFace's f32/f64 templated path: orient3d has both overloads; f32
    // promotes to f64 internally for the adaptive path.
    const T exact_det = crd::geometry::primitives::orient3d(v0, v1, v2, points[p3]);
    if (exact_det == static_cast<T>(0))
    {
        out_coplanar = true;
        out_p0 = p0;
        out_p1 = p1;
        out_p2 = p2;
        return false;
    }

    out_p0 = p0;
    out_p1 = p1;
    out_p2 = p2;
    out_p3 = p3;
    return true;
}

// Build the 4 faces of the initial tetrahedron. Each face's vertex order is
// CCW from outside (with the 4th vertex on the inside / Shewchuk-"below"
// side). Sets neighbor references.
template <crd::math::MathScalar T>
void build_initial_tetrahedron(crd::containers::Array<QhFace<T>>& mesh,
                                crd::containers::ConstSpan<Vec3T<T>> points, crd::u32 p0,
                                crd::u32 p1, crd::u32 p2, crd::u32 p3,
                                crd::memory::IAllocator* alloc) noexcept
{
    // Orient (p0, p1, p2) such that p3 is "below" (inside, Shewchuk
    // convention orient3d > 0). If orient3d < 0, swap p1 and p2 to flip the
    // triangle.
    const T det = crd::geometry::primitives::orient3d(points[p0], points[p1], points[p2], points[p3]);
    crd::u32 a = p0;
    crd::u32 b = p1;
    crd::u32 c = p2;
    if (det < static_cast<T>(0))
    {
        const crd::u32 swap_tmp = b;
        b = c;
        c = swap_tmp;
    }

    // Helper to construct a face given vertex indices + an interior witness.
    auto make_face = [&](crd::u32 va, crd::u32 vb, crd::u32 vc, crd::u32 interior) {
        QhFace<T> f(alloc);
        f.v[0] = va;
        f.v[1] = vb;
        f.v[2] = vc;
        compute_face_plane(f, points[va], points[vb], points[vc]);
        // Verify orientation: orient3d(va, vb, vc, interior) should be > 0
        // (interior is "below" outward normal). If not, flip vb and vc.
        const T orient = crd::geometry::primitives::orient3d(points[va], points[vb], points[vc],
                                                              points[interior]);
        if (orient < static_cast<T>(0))
        {
            const crd::u32 tmp = f.v[1];
            f.v[1] = f.v[2];
            f.v[2] = tmp;
            compute_face_plane(f, points[f.v[0]], points[f.v[1]], points[f.v[2]]);
        }
        return f;
    };

    // 4 tetra faces — each "opposite" one vertex of {a, b, c, p3}:
    //   F0 opposite p3: (a, b, c)
    //   F1 opposite a:  (b, c, p3)
    //   F2 opposite b:  (a, p3, c)  [flipped for outward orientation]
    //   F3 opposite c:  (a, b, p3)  [flipped]
    mesh.push_back(make_face(a, b, c, p3));     // F0
    mesh.push_back(make_face(b, c, p3, a));     // F1
    mesh.push_back(make_face(a, p3, c, b));     // F2
    mesh.push_back(make_face(a, b, p3, c));     // F3

    // Set neighbor relationships. For a tetrahedron, face F_i is opposite
    // vertex v_i, and each face shares an edge with each of the other 3
    // faces. The neighbor across edge (v_a, v_b) of face F is the OTHER
    // face that also contains both v_a and v_b.
    //
    // We assign neighbors by edge-matching. Face F's edge `e` is between
    // F.v[e] and F.v[(e+1) % 3]. Find the OTHER face that contains the same
    // edge (in EITHER direction since the other face's CCW will reverse it).
    for (crd::u32 fi = 0; fi < mesh.size(); ++fi)
    {
        for (int e = 0; e < 3; ++e)
        {
            const crd::u32 va = mesh[fi].v[e];
            const crd::u32 vb = mesh[fi].v[(e + 1) % 3];
            for (crd::u32 gi = 0; gi < mesh.size(); ++gi)
            {
                if (gi == fi)
                    continue;
                for (int g_e = 0; g_e < 3; ++g_e)
                {
                    const crd::u32 ga = mesh[gi].v[g_e];
                    const crd::u32 gb = mesh[gi].v[(g_e + 1) % 3];
                    // Shared edge in either direction.
                    if ((ga == va && gb == vb) || (ga == vb && gb == va))
                    {
                        mesh[fi].neighbors[e] = gi;
                        break;
                    }
                }
                if (mesh[fi].neighbors[e] != ~crd::u32{0})
                    break;
            }
        }
    }
}

} // namespace quickhull_detail

// ===========================================================================
// quickhull — public entry point (skeleton for v3c-a; iteration loop lands
// in the next seam).
// ===========================================================================

template <crd::math::MathScalar T>
QuickhullResult<T> quickhull(crd::containers::ConstSpan<crd::math::Vec3<T>> points,
                              crd::memory::IAllocator* alloc,
                              const QuickhullOptions<T>& opts) noexcept
{
    using namespace quickhull_detail;
    QuickhullResult<T> result(alloc);

    const crd::usize n = points.size();

    // Builder-reject contract (ADR-0076 §15): finite inputs only in debug.
    for (crd::usize i = 0; i < n; ++i)
    {
        CRD_ASSERT(std::isfinite(points[i].x) && std::isfinite(points[i].y) &&
                    std::isfinite(points[i].z));
    }

    // Empty / single-point / two-distinct-points degenerate cases.
    if (n == 0)
    {
        return result;
    }
    if (n == 1)
    {
        result.vertices.push_back(points[0]);
        result.is_coincident = true;
        return result;
    }
    if (n == 2)
    {
        if (points[0].x == points[1].x && points[0].y == points[1].y && points[0].z == points[1].z)
        {
            result.vertices.push_back(points[0]);
            result.is_coincident = true;
        }
        else
        {
            result.vertices.push_back(points[0]);
            result.vertices.push_back(points[1]);
            result.is_colinear = true;
        }
        return result;
    }

    // Step 1: find initial tetrahedron.
    crd::u32 p0 = 0;
    crd::u32 p1 = 0;
    crd::u32 p2 = 0;
    crd::u32 p3 = 0;
    bool is_coincident = false;
    bool is_colinear = false;
    bool is_coplanar = false;
    const bool found = find_initial_tetrahedron<T>(points, opts.degenerate_tetrahedron_eps, p0, p1,
                                                     p2, p3, is_coincident, is_colinear, is_coplanar);

    if (is_coincident)
    {
        result.vertices.push_back(points[0]);
        result.is_coincident = true;
        return result;
    }
    if (is_colinear)
    {
        result.vertices.push_back(points[p0]);
        result.vertices.push_back(points[p1]);
        result.is_colinear = true;
        return result;
    }
    if (is_coplanar)
    {
        // All points lie on a plane. Build the flat 3D hull as 2 face copies
        // (front + back) of the 2D hull on the dominant plane. v2 GJK/EPA
        // operates correctly on this degenerate-volume polytope via the
        // support function (which only reads vertices).
        result.is_coplanar = true;

        // 1. Compute the plane normal from the 3 extremals.
        const Vec3T<T> e1 = points[p1] - points[p0];
        const Vec3T<T> e2 = points[p2] - points[p0];
        Vec3T<T> plane_normal = crd::math::cross(e1, e2);
        const T pn_len = std::sqrt(crd::math::dot(plane_normal, plane_normal));
        if (pn_len <= static_cast<T>(0))
        {
            // Fallback to flag-only result (shouldn't happen after
            // find_initial_tetrahedron passed the colinearity check).
            result.vertices.push_back(points[p0]);
            result.vertices.push_back(points[p1]);
            result.vertices.push_back(points[p2]);
            return result;
        }
        plane_normal = plane_normal * (static_cast<T>(1) / pn_len);

        // 2. Determine dominant axis (the one we project away).
        const T abs_x = std::fabs(plane_normal.x);
        const T abs_y = std::fabs(plane_normal.y);
        const T abs_z = std::fabs(plane_normal.z);
        int dominant = 0;
        if (abs_z >= abs_x && abs_z >= abs_y)
        {
            dominant = 2;
        }
        else if (abs_y >= abs_x)
        {
            dominant = 1;
        }

        // 3. Project all input points to 2D using the non-dominant axes.
        crd::containers::Array<crd::math::Vec2<T>> projected(alloc);
        projected.reserve(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            crd::math::Vec2<T> p2d;
            if (dominant == 0)
            {
                p2d.x = points[i].y;
                p2d.y = points[i].z;
            }
            else if (dominant == 1)
            {
                p2d.x = points[i].z;
                p2d.y = points[i].x;
            }
            else
            {
                p2d.x = points[i].x;
                p2d.y = points[i].y;
            }
            projected.push_back(p2d);
        }

        // 4. Run 2D hull on the projected points.
        crd::containers::Array<crd::u32> hull2d_indices(alloc);
        convex_hull_2d_indices<T>(crd::containers::ConstSpan<crd::math::Vec2<T>>(projected.data(),
                                                                                    projected.size()),
                                    hull2d_indices);

        const crd::usize num_hull_verts = hull2d_indices.size();
        if (num_hull_verts < 3)
        {
            // Degenerate (collinear projection). Just record the extreme
            // vertices without faces.
            for (crd::usize i = 0; i < num_hull_verts; ++i)
            {
                result.vertices.push_back(points[hull2d_indices[i]]);
            }
            return result;
        }

        // 5. Build flat 3D hull vertices (the 2D hull's vertex positions in
        // their original 3D coordinates).
        result.vertices.reserve(num_hull_verts);
        for (crd::usize i = 0; i < num_hull_verts; ++i)
        {
            result.vertices.push_back(points[hull2d_indices[i]]);
        }

        // 6. Determine whether the 2D-CCW order corresponds to a 3D-CCW
        // viewed from +plane_normal. Test using the first 3 result vertices.
        const Vec3T<T> f1 = result.vertices[1] - result.vertices[0];
        const Vec3T<T> f2 = result.vertices[2] - result.vertices[0];
        const Vec3T<T> front_cross = crd::math::cross(f1, f2);
        const bool reverse_for_front = crd::math::dot(front_cross, plane_normal) < static_cast<T>(0);

        // 7. Front face: outward normal = +plane_normal. CCW from outside.
        crd::geometry::primitives::Plane<T> front_plane;
        front_plane.normal = plane_normal;
        front_plane.d = -crd::math::dot(plane_normal, result.vertices[0]);
        result.faces.push_back(front_plane);

        crd::geometry::primitives::Plane<T> back_plane;
        back_plane.normal = Vec3T<T>(-plane_normal.x, -plane_normal.y, -plane_normal.z);
        back_plane.d = crd::math::dot(plane_normal, result.vertices[0]);
        result.faces.push_back(back_plane);

        // 8. face_vertex_indices: front in CCW-from-+plane_normal order,
        // back in reverse (CCW-from-(-plane_normal) order).
        result.face_vertex_offsets.push_back(0);
        for (crd::usize i = 0; i < num_hull_verts; ++i)
        {
            const crd::u32 idx = reverse_for_front
                                       ? static_cast<crd::u32>(num_hull_verts - 1 - i)
                                       : static_cast<crd::u32>(i);
            result.face_vertex_indices.push_back(idx);
        }
        result.face_vertex_offsets.push_back(
            static_cast<crd::u32>(result.face_vertex_indices.size()));

        for (crd::usize i = 0; i < num_hull_verts; ++i)
        {
            const crd::u32 idx = reverse_for_front
                                       ? static_cast<crd::u32>(i)
                                       : static_cast<crd::u32>(num_hull_verts - 1 - i);
            result.face_vertex_indices.push_back(idx);
        }
        result.face_vertex_offsets.push_back(
            static_cast<crd::u32>(result.face_vertex_indices.size()));

        return result;
    }
    if (!found)
    {
        // Should be unreachable — find_initial_tetrahedron sets a degeneracy
        // flag on every path that returns false.
        return result;
    }

    // Step 2: build initial tetrahedron faces with adjacency.
    crd::containers::Array<QhFace<T>> mesh(alloc);
    build_initial_tetrahedron<T>(mesh, points, p0, p1, p2, p3, alloc);

    // Step 3: initialize outside-sets — each input point that is "outside"
    // some face goes into the FIRST face whose outward halfspace contains it
    // (lowest-face-index tiebreak). Points strictly inside all faces are not
    // tracked (they cannot be hull vertices).
    for (crd::u32 i = 0; i < n; ++i)
    {
        if (i == p0 || i == p1 || i == p2 || i == p3)
            continue;
        for (crd::u32 fi = 0; fi < mesh.size(); ++fi)
        {
            // Use the cached plane distance for the fast filter; for the
            // SIGN decision, rely on the sign of the f64 distance (Stage A
            // is enough — the iteration loop catches near-degenerate cases
            // via the eye-selection's exact orient3d).
            const T d = plane_distance(mesh[fi], points[i]);
            if (d > static_cast<T>(0))
            {
                mesh[fi].outside_set.push_back(i);
                break; // lowest-face-index tiebreak
            }
        }
    }

    // Compute interior witness — centroid of the initial tetrahedron. This
    // is guaranteed to lie inside the final hull (since the hull contains
    // the initial tet by construction). Used as the orientation reference
    // for new face vertex ordering during the iteration loop.
    const Vec3T<T> interior_witness =
        (points[p0] + points[p1] + points[p2] + points[p3]) * (static_cast<T>(1) / static_cast<T>(4));

    // Step 4: main iteration loop (Quickhull's algorithmic core).
    //
    //   while (any non-removed face has non-empty outside_set):
    //     1. pick the lowest-face-index face with non-empty outside_set
    //     2. pick the eye = argmax distance to the face's plane (lowest-input-
    //        index tiebreak per ADR-0076 §4 pin #11)
    //     3. DFS from the face through neighbors (visited in increasing index
    //        order) marking visible faces (`orient3d(face, eye) < 0` —
    //        Shewchuk convention: eye is "above" / outside the face)
    //     4. identify horizon edges (visible face's edge whose neighbor is
    //        NOT visible)
    //     5. build new triangle faces (eye, horizon_edge_a, horizon_edge_b)
    //        with vertex order verified against the interior witness for
    //        CCW-from-outside orientation
    //     6. wire neighbor relationships (between new faces and across
    //        horizon edges to the non-visible faces on the other side)
    //     7. tombstone visible faces
    //     8. redistribute orphaned outside-points to new faces (lowest-face-
    //        index tiebreak on multiple-new-face containment)
    //
    crd::u32 iteration = 0;
    crd::containers::Array<bool> face_visible(alloc);
    crd::containers::Array<crd::u32> visible_faces(alloc);
    crd::containers::Array<crd::u32> dfs_stack(alloc);
    struct HorizonEdge
    {
        crd::u32 face_idx;
        int edge_idx;
        crd::u32 outer_neighbor;
        crd::u32 va;
        crd::u32 vb;
    };
    crd::containers::Array<HorizonEdge> horizon(alloc);
    crd::containers::Array<crd::u32> new_face_indices(alloc);
    crd::containers::Array<crd::u32> orphans(alloc);

    while (iteration < opts.max_iterations)
    {
        ++iteration;

        // Step 1: pick lowest-face-index face with non-empty outside_set.
        crd::u32 work_face_idx = ~crd::u32{0};
        for (crd::u32 fi = 0; fi < mesh.size(); ++fi)
        {
            if (!mesh[fi].removed && !mesh[fi].outside_set.empty())
            {
                work_face_idx = fi;
                break;
            }
        }
        if (work_face_idx == ~crd::u32{0})
        {
            break; // hull complete — no face has outside points
        }

        // Step 2: pick eye point (furthest from work face's plane; lowest-
        // input-index tiebreak).
        crd::u32 eye_idx = ~crd::u32{0};
        T best_dist = static_cast<T>(0);
        for (crd::usize i = 0; i < mesh[work_face_idx].outside_set.size(); ++i)
        {
            const crd::u32 p_idx = mesh[work_face_idx].outside_set[i];
            const T d = plane_distance(mesh[work_face_idx], points[p_idx]);
            if (d > best_dist)
            {
                best_dist = d;
                eye_idx = p_idx;
            }
            else if (d == best_dist && eye_idx != ~crd::u32{0} && p_idx < eye_idx)
            {
                eye_idx = p_idx;
            }
        }
        if (eye_idx == ~crd::u32{0})
        {
            break; // defensive — shouldn't happen since outside_set was non-empty
        }

        // Step 3: DFS to find all visible faces. Reset the visibility buffer.
        face_visible.clear();
        face_visible.resize(mesh.size());
        for (crd::usize i = 0; i < face_visible.size(); ++i)
        {
            face_visible[i] = false;
        }
        visible_faces.clear();
        dfs_stack.clear();
        dfs_stack.push_back(work_face_idx);
        face_visible[work_face_idx] = true;
        visible_faces.push_back(work_face_idx);
        while (!dfs_stack.empty())
        {
            const crd::u32 fi = dfs_stack[dfs_stack.size() - 1];
            dfs_stack.pop_back();
            // Walk neighbors in edge-index order (0, 1, 2) for determinism.
            for (int e = 0; e < 3; ++e)
            {
                const crd::u32 ni = mesh[fi].neighbors[e];
                if (ni == ~crd::u32{0} || mesh[ni].removed || face_visible[ni])
                {
                    continue;
                }
                // Exact sign via v3a full Stage D orient3d.
                const T sign = crd::geometry::primitives::orient3d(
                    points[mesh[ni].v[0]], points[mesh[ni].v[1]], points[mesh[ni].v[2]],
                    points[eye_idx]);
                // Shewchuk convention: orient3d < 0 ⇒ eye is "above" the
                // outward-normal halfspace ⇒ visible.
                if (sign < static_cast<T>(0))
                {
                    face_visible[ni] = true;
                    visible_faces.push_back(ni);
                    dfs_stack.push_back(ni);
                }
            }
        }

        // Step 4: identify horizon edges. For each visible face, walk edges
        // in order 0/1/2. If neighbor is NOT visible (or removed), this is
        // a horizon edge.
        horizon.clear();
        for (crd::usize i = 0; i < visible_faces.size(); ++i)
        {
            const crd::u32 fi = visible_faces[i];
            for (int e = 0; e < 3; ++e)
            {
                const crd::u32 ni = mesh[fi].neighbors[e];
                const bool is_visible_neighbor =
                    (ni != ~crd::u32{0}) && (ni < face_visible.size()) && face_visible[ni];
                if (!is_visible_neighbor)
                {
                    HorizonEdge he;
                    he.face_idx = fi;
                    he.edge_idx = e;
                    he.outer_neighbor = ni;
                    he.va = mesh[fi].v[e];
                    he.vb = mesh[fi].v[(e + 1) % 3];
                    horizon.push_back(he);
                }
            }
        }

        // Step 5: build new triangle faces from each horizon edge.
        new_face_indices.clear();
        for (crd::usize i = 0; i < horizon.size(); ++i)
        {
            const HorizonEdge& he = horizon[i];
            // Standard new face vertex order: (eye, va, vb) — to be verified
            // against the interior witness for CCW-from-outside orientation.
            QhFace<T> nf(alloc);
            nf.v[0] = eye_idx;
            nf.v[1] = he.va;
            nf.v[2] = he.vb;
            compute_face_plane(nf, points[eye_idx], points[he.va], points[he.vb]);
            // Check orientation: interior witness should be on the "below"
            // (orient3d > 0 Shewchuk) side.
            const T orient_test = crd::geometry::primitives::orient3d(
                points[nf.v[0]], points[nf.v[1]], points[nf.v[2]], interior_witness);
            if (orient_test < static_cast<T>(0))
            {
                // Flip — swap v[1] and v[2] to reverse the triangle.
                const crd::u32 tmp = nf.v[1];
                nf.v[1] = nf.v[2];
                nf.v[2] = tmp;
                compute_face_plane(nf, points[nf.v[0]], points[nf.v[1]], points[nf.v[2]]);
            }
            const crd::u32 new_idx = static_cast<crd::u32>(mesh.size());
            mesh.push_back(std::move(nf));
            new_face_indices.push_back(new_idx);
        }

        // Step 6a: wire each new face's neighbor across the horizon edge
        // (the original outer_neighbor face). Find which edge on the outer
        // neighbor matches the horizon edge, and update it.
        for (crd::usize i = 0; i < horizon.size(); ++i)
        {
            const HorizonEdge& he = horizon[i];
            const crd::u32 new_idx = new_face_indices[i];
            // Find which edge in new face corresponds to (va, vb) edge —
            // it's the edge between vertices va and vb in the new face's
            // (eye, va, vb) or (eye, vb, va) ordering.
            for (int e = 0; e < 3; ++e)
            {
                const crd::u32 a = mesh[new_idx].v[e];
                const crd::u32 b = mesh[new_idx].v[(e + 1) % 3];
                if ((a == he.va && b == he.vb) || (a == he.vb && b == he.va))
                {
                    mesh[new_idx].neighbors[e] = he.outer_neighbor;
                    break;
                }
            }
            // Update outer_neighbor's back-pointer.
            if (he.outer_neighbor != ~crd::u32{0})
            {
                for (int g_e = 0; g_e < 3; ++g_e)
                {
                    const crd::u32 ga = mesh[he.outer_neighbor].v[g_e];
                    const crd::u32 gb = mesh[he.outer_neighbor].v[(g_e + 1) % 3];
                    if ((ga == he.va && gb == he.vb) || (ga == he.vb && gb == he.va))
                    {
                        mesh[he.outer_neighbor].neighbors[g_e] = new_idx;
                        break;
                    }
                }
            }
        }

        // Step 6b: wire neighbor relationships BETWEEN new faces. Each new
        // face shares 2 edges with 2 other new faces (the edges incident on
        // the eye vertex). The third edge is the horizon (already wired in
        // step 6a).
        for (crd::usize i = 0; i < new_face_indices.size(); ++i)
        {
            const crd::u32 fi = new_face_indices[i];
            for (int e = 0; e < 3; ++e)
            {
                if (mesh[fi].neighbors[e] != ~crd::u32{0})
                {
                    continue; // already wired (horizon edge)
                }
                const crd::u32 va = mesh[fi].v[e];
                const crd::u32 vb = mesh[fi].v[(e + 1) % 3];
                // Find another new face that contains this edge.
                for (crd::usize j = 0; j < new_face_indices.size(); ++j)
                {
                    if (i == j)
                    {
                        continue;
                    }
                    const crd::u32 gi = new_face_indices[j];
                    for (int g_e = 0; g_e < 3; ++g_e)
                    {
                        const crd::u32 ga = mesh[gi].v[g_e];
                        const crd::u32 gb = mesh[gi].v[(g_e + 1) % 3];
                        if ((ga == va && gb == vb) || (ga == vb && gb == va))
                        {
                            mesh[fi].neighbors[e] = gi;
                            break;
                        }
                    }
                    if (mesh[fi].neighbors[e] != ~crd::u32{0})
                    {
                        break;
                    }
                }
            }
        }

        // Step 7: tombstone visible faces + collect orphaned outside points.
        orphans.clear();
        for (crd::usize i = 0; i < visible_faces.size(); ++i)
        {
            const crd::u32 vi = visible_faces[i];
            for (crd::usize j = 0; j < mesh[vi].outside_set.size(); ++j)
            {
                const crd::u32 p_idx = mesh[vi].outside_set[j];
                if (p_idx != eye_idx)
                {
                    orphans.push_back(p_idx);
                }
            }
            mesh[vi].outside_set.clear();
            mesh[vi].removed = true;
        }

        // Step 8: redistribute orphans to new faces. Assign each orphan to
        // the lowest-face-index new face whose plane is above it (Shewchuk
        // convention: plane_distance > 0 ⇒ above ⇒ outside).
        for (crd::usize i = 0; i < orphans.size(); ++i)
        {
            const crd::u32 p_idx = orphans[i];
            for (crd::usize j = 0; j < new_face_indices.size(); ++j)
            {
                const crd::u32 new_idx = new_face_indices[j];
                const T d = plane_distance(mesh[new_idx], points[p_idx]);
                if (d > static_cast<T>(0))
                {
                    mesh[new_idx].outside_set.push_back(p_idx);
                    break; // lowest-face-index tiebreak
                }
            }
        }
    }
    CRD_ASSERT(iteration < opts.max_iterations); // diagnostics: hit the cap is a bug

    // Step 5: result extraction. Walk surviving (non-removed) faces, collect
    // unique vertices in input-index order, write face_vertex_indices using
    // the result-vertex indices.
    crd::containers::Array<crd::u32> input_to_result_idx(alloc);
    input_to_result_idx.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        input_to_result_idx[i] = ~crd::u32{0};
    }

    for (crd::usize fi = 0; fi < mesh.size(); ++fi)
    {
        if (mesh[fi].removed)
            continue;
        for (int e = 0; e < 3; ++e)
        {
            const crd::u32 vi = mesh[fi].v[e];
            if (input_to_result_idx[vi] == ~crd::u32{0})
            {
                input_to_result_idx[vi] = static_cast<crd::u32>(result.vertices.size());
                result.vertices.push_back(points[vi]);
            }
        }
    }

    // Write faces + face_vertex_indices using result-vertex indices.
    result.face_vertex_offsets.push_back(0);
    for (crd::usize fi = 0; fi < mesh.size(); ++fi)
    {
        if (mesh[fi].removed)
            continue;
        for (int e = 0; e < 3; ++e)
        {
            result.face_vertex_indices.push_back(input_to_result_idx[mesh[fi].v[e]]);
        }
        result.face_vertex_offsets.push_back(static_cast<crd::u32>(result.face_vertex_indices.size()));
        crd::geometry::primitives::Plane<T> p;
        p.normal = mesh[fi].normal;
        p.d = mesh[fi].plane_d;
        result.faces.push_back(p);
    }

    return result;
}

// ===========================================================================
// enrich_for_gjk — v3c-c seam (TODO when v3c-b iteration ships).
// ===========================================================================

template <crd::math::MathScalar T> void enrich_for_gjk(QuickhullResult<T>& r) noexcept
{
    if (r.vertices.empty())
    {
        return;
    }

    // 1. Build per-vertex adjacency from face_vertex_indices (v2g hill-climb
    //    hull-support consumer).
    crd::geometry::primitives::compute_vertex_adjacency_from_faces(
        crd::containers::ConstSpan<crd::u32>(r.face_vertex_indices.data(),
                                              r.face_vertex_indices.size()),
        crd::containers::ConstSpan<crd::u32>(r.face_vertex_offsets.data(),
                                              r.face_vertex_offsets.size()),
        r.vertices.size(), r.vertex_adjacency_indices, r.vertex_adjacency_offsets);

    // 2. Build SoA SIMD arrays for the v2h `support_simd_f32` consumer.
    //    f32-only path: f64 hulls keep these arrays empty (the SIMD path
    //    is f32-only by construction per ADR-0076 §16 / v2h pin).
    if constexpr (std::is_same_v<T, crd::f32>)
    {
        const crd::usize n = r.vertices.size();
        // Pad to multiple of 8 (per v2h convention: AVX2 lane width). Padded
        // lanes repeat vertex 0's coordinates — they tie with lane 0 on
        // projection score and lose by lowest-index tiebreak in the SIMD
        // reducer, so they're harmless to support correctness.
        const crd::usize padded = ((n + 7U) / 8U) * 8U;
        r.vx_soa.reserve(padded);
        r.vy_soa.reserve(padded);
        r.vz_soa.reserve(padded);
        for (crd::usize i = 0; i < n; ++i)
        {
            r.vx_soa.push_back(r.vertices[i].x);
            r.vy_soa.push_back(r.vertices[i].y);
            r.vz_soa.push_back(r.vertices[i].z);
        }
        for (crd::usize i = n; i < padded; ++i)
        {
            r.vx_soa.push_back(r.vertices[0].x);
            r.vy_soa.push_back(r.vertices[0].y);
            r.vz_soa.push_back(r.vertices[0].z);
        }
    }
}

// Explicit instantiations for f32 and f64.
template QuickhullResult<crd::f32> quickhull<crd::f32>(
    crd::containers::ConstSpan<crd::math::Vec3<crd::f32>> points, crd::memory::IAllocator* alloc,
    const QuickhullOptions<crd::f32>& opts) noexcept;
template QuickhullResult<crd::f64> quickhull<crd::f64>(
    crd::containers::ConstSpan<crd::math::Vec3<crd::f64>> points, crd::memory::IAllocator* alloc,
    const QuickhullOptions<crd::f64>& opts) noexcept;
template void enrich_for_gjk<crd::f32>(QuickhullResult<crd::f32>& r) noexcept;
template void enrich_for_gjk<crd::f64>(QuickhullResult<crd::f64>& r) noexcept;

} // namespace crd::geometry::convex
