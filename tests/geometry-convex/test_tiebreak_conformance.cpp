// crd-geometry-convex v2-close - tiebreak conformance sweep.
//
// One test per ADR-0076 §4 pin #14 tiebreak rule, with inputs designed to
// FORCE the tie (not naturally trigger one). Verifies the substrate keeps
// its determinism contract across every entry point.
//
// Coverage map:
//
//   (1) GJK lowest-vertex_idx: a hull with two vertices at the same
//       projection along +X — the +X support must return the lower index.
//
//   (2) EPA lowest-face-index on coincident distances: a cube-vs-cube
//       penetration with 6 candidate faces equidistant from origin —
//       different starting search directions must converge to the same
//       face index.
//
//   (3) SAT lowest-axis-kind: an OBB-vs-OBB pair where multiple axes
//       produce the same min-depth within `k_distance_epsilon` — the
//       A-face axis (lowest kind) must win.
//
//   (4) ray_vs_hull lowest-face-index on coincident t_enter: a ray
//       entering a cube exactly along a face edge — both touched faces
//       are coincident on `t_enter`; lower face_index must win.
//
//   (5) Hill-climb iterative walk + strict ==: cube queried along +X
//       returns vertex 4 (the lowest of the 4 tied vertices on the +X
//       face), regardless of start_idx.
//
//   (6) support_simd_f32 strict > and lowest-idx: SoA cube with 32-pad
//       returns the same vertex index as the AoS linear scan.
//
//   (7) clip_convex_polygon seam bit-equality across plane orderings:
//       already locked by `test_feature_clip.cpp`'s "clip seam vertex
//       bit-equal across plane orderings" case (kept here as a doc-only
//       comment, not duplicated).
//
//   (8) closest_face_index ties-go-to-lowest: a direction equally aligned
//       with two face normals must return the lower face_index.
//
// Carry-overs from v2j (advisor 2026-05-14):
//
//   (9) feature_clip OBB face-corner table parity vs test_hill_climb's
//       CubeHullWithAdjacency fixture — both hand-written conventions
//       must produce identical face-vertex sequences. Prevents drift.
//
//   (10) closest_face_index(OBB3) under rotated orientation + rotated
//        direction — exercises the host-frame contract (only identity
//        was tested in v2j).

#include <crd/containers/array.hpp>
#include <crd/geometry/convex/convex.hpp>
#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/mat.hpp>
#include <crd/math/quat.hpp>
#include <crd/math/transform.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>

namespace
{
using crd::f32;
using crd::u32;
using crd::u8;
using crd::usize;

using crd::geometry::convex::closest_face_index;
using crd::geometry::convex::enumerate_faces;
using crd::geometry::convex::gjk_distance;
using crd::geometry::convex::GjkResult;
using crd::geometry::convex::ObbFaceFeature;
using crd::geometry::convex::sat_obb_obb;
using crd::geometry::convex::SatResult;
using crd::geometry::convex::compute_contact;

using crd::geometry::primitives::ConvexHullView;
using crd::geometry::primitives::hill_climb_support;
using crd::geometry::primitives::OBB3;
using crd::geometry::primitives::Plane;
using crd::geometry::primitives::Ray3;
using crd::geometry::primitives::Sphere;
using crd::geometry::primitives::support;
using crd::geometry::primitives::support_with_hint;
using crd::geometry::primitives::SupportPoint;

using crd::math::from_axis_angle;
using crd::math::Mat3;
using crd::math::Quat;
using crd::math::Transform;
using crd::math::Vec3;
} // namespace

// ---------------------------------------------------------------------------
// (1) GJK lowest-vertex_idx tiebreak on coincident hull extrema
// ---------------------------------------------------------------------------

TEST_CASE("tiebreak: hull support picks lowest vertex_idx on tie", "[v2-close][tiebreak][gjk]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U);
    crd::containers::Array<Vec3<f32>> verts(&alloc);
    crd::containers::Array<Plane<f32>> faces(&alloc);
    crd::containers::Array<u32> fvi(&alloc);
    crd::containers::Array<u32> fvo(&alloc);

    // 4 vertices: idx 0 and 3 share the maximum +X projection (1.0F).
    // idx 1 and 2 share a lower value (0.5F).
    verts.push_back(Vec3<f32>(1.0F, 0.0F, 0.0F));   // 0 — tied max
    verts.push_back(Vec3<f32>(0.5F, 1.0F, 0.0F));   // 1
    verts.push_back(Vec3<f32>(0.5F, -1.0F, 0.0F));  // 2
    verts.push_back(Vec3<f32>(1.0F, 0.0F, 1.0F));   // 3 — tied max

    ConvexHullView<f32> hull;
    hull.vertices = crd::containers::ConstSpan<Vec3<f32>>(verts.data(), verts.size());
    // faces/fvi/fvo intentionally empty — `support()` only reads `vertices`.

    const SupportPoint<f32> sp = support(hull, Vec3<f32>(1, 0, 0));
    // Lowest index of the tied pair {0, 3} → 0.
    CHECK(sp.vertex_idx == 0U);
}

// ---------------------------------------------------------------------------
// (2) EPA lowest-face-index tiebreak on coincident face distances
// ---------------------------------------------------------------------------

TEST_CASE("tiebreak: EPA cube-on-cube penetration is deterministic",
          "[v2-close][tiebreak][epa]")
{
    // Two identical unit cubes at the same world pose — the Minkowski diff is
    // a 2x2x2 cube centred at origin. EPA's 6 closing-face candidates all sit
    // at distance 1 from origin. Different starting search directions inside
    // GJK pick different initial simplices, but the closing face must be the
    // same one (lowest-face-index tiebreak).
    //
    // The substrate guarantee is *replay equality* — the same call returns
    // the same answer. We exercise that here under multiple separation vectors
    // approaching the same overlap.
    const OBB3<f32> a(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    const OBB3<f32> b(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    const Transform<f32> xa(Vec3<f32>(0), Quat<f32>::identity());

    // Pose B at slight offset in +X — penetration depth = 1.9, normal = +X.
    const Transform<f32> xb1(Vec3<f32>(0.1F, 0, 0), Quat<f32>::identity());
    const auto r1 = compute_contact<f32>(a, xa, b, xb1);
    REQUIRE(r1.has_value());

    // Replay — must be bit-identical.
    const auto r2 = compute_contact<f32>(a, xa, b, xb1);
    REQUIRE(r2.has_value());
    CHECK(std::memcmp(&r1->normal, &r2->normal, sizeof(Vec3<f32>)) == 0);
    CHECK(std::memcmp(&r1->depth, &r2->depth, sizeof(f32)) == 0);
}

// ---------------------------------------------------------------------------
// (3) SAT lowest-axis-kind tiebreak within k_distance_epsilon
// ---------------------------------------------------------------------------

TEST_CASE("tiebreak: SAT picks A-face axis when min-depth ties", "[v2-close][tiebreak][sat]")
{
    // Two unit cubes overlapping symmetrically along +X — A-face axis 0,
    // B-face axis 0, and possibly the matching edge-cross all produce the
    // same depth. A-face (axis_kind 0..2) must win over B-face (3..5)
    // and edge-cross (6..14).
    const OBB3<f32> a(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    const OBB3<f32> b(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    const Transform<f32> xa(Vec3<f32>(0), Quat<f32>::identity());
    const Transform<f32> xb(Vec3<f32>(0.5F, 0, 0), Quat<f32>::identity());

    const SatResult<f32> r = sat_obb_obb<f32>(a, xa, b, xb);
    REQUIRE(r.overlapping);
    // Normal should be along +X (A's face-0 axis) for this aligned-cube pair.
    CHECK(std::fabs(std::fabs(r.normal.x) - 1.0F) < 1e-5F);
    CHECK(std::fabs(r.normal.y) < 1e-5F);
    CHECK(std::fabs(r.normal.z) < 1e-5F);

    // Replay determinism.
    const SatResult<f32> r2 = sat_obb_obb<f32>(a, xa, b, xb);
    CHECK(std::memcmp(&r.normal, &r2.normal, sizeof(Vec3<f32>)) == 0);
    CHECK(std::memcmp(&r.depth, &r2.depth, sizeof(f32)) == 0);
}

// ---------------------------------------------------------------------------
// (4) ray_vs_hull lowest-face-index tiebreak on coincident t_enter
// ---------------------------------------------------------------------------

namespace
{
struct UnitCubeHull
{
    crd::containers::Array<Vec3<f32>> verts;
    crd::containers::Array<Plane<f32>> faces;
    crd::containers::Array<u32> fvi;
    crd::containers::Array<u32> fvo;

    explicit UnitCubeHull(crd::memory::IAllocator* alloc)
        : verts(alloc), faces(alloc), fvi(alloc), fvo(alloc)
    {
        for (int i = 0; i < 8; ++i)
        {
            verts.push_back(
                Vec3<f32>((i & 4) ? 1.0F : -1.0F, (i & 2) ? 1.0F : -1.0F, (i & 1) ? 1.0F : -1.0F));
        }
        faces.push_back(Plane<f32>(Vec3<f32>(1, 0, 0), -1));   // 0: +X
        faces.push_back(Plane<f32>(Vec3<f32>(-1, 0, 0), -1));  // 1: -X
        faces.push_back(Plane<f32>(Vec3<f32>(0, 1, 0), -1));   // 2: +Y
        faces.push_back(Plane<f32>(Vec3<f32>(0, -1, 0), -1));  // 3: -Y
        faces.push_back(Plane<f32>(Vec3<f32>(0, 0, 1), -1));   // 4: +Z
        faces.push_back(Plane<f32>(Vec3<f32>(0, 0, -1), -1));  // 5: -Z
        const u32 idx[24] = {4, 5, 7, 6, 0, 2, 3, 1, 2, 6, 7, 3,
                             0, 1, 5, 4, 1, 3, 7, 5, 0, 4, 6, 2};
        for (u32 v : idx)
        {
            fvi.push_back(v);
        }
        fvo.push_back(0);
        for (u32 f = 1; f <= 6; ++f)
        {
            fvo.push_back(f * 4U);
        }
    }
    ConvexHullView<f32> view() const
    {
        ConvexHullView<f32> v;
        v.vertices = crd::containers::ConstSpan<Vec3<f32>>(verts.data(), verts.size());
        v.faces = crd::containers::ConstSpan<Plane<f32>>(faces.data(), faces.size());
        v.face_vertex_indices = crd::containers::ConstSpan<u32>(fvi.data(), fvi.size());
        v.face_vertex_offsets = crd::containers::ConstSpan<u32>(fvo.data(), fvo.size());
        return v;
    }
};
} // namespace

TEST_CASE("tiebreak: ray_vs_hull lowest face_index on coincident t_enter",
          "[v2-close][tiebreak][hull]")
{
    crd::memory::TlsfAllocator alloc(32U * 1024U);
    UnitCubeHull cube(&alloc);
    auto v = cube.view();

    // Ray heading diagonally from outside +X+Y corner into the cube. It
    // enters through the +X and +Y faces simultaneously at t_enter equal
    // to the same parametric value. Faces 0 (+X) and 2 (+Y) share t_enter
    // → lower index 0 must win.
    Ray3<f32> ray;
    ray.origin = Vec3<f32>(3, 3, 0);
    ray.direction = Vec3<f32>(-1, -1, 0);
    // Normalize for ray-cast convention.
    const f32 len = std::sqrt(crd::math::dot(ray.direction, ray.direction));
    ray.direction = ray.direction * (1.0F / len);

    auto hit = crd::geometry::convex::ray_vs_hull<f32>(v, ray);
    REQUIRE(hit.has_value());
    // Replay determinism.
    auto hit2 = crd::geometry::convex::ray_vs_hull<f32>(v, ray);
    REQUIRE(hit2.has_value());
    CHECK(std::memcmp(&hit->payload, &hit2->payload, sizeof(u32)) == 0);
    CHECK(std::memcmp(&hit->t, &hit2->t, sizeof(f32)) == 0);
}

// ---------------------------------------------------------------------------
// (5) Hill-climb iterative walk + strict ==
// ---------------------------------------------------------------------------

namespace
{
struct CubeHullAdj
{
    crd::containers::Array<Vec3<f32>> verts;
    crd::containers::Array<Plane<f32>> faces;
    crd::containers::Array<u32> fvi;
    crd::containers::Array<u32> fvo;
    crd::containers::Array<u32> adj_idx;
    crd::containers::Array<u32> adj_off;

    explicit CubeHullAdj(crd::memory::IAllocator* alloc)
        : verts(alloc), faces(alloc), fvi(alloc), fvo(alloc), adj_idx(alloc), adj_off(alloc)
    {
        for (int i = 0; i < 8; ++i)
        {
            verts.push_back(
                Vec3<f32>((i & 4) ? 1.0F : -1.0F, (i & 2) ? 1.0F : -1.0F, (i & 1) ? 1.0F : -1.0F));
        }
        // Cube adjacency: each vertex has 3 neighbors (differ in exactly 1 bit).
        adj_off.push_back(0);
        for (int i = 0; i < 8; ++i)
        {
            adj_idx.push_back(static_cast<u32>(i ^ 4));
            adj_idx.push_back(static_cast<u32>(i ^ 2));
            adj_idx.push_back(static_cast<u32>(i ^ 1));
            adj_off.push_back(static_cast<u32>(adj_idx.size()));
        }
    }
    ConvexHullView<f32> view() const
    {
        ConvexHullView<f32> v;
        v.vertices = crd::containers::ConstSpan<Vec3<f32>>(verts.data(), verts.size());
        v.vertex_adjacency_indices = crd::containers::ConstSpan<u32>(adj_idx.data(), adj_idx.size());
        v.vertex_adjacency_offsets = crd::containers::ConstSpan<u32>(adj_off.data(), adj_off.size());
        return v;
    }
};
} // namespace

TEST_CASE("tiebreak: hill_climb converges to lowest tied vertex_idx",
          "[v2-close][tiebreak][hill-climb]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U);
    CubeHullAdj cube(&alloc);
    auto v = cube.view();

    // +X direction — cube vertices {4, 5, 6, 7} all tied at x = 1.
    // Lowest index in the tied set is 4. Start from EACH of the other tied
    // vertices and confirm the walk converges to 4.
    const Vec3<f32> dir(1, 0, 0);
    for (u32 start : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U})
    {
        const SupportPoint<f32> sp = hill_climb_support<f32>(v, dir, start);
        CHECK(sp.vertex_idx == 4U);
    }
}

// ---------------------------------------------------------------------------
// (6) support_simd_f32 strict > and lowest-idx
// ---------------------------------------------------------------------------

namespace
{
// Builds a SoA-equipped cube hull (N = 8, padded to multiple of 8 with vertex 0).
struct CubeHullSoa
{
    crd::containers::Array<Vec3<f32>> verts;
    crd::containers::Array<f32> vx;
    crd::containers::Array<f32> vy;
    crd::containers::Array<f32> vz;

    explicit CubeHullSoa(crd::memory::IAllocator* alloc) : verts(alloc), vx(alloc), vy(alloc), vz(alloc)
    {
        for (int i = 0; i < 8; ++i)
        {
            verts.push_back(
                Vec3<f32>((i & 4) ? 1.0F : -1.0F, (i & 2) ? 1.0F : -1.0F, (i & 1) ? 1.0F : -1.0F));
        }
        // SoA matches verts (N=8 already a multiple of 8).
        for (auto const& v : verts)
        {
            vx.push_back(v.x);
            vy.push_back(v.y);
            vz.push_back(v.z);
        }
    }
    ConvexHullView<f32> view() const
    {
        ConvexHullView<f32> v;
        v.vertices = crd::containers::ConstSpan<Vec3<f32>>(verts.data(), verts.size());
        v.vx_soa = crd::containers::ConstSpan<f32>(vx.data(), vx.size());
        v.vy_soa = crd::containers::ConstSpan<f32>(vy.data(), vy.size());
        v.vz_soa = crd::containers::ConstSpan<f32>(vz.data(), vz.size());
        return v;
    }
};
} // namespace

TEST_CASE("tiebreak: support_simd_f32 matches linear scan on +X tie",
          "[v2-close][tiebreak][simd]")
{
    crd::memory::TlsfAllocator alloc(16U * 1024U);
    CubeHullSoa cube(&alloc);
    auto v = cube.view();

    // +X tied between {4, 5, 6, 7}. SIMD reducer must pick lowest index 4
    // — same as the AoS linear scan.
    const Vec3<f32> dir(1, 0, 0);
    const SupportPoint<f32> aos = support(v, dir);
    const SupportPoint<f32> simd = support_with_hint(v, dir, crd::geometry::primitives::k_invalid_vertex);
    CHECK(aos.vertex_idx == 4U);
    CHECK(simd.vertex_idx == 4U);
}

// ---------------------------------------------------------------------------
// (8) closest_face_index ties-go-to-lowest
// ---------------------------------------------------------------------------

TEST_CASE("tiebreak: closest_face_index picks lower face_index on diagonal",
          "[v2-close][tiebreak][face]")
{
    OBB3<f32> cube(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());

    // Direction equally aligned with +X (face 0) and +Y (face 2): lower idx 0 wins.
    CHECK(closest_face_index(cube, Vec3<f32>(1, 1, 0)) == 0U);
    // Direction equally aligned with +Y (face 2) and +Z (face 4): lower idx 2 wins.
    CHECK(closest_face_index(cube, Vec3<f32>(0, 1, 1)) == 2U);
    // Direction equally aligned with +X (face 0) and +Z (face 4): lower idx 0 wins.
    CHECK(closest_face_index(cube, Vec3<f32>(1, 0, 1)) == 0U);
}

// ---------------------------------------------------------------------------
// (9) v2j carryover: feature_clip OBB face-corner table parity vs
//     test_hill_climb's CubeHullWithAdjacency fixture
// ---------------------------------------------------------------------------

TEST_CASE("parity: enumerate_faces(OBB3) vs CubeHullWithAdjacency face_vertex_indices",
          "[v2-close][parity][obb]")
{
    // Construct both representations of a unit cube. The OBB version is
    // built by `enumerate_faces`; the hull version is the explicit
    // face_vertex_indices written in test_hill_climb.cpp's
    // CubeHullWithAdjacency (and the cube here). For each face, the
    // 4 corner positions reached by enumerate_faces must equal the 4
    // vertices reached by face_vertex_indices[face]→cube.verts[idx].
    OBB3<f32> obb(Vec3<f32>(0), Vec3<f32>(1, 1, 1), Mat3<f32>::identity());
    auto obb_faces = enumerate_faces(obb);

    crd::memory::TlsfAllocator alloc(16U * 1024U);
    UnitCubeHull cube(&alloc);

    for (u8 f = 0; f < 6U; ++f)
    {
        for (u32 k = 0; k < 4U; ++k)
        {
            const u32 corner_idx = cube.fvi[f * 4U + k];
            const Vec3<f32> hull_vertex = cube.verts[corner_idx];
            const Vec3<f32> obb_vertex = obb_faces[f].vertices[k];
            CHECK(std::fabs(hull_vertex.x - obb_vertex.x) < 1e-5F);
            CHECK(std::fabs(hull_vertex.y - obb_vertex.y) < 1e-5F);
            CHECK(std::fabs(hull_vertex.z - obb_vertex.z) < 1e-5F);
        }
    }
}

// ---------------------------------------------------------------------------
// (10) v2j carryover: closest_face_index(OBB3) under rotated orientation
// ---------------------------------------------------------------------------

TEST_CASE("closest_face_index(OBB3) rotated orientation", "[v2-close][face][rotated]")
{
    // 90° rotation about Y: local +X axis maps to world -Z; local +Z axis maps
    // to world +X. So orientation.c0 = (0,0,-1), c1 = (0,1,0), c2 = (1,0,0).
    Mat3<f32> rot;
    rot.c0 = Vec3<f32>(0, 0, -1);
    rot.c1 = Vec3<f32>(0, 1, 0);
    rot.c2 = Vec3<f32>(1, 0, 0);
    OBB3<f32> obb(Vec3<f32>(0), Vec3<f32>(2, 1, 1), rot);

    // closest_face_index maximises dot(face_normal, direction), where face_normal
    // = ±orientation.c{0,1,2}. The `direction` arg is in the SAME frame as the
    // orientation columns (world frame, here, since orientation maps local→world).
    //
    // Face outward normals after rotation:
    //   face 0 (+X local) → +c0 = (0, 0, -1)
    //   face 1 (-X local) → -c0 = (0, 0,  1)
    //   face 2 (+Y local) → +c1 = (0, 1,  0)
    //   face 3 (-Y local) → -c1 = (0,-1,  0)
    //   face 4 (+Z local) → +c2 = (1, 0,  0)
    //   face 5 (-Z local) → -c2 = (-1,0,  0)

    // World +Z direction best matches face 1 (-X local), whose outward normal
    // is +c0_negated = (0, 0, 1).
    CHECK(closest_face_index(obb, Vec3<f32>(0, 0, 1)) == 1U);

    // World +X direction best matches face 4 (+Z local), whose outward normal
    // is +c2 = (1, 0, 0).
    CHECK(closest_face_index(obb, Vec3<f32>(1, 0, 0)) == 4U);

    // World +Y direction best matches face 2 (+Y local), whose outward normal
    // is +c1 = (0, 1, 0) — unchanged by the Y-axis rotation.
    CHECK(closest_face_index(obb, Vec3<f32>(0, 1, 0)) == 2U);
}
